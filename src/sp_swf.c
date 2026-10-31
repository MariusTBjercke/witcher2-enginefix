// SWF patching; see sp_swf.h. Port of the Swf class, $SwfCompare and Get-SwfState /
// Get-PatchedSwf in ScriptPatches.ps1 (git tag v0.5.1).
#include "sp_swf.h"
#include "third_party/tinf/tinf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWF_MAX (256u * 1024u * 1024u)   // sanity cap for the expanded movie

static unsigned int rd32(const unsigned char* p)
{
    unsigned int v;
    memcpy(&v, p, 4);
    return v;
}

// Every match of `pat` in `b` (wildcards match any byte); returns the number of matches and
// the first offset.
static int swf_find(const sp_buf* b, const short* pat, int len, size_t* first)
{
    int count = 0;

    if (!b || !b->p || !pat || len <= 0 || (size_t)len > b->n) return 0;
    for (size_t i = 0; i + (size_t)len <= b->n; i++) {
        int k = 0;
        while (k < len && (pat[k] < 0 || b->p[i + k] == (unsigned char)pat[k])) k++;
        if (k == len) {
            if (count == 0 && first) *first = i;
            count++;
        }
    }
    return count;
}

int sp_swf_uncompressed(const sp_buf* in, sp_buf* out)
{
    unsigned int len, destLen;

    memset(out, 0, sizeof *out);
    if (!in || !in->p || in->n < 8) return 0;
    if (in->p[0] == 'F' && in->p[1] == 'W' && in->p[2] == 'S') {
        out->p = (unsigned char*)malloc(in->n ? in->n : 1);
        if (!out->p) return 0;
        memcpy(out->p, in->p, in->n);
        out->n = in->n;
        return 1;
    }
    if (!(in->p[0] == 'C' && in->p[1] == 'W' && in->p[2] == 'S')) return 0;
    len = rd32(in->p + 4);
    if (len < 8 || len > SWF_MAX || in->n < 10 || in->n > 0xFFFFFFFFu) return 0;
    out->p = (unsigned char*)malloc(len);
    if (!out->p) return 0;
    out->n = len;
    memcpy(out->p, in->p, 8);
    out->p[0] = 'F';
    destLen = len - 8;
    // The SWF body is a zlib stream; the reference skips its two-byte header and inflates
    // the raw deflate that follows (the adler32 trailer is never read).
    if (tinf_uncompress(out->p + 8, &destLen, in->p + 10, (unsigned int)(in->n - 10)) != TINF_OK ||
        destLen != len - 8) {
        sp_free(out);
        return 0;
    }
    return 1;
}

int sp_swf_state(const sp_buf* fws, const sp_swf_patch* p, int n, int* at, char* why, size_t whylen)
{
    char all[1024];
    int patched = 0, original = 0, unknown = 0;

    if (why && whylen) why[0] = 0;
    if (!fws || !fws->p || n < 0 || (n && !p)) return SP_SWF_UNKNOWN;
    all[0] = 0;
    for (int i = 0; i < n; i++) {
        size_t opos = 0;
        char line[256];
        int no = swf_find(fws, p[i].orig, p[i].len, &opos);
        int nd = swf_find(fws, p[i].patched, p[i].len, NULL);
        if (nd == 1 && no == 0) {
            patched++;
        } else if (no == 1 && nd == 0) {
            original++;
            if (at) at[i] = (int)opos;
        } else {
            unknown++;
        }
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "%s: pattern found %d times, patched form %d times", p[i].name, no, nd);
        if (i) strncat_s(all, sizeof all, "; ", _TRUNCATE);
        strncat_s(all, sizeof all, line, _TRUNCATE);
    }
    if (!unknown && patched == n) return SP_SWF_PATCHED;
    if (!unknown && original == n) return SP_SWF_ORIGINAL;
    if (why && whylen) strncpy_s(why, whylen, all, _TRUNCATE);
    return SP_SWF_UNKNOWN;
}

int sp_swf_apply(const sp_buf* fws, const sp_swf_patch* p, int n, sp_buf* out)
{
    int* at;

    memset(out, 0, sizeof *out);
    if (!fws || !fws->p || !p || n <= 0) return 0;
    at = (int*)malloc((size_t)n * sizeof *at);
    if (!at) return 0;
    if (sp_swf_state(fws, p, n, at, NULL, 0) != SP_SWF_ORIGINAL) {
        free(at);
        return 0;
    }
    out->p = (unsigned char*)malloc(fws->n ? fws->n : 1);
    if (!out->p) {
        free(at);
        return 0;
    }
    memcpy(out->p, fws->p, fws->n);
    out->n = fws->n;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < p[i].len; k++)
            if (p[i].patched[k] >= 0)
                out->p[at[i] + k] = (unsigned char)p[i].patched[k];
    free(at);
    return 1;
}

// $SwfOwnedName (the shared 17-byte tail of both statements) is spelled out in both
// patterns below; -1 is a wildcard. Copied byte for byte from ScriptPatches.ps1.
static const short k_player_orig[33] = {
    0x9D, 0x02, 0x00, 0x1C, 0x00, 0x96, 0x07, 0x00, 0x04, 0x07, 0x09, -1, -1, 0x05, 0x00, 0x4F,
    0x96, 0x07, 0x00, 0x04, 0x03, 0x04, 0x07, 0x09, -1, -1, 0x4E, 0x96, 0x02, 0x00, 0x05, 0x01, 0x4F
};
static const short k_player_patched[33] = {
    0x9D, 0x02, 0x00, 0x1C, 0x00, 0x96, 0x05, 0x00, 0x04, 0x07, 0x04, 0x07, 0x02, 0x17, 0x17, 0x17,
    0x96, 0x07, 0x00, 0x04, 0x03, 0x04, 0x07, 0x09, -1, -1, 0x4E, 0x96, 0x02, 0x00, 0x05, 0x01, 0x4F
};
static const short k_merchant_orig[29] = {
    0x9D, 0x02, 0x00, 0x18, 0x00, 0x96, 0x0C, 0x00, 0x04, 0x0A, 0x09, -1, -1,
    0x04, 0x03, 0x04, 0x0A, 0x09, -1, -1, 0x4E, 0x4E, 0x96, 0x01, 0x00, 0x02, 0x49, 0x12, 0x4F
};
static const short k_merchant_patched[29] = {
    0x9D, 0x02, 0x00, 0x18, 0x00, 0x96, 0x0B, 0x00, 0x04, 0x0A,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17
};

const sp_swf_patch SP_SWF_COMPARE[2] = {
    { "player side",   k_player_orig,   k_player_patched,   33 },
    { "merchant side", k_merchant_orig, k_merchant_patched, 29 },
};
