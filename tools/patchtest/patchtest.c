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
#include "sp_dds.h"
#include "sp_dzip.h"
#include "sp_swf.h"

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

// ---- SWF ----------------------------------------------------------------

static void put_pattern(unsigned char* p, const short* pat, int len)
{
    for (int i = 0; i < len; i++)
        p[i] = pat[i] < 0 ? 0x55 : (unsigned char)pat[i];
}

// A synthetic FWS (8-byte header + filler) whose body holds each pattern `times` times.
static sp_buf make_swf_with(const short* pat0, int times0, const short* pat1, int times1)
{
    size_t l0 = (size_t)SP_SWF_COMPARE[0].len, l1 = (size_t)SP_SWF_COMPARE[1].len;
    size_t need = 8 + 16 + (size_t)times0 * l0 + 8 + (size_t)times1 * l1 + 8;
    sp_buf b;
    size_t at;

    b.n = need;
    b.p = (unsigned char*)malloc(need);
    if (!b.p) { b.n = 0; return b; }
    memset(b.p, 'Z', need);
    b.p[0] = 'F'; b.p[1] = 'W'; b.p[2] = 'S'; b.p[3] = 10;
    wr32(b.p + 4, (unsigned int)need);
    at = 8 + 16;
    for (int i = 0; i < times0; i++) {
        put_pattern(b.p + at, pat0, (int)l0);
        at += l0;
    }
    at += 8;
    for (int i = 0; i < times1; i++) {
        put_pattern(b.p + at, pat1, (int)l1);
        at += l1;
    }
    return b;
}

// Both statements present once: ORIGINAL, apply writes exactly the patched bytes, the
// result is PATCHED.
static const char* test_swf_state(void)
{
    const char* why = NULL;
    const sp_swf_patch* p = SP_SWF_COMPARE;
    sp_buf fws = { NULL, 0 }, out = { NULL, 0 };
    int at[2] = { -1, -1 };
    char whytxt[512];
    size_t at0 = 8 + 16, at1 = at0 + (size_t)p[0].len + 8;

    fws = make_swf_with(p[0].orig, 1, p[1].orig, 1);
    CHECK(fws.p != NULL);
    whytxt[0] = 0;
    CHECK(sp_swf_state(&fws, p, 2, at, whytxt, sizeof whytxt) == SP_SWF_ORIGINAL);
    CHECK(at[0] == (int)at0 && at[1] == (int)at1);
    CHECK(sp_swf_apply(&fws, p, 2, &out));
    CHECK(out.n == fws.n);
    for (int i = 0; i < 2; i++) {
        size_t base = i ? at1 : at0;
        int len = i ? p[1].len : p[0].len;
        for (int k = 0; k < len; k++) {
            if (p[i].patched[k] >= 0)
                CHECK(out.p[base + k] == (unsigned char)p[i].patched[k]);
            else
                CHECK(out.p[base + k] == fws.p[base + k]);
        }
    }
    CHECK(memcmp(out.p, fws.p, at0) == 0);                                  // header + filler
    CHECK(memcmp(out.p + at0 + p[0].len, fws.p + at0 + p[0].len, 8) == 0);  // between
    CHECK(memcmp(out.p + at1 + p[1].len, fws.p + at1 + p[1].len,
                 fws.n - at1 - (size_t)p[1].len) == 0);                     // after
    CHECK(sp_swf_state(&out, p, 2, NULL, whytxt, sizeof whytxt) == SP_SWF_PATCHED);
done:
    sp_free(&out);
    sp_free(&fws);
    return why;
}

// A pattern twice, and one patched next to one original: both are UNKNOWN, and apply
// refuses.
static const char* test_swf_unknown(void)
{
    const char* why = NULL;
    const sp_swf_patch* p = SP_SWF_COMPARE;
    sp_buf fws = { NULL, 0 }, out = { NULL, 0 };
    char whytxt[512];

    fws = make_swf_with(p[0].orig, 2, p[1].orig, 1);
    CHECK(fws.p != NULL);
    whytxt[0] = 0;
    CHECK(sp_swf_state(&fws, p, 2, NULL, whytxt, sizeof whytxt) == SP_SWF_UNKNOWN);
    CHECK(strstr(whytxt, "player side: pattern found 2 times") != NULL);
    CHECK(!sp_swf_apply(&fws, p, 2, &out) && out.p == NULL);
    sp_free(&fws);

    fws = make_swf_with(p[0].patched, 1, p[1].orig, 1);
    CHECK(fws.p != NULL);
    CHECK(sp_swf_state(&fws, p, 2, NULL, whytxt, sizeof whytxt) == SP_SWF_UNKNOWN);
done:
    sp_free(&out);
    sp_free(&fws);
    return why;
}

// CWS -> FWS through the vendored inflate: a final stored deflate block behind the
// two-byte zlib header, exactly the shape the reference reads (b[10..]).
static const char* test_swf_uncompressed(void)
{
    const char* why = NULL;
    enum { BODY = 32, FWS_LEN = 8 + BODY, CWS_LEN = 10 + 5 + BODY + 4 };
    unsigned char fws[FWS_LEN], cws[CWS_LEN], junk[4] = { 'N', 'O', 'P', 'E' };
    sp_buf in = { cws, CWS_LEN }, out = { NULL, 0 }, same = { fws, FWS_LEN };
    sp_buf passthru = { NULL, 0 }, bad = { junk, sizeof junk }, o2 = { NULL, 0 };
    size_t n = 0;

    for (int i = 0; i < BODY; i++) fws[8 + i] = (unsigned char)(0x40 + i);
    fws[0] = 'F'; fws[1] = 'W'; fws[2] = 'S'; fws[3] = 10;
    wr32(fws + 4, FWS_LEN);

    cws[n++] = 'C'; cws[n++] = 'W'; cws[n++] = 'S'; cws[n++] = 10;
    wr32(cws + 4, FWS_LEN);
    n = 8;
    cws[n++] = 0x78; cws[n++] = 0x01;                    // zlib header, skipped
    cws[n++] = 0x01;                                     // final, stored deflate block
    cws[n++] = BODY & 0xFF; cws[n++] = 0;
    cws[n++] = (unsigned char)(~BODY & 0xFF); cws[n++] = 0xFF;
    memcpy(cws + n, fws + 8, BODY);
    n += BODY;
    cws[n++] = 0; cws[n++] = 0; cws[n++] = 0; cws[n++] = 0;   // adler32, never read
    CHECK(n == CWS_LEN);

    CHECK(sp_swf_uncompressed(&in, &out));
    CHECK(out.n == FWS_LEN && memcmp(out.p, fws, FWS_LEN) == 0);
    CHECK(sp_swf_uncompressed(&same, &passthru));
    CHECK(passthru.n == FWS_LEN && memcmp(passthru.p, fws, FWS_LEN) == 0);
    CHECK(!sp_swf_uncompressed(&bad, &o2));
    CHECK(o2.p == NULL && o2.n == 0);
done:
    sp_free(&passthru);
    sp_free(&out);
    return why;
}

// Reads one entry of a DZIP into `out` (owned); NULL on success, else a reason.
static const char* read_dzip_entry(const wchar_t* dzip, const char* name, sp_buf* out)
{
    sp_buf raw = { NULL, 0 };
    sp_dzip z;
    int i;

    memset(out, 0, sizeof *out);
    memset(&z, 0, sizeof z);
    if (!sp_read_file(dzip, &raw)) return "cannot read the archive";
    if (!sp_dzip_open(&raw, &z)) {
        sp_free(&raw);
        return "not a DZIP v2 archive";
    }
    i = sp_dzip_find(&z, name);
    if (i < 0) {
        sp_dzip_close(&z);
        sp_free(&raw);
        return "entry not found";
    }
    if (!sp_dzip_extract(&z, i, out)) {
        sp_dzip_close(&z);
        sp_free(&raw);
        return "extraction failed";
    }
    sp_dzip_close(&z);
    sp_free(&raw);
    return NULL;
}

// The real movies are ORIGINAL: the HD UI originals in the old patcher's backup and the
// copies inside abetterui.dzip. Both sources are optional (a vanilla or migrated install
// has neither), but at least one SWF must have been checked.
static const char* test_swf_real(void)
{
    const char* why = NULL;
    const wchar_t* files[2] = { L"ui_trade.swf", L"ui_storage.swf" };
    const char* entries[2] = { "globals\\gui\\ui_trade.swf", "globals\\gui\\ui_storage.swf" };
    wchar_t cc[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], dzip[MAX_PATH];
    sp_buf raw = { NULL, 0 }, fws = { NULL, 0 };
    char whytxt[512];
    int tested = 0;

    if (!path_join(cc, MAX_PATH, g_game, L"CookedPC") ||
        !path_join(dzip, MAX_PATH, cc, L"abetterui.dzip") ||
        !path_join(dir, MAX_PATH, g_game, L"Witcher2EngineFix_Optional\\ScriptPatches\\backup"))
        return "path too long";

    for (int i = 0; i < 2; i++) {
        if (!path_join(path, MAX_PATH, dir, files[i])) return "path too long";
        if (!sp_read_file(path, &raw)) continue;        // no backup on this install
        if (!sp_swf_uncompressed(&raw, &fws)) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "%ls: not a SWF", path);
            why = g_why;
            goto done;
        }
        whytxt[0] = 0;
        if (sp_swf_state(&fws, SP_SWF_COMPARE, 2, NULL, whytxt, sizeof whytxt) != SP_SWF_ORIGINAL) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "%ls: %s", path, whytxt);
            why = g_why;
            goto done;
        }
        tested++;
        sp_free(&raw);
        sp_free(&fws);
    }
    for (int i = 0; i < 2; i++) {
        const char* err = read_dzip_entry(dzip, entries[i], &raw);
        if (err) {
            if (strcmp(err, "cannot read the archive") == 0) continue;   // no HD UI mod
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "abetterui.dzip %s: %s", entries[i], err);
            why = g_why;
            goto done;
        }
        if (!sp_swf_uncompressed(&raw, &fws)) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "abetterui.dzip %s: not a SWF", entries[i]);
            why = g_why;
            goto done;
        }
        whytxt[0] = 0;
        if (sp_swf_state(&fws, SP_SWF_COMPARE, 2, NULL, whytxt, sizeof whytxt) != SP_SWF_ORIGINAL) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "abetterui.dzip %s: %s", entries[i], whytxt);
            why = g_why;
            goto done;
        }
        tested++;
        sp_free(&raw);
        sp_free(&fws);
    }
    if (tested == 0) return "no ui_trade/ui_storage original to check (no backup, no abetterui.dzip)";
    printf("  %d SWFs checked\n", tested);
done:
    sp_free(&fws);
    sp_free(&raw);
    return why;
}

// ---- DDS icons ----------------------------------------------------------

#define DDS_MAGIC 0x20534444u

// A DXT5 64x64 DDS: 128-byte header, then 16 bytes per 4x4 block. Block 0 (x 0..3) has
// a0 > a1 (8-value alpha, 255,0,218,182,145,109,72,36); block 1 (x 4..7) has a0 < a1
// (6-value alpha, 0,1,0,0,0,0,0,255) - the branch the real alreadyread overlay uses. Both
// use c0 = pure red, c1 = pure blue (four-colour palette), colour indices 0,1,2,3 repeating.
static sp_buf make_dxt5_sample(void)
{
    static const unsigned char block0[16] = {
        255, 0, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA,
        0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4
    };
    static const unsigned char block1[16] = {
        0, 1, 0x88, 0xC6, 0xFA, 0x88, 0xC6, 0xFA,
        0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4
    };
    sp_buf b;
    unsigned char* h;

    b.n = 128 + 256 * 16;
    b.p = (unsigned char*)malloc(b.n);
    if (!b.p) { b.n = 0; return b; }
    memset(b.p, 0, b.n);
    h = b.p;
    wr32(h + 0, DDS_MAGIC);
    wr32(h + 4, 124);                    // dwSize
    wr32(h + 8, 0x81007);                // caps|height|width|pitch|pixelformat|linearsize
    wr32(h + 12, 64);                    // height
    wr32(h + 16, 64);                    // width
    wr32(h + 20, 64 * 16);               // linear size
    wr32(h + 76, 32);                    // pixel format size
    wr32(h + 80, 4);                     // DDPF_FOURCC
    memcpy(h + 84, "DXT5", 4);
    wr32(h + 108, 0x1000);               // caps: texture
    memcpy(h + 128, block0, sizeof block0);
    memcpy(h + 144, block1, sizeof block1);
    return b;
}

// A 32-bit A8R8G8B8 DDS with a hand-written header (the reference's Encode64 layout) and
// the given pixels.
static sp_buf make_dds32(const unsigned* px)
{
    sp_buf b;
    unsigned char* h;

    b.n = 128 + SP_DDS_PIXELS * 4;
    b.p = (unsigned char*)malloc(b.n);
    if (!b.p) { b.n = 0; return b; }
    memset(b.p, 0, b.n);
    h = b.p;
    wr32(h + 0, DDS_MAGIC);
    wr32(h + 4, 124);
    wr32(h + 8, 0x100F);                 // caps|height|width|pitch|pixelformat
    wr32(h + 12, 64);                    // height
    wr32(h + 16, 64);                    // width
    wr32(h + 20, 256);                   // pitch
    wr32(h + 76, 32);                    // pixel format size
    wr32(h + 80, 0x41);                  // DDPF_RGB | DDPF_ALPHAPIXELS
    wr32(h + 88, 32);                    // rgb bit count
    wr32(h + 92, 0x00FF0000u);
    wr32(h + 96, 0x0000FF00u);
    wr32(h + 100, 0x000000FFu);
    wr32(h + 104, 0xFF000000u);
    wr32(h + 108, 0x1000);               // caps: texture
    for (int i = 0; i < SP_DDS_PIXELS; i++) wr32(h + 128 + 4 * i, px[i]);
    return b;
}

// The hand-built DXT5 block decodes to the expected pixels, and non-icons are rejected.
static const char* test_dds_decode_dxt5(void)
{
    static const unsigned expect[16] = {
        0xFFFF0000u, 0x000000FFu, 0xDAAA0055u, 0xB65500AAu,
        0x91FF0000u, 0x6D0000FFu, 0x48AA0055u, 0x245500AAu,
        0xFFFF0000u, 0x000000FFu, 0xDAAA0055u, 0xB65500AAu,
        0x91FF0000u, 0x6D0000FFu, 0x48AA0055u, 0x245500AAu
    };
    static const unsigned expect1[16] = {
        0x00FF0000u, 0x010000FFu, 0x00AA0055u, 0x005500AAu,
        0x00FF0000u, 0x000000FFu, 0x00AA0055u, 0xFF5500AAu,
        0x00FF0000u, 0x010000FFu, 0x00AA0055u, 0x005500AAu,
        0x00FF0000u, 0x000000FFu, 0x00AA0055u, 0xFF5500AAu
    };
    const char* why = NULL;
    sp_buf dds = { NULL, 0 };
    unsigned* px = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *px);

    CHECK(px != NULL);
    dds = make_dxt5_sample();
    CHECK(dds.p != NULL);
    CHECK(sp_dds_decode64(&dds, px));
    for (int i = 0; i < 16; i++) {
        int at = (i / 4) * 64 + i % 4;      // block pixel i is at row i/4, column i%4
        if (px[at] != expect[i]) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "pixel %d (%d) is %08X, expected %08X",
                        i, at, px[at], expect[i]);
            why = g_why;
            goto done;
        }
    }
    for (int i = 0; i < 16; i++) {
        int at = (i / 4) * 64 + 4 + i % 4;  // block 1, a0 < a1
        if (px[at] != expect1[i]) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "block 1 pixel %d (%d) is %08X, expected %08X",
                        i, at, px[at], expect1[i]);
            why = g_why;
            goto done;
        }
    }
    CHECK(sp_dds_decode64(&dds, px));                       // unchanged on a second call
    wr32(dds.p + 12, 32);                                   // 32x32 is not an icon
    CHECK(!sp_dds_decode64(&dds, px));
    wr32(dds.p + 12, 64);
    {
        sp_buf cut = { dds.p, 128 + 255 * 16 };
        CHECK(!sp_dds_decode64(&cut, px));                  // truncated
    }
    wr32(dds.p, 0x20534445u);
    CHECK(!sp_dds_decode64(&dds, px));
done:
    free(px);
    sp_free(&dds);
    return why;
}

// decode64 is exact for a standard 32-bit DDS, and encode64 writes exactly that file back.
static const char* test_dds_roundtrip(void)
{
    const char* why = NULL;
    sp_buf dds32 = { NULL, 0 }, enc = { NULL, 0 };
    unsigned* px = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *px);
    unsigned* back = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *back);
    unsigned seed = 12345;

    CHECK(px != NULL && back != NULL);
    for (int i = 0; i < SP_DDS_PIXELS; i++) {               // deterministic, every byte varies
        seed = seed * 1664525u + 1013904223u;
        px[i] = seed;
    }
    dds32 = make_dds32(px);
    CHECK(dds32.p != NULL);
    CHECK(sp_dds_decode64(&dds32, back));
    CHECK(memcmp(back, px, SP_DDS_PIXELS * sizeof *px) == 0);
    CHECK(sp_dds_encode64(back, &enc));
    CHECK(enc.n == dds32.n && memcmp(enc.p, dds32.p, dds32.n) == 0);
done:
    sp_free(&enc);
    sp_free(&dds32);
    free(back);
    free(px);
    return why;
}

// The alpha-over used for the read-icon overlay, with the reference's rounding.
static const char* test_dds_over(void)
{
    struct { unsigned base, top; double opacity; unsigned expect; } cases[] = {
        { 0xFF000000u, 0x80FF0000u, 0.9, 0xFF730000u },
        { 0xFFFFFFFFu, 0xFFFFFFFFu, 0.0, 0xFFFFFFFFu },
        { 0x00000000u, 0xFF00FF00u, 0.9, 0xE600FF00u },
        { 0xFFFFFFFFu, 0x00FFFFFFu, 0.9, 0xFFFFFFFFu },
        { 0x00000000u, 0xFD336699u, 0.5, 0x7E336699u },
        { 0x80808080u, 0x80402010u, 1.0, 0xC0554035u },
        { 0x00000000u, 0x00000000u, 0.9, 0x00000000u }
    };
    const char* why = NULL;
    unsigned base[SP_DDS_PIXELS], top[SP_DDS_PIXELS];

    for (int i = 0; i < SP_DDS_PIXELS; i++) { base[i] = 0; top[i] = 0; }
    for (int c = 0; c < (int)(sizeof cases / sizeof cases[0]); c++) {
        for (int i = 0; i < SP_DDS_PIXELS; i++) {
            base[i] = cases[c].base;
            top[i] = cases[c].top;
        }
        sp_dds_over(base, top, cases[c].opacity);
        if (base[0] != cases[c].expect) {
            _snprintf_s(g_why, sizeof g_why, _TRUNCATE,
                        "case %d (%08X over %08X at %g) is %08X, expected %08X", c,
                        cases[c].top, cases[c].base, cases[c].opacity, base[0], cases[c].expect);
            why = g_why;
            goto done;
        }
    }
done:
    return why;
}

static unsigned long long fnv1a64(const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    unsigned long long h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// The real read-icon overlay (DXT5, 64x64) decodes to the same pixels as the reference:
// FNV-1a 64 over the 4096 RGBA values was taken from a Python port of the .ps1 Decode64.
static const char* test_dds_real_overlay(void)
{
    const char* why = NULL;
    wchar_t cc[MAX_PATH], dzip[MAX_PATH];
    sp_buf raw = { NULL, 0 };
    unsigned* px = (unsigned*)malloc(SP_DDS_PIXELS * sizeof *px);
    const char* err;

    CHECK(px != NULL);
    if (!path_join(cc, MAX_PATH, g_game, L"CookedPC") ||
        !path_join(dzip, MAX_PATH, cc, L"abetterui.dzip"))
        return "path too long";
    err = read_dzip_entry(dzip, "globals\\gui\\icons\\tooltip\\alreadyread_64x64.dds", &raw);
    if (err) {
        if (strcmp(err, "cannot read the archive") == 0) {
            printf("  (no abetterui.dzip - skipped)\n");
            goto done;
        }
        _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "alreadyread_64x64.dds: %s", err);
        why = g_why;
        goto done;
    }
    CHECK(sp_dds_decode64(&raw, px));
    CHECK(px[0] == 0x00FFFFFFu && px[1] == 0x00FFFFFFu && px[100] == 0x00FFFFFFu);
    CHECK(px[4095] == 0x00FFFFFFu);
    if (fnv1a64(px, SP_DDS_PIXELS * sizeof *px) != 0x760A865CFC2814ECULL) {
        _snprintf_s(g_why, sizeof g_why, _TRUNCATE, "pixel checksum is %016llX",
                    fnv1a64(px, SP_DDS_PIXELS * sizeof *px));
        why = g_why;
        goto done;
    }
done:
    sp_free(&raw);
    free(px);
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
    { "swf_state",         test_swf_state,         0 },
    { "swf_unknown",       test_swf_unknown,       0 },
    { "swf_uncompressed",  test_swf_uncompressed,  0 },
    { "dds_decode_dxt5",   test_dds_decode_dxt5,   0 },
    { "dds_roundtrip",     test_dds_roundtrip,     0 },
    { "dds_over",          test_dds_over,          0 },
    { "dzip_real_hash",    test_dzip_real_hash,    1 },
    { "dzip_real_extract", test_dzip_real_extract, 1 },
    { "swf_real",          test_swf_real,          1 },
    { "dds_real_overlay",  test_dds_real_overlay,  1 },
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
