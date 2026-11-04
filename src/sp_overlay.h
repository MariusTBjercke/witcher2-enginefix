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
// `user_root` is the game's user folder, `<Documents>\Witcher 2` (sp_user_root), or NULL to
// ignore user content. The game compiles a script from the last mounted user content package
// whose scripts.dzip holds it before base_scripts.dzip, so each patched script is taken from,
// and written back to, whichever copy the game compiles.
// 1 = ok (parts may have been skipped), 0 = the build failed and the caller should serve the
// original files.
int sp_overlay_build(const wchar_t* game, const wchar_t* cache_dir, sp_features f,
                     long long filetime, sp_redirect* r, int* nr, int maxr,
                     const wchar_t* user_root, sp_log_fn log);

// `<Documents>\Witcher 2`, from SHGetFolderPathW(CSIDL_PERSONAL) as the game builds it
// (00932589). 0 when the shell has no Documents folder.
int sp_user_root(wchar_t* out, size_t cap);

// Calls `fn` with every Mount= value of section [Packages] of a UserContent.ini, in file order,
// read the way the game reads it (00AC6AAE, 00AC1D74): UTF-16LE only with an FF FE start, else
// ANSI; no trimming; case-sensitive names; reading stops at the first line the game cannot
// parse, keeping what came before. The value is not NUL-terminated. Returns 1 when the whole
// file parsed, 0 when it stopped early, -1 when out of memory.
typedef void (*sp_mount_fn)(void* ctx, const wchar_t* value, size_t len);
int sp_user_content_mounts(const sp_buf* ini, sp_mount_fn fn, void* ctx);

// Collapses repeated backslashes after the first two characters (a UNC or \\?\ prefix stays):
// the game opens user content as "...\Witcher 2\\UserContent\...".
void sp_normalize_path(wchar_t* p);

// One line describing the last build for the DLL's single log line, e.g.
// "book_marker=on continue=on pause_quit=on cache=built 110 ms" (or "cache=reused",
// "skipped: <why>", "build failed"). Empty before the first build. Written under the
// caller's build lock, so read it right after sp_overlay_build.
const char* sp_overlay_status(void);

#endif
