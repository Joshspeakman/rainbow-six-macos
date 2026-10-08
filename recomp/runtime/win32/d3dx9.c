/* D3DX 9 (d3dx9_29.dll) as Dungeon Siege II uses it: textures created with D3DX's defaults, textures loaded from image
 * files in memory (DDS with its mipmaps, cube maps and volumes; TGA; BMP) and D3DXLoadSurfaceFromMemory. Missing mipmap
 * levels are made by 2x2 box filtering (and re-encoded for DXT formats). Effects and HLSL compilation are not here. */
#include "d3d9_priv.h"

#define D3DX_DEFAULT 0xffffffffu
#define D3DX_DEFAULT_NONPOW2 0xfffffffeu
#define D3DXERR_INVALIDDATA 0x88760b59u
#define E_OUTOFMEMORY_ 0x8007000eu
enum { X_R8G8B8 = 20, X_A8R8G8B8 = 21, X_X8R8G8B8 = 22, X_L8 = 50, X_P8 = 41 };

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* ---- texture requirements (D3DXCheckTextureRequirements' defaults) ---- */
static uint32_t req_size(uint32_t v) { return v == 0 || v == D3DX_DEFAULT || v == D3DX_DEFAULT_NONPOW2 ? 256 : v > 4096 ? 4096 : v; }
static uint32_t req_fmt(uint32_t f) { return f == 0 || f == D3DX_DEFAULT ? X_A8R8G8B8 : d9_fmt_known(f) ? f : d9_fmt_alpha(f) ? X_A8R8G8B8 : X_X8R8G8B8; }
static uint32_t req_levels(uint32_t m) { return m == D3DX_DEFAULT ? 0 : m; }   /* 0: the full chain */

IMPL(d3dx9_29, D3DXCreateTexture)          /* (device, width, height, levels, usage, format, pool, texture) */
{
    Dev9 *d = obj(ARG(0), O9_DEV); if (!d || !ARG(7)) RET(D3DERR_INVALIDCALL, 8);
    RET(d9_create_tex(d, O9_TEX, req_size(ARG(1)), req_size(ARG(2)), 1, req_levels(ARG(3)), ARG(4), req_fmt(ARG(5)), ARG(6), ARG(7)), 8);
}
IMPL(d3dx9_29, D3DXCreateCubeTexture)      /* (device, size, levels, usage, format, pool, texture) */
{
    Dev9 *d = obj(ARG(0), O9_DEV); if (!d || !ARG(6)) RET(D3DERR_INVALIDCALL, 7);
    uint32_t n = req_size(ARG(1));
    RET(d9_create_tex(d, O9_CUBE, n, n, 1, req_levels(ARG(2)), ARG(3), req_fmt(ARG(4)), ARG(5), ARG(6)), 7);
}
IMPL(d3dx9_29, D3DXCreateVolumeTexture)    /* (device, width, height, depth, levels, usage, format, pool, texture) */
{
    Dev9 *d = obj(ARG(0), O9_DEV); if (!d || !ARG(8)) RET(D3DERR_INVALIDCALL, 9);
    uint32_t dep = ARG(3) == 0 || ARG(3) == D3DX_DEFAULT ? 1 : ARG(3);
    RET(d9_create_tex(d, O9_VOLTEX, req_size(ARG(1)), req_size(ARG(2)), dep, req_levels(ARG(4)), ARG(5), req_fmt(ARG(6)), ARG(7), ARG(8)), 9);
}

/* ---- images ---- */
typedef struct Img {
    uint32_t w, h, d, levels, faces, fmt;               /* fmt: the format of data (a Direct3D format d3d9_fmt.c knows) */
    const uint8_t *data; size_t len; uint8_t *own;      /* faces x levels (x depth slices), each level d9_size bytes */
} Img;
static size_t level_bytes(const Img *im, uint32_t l)
{
    uint32_t w = im->w >> l, h = im->h >> l, d = im->d >> l;
    return (size_t)d9_size(im->fmt, w ? w : 1, h ? h : 1) * (d ? d : 1);
}
static uint32_t dds_format(const uint8_t *pf)
{
    uint32_t fl = le32(pf + 4), cc = le32(pf + 8), bits = le32(pf + 12), rm = le32(pf + 16), gm = le32(pf + 20), bm = le32(pf + 24), am = le32(pf + 28);
    int alpha = (fl & 1) && am;
    if (fl & 4) return cc;                              /* FOURCC: DXT1-5, or a Direct3D format number */
    if (fl & 0x40) {                                    /* RGB */
        if (bits == 32 && rm == 0xff0000 && gm == 0xff00 && bm == 0xff) return alpha ? X_A8R8G8B8 : X_X8R8G8B8;
        if (bits == 32 && rm == 0xff && gm == 0xff00 && bm == 0xff0000) return alpha ? 32 : 33;
        if (bits == 24) return X_R8G8B8;
        if (bits == 16 && rm == 0xf800) return 23;
        if (bits == 16 && rm == 0x7c00) return alpha ? 25 : 24;
        if (bits == 16 && rm == 0x0f00) return alpha ? 26 : 30;
        if (bits == 8 && rm == 0xe0) return 27;
        if (bits == 16 && rm == 0xe0) return 29;
    }
    if (fl & 0x20000) return bits == 16 ? 51 : bits == 8 && (fl & 1) ? 52 : X_L8;   /* luminance */
    if ((fl & 2) && bits == 8) return 28;               /* alpha only */
    if ((fl & 0x80000) && bits == 16) return 60;        /* V8U8 */
    return 0;
}
static int load_dds(const uint8_t *p, size_t n, Img *im)
{
    if (n < 128 || memcmp(p, "DDS ", 4) || le32(p + 4) != 124) return 0;
    im->h = le32(p + 12); im->w = le32(p + 16); im->d = (le32(p + 8) & 0x800000) && le32(p + 24) ? le32(p + 24) : 1;
    im->levels = (le32(p + 8) & 0x20000) && le32(p + 28) ? le32(p + 28) : 1; if (im->levels > 16) im->levels = 16;
    uint32_t caps2 = le32(p + 112);
    im->faces = (caps2 & 0x200) ? 0 : 1;
    if (caps2 & 0x200) for (int k = 0; k < 6; k++) if (caps2 & (0x400u << k)) im->faces++;
    if (!(caps2 & 0x200000)) im->d = 1;
    im->fmt = dds_format(p + 76);
    if (!im->w || !im->h || !im->faces) return 0;
    im->data = p + 128; im->len = n - 128;
    if (im->fmt == X_R8G8B8) {                          /* 24-bit: widened to X8R8G8B8 */
        Img o = *im; o.fmt = X_X8R8G8B8; size_t tot = 0, src = 0;
        for (uint32_t f = 0; f < im->faces; f++) for (uint32_t l = 0; l < im->levels; l++) tot += level_bytes(&o, l);
        o.own = malloc(tot); uint8_t *q = o.own;
        for (uint32_t f = 0; f < im->faces; f++) for (uint32_t l = 0; l < im->levels; l++) {
            uint32_t w = im->w >> l ? im->w >> l : 1, h = im->h >> l ? im->h >> l : 1, d = im->d >> l ? im->d >> l : 1;
            for (size_t k = 0; k < (size_t)w * h * d; k++, q += 4, src += 3) {
                if (src + 3 > im->len) { free(o.own); return 0; }
                q[0] = im->data[src]; q[1] = im->data[src + 1]; q[2] = im->data[src + 2]; q[3] = 255;
            }
        }
        o.data = o.own; o.len = tot; *im = o;
    }
    if (!d9_fmt_known(im->fmt)) { fprintf(stderr, "d3dx9: DDS format %#x is not supported\n", im->fmt); return 0; }
    size_t need = 0; for (uint32_t l = 0; l < im->levels; l++) need += level_bytes(im, l);
    if (need * im->faces > im->len) {                   /* short file: keep the levels that are complete */
        uint32_t l = 0; size_t s = 0; while (l < im->levels && (s + level_bytes(im, l)) * im->faces <= im->len) s += level_bytes(im, l++);
        if (!l || im->faces > 1) return 0;
        im->levels = l;
    }
    return 1;
}
static uint8_t *bgra_image(Img *im, uint32_t w, uint32_t h, uint32_t fmt)
{
    im->w = w; im->h = h; im->d = 1; im->levels = 1; im->faces = 1; im->fmt = fmt;
    im->own = calloc((size_t)w * h, 4); im->data = im->own; im->len = (size_t)w * h * 4; return im->own;
}
static uint32_t px16(uint32_t p, int a1)                /* X1R5G5B5 / A1R5G5B5 -> ARGB */
{
    uint32_t r = p >> 10 & 31, g = p >> 5 & 31, b = p & 31;
    return (a1 ? (p & 0x8000 ? 0xff000000u : 0) : 0xff000000u) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static int load_tga(const uint8_t *p, size_t n, Img *im)
{
    if (n < 18) return 0;
    uint32_t idl = p[0], cmt = p[1], type = p[2], cmf = le16(p + 3), cml = le16(p + 5), cmb = p[7], w = le16(p + 12), h = le16(p + 14), bpp = p[16], desc = p[17];
    int rle = type >= 9, base = type & 7;
    if ((base != 1 && base != 2 && base != 3) || !w || !h || (cmt > 1)) return 0;
    if (base == 1 && (bpp != 8 || !cmt)) return 0;
    if (base == 2 && bpp != 15 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    if (base == 3 && bpp != 8) return 0;
    size_t o = 18 + idl, cmsz = cmt ? (size_t)cml * ((cmb + 7) / 8) : 0;
    if (o + cmsz > n) return 0;
    const uint8_t *cm = p + o; o += cmsz;
    int alpha = (base == 2 && bpp == 32 && (desc & 15)) || (base == 2 && bpp == 16 && (desc & 15)) || (base == 1 && cmb == 32);
    uint32_t *out = (uint32_t *)bgra_image(im, w, h, base == 3 ? X_L8 : alpha ? X_A8R8G8B8 : X_X8R8G8B8);
    uint32_t bytes = (bpp + 7) / 8, total = w * h, i = 0;
    uint8_t px[4] = {0};
    #define TGA_PIX(src, dst) do { const uint8_t *s_ = (src); uint32_t v_; \
        if (base == 3) v_ = 0xff000000u | s_[0] * 0x010101u; \
        else if (base == 1) { uint32_t k_ = s_[0] - cmf; const uint8_t *c_ = cm + (size_t)(k_ < cml ? k_ : 0) * ((cmb + 7) / 8); \
            v_ = cmb == 32 ? le32(c_) : cmb == 24 ? 0xff000000u | (uint32_t)c_[2] << 16 | (uint32_t)c_[1] << 8 | c_[0] : px16(le16(c_), 0); } \
        else if (bytes == 4) v_ = alpha ? le32(s_) : le32(s_) | 0xff000000u; \
        else if (bytes == 3) v_ = 0xff000000u | (uint32_t)s_[2] << 16 | (uint32_t)s_[1] << 8 | s_[0]; \
        else v_ = px16(le16(s_), alpha); \
        (dst) = v_; } while (0)
    while (i < total) {
        if (!rle) { if (o + bytes > n) break; TGA_PIX(p + o, out[i]); o += bytes; i++; continue; }
        if (o >= n) break;
        uint32_t hd = p[o++], cnt = (hd & 127) + 1;
        if (hd & 128) { if (o + bytes > n) break; memcpy(px, p + o, bytes); o += bytes; uint32_t v; TGA_PIX(px, v); for (uint32_t k = 0; k < cnt && i < total; k++) out[i++] = v; }
        else for (uint32_t k = 0; k < cnt && i < total; k++) { if (o + bytes > n) { i = total; break; } TGA_PIX(p + o, out[i]); o += bytes; i++; }
    }
    #undef TGA_PIX
    if (!(desc & 0x20)) for (uint32_t y = 0; y < h / 2; y++)          /* bottom-up rows */
        for (uint32_t x = 0; x < w; x++) { uint32_t t = out[y * w + x]; out[y * w + x] = out[(h - 1 - y) * w + x]; out[(h - 1 - y) * w + x] = t; }
    if (base == 3) {                                    /* grey: stored as L8 */
        uint8_t *l8 = malloc((size_t)w * h); for (uint32_t k = 0; k < total; k++) l8[k] = (uint8_t)out[k];
        free(im->own); im->own = l8; im->data = l8; im->len = (size_t)w * h;
    }
    return 1;
}
static int load_bmp(const uint8_t *p, size_t n, Img *im)
{
    if (n < 54 || p[0] != 'B' || p[1] != 'M') return 0;
    uint32_t off = le32(p + 10), hs = le32(p + 14); int32_t w = (int32_t)le32(p + 18), h = (int32_t)le32(p + 22);
    uint32_t bpp = le16(p + 28), comp = le32(p + 30), used = hs >= 40 ? le32(p + 46) : 0;
    if (w <= 0 || !h || w > 16384 || h > 16384 || h < -16384 || (comp != 0 && comp != 3) || off >= n) return 0;
    int top = h < 0; uint32_t H = (uint32_t)(top ? -h : h), W = (uint32_t)w;
    uint32_t masks[4] = {0x7c00, 0x3e0, 0x1f, 0};
    if (bpp == 32) { masks[0] = 0xff0000; masks[1] = 0xff00; masks[2] = 0xff; }
    if (comp == 3 && 14 + hs + (hs == 40 ? 12 : 0) <= n) { const uint8_t *m = p + 14 + 40; masks[0] = le32(m); masks[1] = le32(m + 4); masks[2] = le32(m + 8); if (hs >= 56) masks[3] = le32(m + 12); }
    const uint8_t *pal = p + 14 + hs; uint32_t npal = used ? used : bpp <= 8 ? 1u << bpp : 0;
    if (bpp <= 8 && 14 + hs + 4 * npal > n) return 0;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return 0;
    uint32_t *out = (uint32_t *)bgra_image(im, W, H, masks[3] ? X_A8R8G8B8 : X_X8R8G8B8);
    uint32_t stride = ((W * bpp + 31) / 32) * 4;
    for (uint32_t y = 0; y < H; y++) {
        size_t ro = off + (size_t)(top ? y : H - 1 - y) * stride; if (ro + stride > n) break;
        const uint8_t *r = p + ro; uint32_t *d = out + (size_t)y * W;
        for (uint32_t x = 0; x < W; x++) {
            uint32_t v;
            if (bpp <= 8) { uint32_t k = bpp == 8 ? r[x] : bpp == 4 ? (r[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (r[x / 8] >> (7 - (x & 7))) & 1;
                v = k < npal ? (le32(pal + 4 * k) & 0xffffff) | 0xff000000u : 0xff000000u; }
            else if (bpp == 24) v = 0xff000000u | (uint32_t)r[3 * x + 2] << 16 | (uint32_t)r[3 * x + 1] << 8 | r[3 * x];
            else {
                uint32_t raw = bpp == 32 ? le32(r + 4 * x) : le16(r + 2 * x); v = 0;
                for (int c = 0; c < 4; c++) {
                    uint32_t m = masks[c]; if (!m) { if (c == 3) v |= 0xff000000u; continue; }
                    int sh = __builtin_ctz(m), bits = __builtin_popcount(m); uint32_t comp_ = (raw & m) >> sh;
                    uint32_t c8 = bits >= 8 ? comp_ >> (bits - 8) : (comp_ * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
                    v |= c8 << (c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24);
                }
            }
            d[x] = v;
        }
    }
    return 1;
}
static int load_image(const uint8_t *p, size_t n, Img *im)
{
    memset(im, 0, sizeof *im);
    if (load_dds(p, n, im) || load_bmp(p, n, im)) return 1;
    if (n >= 4 && (!memcmp(p, "\x89PNG", 4) || (p[0] == 0xff && p[1] == 0xd8))) {
        fprintf(stderr, "d3dx9: PNG/JPEG textures are not supported yet\n"); return 0;
    }
    return load_tga(p, n, im);                          /* TGA has no magic number: tried last */
}

/* ---- filling textures ---- */
static void box_down(const uint8_t *s, uint32_t w, uint32_t h, uint8_t *d, uint32_t nw, uint32_t nh)
{
    for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int k = 0; k < 4; k++) {
        uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
        d[(y * nw + x) * 4 + k] = (uint8_t)((s[(y0 * w + x0) * 4 + k] + s[(y0 * w + x1) * 4 + k] + s[(y1 * w + x0) * 4 + k] + s[(y1 * w + x1) * 4 + k] + 2) / 4);
    }
}
/* a texture holding the image: the file's levels, the rest of the chain made from the last one */
static uint32_t tex_from_image(Dev9 *d, int type, Img *im, uint32_t out)
{
    uint32_t dep = type == O9_VOLTEX ? im->d : 1;
    uint32_t want = type == O9_VOLTEX || im->levels > 1 ? im->levels : 0;
    uint32_t hr = d9_create_tex(d, type, im->w, im->h, dep, want, 0, im->fmt, 1 /* MANAGED */, out);
    if (hr) return hr;
    Tex9 *t = d9_any_tex(rt_r32(G_MEM, out));
    uint32_t faces = type == O9_CUBE ? 6 : 1; size_t o = 0;
    for (uint32_t f = 0; f < faces && f < im->faces; f++) {
        for (uint32_t l = 0; l < t->levels; l++) {
            Surf9 *s = t->lv[f][l];
            if (l < im->levels) {
                size_t n = level_bytes(im, l); if (o + n > im->len) n = o < im->len ? im->len - o : 0;
                memcpy(G_MEM + s->mem, im->data + o, n < s->size ? n : s->size); o += level_bytes(im, l);
            } else {                                    /* generated: 2x2 box filter of the level above */
                Surf9 *u = t->lv[f][l - 1];
                uint8_t *a = malloc((size_t)u->w * u->h * 4), *b = malloc((size_t)s->w * s->h * 4);
                d9_to_bgra(u->fmt, G_MEM + u->mem, u->pitch, u->w, 0, u->h, a);
                box_down(a, u->w, u->h, b, s->w, s->h);
                d9_from_bgra(s->fmt, b, s->w, s->h, G_MEM + s->mem, s->pitch);
                free(a); free(b);
            }
            d9_surf_upload(s, 0, s->h);
        }
    }
    return D3D_OK;
}
static uint32_t from_file(Ctx *c, int type, const char *what)
{
    Dev9 *d = obj(ARG(0), O9_DEV); uint32_t src = ARG(1), len = ARG(2), out = ARG(3);
    if (!d || !src || !len || !out) return D3DERR_INVALIDCALL;
    Img im; if (!load_image(G_MEM + src, len, &im)) {
        fprintf(stderr, "d3dx9: %s: unreadable image (%u bytes, starting %02x %02x %02x %02x)\n", what, len, G_MEM[src], G_MEM[src + 1], G_MEM[src + 2], G_MEM[src + 3]);
        return D3DXERR_INVALIDDATA;
    }
    if (type == O9_CUBE && im.faces != 6) { free(im.own); return D3DXERR_INVALIDDATA; }
    uint32_t hr = tex_from_image(d, type, &im, out);
    if (logon()) fprintf(stderr, "d3dx9: %s %ux%ux%u format %#x, %u levels -> %08x\n", what, im.w, im.h, im.d, im.fmt, im.levels, hr);
    free(im.own); return hr;
}
IMPL(d3dx9_29, D3DXGetImageInfoFromFileInMemory)      /* (data, size, D3DXIMAGE_INFO *): width, height, depth, levels, format, type, file format */
{
    uint32_t src = ARG(0), len = ARG(1), out = ARG(2);
    if (!src || !len) RET(D3DERR_INVALIDCALL, 3);
    Img im; if (!load_image(G_MEM + src, len, &im)) RET(D3DXERR_INVALIDDATA, 3);
    int dds = len >= 4 && !memcmp(G_MEM + src, "DDS ", 4), bmp = len >= 2 && !memcmp(G_MEM + src, "BM", 2);
    uint32_t v[7] = {im.w, im.h, im.d, im.levels, im.fmt, im.faces == 6 ? 5u : im.d > 1 ? 4u : 3u, dds ? 4u : bmp ? 0u : 2u};
    if (out) memcpy(GP(out), v, sizeof v);
    free(im.own); RET(D3D_OK, 3);
}
IMPL(d3dx9_29, D3DXCreateTextureFromFileInMemory) { RET(from_file(c, O9_TEX, "texture"), 4); }
IMPL(d3dx9_29, D3DXCreateCubeTextureFromFileInMemory) { RET(from_file(c, O9_CUBE, "cube texture"), 4); }
IMPL(d3dx9_29, D3DXCreateVolumeTextureFromFileInMemory) { RET(from_file(c, O9_VOLTEX, "volume texture"), 4); }

/* ---- D3DXLoadSurfaceFromMemory(dest, dest palette, dest rect, src, src format, src pitch, src palette, src rect,
 *      filter, colour key) ---- */
static int decode_rect(uint32_t fmt, const uint8_t *base, uint32_t pitch, const int32_t *r, const uint8_t *pal, uint8_t *out)
{
    uint32_t w = (uint32_t)(r[2] - r[0]), h = (uint32_t)(r[3] - r[1]);
    if (fmt == X_P8 || fmt == X_R8G8B8) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = base + (size_t)(r[1] + (int32_t)y) * pitch + (size_t)(r[0] + (int32_t)x) * (fmt == X_P8 ? 1 : 3);
            uint32_t v = fmt == X_R8G8B8 ? 0xff000000u | (uint32_t)s[2] << 16 | (uint32_t)s[1] << 8 | s[0]
                       : pal ? (uint32_t)pal[4 * s[0] + 3] << 24 | (uint32_t)pal[4 * s[0]] << 16 | (uint32_t)pal[4 * s[0] + 1] << 8 | pal[4 * s[0] + 2] : 0xff000000u | s[0] * 0x010101u;
            memcpy(out + ((size_t)y * w + x) * 4, &v, 4);
        }
        return 1;
    }
    if (!d9_fmt_known(fmt)) return 0;
    const uint8_t *p = d9_fmt_block(fmt) ? base + (size_t)(r[1] / 4) * pitch + (size_t)(r[0] / 4) * (size_t)d9_fmt_block(fmt)
                                         : base + (size_t)r[1] * pitch + (size_t)r[0] * (size_t)d9_fmt_bytes(fmt);
    d9_to_bgra(fmt, p, pitch, w, 0, h, out);
    return 1;
}
static void resample(const uint8_t *s, uint32_t sw, uint32_t sh, uint8_t *d, uint32_t dw, uint32_t dh)
{
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0 = (uint32_t)((uint64_t)y * sh / dh), y1 = (uint32_t)((uint64_t)(y + 1) * sh / dh); if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = (uint32_t)((uint64_t)x * sw / dw), x1 = (uint32_t)((uint64_t)(x + 1) * sw / dw); if (x1 <= x0) x1 = x0 + 1;
            uint32_t acc[4] = {0}, n = 0;
            for (uint32_t yy = y0; yy < y1; yy++) for (uint32_t xx = x0; xx < x1; xx++, n++) for (int k = 0; k < 4; k++) acc[k] += s[((size_t)yy * sw + xx) * 4 + k];
            for (int k = 0; k < 4; k++) d[((size_t)y * dw + x) * 4 + k] = (uint8_t)((acc[k] + n / 2) / n);
        }
    }
}
static uint32_t lock_scratch;
IMPL(d3dx9_29, D3DXLoadSurfaceFromMemory)
{
    Surf9 *s = obj(ARG(0), O9_SURF); uint32_t src = ARG(3), sf = ARG(4), sp = ARG(5), spal = ARG(6), sr = ARG(7), key = ARG(9);
    if (!s || !src || !sr) RET(D3DERR_INVALIDCALL, 10);
    int32_t r[4], dr[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    memcpy(r, GP(sr), 16); if (ARG(2)) memcpy(dr, GP(ARG(2)), 16);
    if (r[2] <= r[0] || r[3] <= r[1] || r[0] < 0 || r[1] < 0 || r[2] - r[0] > 16384 || r[3] - r[1] > 16384 ||
        dr[2] <= dr[0] || dr[3] <= dr[1] || dr[0] < 0 || dr[1] < 0 || dr[2] > (int32_t)s->w || dr[3] > (int32_t)s->h) RET(D3DERR_INVALIDCALL, 10);
    uint32_t sw = (uint32_t)(r[2] - r[0]), sh = (uint32_t)(r[3] - r[1]), dw = (uint32_t)(dr[2] - dr[0]), dh = (uint32_t)(dr[3] - dr[1]);
    uint8_t *a = malloc((size_t)sw * sh * 4);
    if (!decode_rect(sf, G_MEM + src, sp, r, spal ? G_MEM + spal : NULL, a)) {
        static int once; if (!once++) fprintf(stderr, "d3dx9: D3DXLoadSurfaceFromMemory: source format %#x is not supported\n", sf);
        free(a); RET(D3DXERR_INVALIDDATA, 10);
    }
    if (key) for (size_t k = 0; k < (size_t)sw * sh; k++) { uint32_t v; memcpy(&v, a + 4 * k, 4); if (v == key) memset(a + 4 * k, 0, 4); }
    uint8_t *b = a;
    if (sw != dw || sh != dh) { b = malloc((size_t)dw * dh * 4); resample(a, sw, sh, b, dw, dh); }
    if (!lock_scratch) lock_scratch = heap_alloc(w32_process_heap, 8, 24);
    memcpy(GP(lock_scratch + 8), dr, 16);
    uint32_t hr = d9_surf_lock(s, lock_scratch, lock_scratch + 8, 0);
    if (!hr) {
        d9_from_bgra(s->fmt, b, dw, dh, G_MEM + rt_r32(G_MEM, lock_scratch + 4), rt_r32(G_MEM, lock_scratch));
        d9_surf_unlock(s);
    }
    if (b != a) free(b);
    free(a);
    if (logon() > 1) fprintf(stderr, "d3dx9: LoadSurfaceFromMemory %ux%u format %#x -> %ux%u format %#x: %08x\n", sw, sh, sf, dw, dh, s->fmt, hr);
    RET(hr, 10);
}
