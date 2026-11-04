// bootprofile: where does a process spend its first seconds?
//
// A sampling profiler for the game's startup, from outside the process. It waits for a NEW process
// with the given name, then every period suspends the threads that used CPU (and always the main
// thread) for a moment, reads EIP and the stack, and writes it all to a text file when it is done.
// No debugger is attached and nothing in the game, its files or the DLL changes, so it needs no
// start test and the game's exception handling stays as it is. See docs/fixes/startup-time.md.
//
// Build from an x86 developer prompt (the game is 32-bit, and this reads its stacks as 32-bit):
//   cl /nologo /O2 /W4 /MT tools\bootprofile\bootprofile.c psapi.lib winmm.lib user32.lib ^
//      /Fetools\bootprofile\bootprofile.exe /Fotools\bootprofile\
//
// Usage: bootprofile [--name witcher2.exe] [--seconds 40] [--period 5] [--out bootprofile.txt]
//                    [--window W2ViewportClass]
// Start it, then start the game. Read the result with tools/bootprofile/report.py.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <mmsystem.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_THREADS   1024
#define MAX_FRAMES    400          // return address candidates kept per sample
#define MAX_STACK     (256 * 1024) // bytes read from ESP upwards
#define BUF_DWORDS    (64u * 1024 * 1024 / 4)
#define KNOWN_BITS    20
#define ACTIVE_CYCLES 2000000ull   // a thread that used less since the last tick is left alone

typedef LONG(NTAPI* NtQIT_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { LONG status; PVOID teb; ULONG_PTR pid, tid; ULONG_PTR affinity; LONG prio, base; } TBI;

typedef struct {
    DWORD tid;
    HANDLE h;
    DWORD stack_base;
    ULONG64 cycles;
    FILETIME created;
} Thr;

typedef struct { DWORD base, size; wchar_t name[64]; } Mod;

static Thr g_thr[MAX_THREADS];
static int g_nthr;
static HANDLE g_proc;
static DWORD g_pid;
static DWORD g_main_tid;
static NtQIT_t g_NtQIT;

static Mod g_mod[512];
static int g_nmod;
static unsigned char g_pagemap[65536];   // address >> 16 is inside a loaded module

static DWORD* g_buf;     // records: 'S', t_ms, tid, kcycles, eip, n, frames... / 'C', t_ms, main_ms, proc_ms
static DWORD g_used;
static DWORD g_known[1u << KNOWN_BITS];  // hash table of addresses already judged by looks_like_call
static unsigned char g_knownflag[1u << KNOWN_BITS];
static DWORD g_stack[MAX_STACK / 4];

// Is the instruction just before addr a call? Anything else on the stack is an old value or data.
static int looks_like_call(DWORD addr)
{
    unsigned char b[8];
    SIZE_T got = 0;
    static const int forms[] = { 2, 3, 4, 6, 7 };
    int i;

    if (addr < 8 || !ReadProcessMemory(g_proc, (void*)(ULONG_PTR)(addr - 8), b, 8, &got) || got != 8) return 0;
    if (b[3] == 0xE8) return 1;                   // call rel32: E8 at addr-5
    for (i = 0; i < 5; i++) {
        int at = 8 - forms[i];
        if (b[at] == 0xFF && ((b[at + 1] >> 3) & 7) == 2) return 1;   // call r/m32: FF /2
    }
    return 0;
}

static int known_call(DWORD addr)
{
    DWORD slot = (addr * 2654435761u) >> (32 - KNOWN_BITS);
    int probes;

    for (probes = 0; probes < 32; probes++) {
        if (g_known[slot] == addr) return g_knownflag[slot];
        if (g_known[slot] == 0) {
            g_known[slot] = addr;
            g_knownflag[slot] = (unsigned char)looks_like_call(addr);
            return g_knownflag[slot];
        }
        slot = (slot + 1) & ((1u << KNOWN_BITS) - 1);
    }
    return looks_like_call(addr);
}

static void refresh_modules(void)
{
    HMODULE list[512];
    DWORD need = 0, i;

    if (!EnumProcessModules(g_proc, list, sizeof(list), &need)) return;
    if (need > sizeof(list)) need = sizeof(list);
    for (i = 0; i < need / sizeof(HMODULE); i++) {
        MODULEINFO mi;
        DWORD base = (DWORD)(ULONG_PTR)list[i], p;
        int j, seen = 0;

        for (j = 0; j < g_nmod; j++) if (g_mod[j].base == base) seen = 1;
        if (seen || g_nmod >= 512) continue;
        if (!GetModuleInformation(g_proc, list[i], &mi, sizeof(mi))) continue;
        g_mod[g_nmod].base = base;
        g_mod[g_nmod].size = mi.SizeOfImage;
        if (!GetModuleBaseNameW(g_proc, list[i], g_mod[g_nmod].name, 64)) wcscpy(g_mod[g_nmod].name, L"?");
        g_nmod++;
        for (p = base >> 16; p <= (base + mi.SizeOfImage - 1) >> 16; p++) g_pagemap[p] = 1;
    }
}

static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;

    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            int i, seen = 0;
            HANDLE h;

            if (te.th32OwnerProcessID != g_pid) continue;
            for (i = 0; i < g_nthr; i++) if (g_thr[i].tid == te.th32ThreadID) seen = 1;
            if (seen || g_nthr >= MAX_THREADS) continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            {
                Thr* t = &g_thr[g_nthr];
                TBI tbi;
                DWORD base = 0;
                FILETIME e, k, u;

                memset(t, 0, sizeof(*t));
                t->tid = te.th32ThreadID;
                t->h = h;
                memset(&tbi, 0, sizeof(tbi));
                if (g_NtQIT && g_NtQIT(h, 0, &tbi, sizeof(tbi), NULL) >= 0 && tbi.teb) {
                    ReadProcessMemory(g_proc, (BYTE*)tbi.teb + 4, &base, 4, NULL);   // NT_TIB.StackBase
                }
                t->stack_base = base;
                GetThreadTimes(h, &t->created, &e, &k, &u);
                // The main thread is the oldest one.
                {
                    int oldest = 1;
                    for (i = 0; i < g_nthr; i++) {
                        if (CompareFileTime(&g_thr[i].created, &t->created) <= 0) oldest = 0;
                    }
                    if (oldest) g_main_tid = t->tid;
                }
                g_nthr++;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static void sample(Thr* t, DWORD now_ms, DWORD kcycles)
{
    CONTEXT c;
    DWORD eip, esp, want, n = 0, i;
    SIZE_T got = 0;
    DWORD* rec;

    if (g_used + 6 + MAX_FRAMES >= BUF_DWORDS) return;
    c.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(t->h) == (DWORD)-1) return;
    if (!GetThreadContext(t->h, &c)) { ResumeThread(t->h); return; }
    eip = c.Eip;
    esp = c.Esp;
    want = 0;
    if (t->stack_base > esp) {
        want = t->stack_base - esp;
        if (want > MAX_STACK) want = MAX_STACK;
        if (!ReadProcessMemory(g_proc, (void*)(ULONG_PTR)esp, g_stack, want, &got)) got = 0;
    }
    ResumeThread(t->h);

    rec = g_buf + g_used;
    rec[0] = 'S';
    rec[1] = now_ms;
    rec[2] = t->tid;
    rec[3] = kcycles;
    rec[4] = eip;
    for (i = 0; i < got / 4 && n < MAX_FRAMES; i++) {
        DWORD v = g_stack[i];
        if (g_pagemap[v >> 16] && known_call(v)) rec[6 + n++] = v;
    }
    rec[5] = n;
    g_used += 6 + n;
}

static double ft_ms(const FILETIME* a, const FILETIME* b)
{
    ULARGE_INTEGER x, y;
    x.LowPart = a->dwLowDateTime; x.HighPart = a->dwHighDateTime;
    y.LowPart = b->dwLowDateTime; y.HighPart = b->dwHighDateTime;
    return (double)(x.QuadPart + y.QuadPart) / 10000.0;
}

static DWORD find_process(const wchar_t* name, DWORD* ignore, int nignore)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    DWORD found = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                int i, old = 0;
                for (i = 0; i < nignore; i++) if (ignore[i] == pe.th32ProcessID) old = 1;
                if (!old) { found = pe.th32ProcessID; break; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Remember what already runs, so a game left open is not mistaken for the start being measured.
static int running_now(const wchar_t* name, DWORD* out, int max)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0 && n < max) out[n++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* name = L"witcher2.exe";
    const wchar_t* out = L"bootprofile.txt";
    const wchar_t* wndclass = L"W2ViewportClass";
    int seconds = 40, period = 5, i;
    DWORD old[32];
    int nold;
    SYSTEMTIME st;
    LARGE_INTEGER f, t0, now;
    DWORD window_ms = 0, next_threads = 0, next_mods = 0, next_cpu = 0;
    DWORD reset_known_at = 1000;
    FILE* fo;
    DWORD samples = 0;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!wcscmp(argv[i], L"--name")) name = argv[i + 1];
        else if (!wcscmp(argv[i], L"--out")) out = argv[i + 1];
        else if (!wcscmp(argv[i], L"--seconds")) seconds = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--period")) period = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--window")) wndclass = argv[i + 1];
        else { fwprintf(stderr, L"unknown option %s\n", argv[i]); return 2; }
    }
    g_buf = (DWORD*)VirtualAlloc(NULL, BUF_DWORDS * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_buf) { fputs("out of memory\n", stderr); return 1; }
    g_NtQIT = (NtQIT_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    timeBeginPeriod(1);

    nold = running_now(name, old, 32);
    if (nold) wprintf(L"%d %s already running; waiting for a NEW one.\n", nold, name);
    wprintf(L"Waiting for %s. Start it now.\n", name);
    for (;;) {
        g_pid = find_process(name, old, nold);
        if (g_pid) break;
        Sleep(10);
    }
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, g_pid);
    if (!g_proc) { fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError()); return 1; }

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    GetLocalTime(&st);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    wprintf(L"Found pid %lu at %02d:%02d:%02d.%03d, sampling for %d s every %d ms.\n", g_pid, st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, seconds, period);

    for (;;) {
        DWORD ms, code = 0;
        int k;

        QueryPerformanceCounter(&now);
        ms = (DWORD)((now.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);
        if (ms >= (DWORD)seconds * 1000) break;
        if (!GetExitCodeProcess(g_proc, &code) || code != STILL_ACTIVE) break;

        if (ms >= next_mods) { refresh_modules(); next_mods = ms + 200; }
        if (ms >= next_threads) { refresh_threads(); next_threads = ms + 200; }   // a system-wide snapshot is slow
        if (ms >= reset_known_at) {
            // SteamStub decrypts .text in the first moments: forget what was judged before that.
            memset(g_known, 0, sizeof(g_known));
            reset_known_at = 0xFFFFFFFF;
        }
        if (!window_ms && wndclass[0]) {
            HWND w = FindWindowW(wndclass, NULL);
            DWORD pid = 0;
            if (w) GetWindowThreadProcessId(w, &pid);
            if (w && pid == g_pid) window_ms = ms ? ms : 1;
        }

        for (k = 0; k < g_nthr; k++) {
            Thr* t = &g_thr[k];
            ULONG64 cyc = 0;
            ULONG64 d;

            if (!QueryThreadCycleTime(t->h, &cyc)) continue;
            d = cyc - t->cycles;
            t->cycles = cyc;
            if (t->tid == g_main_tid || d >= ACTIVE_CYCLES) {
                sample(t, ms, (DWORD)(d / 1000));
                samples++;
            }
        }

        if (ms >= next_cpu && g_used + 4 < BUF_DWORDS) {
            FILETIME c, e, kt, ut;
            DWORD main_ms = 0, proc_ms = 0;
            for (k = 0; k < g_nthr; k++) {
                if (g_thr[k].tid == g_main_tid && GetThreadTimes(g_thr[k].h, &c, &e, &kt, &ut)) {
                    main_ms = (DWORD)ft_ms(&kt, &ut);
                }
            }
            if (GetProcessTimes(g_proc, &c, &e, &kt, &ut)) proc_ms = (DWORD)ft_ms(&kt, &ut);
            g_buf[g_used++] = 'C';
            g_buf[g_used++] = ms;
            g_buf[g_used++] = main_ms;
            g_buf[g_used++] = proc_ms;
            next_cpu = ms + 250;
        }

        {
            LARGE_INTEGER after;
            LONG64 spent;
            QueryPerformanceCounter(&after);
            spent = (after.QuadPart - now.QuadPart) * 1000 / f.QuadPart;
            if (spent < period) Sleep((DWORD)(period - spent));
        }
    }

    fo = _wfopen(out, L"w");
    if (!fo) { fwprintf(stderr, L"cannot write %s\n", out); return 1; }
    fprintf(fo, "# bootprofile 1\n");
    fprintf(fo, "# started %02d:%02d:%02d.%03d pid %lu main_tid %lu period %d ms samples %lu\n", st.wHour, st.wMinute,
            st.wSecond, st.wMilliseconds, g_pid, g_main_tid, period, samples);
    if (window_ms) fprintf(fo, "W %lu\n", window_ms);
    for (i = 0; i < g_nmod; i++) fprintf(fo, "M %08lX %08lX %ls\n", g_mod[i].base, g_mod[i].size, g_mod[i].name);
    {
        DWORD p = 0;
        while (p < g_used) {
            DWORD kind = g_buf[p];
            if (kind == 'C') {
                fprintf(fo, "C %lu %lu %lu\n", g_buf[p + 1], g_buf[p + 2], g_buf[p + 3]);
                p += 4;
            } else {
                DWORD n = g_buf[p + 5], j;
                fprintf(fo, "S %lu %lu %lu %08lX", g_buf[p + 1], g_buf[p + 2], g_buf[p + 3], g_buf[p + 4]);
                for (j = 0; j < n; j++) fprintf(fo, " %08lX", g_buf[p + 6 + j]);
                fputc('\n', fo);
                p += 6 + n;
            }
        }
    }
    fclose(fo);
    wprintf(L"Wrote %s: %lu samples, %lu MB in memory. window at %lu ms.\n", out, samples, g_used / (256 * 1024), window_ms);
    timeEndPeriod(1);
    return 0;
}
