// Experimental Steam 3.5.0.26 mesh visibility / caster-distance hooks.
// Shared with the native machine-code probe. No engine fields are overwritten.
#ifndef W2_DISTANCE_PATCH_H
#define W2_DISTANCE_PATCH_H

#include <stdint.h>

#define EF_RANGE_RVA  0x005BB09Fu
#define EF_SHADOW_RVA 0x005BB465u
static const unsigned char ef_range_signature[] = {
    0xf3,0x0f,0x10,0x83,0xf0,0,0,0,0x0f,0x2f,0xc6,0x0f,0x86,0xd2,1,0,0
};
static const unsigned char ef_shadow_signature[] = {
    0xf3,0x0f,0x10,0x44,0x84,0x30,0x0f,0x2f,0xc6,0x76,7,0xb0,1
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

// Native enumeration is a kernel call, so repeated passes need no heap/snapshot
// allocation while peers are stopped. Signature checked against System Informer
// phnt ntpsapi.h (NtGetNextThread). Resolve the export before stopping anyone.
// Freeze to a fixed point: a creator may have made a child during an earlier
// pass, but after a pass adds no threads all in-process creators are stopped.
// Remote injection by another process is outside this startup-only protocol.
typedef LONG (NTAPI *EF_NextThread)(HANDLE, HANDLE, DWORD, ULONG, ULONG, PHANDLE);
static int ef_stop_peers(HANDLE* peers, unsigned* count,
                        unsigned char* range, unsigned char* shadow) {
    EF_NextThread next_thread = (EF_NextThread)(uintptr_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtGetNextThread");
    DWORD ids[256], self = GetCurrentThreadId();
    unsigned n = 0, pass, i;
    int ok = 0;
    *count = 0;
    if (!next_thread) return 0;
    for (pass = 0; pass < 8; pass++) {
        HANDLE cursor = NULL;
        int cursor_owned = 0;
        unsigned added = 0;
        for (;;) {
            HANDLE thread = NULL;
            LONG status = next_thread(GetCurrentProcess(), cursor,
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                0, 0, &thread);
            if (cursor && !cursor_owned) CloseHandle(cursor);
            cursor = NULL;
            cursor_owned = 0;
            if ((ULONG)status == 0x8000001aUL) break; // STATUS_NO_MORE_ENTRIES
            if (status < 0 || !thread) goto failed;
            cursor = thread;
            {
                DWORD id = GetThreadId(thread);
                CONTEXT context;
                if (!id) { CloseHandle(thread); goto failed; }
                if (id == self) continue;
                for (i = 0; i < n; i++) if (ids[i] == id) break;
                if (i < n) continue;
                if (n == 256 || SuspendThread(thread) == (DWORD)-1) {
                    CloseHandle(thread);
                    goto failed;
                }
                ids[n] = id;
                peers[n++] = thread;
                cursor_owned = 1;
                added++;
                context.ContextFlags = CONTEXT_CONTROL;
                if (!GetThreadContext(thread, &context) ||
                    (context.Eip >= (DWORD)(uintptr_t)range &&
                     context.Eip < (DWORD)(uintptr_t)(range+8)) ||
                    (context.Eip >= (DWORD)(uintptr_t)shadow &&
                     context.Eip < (DWORD)(uintptr_t)(shadow+6))) goto failed;
            }
        }
        if (!added) { ok = 1; break; }
    }
failed:
    if (!ok) {
        for (i = 0; i < n; i++) ResumeThread(peers[i]);
        for (i = 0; i < n; i++) CloseHandle(peers[i]);
        return 0;
    }
    *count = n;
    return 1;
}
static int ef_distance_install(unsigned char* base, size_t span) {
    unsigned char *range, *shadow;
    unsigned char range_jump[8] = {0xe9,0,0,0,0,0x90,0x90,0x90};
    unsigned char shadow_jump[6] = {0xe9,0,0,0,0,0x90};
    DWORD range_protect, shadow_protect, ignored;
    HANDLE peers[256];
    unsigned count = 0, i;
    if (ef_distance_active) return 1;
    if (!base || span < EF_SHADOW_RVA + sizeof ef_shadow_signature) return 0;
    range = base + EF_RANGE_RVA;
    shadow = base + EF_SHADOW_RVA;
    if (memcmp(range, ef_range_signature, sizeof ef_range_signature) ||
        memcmp(shadow, ef_shadow_signature, sizeof ef_shadow_signature)) return 0;
    ef_range_return = range + 8;
    ef_shadow_return = shadow + 6;
    *(uint32_t*)(range_jump+1) = (uint32_t)((uintptr_t)ef_range_bridge - (uintptr_t)(range+5));
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
    if (memcmp(range, ef_range_signature, sizeof ef_range_signature) ||
        memcmp(shadow, ef_shadow_signature, sizeof ef_shadow_signature)) {
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
