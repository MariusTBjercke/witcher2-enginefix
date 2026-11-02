// Script-patch file redirects for the DLL (plan Task 7): triggers the overlay build on the
// first touch of a target, answers CreateFileW with the cached copy and adjusts the find data
// of the redirected archive. The IAT plumbing stays in enginefix.c, where one hook_CreateFileW
// chains save_trace, file_trace and this redirect, and hook_FindFirstFileW/hook_FindNextFileW
// carry the find data; this file holds the decisions and the redirect table.
#ifndef SP_HOOKS_H
#define SP_HOOKS_H

#include <windows.h>
#include "sp_common.h"

// Sets up the redirect state for this process: `game` is the game root (the parent of bin),
// `cache` the folder next to the DLL. The overlay is not built here; the first hook hit does
// it. 0 = no feature is enabled (nothing to hook).
int sp_hooks_start(const wchar_t* game, const wchar_t* cache, int book_marker, int cont,
                   int pause_quit, int window_mode, sp_log_fn log);


// CreateFileW support: builds the overlay when a target is first touched, then answers `out`
// with the cached copy when the absolute `path` is redirected. 1 = serve `out` instead.
int sp_hooks_path(const wchar_t* path, wchar_t* out, size_t cap);

// FindFirstFileW, before the original call: when the game lists `<cooked>\*.dzip` (its first
// CookedPC access), restores old on-disk patches (sp_migrate) and builds the overlay, so the
// listing and every later open already see the final files.
void sp_hooks_prepare(LPCWSTR pattern);

// FindFirstFileW/FindNextFileW support: builds the overlay when the game lists
// `<cooked>\*.dzip` (that find data carries the archives' sizes) and answers the cached file's
// size and times for an entry named like a redirected archive (base_scripts.dzip, the icon
// carrier). `pattern` is NULL for FindNextFileW.
// 1 = data adjusted.
int sp_hooks_find(LPCWSTR pattern, WIN32_FIND_DATAW* data);

#endif
