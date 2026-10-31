// DZIP v2 reader/writer; see sp_dzip.h. Port of the Dzip class in
// ScriptPatches.ps1 (git tag v0.5.1) (ReadTable, Hash, Extract, LzfDecompress,
// EntryData, ReplaceEntries), with the reference's arithmetic kept as it is.
#include "sp_dzip.h"
#include <stdlib.h>
#include <string.h>

#define DZIP_MAGIC   0x50495A44u   // 'D','Z','I','P'
#define DZIP_VERSION 2
#define DZIP_HEAD    32
#define DZIP_BLOCK   0x10000       // entries are compressed in 64 KiB blocks
// Sanity cap for an extracted entry. It cannot be derived from the archive size (compressed
// data is smaller than what it expands to), and the largest real entry is ~44 MB.
#define DZIP_MAX_ENTRY (512ull * 1024 * 1024)

static unsigned int rd16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char* p)
{
    unsigned int v;
    memcpy(&v, p, 4);
    return v;
}

static unsigned long long rd64(const unsigned char* p)
{
    unsigned long long v;
    memcpy(&v, p, 8);
    return v;
}

static void wr32(unsigned char* p, unsigned int v)
{
    memcpy(p, &v, 4);
}

static void wr64(unsigned char* p, unsigned long long v)
{
    memcpy(p, &v, 8);
}

int sp_dzip_open(const sp_buf* data, sp_dzip* out)
{
    const unsigned char* b;
    sp_dzip_entry* e = NULL;
    long long count, table, i;
    unsigned long long hash;
    size_t p;

    memset(out, 0, sizeof *out);
    if (!data || !data->p || data->n < DZIP_HEAD) return 0;
    b = data->p;
    if (rd32(b) != DZIP_MAGIC || rd32(b + 4) != DZIP_VERSION) return 0;
    count = (long long)(int)rd32(b + 8);
    table = (long long)rd64(b + 16);
    hash = rd64(b + 24);
    if (count < 0 || count > 1000000) return 0;
    if (table < DZIP_HEAD || (unsigned long long)table > data->n) return 0;
    if (count) {
        e = (sp_dzip_entry*)calloc((size_t)count, sizeof *e);
        if (!e) return 0;
    }
    p = (size_t)table;
    for (i = 0; i < count; i++) {
        unsigned int n;
        size_t len;
        if (p + 2 > data->n) goto bad;
        n = rd16(b + p);
        p += 2;
        if (n == 0 || n > sizeof e[0].name || p + n > data->n) goto bad;
        len = n;                        // the stored length includes the trailing NUL
        while (len && b[p + len - 1] == 0) len--;
        memcpy(e[i].name, b + p, len);
        e[i].name[len] = 0;
        p += n;
        if (p + 32 > data->n) goto bad;
        e[i].time   = (long long)rd64(b + p);
        e[i].usize  = (long long)rd64(b + p + 8);
        e[i].offset = (long long)rd64(b + p + 16);
        e[i].csize  = (long long)rd64(b + p + 24);
        p += 32;
        if (e[i].usize < 0 || e[i].csize < 0 || e[i].offset < 0) goto bad;
    }
    if (sp_dzip_hash(e, (int)count) != hash) goto bad;
    out->data = *data;          // borrowed: the caller keeps it alive
    out->e = e;
    out->count = (int)count;
    out->table = table;
    return 1;
bad:
    free(e);
    return 0;
}

int sp_dzip_find(const sp_dzip* z, const char* name)
{
    if (!z || !name) return -1;
    for (int i = 0; i < z->count; i++)
        if (_stricmp(z->e[i].name, name) == 0) return i;
    return -1;
}

unsigned long long sp_dzip_hash(const sp_dzip_entry* e, int count)
{
    const unsigned long long P = 0x00000100000001B3ULL;
    unsigned long long h = 0x00000000FFFFFFFFULL;

    for (int i = 0; i < count; i++) {
        size_t len = e[i].name ? strlen(e[i].name) : 0;
        if (len) {
            for (size_t k = 0; k < len; k++) {
                h ^= (unsigned char)e[i].name[k];
                h *= P;
            }
            h ^= (unsigned long long)len;
            h *= P;
        }
        h ^= (unsigned long long)e[i].time;   h *= P;
        h ^= (unsigned long long)e[i].usize;  h *= P;
        h ^= (unsigned long long)e[i].offset; h *= P;
        h ^= (unsigned long long)e[i].csize;  h *= P;
    }
    return h;
}

// One LZF block: literals are runs of c+1 (c < 32) bytes, a back-reference copies
// len+2 bytes from o-(c&31)*256-1-src[i]. Returns the bytes produced. A run that would
// pass the declared entry size is clamped (the reference's Buffer.BlockCopy throws there;
// one real archive, dlc_finishers.dzip, has such an entry), so a stream can only fail by
// coming up short.
static long long lzf_block(const unsigned char* src, long long i, long long end,
                           unsigned char* dst, long long o, long long want)
{
    long long start = o, limit = o + want;

    while (i < end && o < limit) {
        int c = src[i++];
        if (c < 32) {
            long long n;
            c++;
            if (i + c > end) break;
            n = limit - o;
            if (n > c) n = c;
            memcpy(dst + o, src + i, (size_t)n);
            i += c;                     // the whole run is in the stream, keep it aligned
            o += n;
        } else {
            long long len = c >> 5;
            long long r = o - ((long long)(c & 31) << 8) - 1;
            if (len == 7) {
                if (i >= end) break;
                len += src[i++];
            }
            if (i >= end) break;
            r -= src[i++];
            if (r < 0) break;               // corrupt: before this block's output
            for (long long k = 0; k < len + 2 && o < limit; k++)
                dst[o++] = dst[r + k];      // byte-wise: references may overlap (RLE)
        }
    }
    return o - start;
}

int sp_dzip_extract(const sp_dzip* z, int i, sp_buf* out)
{
    const sp_dzip_entry* e;
    const unsigned char* b;
    long long blocks, outPos = 0;
    unsigned char* dst;

    memset(out, 0, sizeof *out);
    if (!z || !z->data.p || i < 0 || i >= z->count) return 0;
    e = &z->e[i];
    if (e->usize == 0) return 1;            // empty entry: valid, nothing to read
    if (e->offset < DZIP_HEAD || e->csize <= 0) return 0;
    if (e->usize < 0 || (unsigned long long)e->usize > DZIP_MAX_ENTRY ||
        (unsigned long long)e->offset > z->data.n ||
        (unsigned long long)e->csize > z->data.n - (unsigned long long)e->offset)
        return 0;
    blocks = (e->usize + DZIP_BLOCK - 1) >> 16;
    if (blocks * 4 > e->csize) return 0;
    dst = (unsigned char*)malloc((size_t)e->usize);
    if (!dst) return 0;
    b = z->data.p;
    for (long long k = 0; k < blocks; k++) {
        long long start = e->offset + (int)rd32(b + e->offset + 4 * k);
        long long end = (k + 1 < blocks) ? e->offset + (int)rd32(b + e->offset + 4 * (k + 1))
                                         : e->offset + e->csize;
        long long left = e->usize - outPos;
        long long want = left < DZIP_BLOCK ? left : DZIP_BLOCK;
        if (start < e->offset || end < start || end > e->offset + e->csize) {
            free(dst);
            return 0;
        }
        outPos += lzf_block(b, start, end, dst, outPos, want);
    }
    if (outPos != e->usize) {
        free(dst);
        return 0;
    }
    out->p = dst;
    out->n = (size_t)e->usize;
    return 1;
}

// ---- writing ------------------------------------------------------------

typedef struct { sp_buf b; size_t cap; } wbuf;

static int wbuf_put(wbuf* w, const void* p, size_t n)
{
    if (w->b.n + n > w->cap) {
        size_t cap = w->cap ? w->cap : 4096;
        unsigned char* q;
        while (cap < w->b.n + n) {
            if (cap > (size_t)-1 / 2) return 0;
            cap *= 2;
        }
        q = (unsigned char*)realloc(w->b.p, cap);
        if (!q) return 0;
        w->b.p = q;
        w->cap = cap;
    }
    if (n) memcpy(w->b.p + w->b.n, p, n);
    w->b.n += n;
    return 1;
}

static int wbuf_u8(wbuf* w, unsigned char v)
{
    return wbuf_put(w, &v, 1);
}

static int wbuf_u64(wbuf* w, unsigned long long v)
{
    unsigned char tmp[8];
    wr64(tmp, v);
    return wbuf_put(w, tmp, 8);
}

// Entry data as an LZF stream of literal runs only (control byte = run length - 1, at
// most 32 bytes): valid LZF, about 3% larger, and what the reference writes.
static int pack_entry(const sp_buf* d, sp_buf* out)
{
    long long blocks = (long long)(((unsigned long long)d->n + DZIP_BLOCK - 1) >> 16);
    long long k, s, e;
    size_t pos;
    size_t* off = NULL;
    wbuf w;

    memset(out, 0, sizeof *out);
    memset(&w, 0, sizeof w);
    if (blocks) {
        off = (size_t*)malloc((size_t)blocks * sizeof *off);
        if (!off) return 0;
    }
    pos = (size_t)blocks * 4;
    for (k = 0; k < blocks; k++) {
        off[k] = pos;
        s = k * DZIP_BLOCK;
        e = (long long)d->n < s + DZIP_BLOCK ? (long long)d->n : s + DZIP_BLOCK;
        pos += (size_t)(e - s) + (size_t)((e - s + 31) / 32);   // literals + control bytes
    }
    for (k = 0; k < blocks; k++) {
        unsigned char tmp[4];
        wr32(tmp, (unsigned int)off[k]);
        if (!wbuf_put(&w, tmp, 4)) goto bad;
    }
    for (k = 0; k < blocks; k++) {
        s = k * DZIP_BLOCK;
        e = (long long)d->n < s + DZIP_BLOCK ? (long long)d->n : s + DZIP_BLOCK;
        for (long long p = s; p < e; p += 32) {
            size_t n = (size_t)(e - p < 32 ? e - p : 32);
            if (!wbuf_u8(&w, (unsigned char)(n - 1))) goto bad;
            if (!wbuf_put(&w, d->p + p, n)) goto bad;
        }
    }
    free(off);
    *out = w.b;
    return 1;
bad:
    free(off);
    sp_free(&w.b);
    return 0;
}

int sp_dzip_write(const sp_dzip* z, int n, const char* const* names, const sp_buf* datas,
                  long long filetime, sp_buf* out)
{
    sp_dzip_entry* e;
    int used, cap, i, k;
    long long table;
    wbuf w;
    int ok = 0;

    memset(out, 0, sizeof *out);
    memset(&w, 0, sizeof w);
    if (!z || !z->data.p || z->table < DZIP_HEAD ||
        (unsigned long long)z->table > z->data.n || n < 0 || (n && (!names || !datas)))
        return 0;
    cap = z->count + n;
    e = cap ? (sp_dzip_entry*)malloc((size_t)cap * sizeof *e) : NULL;
    if (cap && !e) return 0;
    if (z->count) memcpy(e, z->e, (size_t)z->count * sizeof *e);
    used = z->count;
    if (!wbuf_put(&w, z->data.p, (size_t)z->table)) goto done;
    for (i = 0; i < n; i++) {
        sp_buf packed;
        int idx = -1;
        if (!names[i] || !*names[i] || strlen(names[i]) >= sizeof e[0].name) goto done;
        for (k = 0; k < used; k++)
            if (_stricmp(e[k].name, names[i]) == 0) { idx = k; break; }
        if (idx < 0) {                  // new entry: appended to the table (Branch B)
            idx = used++;
            memset(&e[idx], 0, sizeof e[idx]);
            strcpy_s(e[idx].name, sizeof e[idx].name, names[i]);
        }
        if (!pack_entry(&datas[i], &packed)) goto done;
        e[idx].time = filetime;
        e[idx].usize = (long long)datas[i].n;
        e[idx].offset = (long long)w.b.n;
        e[idx].csize = (long long)packed.n;
        if (!wbuf_put(&w, packed.p, packed.n)) {
            sp_free(&packed);
            goto done;
        }
        sp_free(&packed);
    }
    table = (long long)w.b.n;
    for (k = 0; k < used; k++) {
        size_t len = strlen(e[k].name) + 1;
        unsigned char lens[2];
        if (len > 0xFFFF) goto done;
        lens[0] = (unsigned char)(len & 0xFF);
        lens[1] = (unsigned char)(len >> 8);
        if (!wbuf_put(&w, lens, 2)) goto done;
        if (!wbuf_put(&w, e[k].name, len - 1)) goto done;
        if (!wbuf_u8(&w, 0)) goto done;
        if (!wbuf_u64(&w, (unsigned long long)e[k].time) ||
            !wbuf_u64(&w, (unsigned long long)e[k].usize) ||
            !wbuf_u64(&w, (unsigned long long)e[k].offset) ||
            !wbuf_u64(&w, (unsigned long long)e[k].csize)) goto done;
    }
    if (w.b.n < DZIP_HEAD) goto done;
    // The reference never adds entries, so it could copy the count unchanged; adding one
    // (Branch B) changes it.
    wr32(w.b.p + 8, (unsigned int)used);
    wr64(w.b.p + 16, (unsigned long long)table);
    wr64(w.b.p + 24, sp_dzip_hash(e, used));
    out->p = w.b.p;
    out->n = w.b.n;
    w.b.p = NULL;
    ok = 1;
done:
    free(e);
    sp_free(&w.b);
    return ok;
}

void sp_dzip_close(sp_dzip* z)
{
    if (!z) return;
    free(z->e);
    z->e = NULL;
    z->count = 0;
    z->table = 0;
    memset(&z->data, 0, sizeof z->data);   // borrowed, never freed here
}

// ---- file-backed access -------------------------------------------------

// Table cap: pack0.dzip's table is ~2 MB; 64 MB leaves room without risking a bad header
// pulling a huge allocation out of a 10 GB file.
#define DZIP_MAX_TABLE (64u * 1024u * 1024u)

static int read_at(HANDLE h, long long at, void* dst, size_t n)
{
    LARGE_INTEGER pos;
    size_t done = 0;

    pos.QuadPart = at;
    if (!SetFilePointerEx(h, pos, NULL, FILE_BEGIN)) return 0;
    while (done < n) {
        DWORD got = 0;
        DWORD want = (DWORD)((n - done) > 0x1000000 ? 0x1000000 : (n - done));
        if (!ReadFile(h, (unsigned char*)dst + done, want, &got, NULL) || got == 0) return 0;
        done += got;
    }
    return 1;
}

int sp_dzip_file_open(const wchar_t* path, sp_dzip_file* out)
{
    LARGE_INTEGER size;
    unsigned char head[DZIP_HEAD];
    long long table;
    size_t tail;

    memset(out, 0, sizeof *out);
    out->h = INVALID_HANDLE_VALUE;
    if (!path) return 0;
    out->h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out->h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(out->h, &size) || size.QuadPart < DZIP_HEAD) goto bad;
    out->size = (long long)size.QuadPart;
    if (!read_at(out->h, 0, head, DZIP_HEAD)) goto bad;
    if (rd32(head) != DZIP_MAGIC || rd32(head + 4) != DZIP_VERSION) goto bad;
    table = (long long)rd64(head + 16);
    if (table < DZIP_HEAD || table > out->size) goto bad;
    tail = (size_t)(out->size - table);
    if (tail == 0 || tail > DZIP_MAX_TABLE) goto bad;
    out->table.n = DZIP_HEAD + tail;
    out->table.p = (unsigned char*)malloc(out->table.n);
    if (!out->table.p) {
        out->table.n = 0;
        goto bad;
    }
    memcpy(out->table.p, head, DZIP_HEAD);
    wr64(out->table.p + 16, DZIP_HEAD);             // sp_dzip_open: table follows the header
    if (!read_at(out->h, table, out->table.p + DZIP_HEAD, tail)) goto bad;
    if (!sp_dzip_open(&out->table, &out->z)) goto bad;
    return 1;
bad:
    sp_dzip_file_close(out);
    return 0;
}

int sp_dzip_file_extract(sp_dzip_file* f, int i, sp_buf* out)
{
    const sp_dzip_entry* e;
    long long blocks, outPos = 0, k;
    unsigned char* dst = NULL;
    unsigned char* offs = NULL;
    unsigned char* comp = NULL;
    size_t compcap = 0;

    memset(out, 0, sizeof *out);
    if (!f || f->h == INVALID_HANDLE_VALUE || !f->z.count || i < 0 || i >= f->z.count) return 0;
    e = &f->z.e[i];
    if (e->usize == 0) return 1;
    if (e->offset < DZIP_HEAD || e->csize <= 0 || e->usize < 0) return 0;
    if ((unsigned long long)e->usize > DZIP_MAX_ENTRY ||
        (unsigned long long)e->offset > (unsigned long long)f->size ||
        (unsigned long long)e->csize > (unsigned long long)f->size - (unsigned long long)e->offset)
        return 0;
    blocks = (e->usize + DZIP_BLOCK - 1) >> 16;
    if (blocks * 4 > e->csize) return 0;
    offs = (unsigned char*)malloc((size_t)blocks * 4);
    dst = (unsigned char*)malloc((size_t)e->usize);
    if (!offs || !dst) goto bad;
    if (!read_at(f->h, e->offset, offs, (size_t)blocks * 4)) goto bad;
    for (k = 0; k < blocks; k++) {
        long long start = e->offset + (int)rd32(offs + 4 * k);
        long long end = (k + 1 < blocks) ? e->offset + (int)rd32(offs + 4 * (k + 1))
                                         : e->offset + e->csize;
        long long left = e->usize - outPos;
        long long want = left < DZIP_BLOCK ? left : DZIP_BLOCK;
        size_t need;

        if (start < e->offset || end < start || end > e->offset + e->csize) goto bad;
        need = (size_t)(end - start);
        if (need > compcap) {
            unsigned char* grown = (unsigned char*)realloc(comp, need ? need : 1);
            if (!grown) goto bad;
            comp = grown;
            compcap = need;
        }
        if (need && !read_at(f->h, start, comp, need)) goto bad;
        outPos += lzf_block(comp, 0, (long long)need, dst, outPos, want);
    }
    if (outPos != e->usize) goto bad;
    free(offs);
    free(comp);
    out->p = dst;
    out->n = (size_t)e->usize;
    return 1;
bad:
    free(offs);
    free(comp);
    free(dst);
    return 0;
}

void sp_dzip_file_close(sp_dzip_file* f)
{
    if (!f) return;
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    f->h = INVALID_HANDLE_VALUE;
    f->size = 0;
    sp_dzip_close(&f->z);
    sp_free(&f->table);
}
