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
#endif
