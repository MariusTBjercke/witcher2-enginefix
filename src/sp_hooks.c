// Script-patch file redirects; see sp_hooks.h. The overlay build is triggered here, on the
// first hit of a target, under a critical section: the game waits for that one build (a few
// hundred milliseconds on the first start only) and every later start reuses the cache.
#include "sp_hooks.h"
#include "sp_migrate.h"
#include "sp_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP_MAX_REDIRECTS 8

// The two movies BookMarker redirects when the game has them loose.
static const wchar_t* const k_swf_paths[2] = {
    L"globals\\gui\\ui_trade.swf", L"globals\\gui\\ui_storage.swf"
};

static wchar_t      g_game[MAX_PATH];
static wchar_t      g_cache[MAX_PATH];
static wchar_t      g_cooked[MAX_PATH];
static wchar_t      g_archive_glob[MAX_PATH];
static sp_features  g_features;
static sp_redirect  g_redirects[SP_MAX_REDIRECTS];
static int          g_nredirects;
static volatile LONG g_build;           // 0 = not tried, 1 = built, -1 = failed
static CRITICAL_SECTION g_lock;
static int          g_lock_ready;
static int          g_active;
static sp_log_fn    g_log;

static int path_join(wchar_t* out, size_t cap, const wchar_t* base, const wchar_t* rest)
{
    size_t n = base ? wcslen(base) : 0;
    int sep = n > 0 && base[n - 1] != L'\\' && base[n - 1] != L'/';

    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s%s", base ? base : L"",
                        sep ? L"\\" : L"", rest ? rest : L"") >= 0;
}

static int wide_copy(wchar_t* dst, size_t cap, const wchar_t* src)
{
    size_t n = src ? wcslen(src) : 0;

    if (!dst || !src || n + 1 > cap) return 0;
    memcpy(dst, src, (n + 1) * sizeof *dst);
    return 1;
}

// The cached copy for an absolute game path, or NULL.
static const wchar_t* lookup(const wchar_t* full)
{
    int i;

    for (i = 0; i < g_nredirects; i++) {
        if (!_wcsicmp(g_redirects[i].target, full)) return g_redirects[i].cached;
    }
    return NULL;
}

// CookedPC\<name>.dzip (no subfolder): the redirected archives are base_scripts.dzip and the
// icon carrier.
static int is_cooked_archive(const wchar_t* full)
{
    size_t n = wcslen(g_cooked), len = wcslen(full);

    return len > n + 6 && !_wcsnicmp(full, g_cooked, n) && full[n] == L'\\' &&
           !wcschr(full + n + 1, L'\\') && !_wcsicmp(full + len - 5, L".dzip");
}

// Only an archive or one of the two movies can be redirected; a build is worth starting when
// one of them is touched even if the game never listed CookedPC\*.dzip first.
static int candidate_target(const wchar_t* full)
{
    wchar_t p[MAX_PATH];
    int i;

    if (!full || !full[0]) return 0;
    if (is_cooked_archive(full)) return 1;
    for (i = 0; i < 2; i++) {
        if (path_join(p, MAX_PATH, g_cooked, k_swf_paths[i]) && !_wcsicmp(p, full)) return 1;
    }
    return 0;
}

static int is_archive_glob(const wchar_t* pattern)
{
    return g_archive_glob[0] && pattern && !_wcsicmp(g_archive_glob, pattern);
}

static int build_once(void)
{
    LONG state = g_build;

    if (state != 0) return state > 0;
    if (!g_lock_ready) return 0;
    EnterCriticalSection(&g_lock);
    if (g_build == 0) {
        FILETIME ft;
        ULARGE_INTEGER now;
        wchar_t backup[MAX_PATH];
        int ok, m = SP_MIGRATE_NONE;

        // Old on-disk patches first (plan Task 8): the game has not listed or opened any
        // archive yet, so the restored originals are what it sees.
        if (path_join(backup, MAX_PATH, g_game,
                      L"Witcher2EngineFix_Optional\\ScriptPatches\\backup")) {
            m = sp_migrate_run(g_game, backup, g_log);
        }
        g_nredirects = 0;
        if (m == SP_MIGRATE_PARTIAL || m == SP_MIGRATE_FAILED) {
            g_build = -1;
            if (g_log) {
                g_log("script patches: off for this start - %s",
                      m == SP_MIGRATE_PARTIAL
                          ? "some files of the old on-disk patches were changed since (see "
                            "migrate lines); the next start patches what is safe"
                          : "the old on-disk patches could not be restored (see migrate "
                            "lines); the next start tries again");
            }
        } else {
            GetSystemTimeAsFileTime(&ft);
            now.LowPart = ft.dwLowDateTime;
            now.HighPart = ft.dwHighDateTime;
            ok = sp_overlay_build(g_game, g_cache, g_features, (long long)now.QuadPart,
                                  g_redirects, &g_nredirects, SP_MAX_REDIRECTS, g_log);
            g_build = ok ? 1 : -1;
            if (g_log) {
                char line[900];

                _snprintf_s(line, sizeof line, _TRUNCATE, "script patches: %s",
                            sp_overlay_status());
                g_log("%s", line);
            }
        }
    }
    state = g_build;
    LeaveCriticalSection(&g_lock);
    return state > 0;
}

int sp_hooks_start(const wchar_t* game, const wchar_t* cache, int book_marker, int cont,
                   int pause_quit, int window_mode, sp_log_fn log)
{
    if (g_active) return 1;
    if (!game || !cache || !*game || !*cache) return 0;
    if (!book_marker && !cont && !pause_quit && !window_mode) return 0;
    if (!wide_copy(g_game, MAX_PATH, game) || !wide_copy(g_cache, MAX_PATH, cache)) return 0;
    if (!path_join(g_cooked, MAX_PATH, game, L"CookedPC") ||
        !path_join(g_archive_glob, MAX_PATH, g_cooked, L"*.dzip")) {
        return 0;
    }
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    }
    g_features.book_marker = book_marker;
    g_features.cont = cont;
    g_features.pause_quit = pause_quit;
    g_features.window_mode = window_mode;
    g_log = log;
    g_active = 1;
    return 1;
}


int sp_hooks_path(const wchar_t* path, wchar_t* out, size_t cap)
{
    const wchar_t* cached;

    if (!g_active || !path || !*path || !out || cap == 0) return 0;
    if (g_build != 1) {
        if (!candidate_target(path) || !build_once()) return 0;
    }
    cached = lookup(path);
    if (!cached) return 0;
    if (!wide_copy(out, cap, cached)) return 0;
    return 1;
}

void sp_hooks_prepare(LPCWSTR pattern)
{
    if (g_active && g_build == 0 && is_archive_glob(pattern)) build_once();
}

int sp_hooks_find(LPCWSTR pattern, WIN32_FIND_DATAW* data)
{
    wchar_t target[MAX_PATH];
    const wchar_t* cached;
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!g_active || !data) return 0;
    if (g_build != 1) {
        if (!is_archive_glob(pattern) || !build_once()) return 0;
    }
    // A redirected archive (base_scripts.dzip, the icon carrier) is listed by the *.dzip glob,
    // whose data must report the cached size and times so the engine treats it as the file it
    // is about to open. FindNextFileW has no pattern, so the entry name decides; no other
    // folder the game lists holds a .dzip of those names.
    if (!path_join(target, MAX_PATH, g_cooked, data->cFileName) || !is_cooked_archive(target)) {
        return 0;
    }
    cached = lookup(target);
    if (!cached) return 0;
    if (!GetFileAttributesExW(cached, GetFileExInfoStandard, &d)) return 0;
    data->dwFileAttributes = d.dwFileAttributes;
    data->ftCreationTime = d.ftCreationTime;
    data->ftLastAccessTime = d.ftLastAccessTime;
    data->ftLastWriteTime = d.ftLastWriteTime;
    data->nFileSizeHigh = d.nFileSizeHigh;
    data->nFileSizeLow = d.nFileSizeLow;
    return 1;
}
