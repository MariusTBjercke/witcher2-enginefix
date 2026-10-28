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
#pragma warning(pop)

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
    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    // Process exit releases the test image; no game files are loaded or written.
    return failures ? 1 : 0;
}
#endif
