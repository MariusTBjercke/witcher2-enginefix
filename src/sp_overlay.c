// Overlay build; see sp_overlay.h. The reference is the install flow of
// ScriptPatches.ps1 (git tag v0.5.1): Get-Target / Get-ReadIcons / Build-Scripts /
// Get-PatchedSwf and the final writes, with the cache in place of the backup manifest.
#include "sp_overlay.h"
#include "sp_dds.h"
#include "sp_dzip.h"
#include "sp_script.h"
#include "sp_swf.h"
#include "version.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OV_MAX_ARCHIVES 64
#define OV_NAME          128

// The two movies BookMarker patches; the only feature with Swf = true.
static const char* const k_swf_names[2] = { "ui_trade.swf", "ui_storage.swf" };
static const char* const k_swf_entries[2] = {
    "globals\\gui\\ui_trade.swf", "globals\\gui\\ui_storage.swf"
};

static const char* const k_script_entries[5] = {
    "game\\gui\\guiutils.ws",
    "game\\gui\\guishop.ws",
    "game\\gui\\guimainmenu.ws",
    "game\\gameplay\\container_class.ws",
    "game\\gui\\guiinventory.ws"
};

static const char* const k_icon_folder = "globals\\gui\\icons\\items\\w2ef_read\\";

// The archives that can carry the read icons and a patched movie that only exists inside
// abetterui.dzip. Never base_scripts.dzip: the game compiles every entry of that archive as a
// script, and a .dds there stops it with "parse error, near 'DDS'" (Task 7 start test). These
// are tiny DLC archives of every Enhanced Edition install; the first one present is used.
static const wchar_t* const k_carriers[] = {
    L"dlc_finishers.dzip", L"alchemy_suit.dzip", L"magical_suit.dzip", L"roche_jacket.dzip",
    L"swordsman_suit.dzip"
};
#define OV_NCARRIERS ((int)(sizeof k_carriers / sizeof k_carriers[0]))

// Part of the cache key: bump it whenever the cache's contents or layout change, so an older
// cache built by the same ENGINEFIX_VERSION is never served. 2 = icons in a carrier archive;
// 3 = the WindowMode rows in guimainmenu.ws; 4 = one Display mode selector instead of two rows.
#define OV_CACHE_LAYOUT 4

// ---- logging -----------------------------------------------------------

// The log sink is printf-like but cannot take a va_list, so the line is formatted here.
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


// ---- growable text buffer ----------------------------------------------

typedef struct { char* p; size_t n, cap; } obuf;

static void ob_free(obuf* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static void ob_clear(obuf* b)
{
    b->n = 0;
    if (b->p) b->p[0] = 0;
}

static int ob_reserve(obuf* b, size_t extra)
{
    size_t need, cap;
    char* p;

    if (extra > (size_t)-1 - b->n - 1) return 0;
    need = b->n + extra + 1;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 512;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2) return 0;
        cap *= 2;
    }
    p = (char*)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int ob_put(obuf* b, const void* p, size_t n)
{
    if (!ob_reserve(b, n)) return 0;
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

static int ob_puts(obuf* b, const char* s)
{
    return ob_put(b, s, strlen(s));
}

static int ob_printf(obuf* b, const char* fmt, ...)
{
    char tmp[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    return ob_puts(b, tmp);
}

// ---- string list -------------------------------------------------------

typedef struct { char** v; int count, cap; } strlist;

static void strlist_free(strlist* l)
{
    int i;

    for (i = 0; i < l->count; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int strlist_index_ci(const strlist* l, const char* s)
{
    int i;

    for (i = 0; i < l->count; i++) {
        if (_stricmp(l->v[i], s) == 0) return i;
    }
    return -1;
}

static int strlist_add(strlist* l, const char* s)
{
    size_t n;
    char* copy;

    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        char** grown = (char**)realloc(l->v, (size_t)cap * sizeof *l->v);

        if (!grown) return 0;
        l->v = grown;
        l->cap = cap;
    }
    n = strlen(s);
    copy = (char*)malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, s, n + 1);
    l->v[l->count++] = copy;
    return 1;
}

static int strlist_add_unique(strlist* l, const char* s)
{
    if (strlist_index_ci(l, s) >= 0) return 1;
    return strlist_add(l, s);
}

static int str_ci_cmp(const char* a, const char* b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;

        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) break;
        a++;
        b++;
    }
    return strcmp(a, b) == 0 ? 0 : (strcmp(a, b) < 0 ? -1 : 1);
}

static int cmp_str_ci(const void* pa, const void* pb)
{
    return str_ci_cmp(*(const char* const*)pa, *(const char* const*)pb);
}

// ---- entry list (what the cached archive gets) -------------------------

typedef struct { const char* name; const sp_buf* data; } entry_ref;
typedef struct { entry_ref* v; int count, cap; } entry_list;

static void entry_list_free(entry_list* l)
{
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int entry_list_add(entry_list* l, const char* name, const sp_buf* data)
{
    if (l->count == l->cap) {
        int cap = l->cap ? l->cap * 2 : 16;
        entry_ref* grown = (entry_ref*)realloc(l->v, (size_t)cap * sizeof *l->v);

        if (!grown) return 0;
        l->v = grown;
        l->cap = cap;
    }
    l->v[l->count].name = name;
    l->v[l->count].data = data;
    l->count++;
    return 1;
}

// ---- paths and files ---------------------------------------------------

static int path_join(wchar_t* out, size_t cap, const wchar_t* base, const wchar_t* rest)
{
    size_t n = base ? wcslen(base) : 0;
    int sep = n > 0 && base[n - 1] != L'\\' && base[n - 1] != L'/';

    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s%s", base ? base : L"",
                        sep ? L"\\" : L"", rest ? rest : L"") >= 0;
}

static int ascii_to_wide(const char* s, wchar_t* out, size_t cap)
{
    size_t i;

    if (!s || !out || cap == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (i + 1 >= cap) return 0;
        out[i] = (wchar_t)(unsigned char)s[i];
    }
    out[i] = 0;
    return 1;
}

static int wide_to_ascii(const wchar_t* s, char* out, size_t cap)
{
    size_t i;

    if (!s || !out || cap == 0) return 0;
    for (i = 0; s[i]; i++) {
        if (i + 1 >= cap || s[i] < 0x20 || s[i] > 0x7E) return 0;
        out[i] = (char)s[i];
    }
    out[i] = 0;
    return 1;
}

static int file_exists(const wchar_t* path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int file_size_time(const wchar_t* path, unsigned long long* size,
                          unsigned long long* mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA d;

    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return 0;
    if (size) *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (mtime) {
        *mtime = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) |
                 d.ftLastWriteTime.dwLowDateTime;
    }
    return 1;
}

static int ensure_dir(const wchar_t* path)
{
    wchar_t tmp[MAX_PATH];
    size_t i, n;

    if (!path || (n = wcslen(path)) == 0 || n >= MAX_PATH) return 0;
    wcscpy_s(tmp, MAX_PATH, path);
    i = (n > 1 && tmp[1] == L':') ? 3 : 1;
    for (; i < n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'/') {
            wchar_t save = tmp[i];

            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = save;
        }
    }
    if (!CreateDirectoryW(tmp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    return 1;
}

// ---- build status ------------------------------------------------------
// One line for the DLL's log (sp_overlay_status), built from the per-feature outcomes below.

static char g_status[1000];

const char* sp_overlay_status(void)
{
    return g_status;
}
// cache.status remembers the per-feature outcome of a build, so a cache hit reports the same
// "on" / "skipped: why" states on later starts instead of guessing.
static int write_status(const wchar_t* cache_dir, const char* features)
{
    wchar_t path[MAX_PATH];
    sp_buf b;

    b.p = (unsigned char*)features;
    b.n = strlen(features);
    if (!path_join(path, MAX_PATH, cache_dir, L"cache.status")) return 0;
    return sp_write_file_atomic(path, &b);
}

static int read_status(const wchar_t* cache_dir, char* out, size_t cap)
{
    wchar_t path[MAX_PATH];
    sp_buf b;
    size_t n;

    out[0] = 0;
    if (!path_join(path, MAX_PATH, cache_dir, L"cache.status")) return 0;
    if (!sp_read_file(path, &b)) return 0;
    n = b.n < cap - 1 ? b.n : cap - 1;
    if (n) memcpy(out, b.p, n);
    out[n] = 0;
    sp_free(&b);
    return 1;
}
// ---- the game's archives -----------------------------------------------

typedef struct {
    wchar_t      path[MAX_PATH];
    wchar_t      name[OV_NAME];
    sp_dzip_file f;
} ov_archive;

typedef struct { wchar_t name[OV_NAME]; } ov_sort_name;

typedef struct {
    wchar_t    cooked[MAX_PATH];
    ov_archive arch[OV_MAX_ARCHIVES];
    int        narch;
    sp_log_fn  log;
} ov_ctx;

static int cmp_name_ci(const void* pa, const void* pb)
{
    const wchar_t* a = ((const ov_sort_name*)pa)->name;
    const wchar_t* b = ((const ov_sort_name*)pb)->name;

    for (;;) {
        wchar_t ca = *a, cb = *b;

        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb - L'A' + L'a');
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
        a++;
        b++;
    }
}

static int open_archives(ov_ctx* c, const wchar_t* cooked)
{
    ov_sort_name names[OV_MAX_ARCHIVES];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t pattern[MAX_PATH];
    int n = 0, i;

    wcscpy_s(c->cooked, MAX_PATH, cooked);
    if (!path_join(pattern, MAX_PATH, cooked, L"*.dzip")) return 0;
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (n < OV_MAX_ARCHIVES && wcslen(fd.cFileName) < OV_NAME) {
            wcscpy_s(names[n].name, OV_NAME, fd.cFileName);
            n++;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    qsort(names, (size_t)n, sizeof names[0], cmp_name_ci);
    for (i = 0; i < n; i++) {
        ov_archive* a = &c->arch[c->narch];

        if (!path_join(a->path, MAX_PATH, cooked, names[i].name)) continue;
        wcscpy_s(a->name, OV_NAME, names[i].name);
        if (sp_dzip_file_open(a->path, &a->f)) {
            c->narch++;
        } else {
            vlogf(c->log, "script overlay: %ls: not readable as DZIP v2, skipped", names[i].name);
        }
    }
    return c->narch > 0;
}

static void close_archives(ov_ctx* c)
{
    int i;

    for (i = 0; i < c->narch; i++) sp_dzip_file_close(&c->arch[i].f);
    c->narch = 0;
}

static int find_archive(const ov_ctx* c, const wchar_t* name)
{
    int i;

    for (i = 0; i < c->narch; i++) {
        if (_wcsicmp(c->arch[i].name, name) == 0) return i;
    }
    return -1;
}

// Get-ReadIcons' $find: a loose file under CookedPC wins, then the last archive in name
// order that holds the entry.
static int find_file(ov_ctx* c, const char* name, sp_buf* out)
{
    wchar_t wname[MAX_PATH], path[MAX_PATH];
    int i;

    memset(out, 0, sizeof *out);
    if (!ascii_to_wide(name, wname, MAX_PATH)) return 0;
    if (path_join(path, MAX_PATH, c->cooked, wname) && sp_read_file(path, out)) return 1;
    for (i = c->narch - 1; i >= 0; i--) {
        int idx = sp_dzip_find(&c->arch[i].f.z, name);

        if (idx < 0) continue;
        if (sp_dzip_file_extract(&c->arch[i].f, idx, out)) return 1;
        vlogf(c->log, "script overlay: %ls: cannot read '%s'", c->arch[i].name, name);
        return 0;
    }
    return 0;
}

// ---- cache key ---------------------------------------------------------

static int loose_swf_path(const ov_ctx* c, int i, wchar_t* out, size_t cap)
{
    wchar_t rel[MAX_PATH];

    return _snwprintf_s(rel, MAX_PATH, _TRUNCATE, L"globals\\gui\\%hs", k_swf_names[i]) >= 0 &&
           path_join(out, cap, c->cooked, rel);
}

static int build_key(ov_ctx* c, sp_features f, obuf* key)
{
    int i;

    ob_clear(key);
    if (!ob_printf(key, "witcher2-enginefix %s layout %d\n", ENGINEFIX_VERSION,
                   OV_CACHE_LAYOUT) ||
        !ob_printf(key, "features book_marker=%d continue=%d pause_quit=%d window_mode=%d\n",
                   f.book_marker, f.cont, f.pause_quit, f.window_mode)) {
        return 0;
    }
    for (i = 0; i < c->narch; i++) {
        unsigned long long size = 0, time = 0;
        char name[OV_NAME];

        if (!wide_to_ascii(c->arch[i].name, name, sizeof name)) return 0;
        if (!file_size_time(c->arch[i].path, &size, &time)) {
            if (!ob_printf(key, "dzip\t%s\tmissing\n", name)) return 0;
        } else if (!ob_printf(key, "dzip\t%s\t%llu\t%llu\n", name, size, time)) {
            return 0;
        }
    }
    for (i = 0; i < 2; i++) {
        wchar_t path[MAX_PATH];
        unsigned long long size = 0, time = 0;

        if (!loose_swf_path(c, i, path, MAX_PATH)) return 0;
        if (!file_size_time(path, &size, &time)) {
            if (!ob_printf(key, "swf\t%s\tmissing\n", k_swf_names[i])) return 0;
        } else if (!ob_printf(key, "swf\t%s\t%llu\t%llu\n", k_swf_names[i], size, time)) {
            return 0;
        }
    }
    return 1;
}

static int key_matches(const wchar_t* cache_dir, const obuf* key)
{
    wchar_t path[MAX_PATH];
    sp_buf on_disk;
    int same;

    if (!path_join(path, MAX_PATH, cache_dir, L"cache.key")) return 0;
    if (!sp_read_file(path, &on_disk)) return 0;
    same = on_disk.n == key->n && (key->n == 0 || memcmp(on_disk.p, key->p, key->n) == 0);
    sp_free(&on_disk);
    return same;
}

// ---- read-book icons (Get-ReadIcons) -----------------------------------

typedef struct { char* name; sp_buf dds; } ov_icon;

typedef struct {
    strlist  books;         // books whose icon was written, Get-ReadIcons order
    ov_icon* files;         // archive entry name -> DDS bytes
    int      nfiles, capfiles;
    char     why[300];
} ov_icons;

static void icons_free(ov_icons* ic)
{
    int i;

    strlist_free(&ic->books);
    for (i = 0; i < ic->nfiles; i++) {
        free(ic->files[i].name);
        sp_free(&ic->files[i].dds);
    }
    free(ic->files);
    memset(ic, 0, sizeof *ic);
}

static int icons_add(ov_icons* ic, const char* archive_name, const sp_buf* dds)
{
    int i;
    char* name;

    for (i = 0; i < ic->nfiles; i++) {
        if (_stricmp(ic->files[i].name, archive_name) == 0) {
            sp_buf copy = { NULL, 0 };

            copy.p = (unsigned char*)malloc(dds->n ? dds->n : 1);
            if (!copy.p) return 0;
            if (dds->n) memcpy(copy.p, dds->p, dds->n);
            copy.n = dds->n;
            sp_free(&ic->files[i].dds);
            ic->files[i].dds = copy;
            return 1;
        }
    }
    if (ic->nfiles == ic->capfiles) {
        int cap = ic->capfiles ? ic->capfiles * 2 : 32;
        ov_icon* grown = (ov_icon*)realloc(ic->files, (size_t)cap * sizeof *ic->files);

        if (!grown) return 0;
        ic->files = grown;
        ic->capfiles = cap;
    }
    name = (char*)malloc(strlen(archive_name) + 1);
    if (!name) return 0;
    strcpy_s(name, strlen(archive_name) + 1, archive_name);
    memset(&ic->files[ic->nfiles], 0, sizeof ic->files[0]);
    ic->files[ic->nfiles].name = name;
    ic->files[ic->nfiles].dds.p = (unsigned char*)malloc(dds->n ? dds->n : 1);
    if (!ic->files[ic->nfiles].dds.p) {
        free(name);
        return 0;
    }
    if (dds->n) memcpy(ic->files[ic->nfiles].dds.p, dds->p, dds->n);
    ic->files[ic->nfiles].dds.n = dds->n;
    ic->nfiles++;
    return 1;
}

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int ci_equal(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
    return a == b;
}

static int ci_starts(const unsigned char* p, size_t n, const char* lit)
{
    size_t i, l = strlen(lit);

    if (n < l) return 0;
    for (i = 0; i < l; i++) {
        if (!ci_equal(p[i], (unsigned char)lit[i])) return 0;
    }
    return 1;
}

static int ci_find(const unsigned char* p, size_t n, size_t from, const char* lit, size_t* at)
{
    size_t i, l = strlen(lit);

    for (i = from; i + l <= n; i++) {
        if (ci_starts(p + i, n - i, lit)) {
            *at = i;
            return 1;
        }
    }
    return 0;
}

// (^|\\)def_[^\\]*\.xml$, as Get-ReadIcons filters the archives' entry names.
static int is_def_name(const char* name)
{
    const char* leaf = name;
    const char* s;
    size_t len;

    for (s = name; *s; s++) {
        if (*s == '\\') leaf = s + 1;
    }
    len = strlen(leaf);
    return len >= 8 && ci_starts((const unsigned char*)leaf, len, "def_") &&
           ci_starts((const unsigned char*)leaf + len - 4, 4, ".xml");
}

// <tags>[^<]*SortTypeBook anywhere in the item element.
static int has_book_tag(const unsigned char* body, size_t n)
{
    static const char tag[] = "<tags>";
    static const char wanted[] = "SortTypeBook";
    size_t at, i, k, wl = strlen(wanted);

    if (!ci_find(body, n, 0, tag, &at)) return 0;
    for (i = at + strlen(tag); i < n && body[i] != '<'; i++) {
        if (i + wl > n) break;
        for (k = 0; k < wl; k++) {
            if (!ci_equal(body[i + k], (unsigned char)wanted[k])) break;
        }
        if (k == wl) return 1;
    }
    return 0;
}

// <item\s+name\s*=\s*"([^"]+)"(.*?)</item> with Singleline, collecting the names whose element
// has SortTypeBook in its tags.
static int scan_def(const sp_buf* text, strlist* books)
{
    size_t i = 0;

    while (i < text->n) {
        size_t at, p, name_start, name_end, body_end;

        if (!ci_find(text->p, text->n, i, "<item", &at)) break;
        p = at + 5;
        if (p >= text->n || !is_ws(text->p[p])) {
            i = p;
            continue;
        }
        while (p < text->n && is_ws(text->p[p])) p++;
        if (!ci_starts(text->p + p, text->n - p, "name")) {
            i = p;
            continue;
        }
        p += 4;
        while (p < text->n && is_ws(text->p[p])) p++;
        if (p >= text->n || text->p[p] != '=') {
            i = p;
            continue;
        }
        p++;
        while (p < text->n && is_ws(text->p[p])) p++;
        if (p >= text->n || text->p[p] != '"') {
            i = p;
            continue;
        }
        p++;
        name_start = p;
        while (p < text->n && text->p[p] != '"') p++;
        if (p >= text->n || p == name_start) {
            i = p;
            continue;
        }
        name_end = p;
        p++;
        if (!ci_find(text->p, text->n, p, "</item>", &body_end)) break;
        if (has_book_tag(text->p + p, body_end - p)) {
            char name[256];
            size_t n = name_end - name_start;

            if (n < sizeof name) {
                memcpy(name, text->p + name_start, n);
                name[n] = 0;
                if (!strlist_add_unique(books, name)) return 0;
            }
        }
        i = body_end + 7;
    }
    return 1;
}

// A def file is UTF-16LE, UTF-16BE (BOM) or Latin-1; non-ASCII becomes 0x80 so a name that
// carries such a character fails the printable-ASCII filter, as it does in the reference.
static int decode_def(const sp_buf* raw, obuf* text)
{
    size_t i;

    ob_clear(text);
    if (raw->n >= 2 && raw->p[0] == 0xFF && raw->p[1] == 0xFE) {
        for (i = 2; i + 1 < raw->n; i += 2) {
            unsigned int u = (unsigned int)raw->p[i] | ((unsigned int)raw->p[i + 1] << 8);
            char c = (char)(u < 0x80 ? u : 0x80u);

            if (!ob_put(text, &c, 1)) return 0;
        }
    } else if (raw->n >= 2 && raw->p[0] == 0xFE && raw->p[1] == 0xFF) {
        for (i = 2; i + 1 < raw->n; i += 2) {
            unsigned int u = ((unsigned int)raw->p[i] << 8) | (unsigned int)raw->p[i + 1];
            char c = (char)(u < 0x80 ? u : 0x80u);

            if (!ob_put(text, &c, 1)) return 0;
        }
    } else if (!ob_put(text, raw->p, raw->n)) {
        return 0;
    }
    return 1;
}

// Get-ReadIcons: every book's icon with the inventory's alreadyread overlay at 0.9, as entries
// under w2ef_read for the carrier archive (Branch B: the engine never looks up a loose file it
// did not see in its startup walk). Sets icons->why when there are none; other failures are
// logged too.
static void read_icons(ov_ctx* c, ov_icons* icons)
{
    sp_buf tick_raw = { NULL, 0 };
    obuf text = { 0 };
    strlist defs = { 0 }, books = { 0 };
    unsigned* tick = NULL;
    unsigned* px = NULL;
    int i;

    memset(icons, 0, sizeof *icons);
    tick = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *tick);
    px = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *px);
    if (!tick || !px) {
        _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
        goto done;
    }
    if (!find_file(c, "globals\\gui\\icons\\tooltip\\alreadyread_64x64.dds", &tick_raw)) {
        _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "alreadyread_64x64.dds not found");
        goto done;
    }
    if (!sp_dds_decode64(&tick_raw, tick)) {
        _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE,
                    "alreadyread_64x64.dds is not a 64x64 icon");
        goto done;
    }
    for (i = 0; i < c->narch; i++) {
        int k;

        for (k = 0; k < c->arch[i].f.z.count; k++) {
            const char* name = c->arch[i].f.z.e[k].name;

            if (is_def_name(name) && !strlist_add_unique(&defs, name)) {
                _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
                goto done;
            }
        }
    }
    qsort(defs.v, (size_t)defs.count, sizeof defs.v[0], cmp_str_ci);

    for (i = 0; i < defs.count; i++) {
        sp_buf raw = { NULL, 0 };
        sp_buf plain;

        if (!find_file(c, defs.v[i], &raw)) continue;
        if (!decode_def(&raw, &text)) {
            sp_free(&raw);
            _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
            goto done;
        }
        sp_free(&raw);
        plain.p = (unsigned char*)text.p;
        plain.n = text.n;
        if (!scan_def(&plain, &books)) {
            _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
            goto done;
        }
    }

    for (i = 0; i < books.count; i++) {
        const char* book = books.v[i];
        char icon[300], path[400], archive_name[500];
        sp_buf raw = { NULL, 0 }, out = { NULL, 0 };
        size_t k, w = 0;
        int bad = 0;

        for (k = 0; book[k]; k++) {
            unsigned char ch = (unsigned char)book[k];

            if (ch < 0x20 || ch > 0x7E || ch == '"' || ch == '\\') {
                bad = 1;
                break;
            }
        }
        if (bad || !book[0]) continue;
        for (k = 0; book[k]; k++) {
            unsigned char ch = (unsigned char)book[k];

            if (ch == ' ' || ch == '\'') continue;
            if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
            if (w + 1 >= sizeof icon) {
                bad = 1;
                break;
            }
            icon[w++] = (char)ch;
        }
        if (bad) continue;
        icon[w] = 0;
        if (_snprintf_s(path, sizeof path, _TRUNCATE, "globals\\gui\\icons\\items\\%s_64x64.dds",
                        icon) < 0) {
            continue;
        }
        if (!find_file(c, path, &raw)) continue;
        if (!sp_dds_decode64(&raw, px)) {
            sp_free(&raw);
            continue;
        }
        sp_free(&raw);
        sp_dds_over(px, tick, 0.9);
        if (!sp_dds_encode64(px, &out)) {
            _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
            goto done;
        }
        if (_snprintf_s(archive_name, sizeof archive_name, _TRUNCATE, "%s%s_64x64.dds",
                        k_icon_folder, icon) < 0) {
            sp_free(&out);
            continue;
        }
        if (!icons_add(icons, archive_name, &out) ||
            !strlist_add_unique(&icons->books, book)) {
            sp_free(&out);
            _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "out of memory");
            goto done;
        }
        sp_free(&out);
    }
    if (icons->nfiles == 0 && !icons->why[0]) {
        _snprintf_s(icons->why, sizeof icons->why, _TRUNCATE, "no book icons found");
    }
done:
    free(tick);
    free(px);
    sp_free(&tick_raw);
    ob_free(&text);
    strlist_free(&defs);
    strlist_free(&books);
}

// ---- the two UI movies -------------------------------------------------

// 1 = patched, `kind` 1 for a loose source (write a cached file) or 2 for an archive-only
// source (add an archive entry); 0 = skipped, with the reason in why.
static int patch_swf(ov_ctx* c, int which, int* kind, sp_buf* out, char* why, size_t whylen)
{
    sp_buf raw = { NULL, 0 }, fws = { NULL, 0 };
    wchar_t loose[MAX_PATH];
    char entry[MAX_PATH];
    int ab, idx;

    *kind = 0;
    memset(out, 0, sizeof *out);
    if (!loose_swf_path(c, which, loose, MAX_PATH)) {
        _snprintf_s(why, whylen, _TRUNCATE, "path too long");
        return 0;
    }
    if (sp_read_file(loose, &raw)) {
        *kind = 1;
    } else {
        ab = find_archive(c, L"abetterui.dzip");
        if (ab < 0) {
            _snprintf_s(why, whylen, _TRUNCATE,
                        "not found, neither loose nor in abetterui.dzip");
            return 0;
        }
        if (_snprintf_s(entry, sizeof entry, _TRUNCATE, "globals\\gui\\%s",
                        k_swf_names[which]) < 0) {
            _snprintf_s(why, whylen, _TRUNCATE, "path too long");
            return 0;
        }
        idx = sp_dzip_find(&c->arch[ab].f.z, entry);
        if (idx < 0) {
            _snprintf_s(why, whylen, _TRUNCATE,
                        "not found, neither loose nor in abetterui.dzip");
            return 0;
        }
        if (!sp_dzip_file_extract(&c->arch[ab].f, idx, &raw)) {
            _snprintf_s(why, whylen, _TRUNCATE, "cannot read from abetterui.dzip");
            return 0;
        }
        *kind = 2;
    }
    if (!sp_swf_uncompressed(&raw, &fws)) {
        sp_free(&raw);
        _snprintf_s(why, whylen, _TRUNCATE, "not a SWF file");
        return 0;
    }
    sp_free(&raw);
    why[0] = 0;
    if (sp_swf_state(&fws, SP_SWF_COMPARE, 2, NULL, why, whylen) != SP_SWF_ORIGINAL) {
        char reason[400];

        _snprintf_s(reason, sizeof reason, _TRUNCATE, "%s (another UI mod?)", why);
        _snprintf_s(why, whylen, _TRUNCATE, "%s", reason);
        sp_free(&fws);
        return 0;
    }
    if (!sp_swf_apply(&fws, SP_SWF_COMPARE, 2, out)) {
        sp_free(&fws);
        _snprintf_s(why, whylen, _TRUNCATE, "cannot patch the movie");
        return 0;
    }
    sp_free(&fws);
    return 1;
}

// ---- redirects ---------------------------------------------------------

static int add_redirect(sp_redirect* r, int* nr, int maxr, const wchar_t* target,
                        const wchar_t* cached)
{
    if (*nr >= maxr) return 0;
    wcscpy_s(r[*nr].target, MAX_PATH, target);
    wcscpy_s(r[*nr].cached, MAX_PATH, cached);
    (*nr)++;
    return 1;
}

static void remove_cache_outputs(const wchar_t* cache_dir)
{
    static const wchar_t* const leaves[5] = {
        L"cache.key", L"cache.status", L"base_scripts.dzip", L"ui_trade.swf", L"ui_storage.swf"
    };
    int i;

    for (i = 0; i < 5; i++) {
        wchar_t path[MAX_PATH];

        if (path_join(path, MAX_PATH, cache_dir, leaves[i])) DeleteFileW(path);
    }
    for (i = 0; i < OV_NCARRIERS; i++) {
        wchar_t path[MAX_PATH];

        if (path_join(path, MAX_PATH, cache_dir, k_carriers[i])) DeleteFileW(path);
    }
}

// The carrier for this install (index into ctx->arch), or -1 when none is present.
static int find_carrier(const ov_ctx* c)
{
    int i, a;

    for (i = 0; i < OV_NCARRIERS; i++) {
        a = find_archive(c, k_carriers[i]);
        if (a >= 0) return a;
    }
    return -1;
}

// Writes `src` with `l`'s entries replaced or added to cache_dir\<leaf> and redirects
// CookedPC\<leaf> there.
static int write_archive(const sp_dzip* src, const entry_list* l, long long filetime,
                         const wchar_t* cache_dir, const wchar_t* cooked, const wchar_t* leaf,
                         sp_redirect* r, int* nr, int maxr, sp_log_fn log)
{
    const char** names = (const char**)malloc((size_t)l->count * sizeof *names);
    sp_buf* datas = (sp_buf*)malloc((size_t)l->count * sizeof *datas);
    sp_buf archive = { NULL, 0 };
    wchar_t cached[MAX_PATH], target[MAX_PATH];
    int i, ok = 0;

    if (!names || !datas) {
        vlogf(log, "script overlay: out of memory");
        goto out;
    }
    for (i = 0; i < l->count; i++) {
        names[i] = l->v[i].name;
        datas[i] = *l->v[i].data;
    }
    if (!sp_dzip_write(src, l->count, names, datas, filetime, &archive)) {
        vlogf(log, "script overlay: cannot write the patched %ls", leaf);
        goto out;
    }
    if (!path_join(cached, MAX_PATH, cache_dir, leaf) || !sp_write_file_atomic(cached, &archive)) {
        vlogf(log, "script overlay: cannot write %ls\\%ls", cache_dir, leaf);
        goto out;
    }
    if (!path_join(target, MAX_PATH, cooked, leaf) || !add_redirect(r, nr, maxr, target, cached)) {
        vlogf(log, "script overlay: game path too long");
        goto out;
    }
    vlogf(log, "script overlay: %ls: %d entries replaced or added", leaf, l->count);
    ok = 1;
out:
    free(names);
    free(datas);
    sp_free(&archive);
    return ok;
}

static int write_key(const wchar_t* cache_dir, const obuf* key)
{
    wchar_t path[MAX_PATH];
    sp_buf b;

    b.p = (unsigned char*)key->p;
    b.n = key->n;
    if (!path_join(path, MAX_PATH, cache_dir, L"cache.key")) return 0;
    return sp_write_file_atomic(path, &b);
}

// A reused cache: serve what is there, but never a path that no longer exists.
static void reuse_redirects(ov_ctx* c, const wchar_t* cache_dir, sp_features f,
                            sp_redirect* r, int* nr, int maxr, sp_log_fn log)
{
    wchar_t cached[MAX_PATH], target[MAX_PATH];
    int i;

    if (path_join(cached, MAX_PATH, cache_dir, L"base_scripts.dzip") && file_exists(cached)) {
        if (path_join(target, MAX_PATH, c->cooked, L"base_scripts.dzip")) {
            add_redirect(r, nr, maxr, target, cached);
        }
    } else {
        vlogf(log, "script overlay: cache: no base_scripts.dzip in it - serving the originals");
    }
    if (!f.book_marker) return;
    for (i = 0; i < OV_NCARRIERS; i++) {
        if (!path_join(cached, MAX_PATH, cache_dir, k_carriers[i]) || !file_exists(cached)) continue;
        if (find_archive(c, k_carriers[i]) < 0) continue;
        if (path_join(target, MAX_PATH, c->cooked, k_carriers[i])) {
            add_redirect(r, nr, maxr, target, cached);
        }
    }
    for (i = 0; i < 2; i++) {
        wchar_t name[MAX_PATH];

        if (!ascii_to_wide(k_swf_names[i], name, MAX_PATH)) continue;
        if (!path_join(cached, MAX_PATH, cache_dir, name) || !file_exists(cached)) continue;
        if (loose_swf_path(c, i, target, MAX_PATH)) add_redirect(r, nr, maxr, target, cached);
    }
}

// ---- the overlay -------------------------------------------------------

int sp_overlay_build(const wchar_t* game, const wchar_t* cache_dir, sp_features f,
                     long long filetime, sp_redirect* r, int* nr, int maxr, sp_log_fn log)
{
    ov_ctx ctx;
    ov_icons icons;
    obuf key = { 0 };
    wchar_t cooked[MAX_PATH], scripts_path[MAX_PATH];
    sp_buf scripts_raw = { NULL, 0 }, collect = { NULL, 0 }, carrier_raw = { NULL, 0 };
    sp_dzip scripts, carrier_z;
    sp_text texts[5];
    sp_buf orig[5];
    entry_list entries, carried;
    int carrier = -1;
    sp_buf swf_data[2];
    int swf_kind[2] = { 0, 0 };
    sp_ctx sctx;
    sp_ctx* pctx = NULL;
    char st_book[240] = "off", st_cont[240] = "off", st_quit[240] = "off";
    char st_window[240] = "off";
    char status_feats[900] = "";
    int i, ok = 0, skip_scripts = 0, have_collect = 0, book_ok = 0, any, from_cache = 0;
    DWORD started = GetTickCount();

    memset(&ctx, 0, sizeof ctx);
    memset(&icons, 0, sizeof icons);
    memset(texts, 0, sizeof texts);
    memset(orig, 0, sizeof orig);
    memset(&entries, 0, sizeof entries);
    memset(&carried, 0, sizeof carried);
    memset(&scripts, 0, sizeof scripts);
    memset(&carrier_z, 0, sizeof carrier_z);
    memset(swf_data, 0, sizeof swf_data);
    memset(&sctx, 0, sizeof sctx);
    if (nr) *nr = 0;
    if (!game || !cache_dir || !nr || !r || maxr <= 0) {
        vlogf(log, "script overlay: bad arguments");
        return 0;
    }
    ctx.log = log;
    any = f.book_marker || f.cont || f.pause_quit || f.window_mode;
    if (!any) {
        _snprintf_s(g_status, sizeof g_status, _TRUNCATE,
                    "book_marker=off continue=off pause_quit=off window_mode=off cache=-");
        vlogf(log, "script overlay: no feature enabled");
        return 1;
    }
    if (!path_join(cooked, MAX_PATH, game, L"CookedPC") ||
        !path_join(scripts_path, MAX_PATH, cooked, L"base_scripts.dzip")) {
        vlogf(log, "script overlay: game path too long");
        return 0;
    }
    vlogf(log, "script overlay: cache %ls", cache_dir);
    if (!open_archives(&ctx, cooked)) {
        vlogf(log, "script overlay: no readable archive under %ls", cooked);
        return 0;
    }
    if (!build_key(&ctx, f, &key)) {
        vlogf(log, "script overlay: cannot build the cache key");
        goto done;
    }
    if (key_matches(cache_dir, &key)) {
        char remembered[900];

        reuse_redirects(&ctx, cache_dir, f, r, nr, maxr, log);
        if (read_status(cache_dir, remembered, sizeof remembered) && remembered[0]) {
            _snprintf_s(status_feats, sizeof status_feats, _TRUNCATE, "%s", remembered);
        } else {
            _snprintf_s(status_feats, sizeof status_feats, _TRUNCATE,
                        "book_marker=%s continue=%s pause_quit=%s window_mode=%s",
                        f.book_marker ? "on" : "off", f.cont ? "on" : "off",
                        f.pause_quit ? "on" : "off", f.window_mode ? "on" : "off");
        }
        from_cache = 1;
        vlogf(log, "script overlay: cache: reused (%d redirects)", *nr);
        ok = 1;
        goto done;
    }
    if (!ensure_dir(cache_dir)) {
        vlogf(log, "script overlay: cannot create %ls", cache_dir);
        goto done;
    }
    remove_cache_outputs(cache_dir);

    // The five script entries; a marked archive means another copy of the patcher is on disk.
    if (!sp_read_file(scripts_path, &scripts_raw)) {
        vlogf(log, "script overlay: cannot read %ls", scripts_path);
        goto done;
    }
    if (!sp_dzip_open(&scripts_raw, &scripts)) {
        vlogf(log, "script overlay: base_scripts.dzip is not a DZIP v2 archive");
        goto done;
    }
    for (i = 0; i < 5; i++) {
        int idx = sp_dzip_find(&scripts, k_script_entries[i]);

        if (idx < 0) {
            vlogf(log, "script overlay: %s not in base_scripts.dzip", k_script_entries[i]);
            goto done;
        }
        if (!sp_dzip_extract(&scripts, idx, &orig[i])) {
            vlogf(log, "script overlay: cannot extract %s", k_script_entries[i]);
            goto done;
        }
        texts[i].name = k_script_entries[i];
        texts[i].text.p = (unsigned char*)malloc(orig[i].n ? orig[i].n : 1);
        if (!texts[i].text.p) {
            vlogf(log, "script overlay: out of memory");
            goto done;
        }
        if (orig[i].n) memcpy(texts[i].text.p, orig[i].p, orig[i].n);
        texts[i].text.n = orig[i].n;
        if (sp_script_marked(&texts[i].text)) {
            vlogf(log, "script overlay: %s already holds witcher2-enginefix changes - run the "
                       "old Uninstall.bat once (Task 8 automates this); script patches off",
                  k_script_entries[i]);
            skip_scripts = 1;
            break;
        }
    }
    if (skip_scripts) {
        if (f.book_marker) _snprintf_s(st_book, sizeof st_book, _TRUNCATE, "skipped: scripts already patched");
        if (f.cont) _snprintf_s(st_cont, sizeof st_cont, _TRUNCATE, "skipped: scripts already patched");
        if (f.pause_quit) _snprintf_s(st_quit, sizeof st_quit, _TRUNCATE, "skipped: scripts already patched");
        if (f.window_mode) _snprintf_s(st_window, sizeof st_window, _TRUNCATE, "skipped: scripts already patched");
        _snprintf_s(status_feats, sizeof status_feats, _TRUNCATE,
                    "book_marker=%s continue=%s pause_quit=%s window_mode=%s", st_book, st_cont,
                    st_quit, st_window);
        write_status(cache_dir, status_feats);
        if (!write_key(cache_dir, &key)) {
            vlogf(log, "script overlay: cannot write the cache key");
        }
        ok = 1;
        goto done;
    }
    // The icons need an archive to live in; without one the scripts get no read-icon list,
    // so they never name an icon that does not exist.
    if (f.book_marker) {
        carrier = find_carrier(&ctx);
        if (carrier < 0) {
            vlogf(log, "script overlay: read icons: off - no archive to carry them (%ls and the "
                       "other DLC archives are missing); the marker keeps working", k_carriers[0]);
        }
    }
    if (f.book_marker && carrier >= 0) {
        read_icons(&ctx, &icons);
        if (icons.nfiles > 0) {
            vlogf(log, "script overlay: read icons: %d books, %d icons", icons.books.count,
                  icons.nfiles);
        } else {
            vlogf(log, "script overlay: read icons: none (%s) - the marker keeps working",
                  icons.why[0] ? icons.why : "not built");
        }
    }

    if (f.book_marker) {
        if (!sp_script_collect_body(&texts[4].text, &collect, NULL, 0)) {
            vlogf(log, "script overlay: book marker: %s", sp_script_error());
        } else {
            have_collect = 1;
        }
    }
    sctx.collect_body = have_collect ? (const char*)collect.p : NULL;
    sctx.books = (const char* const*)icons.books.v;
    sctx.nbooks = icons.books.count;
    pctx = &sctx;

    if (f.book_marker) {
        if (!have_collect) {
            _snprintf_s(st_book, sizeof st_book, _TRUNCATE, "skipped: %s", sp_script_error());
            vlogf(log, "script overlay: book_marker=%s", st_book);
        } else if (!sp_script_apply("BookMarker", texts, 5, pctx, NULL, 0)) {
            _snprintf_s(st_book, sizeof st_book, _TRUNCATE, "skipped: %s", sp_script_error());
            vlogf(log, "script overlay: book_marker=%s", st_book);
        } else {
            _snprintf_s(st_book, sizeof st_book, _TRUNCATE, "on");
            book_ok = 1;
            vlogf(log, "script overlay: book_marker=on");
        }
    }
    if (f.cont) {
        if (!sp_script_apply("Continue", texts, 5, pctx, NULL, 0)) {
            _snprintf_s(st_cont, sizeof st_cont, _TRUNCATE, "skipped: %s", sp_script_error());
            vlogf(log, "script overlay: continue=%s", st_cont);
        } else {
            _snprintf_s(st_cont, sizeof st_cont, _TRUNCATE, "on");
            vlogf(log, "script overlay: continue=on");
        }
    }
    if (f.pause_quit) {
        if (!sp_script_apply("PauseQuit", texts, 5, pctx, NULL, 0)) {
            _snprintf_s(st_quit, sizeof st_quit, _TRUNCATE, "skipped: %s", sp_script_error());
            vlogf(log, "script overlay: pause_quit=%s", st_quit);
        } else {
            _snprintf_s(st_quit, sizeof st_quit, _TRUNCATE, "on");
            vlogf(log, "script overlay: pause_quit=on");
        }
    }
    if (f.window_mode) {
        if (!sp_script_apply("WindowMode", texts, 5, pctx, NULL, 0)) {
            _snprintf_s(st_window, sizeof st_window, _TRUNCATE, "skipped: %s", sp_script_error());
            vlogf(log, "script overlay: window_mode=%s", st_window);
        } else {
            _snprintf_s(st_window, sizeof st_window, _TRUNCATE, "on");
            vlogf(log, "script overlay: window_mode=on");
        }
    }

    // The movies: only BookMarker has Swf = true in the reference, and a half-patched marker
    // (scripts without the tick no-op) must not be served.
    if (book_ok) {
        for (i = 0; i < 2; i++) {
            char why[400];
            int patched;

            why[0] = 0;
            patched = patch_swf(&ctx, i, &swf_kind[i], &swf_data[i], why, sizeof why);
            if (patched && swf_kind[i] == 2 && carrier < 0) {
                sp_free(&swf_data[i]);
                swf_kind[i] = 0;
                vlogf(log, "script overlay: %s: left as it is (only in abetterui.dzip and no "
                           "archive to carry the patched copy); the book marker works without "
                           "that part", k_swf_names[i]);
            } else if (patched) {
                vlogf(log, "script overlay: %s: patched (%s)", k_swf_names[i],
                      swf_kind[i] == 1 ? "loose" : "abetterui.dzip");
            } else {
                swf_kind[i] = 0;
                vlogf(log, "script overlay: %s: left as it is (%s); the book marker works "
                           "without that part", k_swf_names[i], why);
            }
        }
    }

    // What the cached archives get. base_scripts.dzip: only the changed script entries, because
    // the game compiles every entry in it. The carrier: the read icons and any patched movie
    // that only exists inside abetterui.dzip.
    for (i = 0; i < 5; i++) {
        if (texts[i].text.n != orig[i].n ||
            (orig[i].n && memcmp(texts[i].text.p, orig[i].p, orig[i].n) != 0)) {
            if (!entry_list_add(&entries, k_script_entries[i], &texts[i].text)) {
                vlogf(log, "script overlay: out of memory");
                goto done;
            }
        }
    }
    for (i = 0; i < icons.nfiles; i++) {
        if (!entry_list_add(&carried, icons.files[i].name, &icons.files[i].dds)) {
            vlogf(log, "script overlay: out of memory");
            goto done;
        }
    }
    for (i = 0; i < 2; i++) {
        if (swf_kind[i] != 2) continue;
        if (!entry_list_add(&carried, k_swf_entries[i], &swf_data[i])) {
            vlogf(log, "script overlay: out of memory");
            goto done;
        }
    }

    if (entries.count > 0 &&
        !write_archive(&scripts, &entries, filetime, cache_dir, cooked, L"base_scripts.dzip", r,
                       nr, maxr, log)) {
        goto done;
    }
    if (carried.count > 0) {
        const ov_archive* a = &ctx.arch[carrier];

        if (!sp_read_file(a->path, &carrier_raw) || !sp_dzip_open(&carrier_raw, &carrier_z)) {
            vlogf(log, "script overlay: cannot read %ls", a->path);
            goto done;
        }
        if (!write_archive(&carrier_z, &carried, filetime, cache_dir, cooked, a->name, r, nr,
                           maxr, log)) {
            goto done;
        }
    }

    for (i = 0; i < 2; i++) {
        wchar_t cached[MAX_PATH], target[MAX_PATH], name[MAX_PATH];

        if (swf_kind[i] != 1) continue;
        if (!ascii_to_wide(k_swf_names[i], name, MAX_PATH) ||
            !path_join(cached, MAX_PATH, cache_dir, name)) {
            vlogf(log, "script overlay: cache path too long");
            goto done;
        }
        if (!sp_write_file_atomic(cached, &swf_data[i])) {
            vlogf(log, "script overlay: cannot write %ls", cached);
            goto done;
        }
        if (!loose_swf_path(&ctx, i, target, MAX_PATH) ||
            !add_redirect(r, nr, maxr, target, cached)) {
            vlogf(log, "script overlay: game path too long");
            goto done;
        }
    }

    _snprintf_s(status_feats, sizeof status_feats, _TRUNCATE,
                "book_marker=%s continue=%s pause_quit=%s window_mode=%s", st_book, st_cont,
                st_quit, st_window);
    write_status(cache_dir, status_feats);
    if (!write_key(cache_dir, &key)) {
        vlogf(log, "script overlay: cannot write the cache key");
        goto done;
    }
    vlogf(log, "script overlay: cache: built in %lu ms (%d redirects)",
          (unsigned long)(GetTickCount() - started), *nr);
    ok = 1;
done:
    if (!ok) {
        _snprintf_s(g_status, sizeof g_status, _TRUNCATE, "build failed");
    } else if (from_cache) {
        _snprintf_s(g_status, sizeof g_status, _TRUNCATE, "%s cache=reused", status_feats);
    } else {
        DWORD ms = GetTickCount() - started;

        _snprintf_s(g_status, sizeof g_status, _TRUNCATE, "%s cache=built %lu ms", status_feats,
                    (unsigned long)ms);
    }
    for (i = 0; i < 5; i++) sp_free(&texts[i].text);
    for (i = 0; i < 5; i++) sp_free(&orig[i]);
    for (i = 0; i < 2; i++) sp_free(&swf_data[i]);
    entry_list_free(&entries);
    entry_list_free(&carried);
    sp_dzip_close(&scripts);
    sp_dzip_close(&carrier_z);
    sp_free(&scripts_raw);
    sp_free(&carrier_raw);
    sp_free(&collect);
    icons_free(&icons);
    ob_free(&key);
    close_archives(&ctx);
    if (!ok) {
        *nr = 0;
        vlogf(log, "script overlay: build failed - serving the original files");
    }
    return ok;
}
