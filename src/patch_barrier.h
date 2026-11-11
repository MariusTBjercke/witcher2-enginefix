// Suspends every other thread in the process and checks that none of them is executing inside
// one of the byte ranges a caller is about to rewrite. On success the peers stay suspended and
// the caller patches (then calls pb_resume_peers). On failure everything is resumed here.
//
// The freeze is repeated until a pass adds no new thread: a creator may have made a child during
// an earlier pass, but after a pass adds no threads all in-process creators are stopped. Remote
// injection by another process is outside this protocol. Shared by the renderer distance hooks
// (distance_patch.h), the window-mode patches (window_patch.h) and the script-binding hook.
//
// Only one caller may freeze the process at a time: two barriers running together suspend each
// other, and neither can resume the other. Measured 2026-11-11 (a start with the script-binding
// hook): every thread of the game was left suspended and no hook was installed. g_pbLock is
// taken in pb_stop_peers and released by pb_resume_peers, so a successful stop must always be
// paired with one resume - never resume the peers by hand.
#ifndef W2_PATCH_BARRIER_H
#define W2_PATCH_BARRIER_H

#include <windows.h>
#include <stdint.h>

extern CRITICAL_SECTION g_pbLock;

// One span of engine code that must not be executing while it is rewritten.
typedef struct {
    const unsigned char* start;
    unsigned             len;
} pb_range;

typedef LONG (NTAPI *PB_NextThread)(HANDLE, HANDLE, DWORD, ULONG, ULONG, PHANDLE);

static int pb_range_contains(const pb_range* ranges, unsigned nranges, DWORD eip)
{
    unsigned i;

    for (i = 0; i < nranges; i++) {
        DWORD at = (DWORD)(uintptr_t)ranges[i].start;

        if (eip >= at && eip - at < ranges[i].len) return 1;
    }
    return 0;
}

// Returns 1 with *count suspended peers (all outside the ranges) when the process has
// quiesced. Returns 0 with every peer resumed and closed when it could not be proven.
static int pb_stop_peers(HANDLE* peers, unsigned* count, const pb_range* ranges,
                         unsigned nranges)
{
    PB_NextThread next_thread = (PB_NextThread)(uintptr_t)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtGetNextThread");
    DWORD ids[256], self = GetCurrentThreadId();
    unsigned n = 0, pass, i;
    int ok = 0;

    *count = 0;
    if (!next_thread) return 0;
    EnterCriticalSection(&g_pbLock);   // one freeze at a time; released in pb_resume_peers
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
                    pb_range_contains(ranges, nranges, context.Eip)) goto failed;
            }
        }
        if (!added) { ok = 1; break; }
    }
failed:
    if (!ok) {
        for (i = 0; i < n; i++) ResumeThread(peers[i]);
        for (i = 0; i < n; i++) CloseHandle(peers[i]);
        LeaveCriticalSection(&g_pbLock);
        return 0;
    }
    *count = n;
    return 1;
}

static void pb_resume_peers(HANDLE* peers, unsigned count)
{
    unsigned i;

    for (i = 0; i < count; i++) ResumeThread(peers[i]);
    for (i = 0; i < count; i++) CloseHandle(peers[i]);
    LeaveCriticalSection(&g_pbLock);
}

#endif
