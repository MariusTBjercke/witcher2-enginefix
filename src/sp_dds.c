// DDS icon decode/encode; see sp_dds.h. Port of the Dds class in
// ScriptPatches.ps1 (git tag v0.5.1) (Decode64, Channel, Rgb565, Mix, Over, Encode64),
// keeping the reference's integer division and .NET Math.Round (ties to even).
#include "sp_dds.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DDS_MAGIC 0x20534444u
#define DDS_HEAD  128

static unsigned int rd16(const unsigned char* p)
{
    unsigned int v;
    v = (unsigned int)p[0] | ((unsigned int)p[1] << 8);
    return v;
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

// .NET Math.Round(double): nearest, ties to even. The reference relies on it.
static double round_even(double v)
{
    double f = floor(v), d = v - f;
    if (d > 0.5) return f + 1.0;
    if (d < 0.5) return f;
    return (fmod(f, 2.0) == 0.0) ? f : f + 1.0;
}

static unsigned int channel(unsigned int v, unsigned int mask)
{
    unsigned int shift = 0, max;
    if (mask == 0) return 0;
    while (((mask >> shift) & 1) == 0) shift++;
    max = mask >> shift;
    return ((v & mask) >> shift) * 255 / max;
}

static unsigned int rgb565(unsigned int c)
{
    unsigned int r = ((c >> 11) & 31) * 255 / 31;
    unsigned int g = ((c >> 5) & 63) * 255 / 63;
    unsigned int b = (c & 31) * 255 / 31;
    return (r << 16) | (g << 8) | b;
}

static unsigned int mix(unsigned int p, unsigned int q, int wp, int wq, int d)
{
    unsigned int res = 0xFF000000u;
    for (int s = 0; s < 24; s += 8)
        res |= (unsigned int)((((p >> s) & 255) * (unsigned int)wp +
                               ((q >> s) & 255) * (unsigned int)wq) / (unsigned int)d) << s;
    return res;
}

int sp_dds_decode64(const sp_buf* dds, unsigned* px)
{
    const unsigned char* b;
    unsigned int pf_flags;

    if (!dds || !dds->p || !px || dds->n < DDS_HEAD) return 0;
    b = dds->p;
    if (rd32(b) != DDS_MAGIC) return 0;
    if (rd32(b + 16) != 64 || rd32(b + 12) != 64) return 0;
    pf_flags = rd32(b + 80);
    if ((pf_flags & 4) == 0) {                       // plain BGRA
        unsigned int rm, gm, bm, am;
        if (rd32(b + 88) != 32 || dds->n < (size_t)(DDS_HEAD + SP_DDS_PIXELS * 4)) return 0;
        rm = rd32(b + 92);
        gm = rd32(b + 96);
        bm = rd32(b + 100);
        am = rd32(b + 104);
        for (int i = 0; i < SP_DDS_PIXELS; i++) {
            unsigned int v = rd32(b + DDS_HEAD + 4 * i);
            unsigned int a = (pf_flags & 1) ? channel(v, am) : 255u;
            px[i] = (a << 24) | (channel(v, rm) << 16) | (channel(v, gm) << 8) | channel(v, bm);
        }
        return 1;
    }
    {
        int is1 = memcmp(b + 84, "DXT1", 4) == 0;
        int is3 = memcmp(b + 84, "DXT3", 4) == 0;
        int is5 = memcmp(b + 84, "DXT5", 4) == 0;
        int block = is1 ? 8 : 16;
        int k = DDS_HEAD;
        if (!is1 && !is3 && !is5) return 0;
        if (dds->n < (size_t)(DDS_HEAD + 256 * block)) return 0;
        for (int by = 0; by < 64; by += 4) {
            for (int bx = 0; bx < 64; bx += 4, k += block) {
                int c = is1 ? k : k + 8;
                unsigned int c0 = rd16(b + c), c1 = rd16(b + c + 2);
                unsigned int idx = rd32(b + c + 4);
                unsigned int pal[4], alpha[16];
                pal[0] = 0xFF000000u | rgb565(c0);
                pal[1] = 0xFF000000u | rgb565(c1);
                if (!is1 || c0 > c1) {
                    pal[2] = mix(pal[0], pal[1], 2, 1, 3);
                    pal[3] = mix(pal[0], pal[1], 1, 2, 3);
                } else {
                    pal[2] = mix(pal[0], pal[1], 1, 1, 2);
                    pal[3] = 0;
                }
                for (int i = 0; i < 16; i++) alpha[i] = 0;   // DXT1 does not use alpha
                if (is3) {
                    unsigned long long av = rd64(b + k);
                    for (int i = 0; i < 16; i++) alpha[i] = (unsigned int)((av >> (4 * i)) & 15) * 17;
                } else if (is5) {
                    int a0 = b[k], a1 = b[k + 1], ap[8];
                    unsigned long long ai = 0;
                    for (int i = 0; i < 6; i++) ai |= (unsigned long long)b[k + 2 + i] << (8 * i);
                    ap[0] = a0;
                    ap[1] = a1;
                    if (a0 > a1) {
                        for (int i = 0; i < 6; i++) ap[2 + i] = ((6 - i) * a0 + (i + 1) * a1) / 7;
                    } else {
                        for (int i = 0; i < 4; i++) ap[2 + i] = ((4 - i) * a0 + (i + 1) * a1) / 5;
                        ap[6] = 0;
                        ap[7] = 255;
                    }
                    for (int i = 0; i < 16; i++) alpha[i] = (unsigned int)ap[(ai >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; i++) {
                    unsigned int col = pal[(idx >> (2 * i)) & 3];
                    if (!is1) col = (col & 0x00FFFFFFu) | (alpha[i] << 24);
                    px[(by + i / 4) * 64 + bx + i % 4] = col;
                }
            }
        }
    }
    return 1;
}

void sp_dds_over(unsigned* base, const unsigned* top, double opacity)
{
    if (!base || !top) return;
    for (int i = 0; i < SP_DDS_PIXELS; i++) {
        double ta = (double)((top[i] >> 24) & 255) / 255.0 * opacity;
        double ba = (double)((base[i] >> 24) & 255) / 255.0;
        double oa = ta + ba * (1 - ta);
        unsigned int out;
        if (oa <= 0) {
            base[i] = 0;
            continue;
        }
        out = (unsigned int)round_even(oa * 255.0) << 24;
        for (int s = 0; s < 24; s += 8) {
            double v = ((double)((top[i] >> s) & 255) * ta +
                        (double)((base[i] >> s) & 255) * ba * (1 - ta)) / oa;
            double r = round_even(v);
            if (r < 0) r = 0;
            if (r > 255) r = 255;
            out |= (unsigned int)r << s;
        }
        base[i] = out;
    }
}

int sp_dds_encode64(const unsigned* px, sp_buf* out)
{
    unsigned char* h;

    memset(out, 0, sizeof *out);
    if (!px) return 0;
    out->n = DDS_HEAD + SP_DDS_PIXELS * 4;
    out->p = (unsigned char*)malloc(out->n);
    if (!out->p) {
        out->n = 0;
        return 0;
    }
    memset(out->p, 0, out->n);
    h = out->p;
    wr32(h + 0, DDS_MAGIC);
    wr32(h + 4, 124);                    // dwSize
    wr32(h + 8, 0x100F);                 // caps|height|width|pitch|pixelformat
    wr32(h + 12, 64);                    // height
    wr32(h + 16, 64);                    // width
    wr32(h + 20, 256);                   // pitch
    wr32(h + 76, 32);                    // pixel format size
    wr32(h + 80, 0x41);                  // DDPF_RGB | DDPF_ALPHAPIXELS
    wr32(h + 88, 32);                    // rgb bit count
    wr32(h + 92, 0x00FF0000u);           // r
    wr32(h + 96, 0x0000FF00u);           // g
    wr32(h + 100, 0x000000FFu);          // b
    wr32(h + 104, 0xFF000000u);          // a
    wr32(h + 108, 0x1000);               // caps: texture
    for (int i = 0; i < SP_DDS_PIXELS; i++) wr32(out->p + DDS_HEAD + 4 * i, px[i]);
    return 1;
}
