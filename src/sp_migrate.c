// One-time restore of the old on-disk script patches; see sp_migrate.h. The reference is
// ScriptPatches.ps1's Uninstall (Read-Manifest, Get-LeafPath, Restore-Leaf), with two
// differences: only the four leaves that patcher ever wrote are accepted (the manifest is a
// file on disk and decides what gets deleted), and the backup is renamed, not deleted.
#include "sp_migrate.h"
#include <windows.h>
#include <bcrypt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "bcrypt.lib")

#define MG_MAX_LEAVES 16

static void vlogf(sp_log_fn log, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;

    if (!log) return;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    log("%s", buf);
}

static int path_join(wchar_t* out, size_t cap, const wchar_t* base, const wchar_t* rest)
{
    size_t n = base ? wcslen(base) : 0;
    int sep = n > 0 && base[n - 1] != L'\\' && base[n - 1] != L'/';

    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s%s", base ? base : L"",
                        sep ? L"\\" : L"", rest ? rest : L"") >= 0;
}

int sp_sha256_hex(const sp_buf* b, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char digest[32];
    size_t done = 0;
    int ok = 0, i;

    out[0] = 0;
    if (!b) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) < 0) goto out;
    while (done < b->n) {
        ULONG chunk = b->n - done > 0x40000000u ? 0x40000000u : (ULONG)(b->n - done);

        if (BCryptHashData(h, b->p + done, chunk, 0) < 0) goto out;
        done += chunk;
    }
    if (BCryptFinishHash(h, digest, sizeof digest, 0) < 0) goto out;
    for (i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
    ok = 1;
out:
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

static int file_hash(const wchar_t* path, char out[65])
{
    sp_buf b;
    int ok;

    out[0] = 0;
    if (!sp_read_file(path, &b)) return 0;
    ok = sp_sha256_hex(&b, out);
    sp_free(&b);
    return ok;
}

// ---- manifest ----------------------------------------------------------

typedef struct { char leaf[64], mode[16], hash[80]; } mg_entry;

// Read-Manifest: "leaf<TAB>mode<TAB>hash" per line (ASCII, CRLF from Set-Content).
static int read_manifest(const wchar_t* path, mg_entry* e, int max, int* n)
{
    sp_buf b;
    size_t i = 0;

    *n = 0;
    if (!sp_read_file(path, &b)) return 0;
    while (i < b.n) {
        char line[256], *f1, *f2;
        size_t len = 0;

        while (i < b.n && b.p[i] != '\n') {
            if (b.p[i] != '\r' && len < sizeof line - 1) line[len++] = (char)b.p[i];
            i++;
        }
        i++;
        line[len] = 0;
        if (!line[0]) continue;
        f1 = strchr(line, '\t');
        f2 = f1 ? strchr(f1 + 1, '\t') : NULL;
        if (*n >= max) break;
        memset(&e[*n], 0, sizeof e[*n]);
        if (f1) *f1 = 0;
        if (f2) *f2 = 0;
        strncpy_s(e[*n].leaf, sizeof e[*n].leaf, line, _TRUNCATE);
        if (f1) strncpy_s(e[*n].mode, sizeof e[*n].mode, f1 + 1, _TRUNCATE);
        if (f2) strncpy_s(e[*n].hash, sizeof e[*n].hash, f2 + 1, _TRUNCATE);
        (*n)++;
    }
    sp_free(&b);
    return 1;
}

// Get-LeafPath, limited to what the old patcher wrote. 0 = not one of its leaves.
static int leaf_path(const wchar_t* cooked, const char* leaf, const char* mode, wchar_t* out)
{
    if (!_stricmp(leaf, "base_scripts.dzip") &&
        (!_stricmp(mode, "replaced") || !_stricmp(mode, "created"))) {
        return path_join(out, MAX_PATH, cooked, L"base_scripts.dzip");
    }
    if (!_stricmp(leaf, "ui_trade.swf") &&
        (!_stricmp(mode, "replaced") || !_stricmp(mode, "created"))) {
        return path_join(out, MAX_PATH, cooked, L"globals\\gui\\ui_trade.swf");
    }
    if (!_stricmp(leaf, "ui_storage.swf") &&
        (!_stricmp(mode, "replaced") || !_stricmp(mode, "created"))) {
        return path_join(out, MAX_PATH, cooked, L"globals\\gui\\ui_storage.swf");
    }
    if (!_stricmp(leaf, "w2ef_read") && !_stricmp(mode, "folder")) {
        return path_join(out, MAX_PATH, cooked, L"globals\\gui\\icons\\items\\w2ef_read");
    }
    return 0;
}

// The generated icon folder: its files, then the folder. Subfolders are not ours; they and
// the folder stay. 1 = gone.
static int remove_icon_folder(const wchar_t* dir, int* files)
{
    wchar_t pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    int ok = 1;

    *files = 0;
    if (!path_join(pattern, MAX_PATH, dir, L"*")) return 0;
    find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!path_join(path, MAX_PATH, dir, fd.cFileName)) {
                ok = 0;
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                SetFileAttributesW(path, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            }
            if (DeleteFileW(path)) (*files)++;
            else ok = 0;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return RemoveDirectoryW(dir) && ok;
}

// "<backup>.restored-by-dll", or "-2", "-3", ... when an earlier one is still there.
static int rename_backup(const wchar_t* backup_dir, wchar_t* to)
{
    int i;

    for (i = 1; i < 100; i++) {
        if (i == 1) {
            if (_snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%s.restored-by-dll", backup_dir) < 0)
                return 0;
        } else if (_snwprintf_s(to, MAX_PATH, _TRUNCATE, L"%s.restored-by-dll-%d", backup_dir,
                                i) < 0) {
            return 0;
        }
        if (GetFileAttributesW(to) != INVALID_FILE_ATTRIBUTES) continue;
        return MoveFileExW(backup_dir, to, 0) != 0;
    }
    return 0;
}

// ---- restore -----------------------------------------------------------

int sp_migrate_run(const wchar_t* game, const wchar_t* backup_dir, sp_log_fn log)
{
    mg_entry e[MG_MAX_LEAVES];
    wchar_t cooked[MAX_PATH], manifest[MAX_PATH], renamed[MAX_PATH];
    int n = 0, i, restored = 0, removed = 0, icons = -1, partial = 0, failed = 0;

    if (!game || !backup_dir) return SP_MIGRATE_NONE;
    if (!path_join(manifest, MAX_PATH, backup_dir, L"manifest.txt") ||
        !path_join(cooked, MAX_PATH, game, L"CookedPC")) {
        return SP_MIGRATE_NONE;
    }
    if (GetFileAttributesW(manifest) == INVALID_FILE_ATTRIBUTES) return SP_MIGRATE_NONE;
    if (!read_manifest(manifest, e, MG_MAX_LEAVES, &n)) {
        vlogf(log, "migrate: cannot read %ls - old script patches left as they are", manifest);
        return SP_MIGRATE_FAILED;
    }
    vlogf(log, "migrate: old script patches found (%ls, %d entries)", backup_dir, n);

    for (i = 0; i < n; i++) {
        wchar_t path[MAX_PATH], wleaf[64], bak[MAX_PATH];
        char now[65], orig[65];

        if (!leaf_path(cooked, e[i].leaf, e[i].mode, path)) {
            vlogf(log, "migrate: '%s' (%s) is not a file the old patcher writes, left alone",
                  e[i].leaf, e[i].mode);
            partial = 1;
            continue;
        }
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
            vlogf(log, "migrate: %s is gone, skipped", e[i].leaf);
            continue;
        }
        if (!_stricmp(e[i].mode, "folder")) {
            if (remove_icon_folder(path, &icons)) {
                vlogf(log, "migrate: removed the read-book icons (%d files)", icons);
            } else {
                vlogf(log, "migrate: could not remove %ls completely (%d files deleted)", path,
                      icons);
                failed = 1;
            }
            continue;
        }
        if (!file_hash(path, now)) {
            vlogf(log, "migrate: cannot read %ls", path);
            failed = 1;
            continue;
        }
        if (_stricmp(now, e[i].hash) != 0) {
            // Already the backup's bytes: an earlier run restored it and could not rename.
            if (MultiByteToWideChar(CP_ACP, 0, e[i].leaf, -1, wleaf, 64) > 0 &&
                path_join(bak, MAX_PATH, backup_dir, wleaf) && file_hash(bak, orig) &&
                !_stricmp(now, orig)) {
                vlogf(log, "migrate: %s is already the original", e[i].leaf);
                continue;
            }
            vlogf(log, "migrate: %s was changed after patching (another mod or update?), left "
                       "as it is", e[i].leaf);
            partial = 1;
            continue;
        }
        if (!_stricmp(e[i].mode, "created")) {
            if (DeleteFileW(path)) {
                vlogf(log, "migrate: removed %s (the game uses its packed copy again)",
                      e[i].leaf);
                removed++;
            } else {
                vlogf(log, "migrate: cannot delete %ls (error %lu)", path, GetLastError());
                failed = 1;
            }
            continue;
        }
        {
            sp_buf data;

            if (MultiByteToWideChar(CP_ACP, 0, e[i].leaf, -1, wleaf, 64) <= 0 ||
                !path_join(bak, MAX_PATH, backup_dir, wleaf) || !sp_read_file(bak, &data)) {
                vlogf(log, "migrate: no backup copy of %s - left as it is", e[i].leaf);
                partial = 1;
                continue;
            }
            if (sp_write_file_atomic(path, &data)) {
                vlogf(log, "migrate: restored %s", e[i].leaf);
                restored++;
            } else {
                vlogf(log, "migrate: cannot write %ls (error %lu)", path, GetLastError());
                failed = 1;
            }
            sp_free(&data);
        }
    }

    if (failed) {
        vlogf(log, "migrate: not finished, the backup stays and the next start tries again");
        return SP_MIGRATE_FAILED;
    }
    vlogf(log, "migrate: restored %d file%s%s%s", restored + removed,
          restored + removed == 1 ? "" : "s", icons >= 0 ? ", removed w2ef_read" : "",
          partial ? " (some files left as they are)" : "");
    if (rename_backup(backup_dir, renamed)) {
        vlogf(log, "migrate: old backup kept as %ls", renamed);
    } else {
        // Every file is restored; the next start finds them already original and only
        // retries the rename.
        vlogf(log, "migrate: cannot rename %ls (error %lu)", backup_dir, GetLastError());
    }
    return partial ? SP_MIGRATE_PARTIAL : SP_MIGRATE_DONE;
}
