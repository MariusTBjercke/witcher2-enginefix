// Executes the real distance hook bridges against synthetic x86 engine sites.
// This tests branch behavior and ABI preservation without starting Witcher 2.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#if !__has_include("../../src/distance_patch.h")
int main(void) { puts("FAIL: distance hook implementation is absent"); return 1; }
#else
#include "../../src/distance_patch.h"
static int failures;
static DWORD WINAPI peer_return(LPVOID arg) { (void)arg; return 0; }
static void check(const char* what, int pass) {
    printf("%s %s\n", pass ? "PASS" : "FAIL", what);
    if (!pass) failures++;
}

// The engine's range site expects EBX=proxy, XMM6=distance squared.
#pragma warning(push)
#pragma warning(disable: 4100) // Named parameters are referenced by stack slot in naked assembly.
__declspec(naked) static int run_range(void* entry, void* proxy, float d2) {
    __asm {
        push ebx
        push esi
        push edi
        mov ecx, [esp+16]
        mov ebx, [esp+20]
        movss xmm6, [esp+24]
        mov esi, 12345678h
        mov edi, 76543210h
        call ecx
        cmp ebx, [esp+20]
        jne bad
        cmp esi, 12345678h
        jne bad
        cmp edi, 76543210h
        jne bad
        jmp done
    bad:
        mov eax, -1
    done:
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// Shadow site reads [ESP+EAX*4+0x30]; account for CALL's return address.
__declspec(naked) static int run_shadow(void* entry, unsigned tier, float d2) {
    __asm {
        push ebx
        mov ecx, [esp+8]
        mov eax, [esp+12]
        movss xmm6, [esp+16]
        sub esp, 80h
        mov dword ptr [esp+2ch], 40800000h
        mov dword ptr [esp+30h], 41c80000h
        mov dword ptr [esp+34h], 44992000h
        mov dword ptr [esp+38h], 45992000h
        mov dword ptr [esp+3ch], 471c4000h
        mov dword ptr [esp+40h], 49742400h
        mov ebx, 12345678h
        call ecx
        cmp ebx, 12345678h
        je done
        mov eax, -1
    done:
        add esp, 80h
        pop ebx
        ret
    }
}
// GOG's range site loads the squared distance from [ESP+0x18] into XMM7 itself, after the
// replaced instruction; XMM6 holds the compiler's 0.0 there.
__declspec(naked) static int run_range_gog(void* entry, void* proxy, float d2) {
    __asm {
        push ebx
        push esi
        push edi
        mov ecx, [esp+16]
        mov ebx, [esp+20]
        mov eax, [esp+24]
        sub esp, 20h
        mov [esp+14h], eax          ; [ESP+18h] at the site, after CALL's return address
        xorps xmm6, xmm6
        mov esi, 12345678h
        mov edi, 76543210h
        call ecx
        add esp, 20h
        cmp ebx, [esp+20]
        jne bad
        cmp esi, 12345678h
        jne bad
        cmp edi, 76543210h
        jne bad
        jmp done
    bad:
        mov eax, -1
    done:
        pop edi
        pop esi
        pop ebx
        ret
    }
}

// GOG's shadow site compares with XMM7.
__declspec(naked) static int run_shadow_gog(void* entry, unsigned tier, float d2) {
    __asm {
        push ebx
        mov ecx, [esp+8]
        mov eax, [esp+12]
        movss xmm7, [esp+16]
        xorps xmm6, xmm6
        sub esp, 80h
        mov dword ptr [esp+2ch], 40800000h
        mov dword ptr [esp+30h], 41c80000h
        mov dword ptr [esp+34h], 44992000h
        mov dword ptr [esp+38h], 45992000h
        mov dword ptr [esp+3ch], 471c4000h
        mov dword ptr [esp+40h], 49742400h
        mov ebx, 12345678h
        call ecx
        cmp ebx, 12345678h
        je done
        mov eax, -1
    done:
        add esp, 80h
        pop ebx
        ret
    }
}
#pragma warning(pop)

// GOG 3.5.0.26g: its own bytes at its own RVAs, its range bridge, the same shadow bridge.
static void gog_fixture(unsigned char* proxy) {
    const uintptr_t range_rva = 0x0060C120u, shadow_rva = 0x0060C4F6u;
    const SIZE_T span = shadow_rva + 0x1000;
    unsigned char* base = (unsigned char*)VirtualAlloc(NULL, span,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char *range, *shadow;
    if (!base) { check("GOG fixture allocated", 0); return; }
    range = base + range_rva;
    shadow = base + shadow_rva;
    memcpy(range, ef_range_signature_gog, sizeof ef_range_signature_gog);
    memcpy(shadow, ef_shadow_signature_gog, sizeof ef_shadow_signature_gog);
    // The range JBE (at +17, rel32 1D7) lands at +0x1EE; the shadow layout is Steam's.
    memcpy(range+23, "\xB8\x01\x00\x00\x00\xC3", 6);
    memcpy(range+0x1ee, "\x31\xC0\xC3", 3);
    memcpy(shadow+13, "\x0F\xB6\xC0\xC3", 4);
    memcpy(shadow+18, "\x31\xC0\xC3", 3);
    *(float*)(proxy+0xf0) = 400.0f;
    check("GOG: unpatched range fixture rejects 30 units beyond 20",
        run_range_gog(range, proxy, 900.0f) == 0 && run_range_gog(range, proxy, 399.0f) == 1);
    check("GOG: unpatched shadow fixture rejects 50 units at tier 35",
        run_shadow_gog(shadow, 2, 2500.0f) == 0);
    ef_distance_active = 0;
    ef_range_rva = range_rva;
    ef_shadow_rva = shadow_rva;
    ef_select_build(0);
    check("GOG: Steam signatures fail closed on GOG bytes",
        !ef_distance_install(base, span) && range[0] == 0xf3 && shadow[0] == 0xf3);
    ef_select_build(1);
    ef_range_sq = ef_shadow_sq = 1.0f;
    check("GOG: verified sites install", ef_distance_install(base, span));
    if (!ef_distance_active) return;
    ef_range_hits = ef_range_rejected = ef_range_admitted = ef_shadow_hits = 0;
    check("GOG: 1x range keeps inside, boundary and outside",
        run_range_gog(range, proxy, 399.0f) == 1 && run_range_gog(range, proxy, 400.0f) == 0 &&
        run_range_gog(range, proxy, 900.0f) == 0);
    check("GOG: 1x telemetry sees the baseline rejections", ef_range_rejected == 2 && ef_range_admitted == 0);
    ef_range_sq = 4.0f;
    check("GOG: 2x range admits 30 units and rejects 40",
        run_range_gog(range, proxy, 900.0f) == 1 && run_range_gog(range, proxy, 1600.0f) == 0);
    check("GOG: telemetry compares with the stack distance, not XMM6", ef_range_admitted == 1);
    check("GOG: proxy threshold is never rewritten", *(float*)(proxy+0xf0) == 400.0f);
    ef_shadow_sq = 4.0f;
    check("GOG: 2x shadow admits 50 units at tier 35 and rejects 70",
        run_shadow_gog(shadow, 2, 2500.0f) == 1 && run_shadow_gog(shadow, 2, 4900.0f) == 0);
    check("GOG: both bridges executed", ef_range_hits > 0 && ef_shadow_hits > 0);
    ef_select_build(0);
}

// The atlas size: only the immediate's second byte changes, and only over the game's bytes.
static void atlas_fixture(void) {
    unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) { check("atlas fixture allocated", 0); return; }
    memcpy(page, ef_atlas_signature, sizeof ef_atlas_signature);
    page[7] = 0xCC;
    page[0] = 0x90;
    check("atlas: other bytes fail closed", !ef_atlas_patch(page, 2048) && page[4] == 0x04);
    page[0] = 0xC7;
    check("atlas: 2048 writes one byte", ef_atlas_patch(page, 2048) &&
        *(uint32_t*)(page + 3) == 2048 && page[0] == 0xC7 && page[7] == 0xCC);
    check("atlas: a patched site is not patched again", !ef_atlas_patch(page, 4096) && *(uint32_t*)(page + 3) == 2048);
    memcpy(page, ef_atlas_signature, sizeof ef_atlas_signature);
    check("atlas: 4096 writes one byte", ef_atlas_patch(page, 4096) && *(uint32_t*)(page + 3) == 4096);
    check("atlas: only 1024, 2048 and 4096 are accepted",
        ef_cascade_atlas(1024) == 1024 && ef_cascade_atlas(2048) == 2048 && ef_cascade_atlas(4096) == 4096 &&
        ef_cascade_atlas(3000) == 1024 && ef_cascade_atlas(0) == 1024 && ef_cascade_atlas(8192) == 1024);
}

// The Poisson setter's bridge: scales the two used components and hands everything else on.
static void poisson_fixture(void) {
    unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    float vec[4] = { 0.5f, -0.25f, 7.0f, 9.0f };
    float* vec_ptr = vec;
    uintptr_t eax_out = 0, esp_before = 0, esp_after = 0;
    void* bridge = (void*)(uintptr_t)ef_poisson_bridge;
    if (!page) { check("Poisson fixture allocated", 0); return; }
    page[0] = 0xC2; page[1] = 0x08; page[2] = 0x00;     // ret 8
    ef_poisson_target = page;
#define RUN_POISSON() __asm { \
        __asm mov esp_before, esp \
        __asm push vec_ptr \
        __asm push 72 \
        __asm mov eax, 12345678h \
        __asm call bridge \
        __asm mov eax_out, eax \
        __asm mov esp_after, esp }
    ef_filter_factor = 1.0f;
    RUN_POISSON();
    check("Poisson: factor 1 leaves the sample alone", vec[0] == 0.5f && vec[1] == -0.25f);
    check("Poisson: EAX reaches the setter and the stack balances",
        eax_out == 0x12345678 && esp_before == esp_after);
    ef_filter_factor = 0.5f;
    RUN_POISSON();
    check("Poisson: the factor scales x and y", vec[0] == 0.25f && vec[1] == -0.125f);
    check("Poisson: z and w are not touched", vec[2] == 7.0f && vec[3] == 9.0f);
    ef_filter_factor = 1.0f;
#undef RUN_POISSON
}

// The billboard distance: the real bytes of both builds, then the bridge with the helper's registers.
#pragma warning(disable: 4731)   // billboard_run sets EBP on purpose, the bridge reads [EBP+8]
static float billboard_run(float show, float hide, float factor, uintptr_t* eax_out) {
    static float proxy[0x100];
    static unsigned char mesh[0x110];
    uintptr_t frame[3];
    uintptr_t frame_ptr = (uintptr_t)frame, mesh_ptr = (uintptr_t)mesh, out_eax = 0;
    void* bridge = (void*)(uintptr_t)ef_billboard_bridge;
    float result = 0.0f;
    frame[2] = (uintptr_t)proxy;
    proxy[0xF0 / 4] = hide * hide;
    memcpy(mesh + 0x100, &show, sizeof show);
    ef_billboard_factor = factor;
    __asm {
        mov esi, frame_ptr
        mov ecx, mesh_ptr
        mov edx, bridge
        push ebp
        mov edi, ecx
        mov ebp, esi
        mov eax, 12345678h
        call edx
        pop ebp
        mov out_eax, eax
        movss result, xmm0
    }
    if (eax_out) *eax_out = out_eax;
    return result;
}
static void billboard_fixture(void) {
    unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char before[sizeof ef_billboard_signature];
    uintptr_t eax_out = 0;
    int gog;
    if (!page) { check("billboard fixture allocated", 0); return; }
    page[0] = 0xCC;
    check("billboard: wrong bytes are refused", !ef_billboard_install(page));
    for (gog = 0; gog < 2; gog++) {
        ef_select_build(gog);
        memset(page, 0xCC, 64);
        memcpy(page, ef_billboard_sig, ef_billboard_sig_len);
        memcpy(before, page, ef_billboard_sig_len);
        ef_billboard_active = 0;
        check("billboard: the build's bytes install", ef_billboard_install(page) == 1);
        check("billboard: the site is a jump and the rest stays",
            page[0] == 0xE9 && page[5] == 0x90 && page[7] == 0x90 &&
            !memcmp(page + 8, before + 8, ef_billboard_sig_len - 8) && ef_billboard_return == page + 8);
    }
    ef_select_build(0);
    // The bridge returns here: a plain ret.
    page[0x80] = 0xC3;
    ef_billboard_return = page + 0x80;
    ef_billboard_hits = ef_billboard_moved = 0;
    check("billboard: factor 1 is the file's value", billboard_run(10.0f, 100.0f, 1.0f, &eax_out) == 10.0f);
    check("billboard: EAX survives", eax_out == 0x12345678);
    check("billboard: factor 1 does not count as moved", ef_billboard_hits == 1 && ef_billboard_moved == 0);
    check("billboard: the factor scales", billboard_run(10.0f, 100.0f, 3.0f, NULL) == 30.0f);
    check("billboard: a move is counted", ef_billboard_hits == 2 && ef_billboard_moved == 1);
    check("billboard: capped at 75% of the hide distance", billboard_run(10.0f, 30.0f, 3.0f, NULL) == 22.5f);
    check("billboard: never below the file's value", billboard_run(25.0f, 30.0f, 3.0f, NULL) == 25.0f);
    check("billboard: factor 1 keeps a value above the cap", billboard_run(25.0f, 30.0f, 1.0f, NULL) == 25.0f);
    check("billboard: scale range", ef_billboard_value(8.0f, 1.0f) == 8.0f && ef_billboard_value(8.5f, 2.0f) == 2.0f &&
        ef_billboard_value(0.5f, 2.0f) == 2.0f);
    ef_billboard_factor = 1.0f;
    ef_billboard_active = 0;
}

// The sun-shadow sites: the real MOVSS three times, each value stored where the probe can read it.
static void cascade_fixture(void) {
    unsigned char* page = (unsigned char*)VirtualAlloc(NULL, 0x1000,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char* sites[3];
    uintptr_t lens[3];
    float* k;
    float* out;
    int i;
    uint32_t disp;
    if (!page) { check("cascade fixture allocated", 0); return; }
    k = (float*)(page + 0x800);
    out = (float*)(page + 0x810);
    k[0] = 7.0f; k[1] = 17.0f; k[2] = 40.0f;
    for (i = 0; i < 3; i++) {
        sites[i] = page + i * 16;
        lens[i] = (uintptr_t)&k[i];
        memcpy(sites[i], "\xF3\x0F\x10\x05", 4);
        disp = (uint32_t)lens[i];
        memcpy(sites[i] + 4, &disp, 4);
        memcpy(sites[i] + 8, "\xF3\x0F\x11\x05", 4);
        disp = (uint32_t)(uintptr_t)&out[i];
        memcpy(sites[i] + 12, &disp, 4);
    }
    page[48] = 0xC3;
#define RUN_CASCADES() ((void (*)(void))(uintptr_t)page)()
#define OUT_IS(a, b, c) (out[0] == (a) && out[1] == (b) && out[2] == (c))

    RUN_CASCADES();
    check("cascade: unpatched sites load the game's 7, 17 and 40", OUT_IS(7.0f, 17.0f, 40.0f));

    lens[1] = (uintptr_t)&out[1];
    check("cascade: a different constant address fails closed",
        !ef_cascade_install(sites, lens) && !ef_cascade_active);
    lens[1] = (uintptr_t)&k[1];
    k[2] = 41.0f;
    check("cascade: one wrong constant leaves all sites alone",
        !ef_cascade_install(sites, lens) && !ef_cascade_active &&
        *(uint32_t*)(sites[0] + 4) == (uint32_t)lens[0] && *(uint32_t*)(sites[1] + 4) == (uint32_t)lens[1]);
    k[2] = 40.0f;
    sites[2][0] = 0x90;
    check("cascade: different opcode bytes fail closed", !ef_cascade_install(sites, lens) && !ef_cascade_active);
    sites[2][0] = 0xF3;

    ef_cascade_set(7.0f, 24.0f, 64.0f);
    check("cascade: verified sites install", ef_cascade_install(sites, lens));
    RUN_CASCADES();
    check("cascade: 7/24/64 m loads the game's 7, 17 and 40", OUT_IS(7.0f, 17.0f, 40.0f));
    ef_cascade_set(7.0f, 24.0f, 128.0f);
    RUN_CASCADES();
    check("cascade: end 128 m loads 104 live and leaves the others", OUT_IS(7.0f, 17.0f, 104.0f));
    ef_cascade_set(7.0f, 48.0f, 64.0f);
    RUN_CASCADES();
    check("cascade: seam 48 m loads 41 and 16 and keeps the 64 m end", OUT_IS(7.0f, 41.0f, 16.0f));
    ef_cascade_set(14.0f, 24.0f, 64.0f);
    RUN_CASCADES();
    check("cascade: first 14 m loads 14 and 10 and keeps 24 and 64 m", OUT_IS(14.0f, 10.0f, 40.0f));
    ef_cascade_set(24.0f, 24.0f, 64.0f);
    RUN_CASCADES();
    check("cascade: first 24 m pushes the seam out to 32 m", OUT_IS(24.0f, 8.0f, 32.0f));
    ef_cascade_set(48.0f, 24.0f, 64.0f);
    RUN_CASCADES();
    check("cascade: first 48 m pushes the seam to 56 m and the end to 72 m", OUT_IS(48.0f, 8.0f, 16.0f));
    ef_cascade_set(7.0f, 96.0f, 64.0f);
    RUN_CASCADES();
    check("cascade: seam 96 m pushes the end out to 112 m", OUT_IS(7.0f, 89.0f, 16.0f));
    check("cascade: the shared constants are never written", k[0] == 7.0f && k[1] == 17.0f && k[2] == 40.0f);
    check("cascade: only the disp32s changed",
        !memcmp(sites[0], "\xF3\x0F\x10\x05", 4) && !memcmp(sites[1], "\xF3\x0F\x10\x05", 4) &&
        !memcmp(sites[2], "\xF3\x0F\x10\x05", 4) && page[48] == 0xC3);
    check("cascade: repeat installation is harmless", ef_cascade_install(sites, lens));
    check("cascade: out-of-range values fall back to the game's",
        ef_cascade_far(10.0f) == 64.0f && ef_cascade_far(1000.0f) == 64.0f &&
        ef_cascade_far((float)NAN) == 64.0f && ef_cascade_far(128.0f) == 128.0f &&
        ef_cascade_seam(7.0f) == 24.0f && ef_cascade_seam((float)NAN) == 24.0f && ef_cascade_seam(48.0f) == 48.0f &&
        ef_cascade_first(1.0f) == 7.0f && ef_cascade_first((float)NAN) == 7.0f && ef_cascade_first(14.0f) == 14.0f);
    ef_cascade_set(7.0f, 24.0f, 64.0f);
#undef RUN_CASCADES
#undef OUT_IS
}

int main(void) {
    const SIZE_T span = EF_SHADOW_RVA + 0x1000;
    unsigned char proxy[0x150] = {0};
    unsigned char* base = (unsigned char*)VirtualAlloc(NULL, span,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned char* range;
    unsigned char* shadow;
    DWORD before;
    DWORD old_protect;
    MEMORY_BASIC_INFORMATION protection;
    HANDLE peer;
    CONTEXT saved_context, parked_context;
    if (!base) return 2;
    range = base + EF_RANGE_RVA;
    shadow = base + EF_SHADOW_RVA;
    memcpy(range, ef_range_signature, sizeof ef_range_signature);
    memcpy(shadow, ef_shadow_signature, sizeof ef_shadow_signature);
    // Original range JBE lands 0x1e3 bytes beyond the beginning of the site.
    memcpy(range+17, "\xB8\x01\x00\x00\x00\xC3", 6);
    memcpy(range+0x1e3, "\x31\xC0\xC3", 3);
    memcpy(shadow+13, "\x0F\xB6\xC0\xC3", 4);
    memcpy(shadow+18, "\x31\xC0\xC3", 3);
    *(float*)(proxy+0xf0) = 400.0f;
    check("unpatched fixture rejects 30 units beyond 20-unit range",
        run_range(range, proxy, 900.0f) == 0);
    check("unpatched shadow fixture rejects 50 units at tier 35",
        run_shadow(shadow, 2, 2500.0f) == 0);
    range[0] ^= 1;
    before = *(DWORD*)shadow;
    check("signature mismatch fails closed without a partial shadow patch",
        !ef_distance_install(base, span) && *(DWORD*)shadow == before);
    range[0] ^= 1;
    check("short image fails closed", !ef_distance_install(base, EF_RANGE_RVA));
    VirtualProtect(base, span, PAGE_EXECUTE_READ, &old_protect);
    peer = CreateThread(NULL, 0, peer_return, NULL, CREATE_SUSPENDED, NULL);
    if (!peer) return 2;
    saved_context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(peer, &saved_context)) return 2;
    parked_context = saved_context;
    parked_context.Eip = (DWORD)(uintptr_t)(range+1);
    if (!SetThreadContext(peer, &parked_context)) return 2;
    check("peer parked inside overwritten instruction refuses both patches",
        !ef_distance_install(base, span) && range[0] == 0xf3 && shadow[0] == 0xf3);
    VirtualQuery(range, &protection, sizeof protection);
    check("refusal restores shared-page RX protection", protection.Protect == PAGE_EXECUTE_READ);
    check("refusal restores the peer's prior suspension count", SuspendThread(peer) == 1);
    check("test removes only its added suspension", ResumeThread(peer) == 2);
    if (!SetThreadContext(peer, &saved_context)) return 2;
    ef_range_sq = ef_shadow_sq = 1.0f;
    check("verified sites install", ef_distance_install(base, span));
    if (!ef_distance_active) return 1;
    VirtualQuery(range, &protection, sizeof protection);
    check("successful install restores shared-page RX protection", protection.Protect == PAGE_EXECUTE_READ);
    check("successful install balances peer suspension", ResumeThread(peer) == 1);
    check("peer completes after successful install", WaitForSingleObject(peer, 2000) == WAIT_OBJECT_0);
    CloseHandle(peer);
    check("1x range retains inside behavior and nonvolatile registers",
        run_range(range, proxy, 399.0f) == 1);
    check("1x range retains exact boundary rejection", run_range(range, proxy, 400.0f) == 0);
    check("1x range retains outside rejection", run_range(range, proxy, 900.0f) == 0);
    ef_range_sq = 4.0f;
    check("2x linear range admits 30 units", run_range(range, proxy, 900.0f) == 1);
    check("2x range still rejects its 40-unit boundary", run_range(range, proxy, 1600.0f) == 0);
    check("proxy threshold is never rewritten or compounded", *(float*)(proxy+0xf0) == 400.0f);
    ef_shadow_sq = 1.0f;
    check("range control does not extend shadows", run_shadow(shadow, 2, 2500.0f) == 0);
    ef_shadow_sq = 4.0f;
    check("2x shadow range admits 50 units at tier 35", run_shadow(shadow, 2, 2500.0f) == 1);
    check("2x shadow range rejects its 70-unit boundary", run_shadow(shadow, 2, 4900.0f) == 0);
    check("shadow tier indexing is preserved", run_shadow(shadow, 1, 101.0f) == 0);
    ef_range_sq = 1.0f;
    check("1x range restores baseline while shadow remains boosted",
        run_range(range, proxy, 900.0f) == 0 && run_shadow(shadow, 2, 2500.0f) == 1);
    check("both bridges actually executed", ef_range_hits > 0 && ef_shadow_hits > 0);
    check("range admission telemetry observed the changed branch", ef_range_admitted > 0);
    check("repeat installation is harmless", ef_distance_install(base, span));
    check("invalid factors revert to baseline",
        ef_distance_factor(0.0f) == 1.0f && ef_distance_factor(-1.0f) == 1.0f &&
        ef_distance_factor(100.0f) == 1.0f && ef_distance_factor((float)NAN) == 1.0f);
    gog_fixture(proxy);
    cascade_fixture();
    atlas_fixture();
    poisson_fixture();
    billboard_fixture();
    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    // Process exit releases the test image; no game files are loaded or written.
    return failures ? 1 : 0;
}
#endif
