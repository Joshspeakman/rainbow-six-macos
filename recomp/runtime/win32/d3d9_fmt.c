/* Direct3D 9 pixel formats for d3d9.c: sizes and pitches of a level in the game's format, and conversion to the
 * renderer's BGRA8 (DXT1/2/3/4/5 decoded on the CPU; 16-bit, luminance and alpha formats expanded). */
#include <stdint.h>
#include <string.h>

enum { F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25, F_A4R4G4B4 = 26, F_R3G3B2 = 27, F_A8 = 28,
       F_A8R3G3B2 = 29, F_X4R4G4B4 = 30, F_A8B8G8R8 = 32, F_X8B8G8R8 = 33, F_L8 = 50, F_A8L8 = 51, F_A4L4 = 52, F_V8U8 = 60,
       F_D16 = 80, F_D24S8 = 75, F_D24X8 = 77, F_D32 = 71, F_D15S1 = 73, F_D16_LOCKABLE = 70,
       F_R16F = 111, F_G16R16F = 112, F_A16B16G16R16F = 113, F_R32F = 114, F_G32R32F = 115, F_A32B32G32R32F = 116,
       F_DXT1 = 0x31545844, F_DXT2 = 0x32545844, F_DXT3 = 0x33545844, F_DXT4 = 0x34545844, F_DXT5 = 0x35545844 };

int d9_fmt_block(uint32_t f) { return f == F_DXT1 ? 8 : (f >= F_DXT2 && f <= F_DXT5 && (f & 0xffffff) == 0x545844) ? 16 : 0; }
int d9_fmt_bytes(uint32_t f)           /* bytes per pixel (0: block-compressed or unknown) */
{
    switch (f) {
    case F_A8R8G8B8: case F_X8R8G8B8: case F_A8B8G8R8: case F_X8B8G8R8: case F_D24S8: case F_D24X8: case F_D32: return 4;
    case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_X4R4G4B4: case F_A8L8: case F_V8U8: case F_A8R3G3B2:
    case F_D16: case F_D15S1: case F_D16_LOCKABLE: return 2;
    case F_A8: case F_L8: case F_A4L4: case F_R3G3B2: return 1;
    case F_R16F: return 2;
    case F_G16R16F: case F_R32F: return 4;
    case F_A16B16G16R16F: case F_G32R32F: return 8;
    case F_A32B32G32R32F: return 16;
    }
    return 0;
}
int d9_fmt_float(uint32_t f) { return f >= F_R16F && f <= F_A32B32G32R32F; }   /* render targets only: no CPU conversion */
int d9_fmt_known(uint32_t f) { return d9_fmt_bytes(f) || d9_fmt_block(f); }
int d9_fmt_depth(uint32_t f) { return f == F_D16 || f == F_D24S8 || f == F_D24X8 || f == F_D32 || f == F_D15S1 || f == F_D16_LOCKABLE; }
int d9_fmt_alpha(uint32_t f)           /* does the format carry alpha (else the renderer treats it as XRGB) */
{
    switch (f) { case F_X8R8G8B8: case F_X8B8G8R8: case F_R5G6B5: case F_X1R5G5B5: case F_X4R4G4B4: case F_L8: case F_R3G3B2: case F_DXT1: return f == F_DXT1; }
    return 1;
}
uint32_t d9_pitch(uint32_t f, uint32_t w)
{
    int b = d9_fmt_block(f); if (b) return (uint32_t)b * ((w + 3) / 4 ? (w + 3) / 4 : 1);
    return (w * (uint32_t)(d9_fmt_bytes(f) ? d9_fmt_bytes(f) : 4) + 3) & ~3u;
}
uint32_t d9_rows(uint32_t f, uint32_t h) { return d9_fmt_block(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h; }   /* rows of pitch */
uint32_t d9_size(uint32_t f, uint32_t w, uint32_t h) { return d9_pitch(f, w) * d9_rows(f, h); }

static inline uint32_t c565(uint16_t v)  /* -> 0xAARRGGBB */
{
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
static void dxt_colors(const uint8_t *b, uint32_t col[4], int dxt1)
{
    uint16_t c0, c1; memcpy(&c0, b, 2); memcpy(&c1, b + 2, 2);
    col[0] = c565(c0); col[1] = c565(c1);
    uint32_t r0 = col[0] >> 16 & 255, g0 = col[0] >> 8 & 255, b0 = col[0] & 255, r1 = col[1] >> 16 & 255, g1 = col[1] >> 8 & 255, b1 = col[1] & 255;
    if (!dxt1 || c0 > c1) {
        col[2] = 0xff000000u | ((2 * r0 + r1) / 3) << 16 | ((2 * g0 + g1) / 3) << 8 | (2 * b0 + b1) / 3;
        col[3] = 0xff000000u | ((r0 + 2 * r1) / 3) << 16 | ((g0 + 2 * g1) / 3) << 8 | (b0 + 2 * b1) / 3;
    } else {
        col[2] = 0xff000000u | ((r0 + r1) / 2) << 16 | ((g0 + g1) / 2) << 8 | (b0 + b1) / 2;
        col[3] = 0;                                    /* transparent black */
    }
}
/* the rows [y0, y1) of a level (game format, src pitch) -> BGRA8 rows of w pixels (dst pitch w*4); for block formats
 * y0/y1 must be multiples of 4 (or the level's end) */
void d9_to_bgra(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t y0, uint32_t y1, uint8_t *dst)
{
    uint32_t *o = (uint32_t *)dst;
    int blk = d9_fmt_block(f);
    if (blk) {
        for (uint32_t by = y0 / 4; by * 4 < y1; by++) {
            const uint8_t *row = src + by * pitch;
            for (uint32_t bx = 0; bx * 4 < w; bx++) {
                const uint8_t *b = row + (uint32_t)blk * bx, *cb = blk == 16 ? b + 8 : b;
                uint32_t col[4]; dxt_colors(cb, col, blk == 8);
                uint32_t idx; memcpy(&idx, cb + 4, 4);
                uint8_t a[16];
                if (f == F_DXT2 || f == F_DXT3) { for (int k = 0; k < 16; k++) { uint8_t n = (b[k / 2] >> (4 * (k & 1))) & 15; a[k] = (uint8_t)(n * 17); } }
                else if (f == F_DXT4 || f == F_DXT5) {
                    uint32_t al[8] = {b[0], b[1]};
                    if (b[0] > b[1]) for (int k = 1; k < 7; k++) al[k + 1] = ((7 - k) * b[0] + k * b[1]) / 7;
                    else { for (int k = 1; k < 5; k++) al[k + 1] = ((5 - k) * b[0] + k * b[1]) / 5; al[6] = 0; al[7] = 255; }
                    uint64_t bits = 0; for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                    for (int k = 0; k < 16; k++) a[k] = (uint8_t)al[(bits >> (3 * k)) & 7];
                } else for (int k = 0; k < 16; k++) a[k] = 255;
                for (int py = 0; py < 4; py++) {
                    uint32_t y = by * 4 + (uint32_t)py; if (y < y0 || y >= y1) continue;
                    for (int px = 0; px < 4; px++) {
                        uint32_t x = bx * 4 + (uint32_t)px; if (x >= w) continue;
                        int k = py * 4 + px; uint32_t c = col[(idx >> (2 * k)) & 3];
                        if (blk == 16) c = (c & 0xffffffu) | (uint32_t)a[k] << 24;
                        o[(y - y0) * w + x] = c;
                    }
                }
            }
        }
        return;
    }
    for (uint32_t y = y0; y < y1; y++) {
        const uint8_t *s = src + y * pitch; uint32_t *d = o + (y - y0) * w;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v;
            switch (f) {
            case F_A8R8G8B8: memcpy(&v, s + 4 * x, 4); break;
            case F_X8R8G8B8: memcpy(&v, s + 4 * x, 4); v |= 0xff000000u; break;
            case F_A8B8G8R8: case F_X8B8G8R8: memcpy(&v, s + 4 * x, 4); v = (v & 0xff00ff00u) | (v >> 16 & 255) | (v & 255) << 16; if (f == F_X8B8G8R8) v |= 0xff000000u; break;
            case F_R5G6B5: { uint16_t p; memcpy(&p, s + 2 * x, 2); v = c565(p); break; }
            case F_X1R5G5B5: case F_A1R5G5B5: { uint16_t p; memcpy(&p, s + 2 * x, 2); uint32_t r = p >> 10 & 31, g = p >> 5 & 31, b = p & 31;
                v = ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2) | (f == F_X1R5G5B5 || (p & 0x8000) ? 0xff000000u : 0); break; }
            case F_A4R4G4B4: case F_X4R4G4B4: { uint16_t p; memcpy(&p, s + 2 * x, 2);
                v = (uint32_t)((p >> 8 & 15) * 17) << 16 | (uint32_t)((p >> 4 & 15) * 17) << 8 | (uint32_t)(p & 15) * 17 | (f == F_X4R4G4B4 ? 0xff000000u : (uint32_t)((p >> 12) * 17) << 24); break; }
            case F_A8: v = (uint32_t)s[x] << 24 | 0xffffffu; break;
            case F_L8: v = 0xff000000u | (uint32_t)s[x] * 0x010101u; break;
            case F_A8L8: v = (uint32_t)s[2 * x + 1] << 24 | (uint32_t)s[2 * x] * 0x010101u; break;
            case F_A4L4: v = (uint32_t)((s[x] >> 4) * 17) << 24 | (uint32_t)((s[x] & 15) * 17) * 0x010101u; break;
            case F_R3G3B2: case F_A8R3G3B2: { uint16_t p = f == F_R3G3B2 ? s[x] : (uint16_t)(s[2 * x] | s[2 * x + 1] << 8);
                v = (uint32_t)((p >> 5 & 7) * 255 / 7) << 16 | (uint32_t)((p >> 2 & 7) * 255 / 7) << 8 | (uint32_t)((p & 3) * 85) | (f == F_R3G3B2 ? 0xff000000u : (uint32_t)(p >> 8) << 24); break; }
            case F_V8U8: v = 0xff000000u | (uint32_t)(uint8_t)(s[2 * x] + 128) << 16 | (uint32_t)(uint8_t)(s[2 * x + 1] + 128) << 8 | 0x80; break;
            default: v = 0xffff00ffu;                   /* unknown: magenta, so it shows */
            }
            d[x] = v;
        }
    }
}
/* one BGRA8 pixel (as 0xAARRGGBB) -> the game's format */
static void put_px(uint32_t f, uint8_t *d, uint32_t v)
{
    uint32_t a = v >> 24, r = v >> 16 & 255, g = v >> 8 & 255, b = v & 255;
    switch (f) {
    case F_A8R8G8B8: memcpy(d, &v, 4); break;
    case F_X8R8G8B8: v |= 0xff000000u; memcpy(d, &v, 4); break;
    case F_A8B8G8R8: case F_X8B8G8R8: { uint32_t o = (f == F_X8B8G8R8 ? 0xffu : a) << 24 | b << 16 | g << 8 | r; memcpy(d, &o, 4); break; }
    case F_R5G6B5: { uint16_t p = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(d, &p, 2); break; }
    case F_X1R5G5B5: case F_A1R5G5B5: { uint16_t p = (uint16_t)((f == F_X1R5G5B5 || a >= 128 ? 0x8000u : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(d, &p, 2); break; }
    case F_A4R4G4B4: case F_X4R4G4B4: { uint16_t p = (uint16_t)((f == F_X4R4G4B4 ? 15u : a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(d, &p, 2); break; }
    case F_A8: d[0] = (uint8_t)a; break;
    case F_L8: d[0] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); break;
    case F_A8L8: d[0] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); d[1] = (uint8_t)a; break;
    case F_A4L4: d[0] = (uint8_t)((a >> 4) << 4 | ((r * 77 + g * 150 + b * 29) >> 12)); break;
    case F_R3G3B2: d[0] = (uint8_t)((r >> 5) << 5 | (g >> 5) << 2 | b >> 6); break;
    case F_A8R3G3B2: d[0] = (uint8_t)((r >> 5) << 5 | (g >> 5) << 2 | b >> 6); d[1] = (uint8_t)a; break;
    case F_V8U8: d[0] = (uint8_t)(r - 128); d[1] = (uint8_t)(g - 128); break;
    default: memcpy(d, &v, 4 < d9_fmt_bytes(f) ? 4 : (size_t)d9_fmt_bytes(f)); break;
    }
}
/* ---- DXT encoding (bounding-box endpoints, nearest-index; good enough for textures the game builds at run time) ---- */
static uint16_t to565(const uint8_t *c) { return (uint16_t)((c[2] >> 3) << 11 | (c[1] >> 2) << 5 | c[0] >> 3); }
static void from565(uint16_t p, int *c) { c[2] = (p >> 11) * 255 / 31; c[1] = (p >> 5 & 63) * 255 / 63; c[0] = (p & 31) * 255 / 31; }
static void dxt_color(const uint8_t px[16][4], uint8_t *out, int dxt1)
{
    uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}; int transparent = 0;
    for (int k = 0; k < 16; k++) {
        if (dxt1 && px[k][3] < 128) { transparent = 1; continue; }
        for (int j = 0; j < 3; j++) { if (px[k][j] < lo[j]) lo[j] = px[k][j]; if (px[k][j] > hi[j]) hi[j] = px[k][j]; }
    }
    if (lo[0] > hi[0]) { memset(lo, 0, 3); memset(hi, 0, 3); }           /* every pixel transparent */
    uint16_t c0 = to565(hi), c1 = to565(lo);
    int three = dxt1 && transparent;
    if (three ? c0 > c1 : c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
    if (!three && c0 == c1) { if (c1) c1--; else c0++; }                   /* 4-colour mode needs c0 > c1 */
    int p[4][3]; from565(c0, p[0]); from565(c1, p[1]);
    for (int j = 0; j < 3; j++) {
        if (three) { p[2][j] = (p[0][j] + p[1][j]) / 2; p[3][j] = 0; }
        else { p[2][j] = (2 * p[0][j] + p[1][j]) / 3; p[3][j] = (p[0][j] + 2 * p[1][j]) / 3; }
    }
    uint32_t idx = 0;
    for (int k = 0; k < 16; k++) {
        int best = 0, bd = 1 << 30;
        if (three && px[k][3] < 128) best = 3;
        else for (int q = 0; q < (three ? 3 : 4); q++) {
            int dd = 0; for (int j = 0; j < 3; j++) { int e = px[k][j] - p[q][j]; dd += e * e; }
            if (dd < bd) { bd = dd; best = q; }
        }
        idx |= (uint32_t)best << (2 * k);
    }
    memcpy(out, &c0, 2); memcpy(out + 2, &c1, 2); memcpy(out + 4, &idx, 4);
}
static void dxt5_alpha(const uint8_t px[16][4], uint8_t *out)
{
    int lo = 255, hi = 0; for (int k = 0; k < 16; k++) { if (px[k][3] < lo) lo = px[k][3]; if (px[k][3] > hi) hi = px[k][3]; }
    if (hi == lo) { if (hi < 255) hi++; else lo--; }
    int a[8]; a[0] = hi; a[1] = lo; for (int q = 1; q < 7; q++) a[q + 1] = ((7 - q) * hi + q * lo) / 7;
    uint64_t bits = 0;
    for (int k = 0; k < 16; k++) {
        int best = 0, bd = 1 << 30;
        for (int q = 0; q < 8; q++) { int e = px[k][3] - a[q]; if (e * e < bd) { bd = e * e; best = q; } }
        bits |= (uint64_t)best << (3 * k);
    }
    out[0] = (uint8_t)hi; out[1] = (uint8_t)lo; for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}
static void encode_dxt(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch)
{
    int bs = d9_fmt_block(f);
    for (uint32_t by = 0; by < (h + 3) / 4; by++) for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        uint8_t px[16][4];
        for (int k = 0; k < 16; k++) {
            uint32_t x = bx * 4 + (uint32_t)(k & 3), y = by * 4 + (uint32_t)(k >> 2);
            if (x >= w) x = w - 1; if (y >= h) y = h - 1;
            memcpy(px[k], src + ((size_t)y * w + x) * 4, 4);
        }
        uint8_t *o = dst + (size_t)by * pitch + (size_t)bx * (size_t)bs;
        if (f == F_DXT1) dxt_color(px, o, 1);
        else {
            if (f == F_DXT2 || f == F_DXT3) { for (int k = 0; k < 8; k++) o[k] = (uint8_t)((px[2 * k][3] >> 4) | (px[2 * k + 1][3] >> 4) << 4); }
            else dxt5_alpha(px, o);
            dxt_color(px, o + 8, 0);
        }
    }
}
/* BGRA8 rows -> the game's format */
void d9_from_bgra(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch)
{
    if (d9_fmt_block(f)) { encode_dxt(f, src, w, h, dst, pitch); return; }
    int bpp = d9_fmt_bytes(f); if (!bpp) return;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + (size_t)y * w * 4); uint8_t *d = dst + (size_t)y * pitch;
        for (uint32_t x = 0; x < w; x++) put_px(f, d + (size_t)bpp * x, s[x]);
    }
}
