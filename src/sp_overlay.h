// Overlay build: reads the game's own files and writes the patched copies the DLL serves
// through its file-API redirects (Task 7) into a cache directory next to the DLL. Nothing
// under the game directory is written. Port of the install side of ScriptPatches.ps1 (main
// flow, Get-ReadIcons, Build-Scripts and Get-PatchedSwf; removed in 0.6.0, git tag v0.5.1),
// proven byte for byte by the golden test before the removal.
#ifndef SP_OVERLAY_H
#define SP_OVERLAY_H

#include <windows.h>
#include "sp_common.h"

// One file the redirect answers for: the game's own path and the cached copy to serve.
typedef struct { wchar_t target[MAX_PATH]; wchar_t cached[MAX_PATH]; } sp_redirect;

// [scriptpatches] in the ini.
typedef struct { int book_marker, cont, pause_quit, window_mode, medallion; } sp_features;

// Builds the patched files for the enabled features into cache_dir (created when missing).
// An earlier build is reused when the cache key still matches; the redirect table then lists
// the same files. `filetime` is the FILETIME written into the replaced/added archive entries
// (the build time; the reference uses DateTime.UtcNow). `r` gets at most maxr entries and is
// owned by the caller. Each skipped part is logged through `log` (may be NULL).
// 1 = ok (parts may have been skipped), 0 = the build failed and the caller should serve the
// original files.
int sp_overlay_build(const wchar_t* game, const wchar_t* cache_dir, sp_features f,
                     long long filetime, sp_redirect* r, int* nr, int maxr, sp_log_fn log);

// One line describing the last build for the DLL's single log line, e.g.
// "book_marker=on continue=on pause_quit=on cache=built 110 ms" (or "cache=reused",
// "skipped: <why>", "build failed"). Empty before the first build. Written under the
// caller's build lock, so read it right after sp_overlay_build.
const char* sp_overlay_status(void);

#endif
