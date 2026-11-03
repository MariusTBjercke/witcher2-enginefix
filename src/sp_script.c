// WitcherScript edit engine; see sp_script.h. Port of tools/script-patches/ScriptPatches.ps1
// at commit 26237ca - Add-Edit (line 1285), ConvertTo-Tabs (1218), Get-CollectBody (1167) and
// New-ReadIconFunction (1273); proven byte for byte by the golden test until 0.6.0 removed the
// .ps1 (last pass: docs/fixes/dll-script-patches.md).
#include "sp_script.h"
#include "sp_script_ids.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

const char SP_MARKER[] = "//===witcher2-enginefix===";

// ---- errors -------------------------------------------------------------

static char g_err[600];

static int set_err(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf_s(g_err, sizeof g_err, _TRUNCATE, fmt, ap);
    va_end(ap);
    return 0;
}

const char* sp_script_error(void)
{
    return g_err;
}

static int fail_why(char* why, size_t whylen)
{
    if (why && whylen) {
        size_t n = strlen(g_err);

        if (n > whylen - 1) n = whylen - 1;
        memcpy(why, g_err, n);
        why[n] = 0;
    }
    return 0;
}

// ---- growable byte buffer ----------------------------------------------

typedef struct { char* p; size_t n, cap; } sb;

static void sb_free(sb* b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

static int sb_reserve(sb* b, size_t extra)
{
    size_t need, cap;
    char* p;

    if (extra > (size_t)-1 - b->n - 1) return 0;
    need = b->n + extra + 1;                 // room for the trailing NUL
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 256;
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

static int sb_put(sb* b, const void* data, size_t n)
{
    if (!sb_reserve(b, n)) return 0;
    if (n) memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}

static int sb_puts(sb* b, const char* s)
{
    return sb_put(b, s, strlen(s));
}

static int sb_putc(sb* b, char c)
{
    return sb_put(b, &c, 1);
}

static void sb_reset(sb* b)
{
    b->n = 0;
    if (b->p) b->p[0] = 0;
}

static int sb_copy(sb* out, const sp_buf* in)
{
    sb_reset(out);
    return sb_put(out, in->p, in->n);
}

// ---- byte helpers -------------------------------------------------------

static int is_ws(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static const char* find_bytes(const char* hay, size_t hn, const char* needle, size_t nn)
{
    size_t i;

    if (nn == 0) return hay;
    if (!hay || hn < nn) return NULL;
    for (i = 0; i + nn <= hn; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nn) == 0) return hay + i;
    }
    return NULL;
}

static int contains_bytes(const char* hay, size_t hn, const char* needle)
{
    return find_bytes(hay, hn, needle, strlen(needle)) != NULL;
}

// Last '\n' at an index < before, or (size_t)-1 (String.LastIndexOf semantics).
static size_t last_nl_before(const char* t, size_t before)
{
    while (before > 0) {
        before--;
        if (t[before] == '\n') return before;
    }
    return (size_t)-1;
}

// First '\n' at an index >= from, or (size_t)-1 (String.IndexOf semantics).
static size_t first_nl_from(const char* t, size_t n, size_t from)
{
    size_t i;

    for (i = from; i < n; i++) {
        if (t[i] == '\n') return i;
    }
    return (size_t)-1;
}

static size_t line_start_of(const char* t, size_t idx)
{
    size_t p;

    if (idx == 0) return 0;
    p = last_nl_before(t, idx);
    return p == (size_t)-1 ? 0 : p + 1;
}

static size_t leading_ws(const char* t, size_t n)
{
    size_t i = 0;

    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    return i;
}

static int all_ws(const char* t, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (!is_ws((unsigned char)t[i])) return 0;
    }
    return 1;
}

static void detect_eol(const char* t, size_t n, const char** eol, size_t* eollen)
{
    size_t i;

    for (i = 0; i + 1 < n; i++) {
        if (t[i] == '\r' && t[i + 1] == '\n') {
            *eol = "\r\n";
            *eollen = 2;
            return;
        }
    }
    *eol = "\n";
    *eollen = 1;
}

static int name_eq(const char* a, const char* b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static void anchor_short(const char* a, char* out, size_t cap)
{
    if (strlen(a) > 100) {
        _snprintf_s(out, cap, _TRUNCATE, "%.100s...", a);
    } else {
        _snprintf_s(out, cap, _TRUNCATE, "%s", a);
    }
}

// ---- RCDATA fragments ---------------------------------------------------

// The module holding scriptpatches.rc: __ImageBase is dinput8.dll in the game and
// sptest.exe in the harness (GetModuleHandleW(NULL) would be the game process's exe).
EXTERN_C IMAGE_DOS_HEADER __ImageBase;

typedef struct { char* name; char* data; size_t len; } frag;
static frag* g_frags;
static int   g_nfrags;
static int   g_capfrags;

// The RCDATA resources: one row per file in src/scriptpatches/. sp_fragment() is the only
// way the rest of the patch code reads them (edits.txt included).
static const struct { const char* name; int id; } k_resources[] = {
    { "edits.txt",           SP_RC_EDITS },
    { "book_helpers.ws",     SP_RC_BOOK_HELPERS },
    { "book_texts.ws",       SP_RC_BOOK_TEXTS },
    { "continue_button.ws",  SP_RC_CONTINUE_BUTTON },
    { "continue_input.ws",   SP_RC_CONTINUE_INPUT },
    { "quit_button.ws",      SP_RC_QUIT_BUTTON },
    { "quit_input.ws",       SP_RC_QUIT_INPUT },
    { "quit_members.ws",     SP_RC_QUIT_MEMBERS },
    { "quit_box.ws",         SP_RC_QUIT_BOX },
    { "inventory_refill.ws", SP_RC_INVENTORY_REFILL },
    { "window_graphics.ws",  SP_RC_WINDOW_GRAPHICS },
    { "window_input.ws",     SP_RC_WINDOW_INPUT },
    { "window_texts.ws",     SP_RC_WINDOW_TEXTS },
    { "medallion_use.ws",    SP_RC_MEDALLION_USE }
};

static int resource_id(const char* name)
{
    size_t i;

    for (i = 0; i < sizeof k_resources / sizeof k_resources[0]; i++) {
        if (strcmp(k_resources[i].name, name) == 0) return k_resources[i].id;
    }
    return 0;
}

static int load_fragment(const char* name, char** out, size_t* outlen)
{
    HINSTANCE module = (HINSTANCE)&__ImageBase;
    int id = resource_id(name);
    HRSRC res;
    HGLOBAL glob;
    const void* data;
    DWORD size;
    char* copy;

    if (!id) return 0;
    res = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!res) return 0;
    size = SizeofResource(module, res);
    glob = LoadResource(module, res);
    if (!glob) return 0;
    data = LockResource(glob);
    if (!data && size) return 0;
    copy = (char*)malloc((size_t)size + 1);
    if (!copy) return 0;
    if (size) memcpy(copy, data, (size_t)size);
    copy[size] = 0;
    *out = copy;
    *outlen = (size_t)size;
    return 1;
}

const char* sp_fragment(const char* name)
{
    int i;

    if (!name) return NULL;
    for (i = 0; i < g_nfrags; i++) {
        if (strcmp(g_frags[i].name, name) == 0) return g_frags[i].data;
    }
    if (g_nfrags == g_capfrags) {
        int cap = g_capfrags ? g_capfrags * 2 : 8;
        frag* grown = (frag*)realloc(g_frags, (size_t)cap * sizeof *g_frags);

        if (!grown) {
            set_err("out of memory caching '%s'", name);
            return NULL;
        }
        g_frags = grown;
        g_capfrags = cap;
    }
    {
        char* data = NULL;
        size_t len = 0;
        char* copy;
        size_t namelen;

        if (!load_fragment(name, &data, &len)) {
            set_err("fragment '%s' not found (scriptpatches.rc?)", name);
            return NULL;
        }
        namelen = strlen(name);
        copy = (char*)malloc(namelen + 1);
        if (!copy) {
            free(data);
            set_err("out of memory caching '%s'", name);
            return NULL;
        }
        memcpy(copy, name, namelen + 1);
        g_frags[g_nfrags].name = copy;
        g_frags[g_nfrags].data = data;
        g_frags[g_nfrags].len = len;
        g_nfrags++;
    }
    return g_frags[g_nfrags - 1].data;
}

int sp_script_marked(const sp_buf* text)
{
    if (!text || !text->p || !text->n) return 0;
    return contains_bytes((const char*)text->p, text->n, SP_MARKER);
}

// ---- the edit table (RCDATA edits.txt) ---------------------------------

static sp_edit* g_edits;
static int      g_nedits;
static int      g_edits_state;          // 0 = not tried, 1 = loaded, -1 = failed
static char*    g_edits_text;           // owned copy of edits.txt, split in place

static int parse_where(const char* s, int* out)
{
    if (strcmp(s, "Before") == 0) { *out = SP_BEFORE; return 1; }
    if (strcmp(s, "After") == 0) { *out = SP_AFTER; return 1; }
    if (strcmp(s, "Body") == 0) { *out = SP_BODY; return 1; }
    if (strcmp(s, "Replace") == 0) { *out = SP_REPLACE; return 1; }
    if (strcmp(s, "End") == 0) { *out = SP_END; return 1; }
    return 0;
}

static int parse_row(char* line, int row, sp_edit* e)
{
    char* at = line;
    char* fields[7];
    int nfields = 1;

    fields[0] = line;
    for (; *at; at++) {
        if (*at == '\t') {
            *at = 0;
            if (nfields == 7) return set_err("edits.txt row %d: more than 6 columns", row);
            fields[nfields++] = at + 1;
        }
    }
    if (nfields != 6) {
        return set_err("edits.txt row %d: %d columns, expected 6", row, nfields);
    }
    e->feature = fields[0];
    e->entry = fields[1];
    if (!parse_where(fields[2], &e->where)) {
        return set_err("edits.txt row %d: unknown position '%s'", row, fields[2]);
    }
    {
        char* endp = NULL;
        long n = strtol(fields[3], &endp, 10);

        if (!endp || *endp || n < 0 || n > 1000) {
            return set_err("edits.txt row %d: bad count '%s'", row, fields[3]);
        }
        e->count = (int)n;
    }
    e->anchor = fields[4];
    e->code = fields[5];
    if (!e->feature[0] || !e->entry[0] || !e->code[0]) {
        return set_err("edits.txt row %d: empty feature, entry or code", row);
    }
    if (e->where == SP_END) {
        if (e->count != 0 || e->anchor[0]) {
            return set_err("edits.txt row %d: End needs count 0 and no anchor", row);
        }
    } else if (!e->anchor[0]) {
        return set_err("edits.txt row %d: no anchor", row);
    } else if (is_ws((unsigned char)e->anchor[0]) ||
               is_ws((unsigned char)e->anchor[strlen(e->anchor) - 1])) {
        // A leading or trailing whitespace run would match more than the reference's literal
        // (escaped) whitespace does; none of the rows needs one.
        return set_err("edits.txt row %d: the anchor must not start or end with whitespace", row);
    } else if (e->where == SP_REPLACE) {
        if (e->count < 1) return set_err("edits.txt row %d: Replace needs a count", row);
    } else if (e->count != 1) {
        return set_err("edits.txt row %d: %s needs count 1", row, fields[2]);
    }
    if (e->code[0] == '@' && !sp_fragment(e->code + 1)) {
        return set_err("edits.txt row %d: fragment '%s' not found", row, e->code + 1);
    }
    return 1;
}

static int parse_edits(void)
{
    const char* src;
    size_t len, pos, cap, i;
    char* text;
    int rows = 0, lineno = 0;

    src = sp_fragment("edits.txt");
    if (!src) return 0;
    len = strlen(src);
    text = (char*)malloc(len + 1);
    if (!text) return set_err("out of memory reading edits.txt");
    memcpy(text, src, len + 1);
    g_edits_text = text;

    cap = 1;
    for (i = 0; i < len; i++) {
        if (text[i] == '\n') cap++;
    }
    g_edits = (sp_edit*)calloc(cap, sizeof *g_edits);
    if (!g_edits) return set_err("out of memory reading edits.txt");

    pos = 0;
    for (;;) {
        char* line = text + pos;
        size_t stop = pos;

        while (stop < len && text[stop] != '\n') stop++;
        text[stop] = 0;                             // terminate the line in place
        if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = 0;
        lineno++;
        if (line[0] && line[0] != '#') {
            if (!parse_row(line, lineno, &g_edits[rows])) return 0;
            rows++;
        }
        if (stop >= len) break;
        pos = stop + 1;
    }
    g_nedits = rows;
    g_edits_state = 1;
    return 1;
}

int sp_edits_load(const sp_edit** out, int* n)
{
    if (!out || !n) return set_err("sp_edits_load: null argument");
    if (g_edits_state == 0 && !parse_edits()) g_edits_state = -1;
    if (g_edits_state < 0) return 0;
    *out = g_edits;
    *n = g_nedits;
    return 1;
}

// ---- ConvertTo-Tabs ----------------------------------------------------

// CRLF -> LF, then every leading group of four spaces becomes one tab (the .ps1's
// "(?m)^((?:    )+)" replacement). A CR that is not part of CRLF stays, as it does there.
static int convert_tabs(const char* src, size_t n, sb* out)
{
    size_t i = 0;

    sb_reset(out);
    while (i < n) {
        size_t end = i, ce, spaces, k, t;

        while (end < n && src[end] != '\n') end++;
        ce = end;
        if (end < n && ce > i && src[ce - 1] == '\r') ce--;
        spaces = 0;
        for (k = i; k < ce && src[k] == ' '; k++) spaces++;
        for (t = 0; t < spaces / 4; t++) {
            if (!sb_putc(out, '\t')) return 0;
        }
        for (t = 0; t < spaces % 4; t++) {
            if (!sb_putc(out, ' ')) return 0;
        }
        if (!sb_put(out, src + k, ce - k)) return 0;
        if (end < n && !sb_putc(out, '\n')) return 0;
        i = end == n ? n : end + 1;
    }
    return sb_put(out, "", 0);                      // always a NUL-terminated buffer
}

static int replace_all(sb* b, const char* what, const char* with, size_t withlen)
{
    size_t wl = strlen(what), pos = 0;
    sb out;

    memset(&out, 0, sizeof out);
    if (!b->p) return 1;
    for (;;) {
        const char* hit = find_bytes(b->p + pos, b->n - pos, what, wl);
        size_t off;

        if (!hit) break;
        off = (size_t)(hit - b->p);
        if (!sb_put(&out, b->p + pos, off - pos)) goto oom;
        if (withlen && !sb_put(&out, with, withlen)) goto oom;
        pos = off + wl;
    }
    if (!sb_put(&out, b->p + pos, b->n - pos)) goto oom;
    free(b->p);
    *b = out;
    return 1;
oom:
    sb_free(&out);
    return set_err("out of memory expanding an edit");
}

// ---- anchor matching ---------------------------------------------------

typedef struct { size_t start, end; } sp_match;

// One match at `at`: every whitespace run in the anchor matches any run of whitespace.
// A run is taken greedily, which is correct because a literal never starts with whitespace.
static int match_at(const char* text, size_t n, size_t at, const char* anchor, size_t alen,
                    size_t* end)
{
    size_t ti = at, ai = 0;

    while (ai < alen) {
        if (is_ws((unsigned char)anchor[ai])) {
            while (ai < alen && is_ws((unsigned char)anchor[ai])) ai++;
            while (ti < n && is_ws((unsigned char)text[ti])) ti++;
        } else {
            if (ti >= n || text[ti] != anchor[ai]) return 0;
            ti++;
            ai++;
        }
    }
    *end = ti;
    return 1;
}

// Every non-overlapping match, left to right (Regex.Matches order). 1 = ok.
static int find_all(const char* text, size_t n, const char* anchor, sp_match** out, int* count)
{
    sp_match* list = NULL;
    size_t alen = strlen(anchor), at = 0;
    int used = 0, cap = 0;

    *out = NULL;
    *count = 0;
    if (alen == 0) return 1;
    while (at < n) {
        size_t end = 0;

        if (match_at(text, n, at, anchor, alen, &end)) {
            if (used == cap) {
                int grown = cap ? cap * 2 : 8;
                sp_match* p = (sp_match*)realloc(list, (size_t)grown * sizeof *list);

                if (!p) {
                    free(list);
                    return set_err("out of memory matching an anchor");
                }
                list = p;
                cap = grown;
            }
            list[used].start = at;
            list[used].end = end;
            used++;
            at = end > at ? end : at + 1;
        } else {
            at++;
        }
    }
    *out = list;
    *count = used;
    return 1;
}

// ---- inserted block ----------------------------------------------------

typedef struct { const char* p; size_t n; } sp_line;

// The marker lines around the code, as Add-Edit builds them: "BEGIN <feature>", the code
// with its trailing newlines removed, split at every '\n', and "END <feature>".
static int make_lines(const char* feature, const char* code, size_t codelen, sb* joined,
                      sp_line** out, size_t* outn)
{
    char begin[128], endt[128];
    size_t cl = codelen, lines = 1, i, start;

    if (_snprintf_s(begin, sizeof begin, _TRUNCATE, "%s BEGIN %s", SP_MARKER, feature) < 0 ||
        _snprintf_s(endt, sizeof endt, _TRUNCATE, "%s END %s", SP_MARKER, feature) < 0) {
        return set_err("feature name too long");
    }
    while (cl > 0 && code[cl - 1] == '\n') cl--;
    sb_reset(joined);
    if (!sb_puts(joined, begin) || !sb_putc(joined, '\n') ||
        !sb_put(joined, code, cl) || !sb_putc(joined, '\n') ||
        !sb_puts(joined, endt)) {
        return set_err("out of memory building an edit block");
    }
    for (i = 0; i < joined->n; i++) {
        if (joined->p[i] == '\n') lines++;
    }
    *out = (sp_line*)calloc(lines, sizeof **out);
    if (!*out) return set_err("out of memory building an edit block");
    start = 0;
    for (i = 0; i < lines; i++) {
        size_t stop = start;

        while (stop < joined->n && joined->p[stop] != '\n') stop++;
        (*out)[i].p = joined->p + start;
        (*out)[i].n = stop - start;
        start = stop + 1;
    }
    *outn = lines;
    return 1;
}

// Appends the block lines. `join_only`: no eol after the last line (Replace joins with eol
// instead of ending with one). `skip_empty`: an empty line becomes just its eol, without the
// indentation (Before/After/Body; Replace and End keep the indentation on every line).
static int append_lines(sb* out, const sp_line* lines, size_t nlines, const char* indent,
                        size_t indentlen, const char* eol, size_t eollen, int join_only,
                        int skip_empty)
{
    size_t i;

    for (i = 0; i < nlines; i++) {
        if (skip_empty && lines[i].n == 0) {
            if (!sb_put(out, eol, eollen)) return 0;
            continue;
        }
        if (!sb_put(out, indent, indentlen)) return 0;
        if (!sb_put(out, lines[i].p, lines[i].n)) return 0;
        if (!join_only || i + 1 < nlines) {
            if (!sb_put(out, eol, eollen)) return 0;
        }
    }
    return 1;
}

// Applies one edit to `text` in place (the buffer is replaced), exactly like Add-Edit.
static int edit_apply(sb* text, const sp_edit* e, const sp_line* lines, size_t nlines,
                      char* why, size_t whylen)
{
    const char* p = text->p ? text->p : "";
    size_t n = text->n;

    if (e->where == SP_END) {
        const char* eol;
        size_t eollen;
        sb next;

        memset(&next, 0, sizeof next);
        detect_eol(p, n, &eol, &eollen);
        if (!sb_put(&next, p, n)) goto oom;
        if (!(n > 0 && p[n - 1] == '\n')) {
            if (!sb_put(&next, eol, eollen)) goto oom;
        }
        if (!sb_put(&next, eol, eollen)) goto oom;
        if (!append_lines(&next, lines, nlines, "", 0, eol, eollen, 0, 0)) goto oom;
        free(text->p);
        *text = next;
        return 1;
    }

    if (e->where == SP_REPLACE) {
        sp_match* hits = NULL;
        int nhits = 0, k;
        const char* eol;
        size_t eollen;
        char shown[128];

        if (!find_all(p, n, e->anchor, &hits, &nhits)) return fail_why(why, whylen);
        if (nhits == 0 || nhits != e->count) {
            anchor_short(e->anchor, shown, sizeof shown);
            free(hits);
            set_err("anchor '%s' found %d times in %s, expected %d", shown, nhits, e->entry,
                    e->count);
            return fail_why(why, whylen);
        }
        detect_eol(p, n, &eol, &eollen);
        for (k = nhits - 1; k >= 0; k--) {
            size_t start = hits[k].start, end = hits[k].end;
            size_t lineStart = line_start_of(p, start);
            size_t lineStop = first_nl_from(p, n, end);
            size_t contentEnd, indentlen;
            const char* before = p + lineStart;
            size_t beforeN = start - lineStart;
            const char* after = p + end;
            size_t afterN;
            sb block, next;

            contentEnd = lineStop == (size_t)-1 ? n
                        : (lineStop > 0 && p[lineStop - 1] == '\r') ? lineStop - 1
                        : lineStop;
            if (contentEnd < end) {
                free(hits);
                anchor_short(e->anchor, shown, sizeof shown);
                set_err("anchor '%s' does not cover a whole line in %s", shown, e->entry);
                return fail_why(why, whylen);
            }
            afterN = contentEnd - end;
            indentlen = leading_ws(before, beforeN);
            if (!all_ws(before, beforeN) || !all_ws(after, afterN)) {
                anchor_short(e->anchor, shown, sizeof shown);
                free(hits);
                set_err("anchor '%s' does not cover a whole line in %s", shown, e->entry);
                return fail_why(why, whylen);
            }
            memset(&block, 0, sizeof block);
            memset(&next, 0, sizeof next);
            if (!append_lines(&block, lines, nlines, before, indentlen, eol, eollen, 1, 0)) {
                sb_free(&block);
                free(hits);
                goto oom;
            }
            if (!sb_put(&next, p, lineStart) ||
                !sb_put(&next, block.p, block.n) ||
                !sb_put(&next, p + contentEnd, n - contentEnd)) {
                sb_free(&block);
                sb_free(&next);
                free(hits);
                goto oom;
            }
            sb_free(&block);
            free(text->p);
            *text = next;
            p = text->p;
            n = text->n;
        }
        free(hits);
        return 1;
    }

    {
        sp_match* hits = NULL;
        int nhits = 0;
        size_t lineStart, lastLineEnd, afterAnchor, anchorLineStart, indentFrom, indentlen, at;
        const char* eol;
        size_t eollen;
        char shown[128];
        sb next;

        if (!find_all(p, n, e->anchor, &hits, &nhits)) return fail_why(why, whylen);
        if (nhits != 1) {
            anchor_short(e->anchor, shown, sizeof shown);
            free(hits);
            set_err("anchor '%s' found %d times in %s", shown, nhits, e->entry);
            return fail_why(why, whylen);
        }
        lineStart = line_start_of(p, hits[0].start);
        lastLineEnd = first_nl_from(p, n, hits[0].end > 0 ? hits[0].end - 1 : 0);
        if (lastLineEnd == (size_t)-1) {
            anchor_short(e->anchor, shown, sizeof shown);
            free(hits);
            set_err("anchor '%s' is on the last line of %s", shown, e->entry);
            return fail_why(why, whylen);
        }
        eol = (lastLineEnd > 0 && p[lastLineEnd - 1] == '\r') ? "\r\n" : "\n";
        eollen = eol[0] == '\r' ? 2 : 1;
        afterAnchor = lastLineEnd + 1;
        anchorLineStart = line_start_of(p, lastLineEnd);
        switch (e->where) {
        case SP_BEFORE:
            at = lineStart;
            indentFrom = lineStart;
            break;
        case SP_AFTER:
            at = afterAnchor;
            indentFrom = anchorLineStart;
            break;
        case SP_BODY:
            at = afterAnchor;
            indentFrom = afterAnchor;
            break;
        default:
            free(hits);
            set_err("unknown edit position %d", e->where);
            return fail_why(why, whylen);
        }
        indentlen = leading_ws(p + indentFrom, n - indentFrom);
        memset(&next, 0, sizeof next);
        if (!sb_put(&next, p, at) ||
            !append_lines(&next, lines, nlines, p + indentFrom, indentlen, eol, eollen, 0, 1) ||
            !sb_put(&next, p + at, n - at)) {
            sb_free(&next);
            free(hits);
            goto oom;
        }
        free(hits);
        free(text->p);
        *text = next;
        return 1;
    }

oom:
    set_err("out of memory applying an edit");
    return fail_why(why, whylen);
}

// ---- code expansion ----------------------------------------------------

static int valid_book_name(const char* s)
{
    if (!*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return 0;
    }
    return 1;
}

// W2EF_HasReadIcon, built like New-ReadIconFunction: the reference indents it with four
// spaces and then runs ConvertTo-Tabs over it, which is the same as tabs here.
static int new_read_icons(const sp_ctx* ctx, sb* out)
{
    int i;

    sb_reset(out);
    if (!sb_puts(out,
                 "// Books with an icon in globals\\gui\\icons\\items\\w2ef_read\\ (written by the patcher).\n"
                 "function W2EF_HasReadIcon( itemName : name ) : bool\n"
                 "{\n"
                 "\tvar book : string;\n"
                 "\n"
                 "\tbook = NameToString( itemName );\n")) {
        return 0;
    }
    for (i = 0; i < ctx->nbooks; i++) {
        const char* book = ctx->books[i];

        if (!book || !valid_book_name(book)) {
            return set_err("read-icon book name '%s' is not plain ASCII", book ? book : "");
        }
        if (!sb_puts(out, "\tif ( book == \"") || !sb_puts(out, book) ||
            !sb_puts(out, "\" ) { return true; }\n")) {
            return 0;
        }
    }
    return sb_puts(out, "\treturn false;\n}");
}

// The code of one edit: a fragment or the literal code, tab-converted; for the book helpers
// the three placeholders are filled in (Add-Edit's "BOOK_HELPERS" branch).
static int expand_code(const sp_edit* e, const sp_ctx* ctx, sb* out, char* why, size_t whylen)
{
    if (e->code[0] == '@') {
        const char* frag = sp_fragment(e->code + 1);

        if (!frag) return fail_why(why, whylen);
        if (!convert_tabs(frag, strlen(frag), out)) {
            set_err("out of memory expanding %s", e->code + 1);
            return fail_why(why, whylen);
        }
    } else if (!convert_tabs(e->code, strlen(e->code), out)) {
        set_err("out of memory expanding an edit");
        return fail_why(why, whylen);
    }
    if (contains_bytes(out->p, out->n, "__COLLECT_BODY__") ||
        contains_bytes(out->p, out->n, "__TEXT_FUNCTION__") ||
        contains_bytes(out->p, out->n, "__READ_ICONS__")) {
        sb part;

        if (!ctx || !ctx->collect_body || (ctx->nbooks > 0 && !ctx->books)) {
            set_err("the book helpers need the BookMarker context (collect body and books)");
            return fail_why(why, whylen);
        }
        memset(&part, 0, sizeof part);
        if (!sb_putc(&part, '\t') || !sb_puts(&part, ctx->collect_body)) {
            sb_free(&part);
            set_err("out of memory expanding the collect body");
            return fail_why(why, whylen);
        }
        while (part.n > 0 && is_ws((unsigned char)part.p[part.n - 1])) part.n--;
        part.p[part.n] = 0;
        if (!replace_all(out, "__COLLECT_BODY__", part.p, part.n)) {
            sb_free(&part);
            return fail_why(why, whylen);
        }
        sb_free(&part);

        {
            const char* texts = sp_fragment("book_texts.ws");

            if (!texts) return fail_why(why, whylen);
            memset(&part, 0, sizeof part);
            if (!convert_tabs(texts, strlen(texts), &part)) {
                sb_free(&part);
                set_err("out of memory expanding the text function");
                return fail_why(why, whylen);
            }
            if (!replace_all(out, "__TEXT_FUNCTION__", part.p, part.n)) {
                sb_free(&part);
                return fail_why(why, whylen);
            }
            sb_free(&part);
        }
        {
            sb icons, tabs;

            memset(&icons, 0, sizeof icons);
            memset(&tabs, 0, sizeof tabs);
            if (!new_read_icons(ctx, &icons)) {
                sb_free(&icons);
                return fail_why(why, whylen);
            }
            if (!convert_tabs(icons.p, icons.n, &tabs)) {
                sb_free(&icons);
                sb_free(&tabs);
                set_err("out of memory expanding the read icons");
                return fail_why(why, whylen);
            }
            sb_free(&icons);
            if (!replace_all(out, "__READ_ICONS__", tabs.p, tabs.n)) {
                sb_free(&tabs);
                return fail_why(why, whylen);
            }
            sb_free(&tabs);
        }
    }
    return 1;
}

// ---- one feature against the texts -------------------------------------

static int find_text(sp_text* texts, int ntexts, const char* name)
{
    int i;

    for (i = 0; i < ntexts; i++) {
        if (texts[i].name && name_eq(texts[i].name, name)) return i;
    }
    return -1;
}

static int apply_one(sp_text* t, const sp_edit* e, const char* feature, const sp_ctx* ctx,
                     char* why, size_t whylen)
{
    sb code, joined, work;
    sp_line* lines = NULL;
    size_t nlines = 0;
    int ok = 0;

    memset(&code, 0, sizeof code);
    memset(&joined, 0, sizeof joined);
    memset(&work, 0, sizeof work);
    if (!expand_code(e, ctx, &code, why, whylen)) goto done;
    if (!make_lines(feature, code.p, code.n, &joined, &lines, &nlines)) {
        fail_why(why, whylen);
        goto done;
    }
    if (!sb_copy(&work, &t->text)) {
        set_err("out of memory applying an edit");
        fail_why(why, whylen);
        goto done;
    }
    if (!edit_apply(&work, e, lines, nlines, why, whylen)) goto done;
    free(t->text.p);
    t->text.p = (unsigned char*)work.p;
    t->text.n = work.n;
    work.p = NULL;
    work.n = work.cap = 0;
    ok = 1;
done:
    free(lines);
    sb_free(&joined);
    sb_free(&code);
    sb_free(&work);
    return ok;
}

int sp_script_apply_edits(const sp_edit* edits, int nedits, const char* feature,
                          sp_text* texts, int ntexts, const sp_ctx* ctx,
                          char* why, size_t whylen)
{
    int i, found = 0, ok = 0;
    sp_buf* orig = NULL;
    unsigned char* touched = NULL;

    if (why && whylen) why[0] = 0;
    if (!feature || !feature[0] || !texts || ntexts <= 0 || !edits || nedits <= 0) {
        set_err("sp_script_apply_edits: bad arguments");
        return fail_why(why, whylen);
    }
    orig = (sp_buf*)calloc((size_t)ntexts, sizeof *orig);
    touched = (unsigned char*)calloc((size_t)ntexts, 1);
    if (!orig || !touched) {
        set_err("out of memory applying a feature");
        goto done;
    }

    // Refuse to patch twice, and keep the original texts for an all-or-nothing failure.
    for (i = 0; i < nedits; i++) {
        const sp_edit* e = &edits[i];
        char marker[128];
        int idx;

        if (strcmp(e->feature, feature) != 0) continue;
        found = 1;
        idx = find_text(texts, ntexts, e->entry);
        if (idx < 0) {
            set_err("no text for entry '%s'", e->entry);
            goto done;
        }
        if (touched[idx]) continue;
        if (_snprintf_s(marker, sizeof marker, _TRUNCATE, "%s BEGIN %s", SP_MARKER, feature) < 0) {
            set_err("feature name too long");
            goto done;
        }
        if (contains_bytes((const char*)texts[idx].text.p, texts[idx].text.n, marker)) {
            set_err("'%s' already holds witcher2-enginefix changes (BEGIN %s)", e->entry, feature);
            goto done;
        }
        orig[idx].p = (unsigned char*)malloc(texts[idx].text.n ? texts[idx].text.n : 1);
        if (!orig[idx].p) {
            set_err("out of memory applying a feature");
            goto done;
        }
        if (texts[idx].text.n) memcpy(orig[idx].p, texts[idx].text.p, texts[idx].text.n);
        orig[idx].n = texts[idx].text.n;
        touched[idx] = 1;
    }
    if (!found) {
        set_err("no edits for feature '%s' in edits.txt", feature);
        goto done;
    }

    for (i = 0; i < nedits; i++) {
        const sp_edit* e = &edits[i];
        int idx;

        if (strcmp(e->feature, feature) != 0) continue;
        idx = find_text(texts, ntexts, e->entry);
        if (idx < 0) {                          // cannot happen after the loop above
            set_err("no text for entry '%s'", e->entry);
            goto done;
        }
        if (!apply_one(&texts[idx], e, feature, ctx, why, whylen)) goto done;
    }
    ok = 1;

done:
    if (!ok) {
        for (i = 0; i < ntexts; i++) {
            if (touched && touched[i]) {
                free(texts[i].text.p);
                texts[i].text.p = orig[i].p;
                texts[i].text.n = orig[i].n;
                orig[i].p = NULL;
                orig[i].n = 0;
            }
        }
        fail_why(why, whylen);
    }
    if (orig) {
        for (i = 0; i < ntexts; i++) free(orig[i].p);
        free(orig);
    }
    free(touched);
    return ok;
}

int sp_script_apply(const char* feature, sp_text* texts, int ntexts, const sp_ctx* ctx,
                    char* why, size_t whylen)
{
    const sp_edit* edits = NULL;
    int nedits = 0;

    if (!sp_edits_load(&edits, &nedits)) return fail_why(why, whylen);
    return sp_script_apply_edits(edits, nedits, feature, texts, ntexts, ctx, why, whylen);
}

// ---- Get-CollectBody ---------------------------------------------------

int sp_script_collect_body(const sp_buf* inventory, sp_buf* out, char* why, size_t whylen)
{
    static const char anchor[] = "function AddBookKnowledge( itemName : name ) {";
    sp_match* hits = NULL;
    int nhits = 0;
    size_t start, depth = 0, k;
    sb body;
    const char* text = inventory && inventory->p ? (const char*)inventory->p : "";
    size_t n = inventory ? inventory->n : 0;

    if (out) {
        out->p = NULL;
        out->n = 0;
    }
    if (why && whylen) why[0] = 0;
    if (!out || !inventory || !inventory->p) {
        set_err("sp_script_collect_body: no inventory text");
        return fail_why(why, whylen);
    }
    if (!find_all(text, n, anchor, &hits, &nhits)) return fail_why(why, whylen);
    if (nhits != 1) {
        free(hits);
        set_err("AddBookKnowledge found %d times in game\\gui\\guiinventory.ws", nhits);
        return fail_why(why, whylen);
    }
    start = hits[0].end - 1;                    // the '{' the anchor ends with
    free(hits);
    for (k = start; k < n; k++) {
        if (text[k] == '{') {
            depth++;
        } else if (text[k] == '}') {
            depth--;
            if (depth == 0) break;
        }
    }
    if (depth != 0) {
        set_err("AddBookKnowledge has unbalanced braces");
        return fail_why(why, whylen);
    }
    memset(&body, 0, sizeof body);
    if (!sb_put(&body, text + start, k - start + 1) ||
        !replace_all(&body, "thePlayer.MaxKnowledgeAccumulator(", "Knowledge(", 10) ||
        !replace_all(&body, "thePlayer.AddJournalEntry(", "Journal(", 8)) {
        sb_free(&body);
        return fail_why(why, whylen);
    }
    if (contains_bytes(body.p, body.n, "thePlayer")) {
        sb_free(&body);
        set_err("AddBookKnowledge does more than grant knowledge and journal entries");
        return fail_why(why, whylen);
    }
    if (!contains_bytes(body.p, body.n, "Knowledge(") ||
        !contains_bytes(body.p, body.n, "Journal(")) {
        sb_free(&body);
        set_err("AddBookKnowledge grants nothing recognisable");
        return fail_why(why, whylen);
    }
    {
        size_t i, w = 0;                        // CRLF -> LF, as Get-CollectBody does

        for (i = 0; i < body.n; i++) {
            if (body.p[i] == '\r' && i + 1 < body.n && body.p[i + 1] == '\n') continue;
            body.p[w++] = body.p[i];
        }
        body.n = w;
        body.p[body.n] = 0;
    }
    out->p = (unsigned char*)body.p;
    out->n = body.n;
    return 1;
}
