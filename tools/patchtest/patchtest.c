// patchtest - offline tests (and, from Task 6, the overlay build) for the script-patch
// code in src/sp_*.c. It compiles those same sources and never touches the game.
//
// Usage:
//   patchtest test [--game "<game>"]     run the unit tests
//   patchtest build <game> <out>         build the overlay (Task 6)
//
// One PASS/FAIL/SKIP line per test; the exit code is the number of failures.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "sp_common.h"
#include "sp_dzip.h"

#define DZIP_MAGIC 0x50495A44u

static int            g_failures;
static const wchar_t* g_game;          // --game, NULL when not given
static wchar_t        g_gamePath[MAX_PATH];
static char           g_why[512];      // "why" for a failing check, when one string is needed

// ---- little helpers -----------------------------------------------------

static unsigned int rd32(const unsigned char* p)
{
    unsigned int v;
    memcpy(&v, p, 4);
    return v;
}

static void wr32(unsigned char* p, unsigned int v)
{
    memcpy(p, &v, 4);
}

static unsigned long long rd64(const unsigned char* p)
{
    unsigned long long v;
    memcpy(&v, p, 8);
    return v;
}

static void wr64(unsigned char* p, unsigned long long v)
{
    memcpy(p, &v, 8);
}

static int path_join(wchar_t* out, size_t cap, const wchar_t* base, const wchar_t* rest)
{
    size_t n = base ? wcslen(base) : 0;
    int sep = n > 0 && base[n - 1] != L'\\' && base[n - 1] != L'/';
    return _snwprintf_s(out, cap, _TRUNCATE, L"%s%s%s", base ? base : L"",
                        sep ? L"\\" : L"", rest) >= 0;
}

// Deterministic filler; two buffers of the same size compare equal.
static sp_buf make_text(size_t n)
{
    sp_buf b;
    b.n = n;
    b.p = (unsigned char*)malloc(n ? n : 1);
    if (!b.p) { b.n = 0; return b; }
    for (size_t i = 0; i < n; i++)
        b.p[i] = (unsigned char)('a' + (i * 7 + i / 33) % 26);
    return b;
}

// Reads only the 32-byte header and the entry-table tail of a DZIP, so a 10 GB archive
// costs two reads. The header's table offset is rewritten to 32, so the table follows the
// header in `out`: sp_dzip_open only needs those.
static int read_dzip_table(const wchar_t* path, sp_buf* out, long long* file_size)
{
    HANDLE h = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size, at;
    unsigned char head[32];
    DWORD got = 0;
    long long table, tail;
    int ok = 0;

    memset(out, 0, sizeof *out);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) goto done;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 32) goto done;
    if (!ReadFile(h, head, sizeof head, &got, NULL) || got != sizeof head) goto done;
    if (rd32(head) != DZIP_MAGIC || rd32(head + 4) != 2) goto done;
    table = (long long)rd64(head + 16);
    tail = (long long)size.QuadPart - table;
    if (table < 32 || tail <= 0 || tail > 64LL * 1024 * 1024) goto done;
    out->n = 32 + (size_t)tail;
    out->p = (unsigned char*)malloc(out->n);
    if (!out->p) { out->n = 0; goto done; }
    memcpy(out->p, head, 32);
    wr64(out->p + 16, 32);
    at.QuadPart = table;
    if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN) ||
        !ReadFile(h, out->p + 32, (DWORD)tail, &got, NULL) || (long long)got != tail) {
        sp_free(out);
        goto done;
    }
    *file_size = (long long)size.QuadPart;
    ok = 1;
done:
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return ok;
}

static const char* fail_archive(const wchar_t* name, const char* what)
{
    _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "%ls: %s", name, what);
    return g_why;
}

// A DZIP v2 with two entries, built with sp_dzip_write from an empty template: header
// only, no entries, so the table is empty and its hash is the FNV seed value.
static int make_dzip2(sp_buf* out)
{
    unsigned char head[32];
    sp_buf empty = { head, sizeof head }, a = { NULL, 0 }, b = { NULL, 0 };
    const char* names[2] = { "a\\x.ws", "b\\y.ws" };
    sp_buf datas[2];
    sp_dzip z;
    int ok;

    memset(head, 0, sizeof head);
    wr32(head + 0, DZIP_MAGIC);
    wr32(head + 4, 2);
    wr64(head + 16, 32);                        // table right after the header
    wr64(head + 24, 0x00000000FFFFFFFFull);     // hash of zero entries
    a = make_text(5);
    if (a.p) memcpy(a.p, "hello", 5);
    b = make_text(200000);
    datas[0] = a;
    datas[1] = b;
    memset(&z, 0, sizeof z);
    ok = sp_dzip_open(&empty, &z) &&
         sp_dzip_write(&z, 2, names, datas, 1234567890LL, out);
    sp_dzip_close(&z);
    sp_free(&a);
    sp_free(&b);
    return ok;
}

// ---- tests --------------------------------------------------------------
// Each returns NULL on success or a reason; CHECK jumps to `done` so buffers are freed.

#define CHECK(cond) do { if (!(cond)) { why = #cond; goto done; } } while (0)

// Two entries round-trip: a 5-byte one (one literal run) and a 200 000-byte one (four
// 64 KiB blocks), then found by a case-insensitive name and extracted intact.
static const char* test_dzip_roundtrip(void)
{
    const char* why = NULL;
    sp_buf zip = { NULL, 0 }, txt = { NULL, 0 }, want = { NULL, 0 };
    sp_dzip z;

    memset(&z, 0, sizeof z);
    CHECK(make_dzip2(&zip));
    CHECK(sp_dzip_open(&zip, &z));
    CHECK(z.count == 2);
    CHECK(sp_dzip_find(&z, "B\\Y.WS") == 1);
    CHECK(sp_dzip_find(&z, "a\\x.ws") == 0);
    CHECK(sp_dzip_find(&z, "nope\\nope") == -1);
    CHECK(sp_dzip_extract(&z, 0, &txt));
    CHECK(txt.n == 5 && memcmp(txt.p, "hello", 5) == 0);
    sp_free(&txt);
    want = make_text(200000);
    CHECK(want.p != NULL);
    CHECK(sp_dzip_extract(&z, 1, &txt));
    CHECK(txt.n == 200000 && memcmp(txt.p, want.p, 200000) == 0);
    CHECK(sp_dzip_extract(&z, 2, &txt) == 0);      // out of range
    CHECK(sp_dzip_extract(&z, -1, &txt) == 0);
done:
    sp_free(&txt);
    sp_free(&want);
    sp_dzip_close(&z);
    sp_free(&zip);
    return why;
}

// A hand-made LZF block with a back-reference: three literals, then one reference with
// len 7 + extension byte (10 bytes copied) at distance 1, i.e. a run of 'c'. The entry's
// first four bytes are its block-offset table (one block, so the data starts at +4).
static const char* test_lzf_backrefs(void)
{
    const char* why = NULL;
    enum { HEAD = 32, DATA = 43, TOTAL = 83 };     // header + 11 data bytes + 40 table bytes
    unsigned char zip[TOTAL];
    const unsigned char payload[7] = { 0x02, 'a', 'b', 'c', 0xE0, 0x01, 0x00 };
    sp_dzip_entry e;
    sp_dzip z;
    sp_buf raw = { zip, TOTAL }, txt = { NULL, 0 };
    size_t p;

    memset(zip, 0, sizeof zip);
    memset(&e, 0, sizeof e);
    wr32(zip + HEAD, 4);                            // block 0 starts after the 4-byte table
    memcpy(zip + HEAD + 4, payload, sizeof payload);
    strcpy_s(e.name, sizeof e.name, "z\\lzf");
    e.time = 0;
    e.usize = 13;
    e.offset = HEAD;
    e.csize = 4 + sizeof payload;
    memset(&z, 0, sizeof z);
    wr32(zip + 0, DZIP_MAGIC);
    wr32(zip + 4, 2);
    wr32(zip + 8, 1);
    wr64(zip + 16, DATA);
    wr64(zip + 24, sp_dzip_hash(&e, 1));
    p = DATA;
    zip[p++] = (unsigned char)(strlen(e.name) + 1);   // u16 length incl. the trailing NUL
    zip[p++] = 0;
    memcpy(zip + p, e.name, strlen(e.name));
    p += strlen(e.name);
    zip[p++] = 0;
    wr64(zip + p, (unsigned long long)e.time);
    wr64(zip + p + 8, (unsigned long long)e.usize);
    wr64(zip + p + 16, (unsigned long long)e.offset);
    wr64(zip + p + 24, (unsigned long long)e.csize);
    CHECK(sp_dzip_open(&raw, &z));
    CHECK(sp_dzip_extract(&z, 0, &txt));
    CHECK(txt.n == 13 && txt.p[0] == 'a' && txt.p[1] == 'b');
    CHECK(memcmp(txt.p + 2, "ccccccccccc", 11) == 0);
done:
    sp_free(&txt);
    sp_dzip_close(&z);
    return why;
}

// Replacing one entry and adding two: the table keeps the other entries and their
// compressed bytes, the replaced and added entries get the given bytes and filetime,
// and an empty entry round-trips.
static const char* test_dzip_replace(void)
{
    const char* why = NULL;
    sp_buf zip = { NULL, 0 }, out = { NULL, 0 }, txt = { NULL, 0 }, want = { NULL, 0 };
    sp_buf bye = { NULL, 0 }, extra = { NULL, 0 }, empty = { NULL, 0 };
    const char* names[3] = { "a\\x.ws", "c\\z.ws", "d\\empty.ws" };
    sp_buf datas[3];
    sp_dzip z, z2;
    long long b_off = 0, b_size = 0, b_time = 0;
    int ib;

    memset(&z, 0, sizeof z);
    memset(&z2, 0, sizeof z2);
    CHECK(make_dzip2(&zip));
    CHECK(sp_dzip_open(&zip, &z));
    ib = sp_dzip_find(&z, "b\\y.ws");
    CHECK(ib == 1);
    b_off = z.e[ib].offset;
    b_size = z.e[ib].csize;
    b_time = z.e[ib].time;
    bye = make_text(3);
    CHECK(bye.p != NULL);
    memcpy(bye.p, "bye", 3);
    extra = make_text(10);
    want = make_text(200000);
    CHECK(extra.p != NULL && want.p != NULL);
    datas[0] = bye;
    datas[1] = extra;
    datas[2] = empty;
    CHECK(sp_dzip_write(&z, 3, names, datas, 424242, &out));
    CHECK(sp_dzip_open(&out, &z2));
    CHECK(z2.count == 4);
    CHECK(sp_dzip_find(&z2, "a\\x.ws") == 0);
    CHECK(sp_dzip_find(&z2, "b\\y.ws") == 1);
    CHECK(sp_dzip_find(&z2, "c\\z.ws") == 2);
    CHECK(sp_dzip_find(&z2, "d\\empty.ws") == 3);
    CHECK(sp_dzip_extract(&z2, 0, &txt) && txt.n == 3 && memcmp(txt.p, "bye", 3) == 0);
    CHECK(z2.e[0].time == 424242 && z2.e[2].time == 424242);
    sp_free(&txt);
    CHECK(sp_dzip_extract(&z2, 1, &txt) && txt.n == 200000 && memcmp(txt.p, want.p, 200000) == 0);
    CHECK(z2.e[1].offset == b_off && z2.e[1].csize == b_size && z2.e[1].time == b_time);
    sp_free(&txt);
    CHECK(sp_dzip_extract(&z2, 2, &txt) && txt.n == 10 && memcmp(txt.p, extra.p, 10) == 0);
    sp_free(&txt);
    CHECK(sp_dzip_extract(&z2, 3, &txt) && txt.n == 0);
done:
    sp_free(&txt);
    sp_free(&want);
    sp_free(&extra);
    sp_free(&bye);
    sp_dzip_close(&z2);
    sp_dzip_close(&z);
    sp_free(&out);
    sp_free(&zip);
    return why;
}

// sp_write_file_atomic + sp_read_file round-trip through %TEMP%; a missing file fails and
// leaves the buffer empty.
static const char* test_common_io(void)
{
    const char* why = NULL;
    wchar_t dir[MAX_PATH], path[MAX_PATH], missing[MAX_PATH];
    sp_buf w = { NULL, 0 }, r = { NULL, 0 };

    if (GetTempPathW(MAX_PATH, dir) == 0) return "GetTempPathW failed";
    CHECK(path_join(path, MAX_PATH, dir, L"patchtest-roundtrip.bin"));
    CHECK(path_join(missing, MAX_PATH, dir, L"patchtest-does-not-exist.bin"));
    DeleteFileW(missing);
    w = make_text(100000);
    CHECK(w.p != NULL);
    CHECK(sp_write_file_atomic(path, &w));
    CHECK(sp_read_file(path, &r));
    CHECK(r.n == w.n && memcmp(r.p, w.p, w.n) == 0);
    DeleteFileW(path);
    sp_free(&r);
    CHECK(!sp_read_file(missing, &r));
    CHECK(r.p == NULL && r.n == 0);
done:
    DeleteFileW(path);
    sp_free(&r);
    sp_free(&w);
    return why;
}

// Every CookedPC\*.dzip of the install opens: magic, version 2 and the entry-table hash.
// Only the header and the table are read (pack0.dzip is ~10 GB).
static const char* test_dzip_real_hash(void)
{
    const char* why = NULL;
    wchar_t pattern[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    int archives = 0, entries = 0;

    if (!path_join(dir, MAX_PATH, g_game, L"CookedPC")) return "path too long";
    if (!path_join(pattern, MAX_PATH, g_game, L"CookedPC\\*.dzip")) return "path too long";
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return "no CookedPC\\*.dzip found";
    do {
        sp_buf raw = { NULL, 0 };
        sp_dzip z;
        long long size = 0;
        memset(&z, 0, sizeof z);
        if (!path_join(path, MAX_PATH, dir, fd.cFileName))
            why = fail_archive(fd.cFileName, "path too long");
        else if (!read_dzip_table(path, &raw, &size))
            why = fail_archive(fd.cFileName, "header/table read failed");
        else if (!sp_dzip_open(&raw, &z))
            why = fail_archive(fd.cFileName, "sp_dzip_open failed (magic, version or table hash)");
        else {
            for (int i = 0; i < z.count; i++) {
                if (z.e[i].offset < 0 || z.e[i].csize < 0 ||
                    z.e[i].offset + z.e[i].csize > size) {
                    why = fail_archive(fd.cFileName, "entry outside the file");
                    break;
                }
                entries++;
            }
            archives++;
        }
        sp_dzip_close(&z);
        sp_free(&raw);
        if (why) break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (!why) printf("  %d archives, %d entries\n", archives, entries);
    return why;
}

// Extracts every entry of the small archives (< 4 MB) for real, so the LZF decoder meets
// real compressed data with back-references, not just the literal runs this file writes.
static const char* test_dzip_real_extract(void)
{
    const char* why = NULL;
    wchar_t pattern[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    int archives = 0, entries = 0;

    if (!path_join(dir, MAX_PATH, g_game, L"CookedPC")) return "path too long";
    if (!path_join(pattern, MAX_PATH, g_game, L"CookedPC\\*.dzip")) return "path too long";
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return "no CookedPC\\*.dzip found";
    do {
        long long size = ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        sp_buf raw = { NULL, 0 }, txt = { NULL, 0 };
        sp_dzip z;
        if (size <= 0 || size > 4LL * 1024 * 1024) continue;
        memset(&z, 0, sizeof z);
        if (!path_join(path, MAX_PATH, dir, fd.cFileName))
            why = fail_archive(fd.cFileName, "path too long");
        else if (!sp_read_file(path, &raw))
            why = fail_archive(fd.cFileName, "read failed");
        else if (!sp_dzip_open(&raw, &z))
            why = fail_archive(fd.cFileName, "open failed");
        else {
            for (int i = 0; i < z.count; i++) {
                if (!sp_dzip_extract(&z, i, &txt)) {
                    _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "%ls: extract %s failed",
                                fd.cFileName, z.e[i].name);
                    why = g_why;
                    break;
                }
                if ((long long)txt.n != z.e[i].usize) {
                    _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                                "%ls: %s extracted %llu of %lld bytes", fd.cFileName,
                                z.e[i].name, (unsigned long long)txt.n, z.e[i].usize);
                    why = g_why;
                    break;
                }
                entries++;
                sp_free(&txt);
            }
            archives++;
        }
        sp_free(&txt);
        sp_dzip_close(&z);
        sp_free(&raw);
        if (why) break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (!why) printf("  %d archives, %d entries extracted\n", archives, entries);
    return why;
}

// ---- harness ------------------------------------------------------------

typedef const char* (*test_fn)(void);

typedef struct {
    const char* name;
    test_fn     fn;
    int         needs_game;
} test_case;

static const test_case k_tests[] = {
    { "dzip_roundtrip",    test_dzip_roundtrip,    0 },
    { "dzip_replace",      test_dzip_replace,      0 },
    { "lzf_backrefs",      test_lzf_backrefs,      0 },
    { "common_io",         test_common_io,         0 },
    { "dzip_real_hash",    test_dzip_real_hash,    1 },
    { "dzip_real_extract", test_dzip_real_extract, 1 },
};

static int run_tests(void)
{
    for (size_t i = 0; i < sizeof k_tests / sizeof k_tests[0]; i++) {
        const test_case* t = &k_tests[i];
        const char* why;
        if (t->needs_game && !g_game) {
            printf("SKIP %s (needs --game)\n", t->name);
            continue;
        }
        why = t->fn();
        if (why) {
            printf("FAIL %s: %s\n", t->name, why);
            g_failures++;
        } else {
            printf("PASS %s\n", t->name);
        }
    }
    printf("%d failure%s\n", g_failures, g_failures == 1 ? "" : "s");
    return g_failures;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: patchtest test [--game \"<game>\"]\n"
            "       patchtest build <game> <out>   (Task 6)\n");
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 2) return usage();
    if (strcmp(argv[1], "test") == 0) {
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
                if (MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, g_gamePath, MAX_PATH) <= 0) {
                    fprintf(stderr, "bad --game path\n");
                    return 2;
                }
                g_game = g_gamePath;
                printf("game: %ls\n", g_game);
            } else {
                fprintf(stderr, "unknown argument: %s\n", argv[i]);
                return usage();
            }
        }
        return run_tests();
    }
    if (strcmp(argv[1], "build") == 0) {
        fprintf(stderr, "build: not implemented yet (Task 6)\n");
        return 2;
    }
    return usage();
}
