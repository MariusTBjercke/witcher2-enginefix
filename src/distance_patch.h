// Mesh visibility / caster-distance hooks for Steam 3.5.0.26 and GOG 3.5.0.26g.
// Shared with the native machine-code probe. No engine fields are overwritten.
#ifndef W2_DISTANCE_PATCH_H
#define W2_DISTANCE_PATCH_H

#include <stdint.h>
#include "patch_barrier.h"

#define EF_RANGE_RVA  0x005BB09Fu
#define EF_SHADOW_RVA 0x005BB465u
// The sites ef_distance_install patches, as RVAs. The Steam ones by default (the probe uses
// them); enginefix.c sets the running build's (ga_rva) before installing, 0 = not on it.
static uintptr_t ef_range_rva = EF_RANGE_RVA, ef_shadow_rva = EF_SHADOW_RVA;
static const unsigned char ef_range_signature[] = {
    0xf3,0x0f,0x10,0x83,0xf0,0,0,0,0x0f,0x2f,0xc6,0x0f,0x86,0xd2,1,0,0
};
static const unsigned char ef_shadow_signature[] = {
    0xf3,0x0f,0x10,0x44,0x84,0x30,0x0f,0x2f,0xc6,0x76,7,0xb0,1
};
// GOG 3.5.0.26g (RVA 0060C120 / 0060C4F6, docs/fixes/render-distance-experiment.md): the same
// replaced instructions, but the squared distance is loaded into XMM7 after the range site
// ([ESP+0x18]) and XMM6 is 0 there.
static const unsigned char ef_range_signature_gog[] = {
    0xf3,0x0f,0x10,0x83,0xf0,0,0,0,0xf3,0x0f,0x10,0x7c,0x24,0x18,0x0f,0x2f,0xc7,
    0x0f,0x86,0xd7,1,0,0
};
static const unsigned char ef_shadow_signature_gog[] = {
    0xf3,0x0f,0x10,0x44,0x84,0x30,0x0f,0x2f,0xc7,0x76,7,0xb0,1
};
static volatile float ef_range_sq = 1.0f, ef_shadow_sq = 1.0f;
static volatile LONG ef_range_hits, ef_shadow_hits;
static volatile LONG ef_range_rejected, ef_range_admitted;
static void* ef_range_return;
static void* ef_shadow_return;
static int ef_distance_active;

static float ef_distance_factor(float value) {
    return value >= 1.0f && value <= 4.0f ? value : 1.0f;
}

// Reproduce exactly the overwritten load, then scale only XMM0 (threshold).
// Preserve all integer registers, flags, the stack and other SSE registers.
__declspec(naked) static void ef_range_bridge(void) {
    __asm {
        pushfd
        lock inc dword ptr [ef_range_hits]
        movss xmm0, dword ptr [ebx+0f0h]
        comiss xmm0, xmm6
        ja inside
        lock inc dword ptr [ef_range_rejected]
        mulss xmm0, dword ptr [ef_range_sq]
        comiss xmm0, xmm6
        jbe done
        lock inc dword ptr [ef_range_admitted]
        jmp done
    inside:
        mulss xmm0, dword ptr [ef_range_sq]
    done:
        popfd
        jmp dword ptr [ef_range_return]
    }
}

// GOG: the same scaling. The game loads the squared distance from [ESP+0x18] into XMM7 right
// after the replaced instruction, so the telemetry compares against that stack slot (+4 for the
// pushfd) instead of XMM6, which is 0 on GOG.
__declspec(naked) static void ef_range_bridge_gog(void) {
    __asm {
        pushfd
        lock inc dword ptr [ef_range_hits]
        movss xmm0, dword ptr [ebx+0f0h]
        comiss xmm0, dword ptr [esp+1ch]
        ja inside
        lock inc dword ptr [ef_range_rejected]
        mulss xmm0, dword ptr [ef_range_sq]
        comiss xmm0, dword ptr [esp+1ch]
        jbe done
        lock inc dword ptr [ef_range_admitted]
        jmp done
    inside:
        mulss xmm0, dword ptr [ef_range_sq]
    done:
        popfd
        jmp dword ptr [ef_range_return]
    }
}

__declspec(naked) static void ef_shadow_bridge(void) {
    __asm {
        pushfd
        lock inc dword ptr [ef_shadow_hits]
        popfd
        movss xmm0, dword ptr [esp+eax*4+30h]
        mulss xmm0, dword ptr [ef_shadow_sq]
        jmp dword ptr [ef_shadow_return]
    }
}

// Where a plant swaps its mesh for a billboard (docs/fixes/graphics-presets.md). A mesh proxy's LOD
// distances are multiplied by MeshDistanceScale when the proxy is created, but its billboard distance is
// not, so the billboard takes over at the mesh file's value, often 4-15 m for plants, on every level. The replaced MOVSS loads that
// distance in the helper that adds the billboard level: EDI = mesh, [EBP+8] = proxy, whose +0xF0 is the
// squared distance where the proxy hides. Read once per proxy, so a new factor reaches proxies created
// after it. Never below the file's value, so factor 1 is the game's own number exactly, and scaled no
// further than EF_BILLBOARD_CAP of the hide distance, so a plant still fades through its billboard
// instead of vanishing as a full mesh.
static const unsigned char ef_billboard_signature[] = {
    0xf3,0x0f,0x10,0x87,0,1,0,0,0x6a,3,0xf3,0x0f,0x59,0xc0,0x6a,0x0c
};
static const unsigned char ef_billboard_signature_gog[] = {
    0xf3,0x0f,0x10,0x87,0,1,0,0,0xf3,0x0f,0x59,0xc0,0x6a,0x0c
};
static const unsigned char* ef_billboard_sig = ef_billboard_signature;
static unsigned ef_billboard_sig_len = sizeof ef_billboard_signature;
static const float ef_billboard_cap = 0.75f;
static volatile float ef_billboard_factor = 1.0f;
static volatile float ef_billboard_last_file, ef_billboard_last_used;
static volatile LONG ef_billboard_hits, ef_billboard_moved;
static void* ef_billboard_return;
static int ef_billboard_active;

static float ef_billboard_value(float value, float fallback) {
    return value >= 1.0f && value <= 8.0f ? value : fallback;
}

__declspec(naked) static void ef_billboard_bridge(void) {
    __asm {
        pushfd
        push eax
        sub esp, 4
        movss dword ptr [esp], xmm1
        lock inc dword ptr [ef_billboard_hits]
        mov eax, dword ptr [ebp+8]
        sqrtss xmm1, dword ptr [eax+0f0h]
        mulss xmm1, dword ptr [ef_billboard_cap]
        movss xmm0, dword ptr [edi+100h]
        mulss xmm0, dword ptr [ef_billboard_factor]
        minss xmm0, xmm1
        maxss xmm0, dword ptr [edi+100h]
        movss xmm1, dword ptr [edi+100h]
        movss dword ptr [ef_billboard_last_file], xmm1
        movss dword ptr [ef_billboard_last_used], xmm0
        comiss xmm0, xmm1
        jbe same
        lock inc dword ptr [ef_billboard_moved]
    same:
        movss xmm1, dword ptr [esp]
        add esp, 4
        pop eax
        popfd
        jmp dword ptr [ef_billboard_return]
    }
}

// What ef_distance_install checks and jumps to: Steam's by default (the probe's first fixture),
// GOG's after ef_select_build(1). The shadow bridge suits both.
static const unsigned char* ef_range_sig = ef_range_signature;
static unsigned ef_range_sig_len = sizeof ef_range_signature;
static const unsigned char* ef_shadow_sig = ef_shadow_signature;
static unsigned ef_shadow_sig_len = sizeof ef_shadow_signature;
static void (*ef_range_target)(void) = ef_range_bridge;

static void ef_select_build(int gog) {
    ef_range_sig = gog ? ef_range_signature_gog : ef_range_signature;
    ef_range_sig_len = gog ? sizeof ef_range_signature_gog : sizeof ef_range_signature;
    ef_shadow_sig = gog ? ef_shadow_signature_gog : ef_shadow_signature;
    ef_shadow_sig_len = gog ? sizeof ef_shadow_signature_gog : sizeof ef_shadow_signature;
    ef_range_target = gog ? ef_range_bridge_gog : ef_range_bridge;
    ef_billboard_sig = gog ? ef_billboard_signature_gog : ef_billboard_signature;
    ef_billboard_sig_len = gog ? sizeof ef_billboard_signature_gog : sizeof ef_billboard_signature;
}

// The peer-stop protocol lives in patch_barrier.h (shared with the window-mode patches): the
// two distance sites are the ranges a peer must not be executing while they are rewritten.
static int ef_stop_peers(HANDLE* peers, unsigned* count,
                        unsigned char* range, unsigned char* shadow) {
    pb_range ranges[2];

    ranges[0].start = range;
    ranges[0].len = 8;
    ranges[1].start = shadow;
    ranges[1].len = 6;
    return pb_stop_peers(peers, count, ranges, 2);
}
static int ef_distance_install(unsigned char* base, size_t span) {
    unsigned char *range, *shadow;
    unsigned char range_jump[8] = {0xe9,0,0,0,0,0x90,0x90,0x90};
    unsigned char shadow_jump[6] = {0xe9,0,0,0,0,0x90};
    DWORD range_protect, shadow_protect, ignored;
    HANDLE peers[256];
    unsigned count = 0, i;
    if (ef_distance_active) return 1;
    if (!base || !ef_range_rva || !ef_shadow_rva ||
        span < ef_range_rva + ef_range_sig_len ||
        span < ef_shadow_rva + ef_shadow_sig_len) return 0;
    range = base + ef_range_rva;
    shadow = base + ef_shadow_rva;
    if (memcmp(range, ef_range_sig, ef_range_sig_len) ||
        memcmp(shadow, ef_shadow_sig, ef_shadow_sig_len)) return 0;
    ef_range_return = range + 8;
    ef_shadow_return = shadow + 6;
    *(uint32_t*)(range_jump+1) = (uint32_t)((uintptr_t)ef_range_target - (uintptr_t)(range+5));
    *(uint32_t*)(shadow_jump+1) = (uint32_t)((uintptr_t)ef_shadow_bridge - (uintptr_t)(shadow+5));
    if (!VirtualProtect(range, 8, PAGE_EXECUTE_READWRITE, &range_protect)) return 0;
    if (!VirtualProtect(shadow, 6, PAGE_EXECUTE_READWRITE, &shadow_protect)) {
        VirtualProtect(range, 8, range_protect, &ignored);
        return 0;
    }
    if (!ef_stop_peers(peers, &count, range, shadow)) {
        VirtualProtect(shadow, 6, shadow_protect, &ignored);
        VirtualProtect(range, 8, range_protect, &ignored);
        return 0;
    }
    // Revalidate under the barrier in case another module changed either site.
    if (memcmp(range, ef_range_sig, ef_range_sig_len) ||
        memcmp(shadow, ef_shadow_sig, ef_shadow_sig_len)) {
        VirtualProtect(shadow, 6, shadow_protect, &ignored);
        VirtualProtect(range, 8, range_protect, &ignored);
        for (i = 0; i < count; i++) ResumeThread(peers[i]);
        for (i = 0; i < count; i++) CloseHandle(peers[i]);
        return 0;
    }
    // In-process peer creation has quiesced; all peers are outside both spans.
    memcpy(range, range_jump, sizeof range_jump);
    memcpy(shadow, shadow_jump, sizeof shadow_jump);
    FlushInstructionCache(GetCurrentProcess(), range, sizeof range_jump);
    FlushInstructionCache(GetCurrentProcess(), shadow, sizeof shadow_jump);
    // Sites share a page in this build: restore the second protection first.
    VirtualProtect(shadow, 6, shadow_protect, &ignored);
    VirtualProtect(range, 8, range_protect, &ignored);
    ef_distance_active = 1;
    for (i = 0; i < count; i++) ResumeThread(peers[i]);
    for (i = 0; i < count; i++) CloseHandle(peers[i]);
    return 1;
}

// site: the MOVSS in the running image. Patched under the same peer barrier, or left alone.
static int ef_billboard_install(unsigned char* site) {
    unsigned char jump[8] = {0xe9,0,0,0,0,0x90,0x90,0x90};
    DWORD protect, ignored;
    HANDLE peers[256];
    pb_range range;
    unsigned count = 0, i;
    if (ef_billboard_active) return 1;
    if (!site || memcmp(site, ef_billboard_sig, ef_billboard_sig_len)) return 0;
    ef_billboard_return = site + 8;
    *(uint32_t*)(jump+1) = (uint32_t)((uintptr_t)ef_billboard_bridge - (uintptr_t)(site+5));
    if (!VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &protect)) return 0;
    range.start = site;
    range.len = 8;
    if (!pb_stop_peers(peers, &count, &range, 1)) {
        VirtualProtect(site, 8, protect, &ignored);
        return 0;
    }
    if (!memcmp(site, ef_billboard_sig, ef_billboard_sig_len)) {
        memcpy(site, jump, sizeof jump);
        FlushInstructionCache(GetCurrentProcess(), site, sizeof jump);
        ef_billboard_active = 1;
    }
    VirtualProtect(site, 8, protect, &ignored);
    for (i = 0; i < count; i++) ResumeThread(peers[i]);
    for (i = 0; i < count; i++) CloseHandle(peers[i]);
    return ef_billboard_active;
}

// Sun-shadow distances ([shadows] enabled=1, docs/fixes/npc-streaming-sun-shadows.md). The renderer
// builds three camera slices per frame from the lengths 7, 17 and 40 in SetupRenderFrameCollector.
// Each length is one MOVSS XMM0,[disp32] of a .rdata constant other code shares, so the disp32s
// point at these floats; the constants stay as they are. The game reads them again every frame.
#define EF_CASCADE_MIN_MID  8.0f    // the middle slice never gets shorter than this
#define EF_CASCADE_MIN_LAST 16.0f   // nor the last one
static volatile float ef_cascade_first_len = 7.0f, ef_cascade_mid_len = 17.0f, ef_cascade_len = 40.0f;
static int ef_cascade_active;

// Where each slice ends, in metres from the camera near value. Out of range = the game's value.
static float ef_cascade_first(float value) {
    return value >= 7.0f && value <= 128.0f ? value : 7.0f;
}
static float ef_cascade_seam(float value) {
    return value >= 24.0f && value <= 256.0f ? value : 24.0f;
}
static float ef_cascade_far(float value) {
    return value >= 64.0f && value <= 512.0f ? value : 64.0f;
}

// A later slice is pushed out when an earlier one would otherwise pass it.
static float ef_cascade_seam_end(float first, float seam) {
    first = ef_cascade_first(first);
    seam = ef_cascade_seam(seam);
    return seam - first < EF_CASCADE_MIN_MID ? first + EF_CASCADE_MIN_MID : seam;
}
static float ef_cascade_far_end(float first, float seam, float far_end) {
    seam = ef_cascade_seam_end(first, seam);
    far_end = ef_cascade_far(far_end);
    return far_end - seam < EF_CASCADE_MIN_LAST ? seam + EF_CASCADE_MIN_LAST : far_end;
}

static void ef_cascade_set(float first, float seam, float far_end) {
    float end = ef_cascade_far_end(first, seam, far_end);
    float mid = ef_cascade_seam_end(first, seam);
    first = ef_cascade_first(first);
    ef_cascade_first_len = first;
    ef_cascade_mid_len = mid - first;
    ef_cascade_len = end - mid;
}

static int ef_cascade_site_ok(const unsigned char* site, uintptr_t len_va, float expect) {
    static const unsigned char op[4] = {0xf3,0x0f,0x10,0x05};
    uint32_t disp;
    float value;
    if (memcmp(site, op, sizeof op)) return 0;
    memcpy(&disp, site + 4, sizeof disp);
    memcpy(&value, (const void*)len_va, sizeof value);
    return (uintptr_t)disp == len_va && value == expect;
}

// sites: the MOVSS instructions in the running image. len_vas: the running addresses of the
// 7.0, 17.0 and 40.0 they load. All three are verified first and patched together, or none.
static int ef_cascade_install(unsigned char* const sites[3], const uintptr_t len_vas[3]) {
    static const float expect[3] = { 7.0f, 17.0f, 40.0f };
    const volatile float* const lens[3] = { &ef_cascade_first_len, &ef_cascade_mid_len, &ef_cascade_len };
    pb_range ranges[3];
    DWORD protect[3], ignored;
    HANDLE peers[256];
    unsigned count = 0, i;
    int ok = 1;
    if (ef_cascade_active) return 1;
    for (i = 0; i < 3; i++)
        if (!sites[i] || !ef_cascade_site_ok(sites[i], len_vas[i], expect[i])) return 0;
    for (i = 0; i < 3; i++) {
        if (!VirtualProtect(sites[i], 8, PAGE_EXECUTE_READWRITE, &protect[i])) {
            while (i-- > 0) VirtualProtect(sites[i], 8, protect[i], &ignored);
            return 0;
        }
        ranges[i].start = sites[i];
        ranges[i].len = 8;
    }
    if (!pb_stop_peers(peers, &count, ranges, 3)) {
        for (i = 3; i-- > 0;) VirtualProtect(sites[i], 8, protect[i], &ignored);
        return 0;
    }
    // Revalidate under the barrier in case another module changed a site.
    for (i = 0; i < 3; i++)
        if (!ef_cascade_site_ok(sites[i], len_vas[i], expect[i])) ok = 0;
    if (ok) {
        for (i = 0; i < 3; i++) {
            uint32_t disp = (uint32_t)(uintptr_t)lens[i];
            memcpy(sites[i] + 4, &disp, sizeof disp);
            FlushInstructionCache(GetCurrentProcess(), sites[i], 8);
        }
        ef_cascade_active = 1;
    }
    // The sites share a page: restore in reverse order, so the first saved protection ends up on it.
    for (i = 3; i-- > 0;) VirtualProtect(sites[i], 8, protect[i], &ignored);
    for (i = 0; i < count; i++) ResumeThread(peers[i]);
    for (i = 0; i < count; i++) CloseHandle(peers[i]);
    return ef_cascade_active;
}

// Sun-shadow atlas size ([shadows] atlas). The renderer's constructor stores 1024 per cascade at
// renderer+0x20 with MOV dword ptr [EAX+0x20],0x400; the atlas is created from it, as 1 x 3 cascades,
// and again after a device reset. Only the immediate's second byte changes, so the write is one byte.
static const unsigned char ef_atlas_signature[] = { 0xc7,0x40,0x20,0x00,0x04,0x00,0x00 };

static int ef_cascade_atlas(int value) {
    return value == 2048 || value == 4096 ? value : 1024;
}

// 1 when the site holds the game's 1024 and was changed to size, 0 when it does not match (yet).
static int ef_atlas_patch(unsigned char* site, int size) {
    DWORD protect, ignored;
    if (!site || memcmp(site, ef_atlas_signature, sizeof ef_atlas_signature)) return 0;
    if (!VirtualProtect(site + 4, 1, PAGE_EXECUTE_READWRITE, &protect)) return 0;
    site[4] = (unsigned char)(size >> 8);
    FlushInstructionCache(GetCurrentProcess(), site + 4, 1);
    VirtualProtect(site + 4, 1, protect, &ignored);
    return 1;
}

// Sun-shadow filter width. The shader's offset for each of its 6 samples is its UV radius per cascade (a
// def constant) times a sample from vPoissonSamples, and the renderer sets those from the CPU every frame.
// So scaling the samples scales all three cascades alike, live, with no shader change. The bridge sits
// on the setter's call: EAX = device, stack = register, vector, RET 8. It scales the frame's copy of the
// vector. The caller reloads XMM0 and ECX before using them again.
static volatile float ef_filter_factor = 1.0f;
static void* ef_poisson_target;

__declspec(naked) static void ef_poisson_bridge(void) {
    __asm {
        mov ecx, dword ptr [esp+8]
        movss xmm0, dword ptr [ecx]
        mulss xmm0, dword ptr [ef_filter_factor]
        movss dword ptr [ecx], xmm0
        movss xmm0, dword ptr [ecx+4]
        mulss xmm0, dword ptr [ef_filter_factor]
        movss dword ptr [ecx+4], xmm0
        jmp dword ptr [ef_poisson_target]
    }
}
#endif
