/* DirectDraw as Diablo II and Delta Force use it (from the diablo-2-macos port): DirectDrawCreate (an IDirectDraw that is asked for later versions), 8-bit
 * palettized surfaces the game draws into itself, palettes, a clipper, colour fills and copies. All pixels are the
 * game's own (it renders in software); the visible frame is the primary surface's pixels through its palette, sent
 * to the Metal renderer as one picture per Flip (full screen) or per copy onto the primary (in a window).
 *
 * COM objects are small guest blocks {vtable, host object index}; each interface version has its own vtable of
 * thunks (methods shared, argument counts and structure sizes per version). Surface pixels live in guest memory
 * because the game writes through the pointers Lock returns. */
#include "w32.h"
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include "../../../src/dsr/dsr_proto.h"

uint32_t *cmd_begin(uint32_t op, uint32_t payload);   /* ddraw.c: the command ring to the renderer */
void cmd_end(void);
void dsr_present(Ctx *c, uint32_t id);
uint32_t next_object_id(void);
void w32_set_window_pos(Ctx *c, uint32_t hwnd, int x, int y, int cx, int cy, uint32_t fl);
extern int w32_screen_w, w32_screen_h;

#define DD_OK 0u
#define E_NOINTERFACE 0x80004002u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_GENERIC 0x80004005u
#define DDH(n) (0x88760000u | (n))
#define DDERR_NOTFOUND DDH(255)
#define DDERR_NOPALETTEATTACHED DDH(572)
#define DDERR_NOCLIPPERATTACHED DDH(568)
#define DDERR_NOTLOCKED DDH(584)
#define DDERR_CANTCREATEDC DDH(581)
#define DDERR_NOBLTHW DDH(575)
#define DDERR_NOCOLORKEY DDH(215)

enum { DDSD_CAPS = 1, DDSD_HEIGHT = 2, DDSD_WIDTH = 4, DDSD_PITCH = 8, DDSD_BACKBUFFERCOUNT = 0x20, DDSD_REFRESHRATE = 0x40000,
       DDSD_LPSURFACE = 0x800, DDSD_PIXELFORMAT = 0x1000, DDSD_CKSRCBLT = 0x10000 };
enum { DDSCAPS_BACKBUFFER = 4, DDSCAPS_COMPLEX = 8, DDSCAPS_FLIP = 0x10, DDSCAPS_FRONTBUFFER = 0x20, DDSCAPS_OFFSCREENPLAIN = 0x40,
       DDSCAPS_PALETTE = 0x100, DDSCAPS_PRIMARYSURFACE = 0x200, DDSCAPS_SYSTEMMEMORY = 0x800, DDSCAPS_VIDEOMEMORY = 0x4000,
       DDSCAPS_VISIBLE = 0x8000, DDSCAPS_LOCALVIDMEM = 0x10000000 };
enum { DDPF_PALETTEINDEXED8 = 0x20, DDPF_RGB = 0x40 };
enum { DDBLT_COLORFILL = 0x400, DDBLT_KEYSRC = 0x8000, DDBLT_KEYSRCOVERRIDE = 0x10000, DDBLTFAST_SRCCOLORKEY = 1 };
enum { DDCKEY_SRCBLT = 8 };
enum { DDSCL_FULLSCREEN = 1, DDSCL_EXCLUSIVE = 0x10 };
/* DDSURFACEDESC (108 bytes, versions 1-3) and DDSURFACEDESC2 (124, versions 4 and 7) share these offsets */
enum { SD_SIZE = 0, SD_FLAGS = 4, SD_HEIGHT = 8, SD_WIDTH = 12, SD_PITCH = 16, SD_BACKBUFFERS = 20, SD_REFRESH = 24,
       SD_SURFACE = 36, SD_CKSRCBLT = 64, SD_PF = 72, SD_CAPS = 104 };
enum { PF_SIZE = 0, PF_FLAGS = 4, PF_FOURCC = 8, PF_BITS = 12, PF_R = 16, PF_G = 20, PF_B = 24, PF_A = 28 };

static int dd_log = -1;
static void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog(const char *fmt, ...)
{
    if (dd_log < 0) dd_log = getenv("DSR_LOG") != 0 || getenv("W32_DDLOG") != 0;
    if (!dd_log) return;
    va_list a; va_start(a, fmt); fprintf(stderr, "ddraw: "); vfprintf(stderr, fmt, a); fputc('\n', stderr); va_end(a);
}

/* ---- objects ---- */
typedef struct PSurf PSurf;
typedef struct PPal { int ref; uint32_t g; uint32_t entries[256]; uint32_t caps; } PPal;     /* entries: PALETTEENTRY r, g, b, flags */
typedef struct PClip { int ref; uint32_t g, hwnd; } PClip;
typedef struct PDraw { int ref; uint32_t g[4]; uint32_t hwnd, coop, mode_w, mode_h, mode_bpp; PSurf *primary; int d3d; uint32_t g_d3d; } PDraw;   /* d3d: asked for IDirect3D3 */
struct PSurf {
    int ref; uint32_t g[4], g_gamma; PDraw *dd;
    uint32_t w, h, bpp, caps, pitch, mem;            /* mem: guest pixels */
    uint32_t pf[8];                                  /* DDPIXELFORMAT */
    PSurf *back; PSurf *front;                       /* flip chain: primary -> back buffer (and back to the primary) */
    PPal *pal; PClip *clip;
    uint32_t ck_lo, ck_hi; int has_ck;
    int locked;
    /* Direct3D mode: the renderer's copy (textures, the 3D render target, its depth buffer) */
    uint32_t gid, g_tex; int gkind;                  /* gkind: 1 render target, 2 depth, 3 texture, 4 offscreen */
    int gpu_newer;                                   /* drawn into on the GPU since the CPU copy was last read back */
    PSurf *zbuf;                                     /* an attached depth buffer */
};
enum { T_DD = 1, T_SURF, T_PAL, T_CLIP, T_GAMMA, T_D3D, T_DEV, T_VP, T_TEX, T_VB };
typedef struct { int type; void *p; } Obj;
#define MAXO 65536
static Obj objs[MAXO]; static uint32_t nobjs = 1;
static uint32_t free_obj[MAXO], nfree_obj;           /* released slots, reused once all have been handed out */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vt_dd[4], vt_surf[4], vt_pal, vt_clip, vt_gamma, vt_d3d, vt_dev, vt_vp, vt_tex, vt_vb;   /* [version index]: 0 v1, 1 v2, 2 v4, 3 v7 (surfaces: v1/v2+v3, v4, v7 share a layout) */

static uint32_t iface(uint32_t vtable, int type, void *p)
{
    pthread_mutex_lock(&lk);
    uint32_t k = nobjs < MAXO ? nobjs++ : nfree_obj ? free_obj[--nfree_obj] : 0;
    if (k) { objs[k].type = type; objs[k].p = p; } else fprintf(stderr, "ddraw: out of object slots\n");
    pthread_mutex_unlock(&lk);
    uint32_t g = heap_alloc(w32_process_heap, 8, 8);
    rt_w32(G_MEM, g, vtable); rt_w32(G_MEM, g + 4, k);
    return g;
}
static void *the(uint32_t g, int type)
{
    if (!g) return 0;
    uint32_t k = rt_r32(G_MEM, g + 4);
    return k && k < MAXO && objs[k].type == type ? objs[k].p : 0;
}
#define M(name, n) static void name(Ctx *c) { enum { nargs_ = n };
#define END }
#define ME(T, ty) T *me = the(ARG(0), ty); if (!me) { fprintf(stderr, "ddraw: bad " #T " object %08x in %s (type %d)\n", ARG(0), __func__, \
                                                                ARG(0) && rt_r32(G_MEM, ARG(0) + 4) < MAXO ? objs[rt_r32(G_MEM, ARG(0) + 4)].type : -1); RET(DDERR_INVALIDPARAMS, nargs_); }
#define OK_M(name, n) M(name, n) RET(DD_OK, n); END
#define NOT_M(name, n) M(name, n) { static int once; if (!once++) fprintf(stderr, "ddraw: %s not supported\n", #name); } RET(DDERR_UNSUPPORTED, n); END

/* ---- pixel formats ---- */
static void pf_for(uint32_t *pf, uint32_t bpp)
{
    memset(pf, 0, 32); pf[0] = 32; pf[3] = bpp;
    if (bpp == 8) pf[1] = DDPF_RGB | DDPF_PALETTEINDEXED8;
    else if (bpp == 16) { pf[1] = DDPF_RGB; pf[4] = 0xf800; pf[5] = 0x07e0; pf[6] = 0x001f; }
    else { pf[1] = DDPF_RGB; pf[4] = 0xff0000; pf[5] = 0x00ff00; pf[6] = 0x0000ff; }
}

/* ---- the picture: the primary's pixels through its palette, to the renderer ---- */
static uint32_t screen_id, screen_w, screen_h;
static uint32_t last_pal_rgb[256];                   /* the palette of the last 8-bit picture (tests: written beside screenshots) */
static int last_indexed;                             /* the last picture came from 8-bit pixels */
uint32_t ddraw_pal_frames;                           /* pictures sent */
static void to_bgra(const PSurf *s, const uint8_t *src, uint32_t *dst, uint32_t w, uint32_t h, uint32_t pitch)
{
    if (s->bpp == 8) {
        uint32_t lut[256];
        for (int k = 0; k < 256; k++) {
            uint32_t e = s->pal ? s->pal->entries[k] : (uint32_t)(k * 0x010101);              /* r | g << 8 | b << 16 */
            lut[k] = 0xff000000u | (e & 0xff) << 16 | (e & 0xff00) | (e >> 16 & 0xff);       /* BGRA in memory = 0xAARRGGBB */
        }
        memcpy(last_pal_rgb, lut, sizeof lut); last_indexed = 1;
        for (uint32_t y = 0; y < h; y++) { const uint8_t *r = src + y * pitch; uint32_t *o = dst + y * w; for (uint32_t x = 0; x < w; x++) o[x] = lut[r[x]]; }
    } else if (s->bpp == 16) {
        last_indexed = 0;
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t *r = (const uint16_t *)(src + y * pitch); uint32_t *o = dst + y * w;
            for (uint32_t x = 0; x < w; x++) { uint32_t v = r[x], R = (v >> 11) * 255 / 31, G = (v >> 5 & 63) * 255 / 63, B = (v & 31) * 255 / 31; o[x] = 0xff000000u | R << 16 | G << 8 | B; }
        }
    } else {
        last_indexed = 0;
        for (uint32_t y = 0; y < h; y++) { const uint32_t *r = (const uint32_t *)(src + y * pitch); uint32_t *o = dst + y * w; for (uint32_t x = 0; x < w; x++) o[x] = r[x] | 0xff000000u; }
    }
}
/* A picture for the screen, w x h BGRA pixels written by the caller straight into the command ring (DirectDraw's
 * primary here, the GDI window in windowed mode: gdi32.c): frame_begin returns where the rows go (pitch w * 4),
 * frame_end sends it and presents. The picture's size is the game's coordinate space for the mouse. */
extern uint32_t dsr_d3d9_w, dsr_d3d9_h;              /* ddraw.c: the size dsr_mode_size reports */
uint32_t *dsr_frame_begin(uint32_t w, uint32_t h)
{
    if (!screen_id || screen_w != w || screen_h != h) {
        if (!screen_id) screen_id = next_object_id();
        else { uint32_t *p = cmd_begin(DSR_SURFACE_DESTROY, 4); p[0] = screen_id; cmd_end(); }
        uint32_t *p = cmd_begin(DSR_SURFACE_CREATE, 40); memset(p, 0, 40);
        p[0] = screen_id; p[1] = w; p[2] = h; p[3] = 4; p[4] = 2; cmd_end();          /* offscreen, XRGB */
        uint32_t *m = cmd_begin(DSR_MODE, 8); m[0] = w; m[1] = h; cmd_end();
        screen_w = w; screen_h = h; dsr_d3d9_w = w; dsr_d3d9_h = h;
    }
    uint32_t *p = cmd_begin(DSR_SURFACE_UPLOAD, 24 + w * h * 4);
    p[0] = screen_id; p[1] = 0; p[2] = 0; p[3] = w; p[4] = h; p[5] = w * 4;
    return p + 6;
}
void dsr_frame_end(Ctx *c)
{
    cmd_end();
    ddraw_pal_frames++;
    dsr_present(c, screen_id);
}
void dsr_frame_palette(const uint32_t *bgra)         /* (tests: the colours of the last 8-bit picture; 0: not 8-bit) */
{
    if (bgra) memcpy(last_pal_rgb, bgra, sizeof last_pal_rgb);
    last_indexed = bgra != 0;
}
/* the last picture's palette as 0xRRGGBB; 0 when it was not an 8-bit picture */
int dsr_frame_palette_get(uint32_t out[256])
{
    if (!last_indexed) return 0;
    for (int k = 0; k < 256; k++) out[k] = last_pal_rgb[k] & 0xffffff;
    return 1;
}
/* the rectangle x, y, w, h of surface s becomes the picture on screen */
static void show(Ctx *c, const PSurf *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!w || !h || x + w > s->w || y + h > s->h) return;
    uint32_t *px = dsr_frame_begin(w, h);
    to_bgra(s, G_MEM + s->mem + y * s->pitch + x * (s->bpp / 8), px, w, h, s->pitch);
    dsr_frame_end(c);
}
/* DS_PEEKFILE / tests: the last frame's palette as 0xRRGGBB */
uint32_t ddraw_pal_color(int k) { return k >= 0 && k < 256 ? last_pal_rgb[k] & 0xffffff : 0; }
void ddraw_pal_screen(uint32_t *w, uint32_t *h) { *w = screen_w; *h = screen_h; }

/* ---- surfaces ---- */
static void surf_attach_ifaces(PSurf *s)
{
    for (int v = 0; v < 4; v++) s->g[v] = iface(vt_surf[v], T_SURF, s);
    s->g_gamma = iface(vt_gamma, T_GAMMA, s);
}
static PSurf *surf_new(PDraw *dd, uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps)
{
    PSurf *s = calloc(1, sizeof *s); s->ref = 1; s->dd = dd; s->w = w; s->h = h; s->bpp = bpp; s->caps = caps;
    s->pitch = (w * (bpp / 8) + 3) & ~3u;
    s->mem = heap_alloc(w32_process_heap, 8, s->pitch * h + 16);
    pf_for(s->pf, bpp);
    surf_attach_ifaces(s);
    return s;
}
/* ---- Direct3D mode: surfaces with a copy on the GPU ---- */
int cmd_readback(uint32_t id, const int32_t *r, uint8_t *dst, uint32_t pitch, uint32_t bpp);   /* ddraw.c */
static void gpu_attach(PSurf *s, int kind)
{
    if (s->gid) return;
    s->gid = next_object_id(); s->gkind = kind;
    int alpha = (s->pf[1] & 1) != 0 || s->has_ck || s->bpp == 8;            /* DDPF_ALPHAPIXELS, a colour key, a palette */
    uint32_t *p = cmd_begin(DSR_SURFACE_CREATE, 40); memset(p, 0, 40);
    p[0] = s->gid; p[1] = s->w; p[2] = s->h; p[3] = (uint32_t)kind; p[4] = kind == 2 ? 3 : kind == 3 && alpha ? 1 : 2; p[7] = 1;
    cmd_end();
    dlog("surface %ux%ux%u caps %x on the GPU as %u (kind %d)", s->w, s->h, s->bpp, s->caps, s->gid, kind);
}
static void gpu_detach(PSurf *s)
{
    if (!s->gid) return;
    uint32_t *p = cmd_begin(DSR_SURFACE_DESTROY, 4); p[0] = s->gid; cmd_end();
    void surface_id_free(uint32_t); surface_id_free(s->gid); s->gid = 0;
}
static uint32_t chan(uint32_t v, uint32_t mask)                         /* a masked channel scaled to 0..255 */
{
    if (!mask) return 255;
    int sh = __builtin_ctz(mask), bits = __builtin_popcount(mask); uint32_t x = (v & mask) >> sh, mx = (1u << bits) - 1;
    return bits >= 8 ? x >> (bits - 8) : (x * 255 + mx / 2) / mx;
}
/* the surface's pixels as BGRA (0xAARRGGBB), through its pixel format; a colour-keyed pixel is fully transparent */
static void surf_to_bgra(const PSurf *s, uint32_t *dst)
{
    const uint8_t *src = G_MEM + s->mem; uint32_t bp = s->bpp / 8;
    uint32_t rm = s->pf[4], gm = s->pf[5], bm = s->pf[6], am = (s->pf[1] & 1) ? s->pf[7] : 0;
    uint32_t lut[256];
    if (s->bpp == 8) for (int k = 0; k < 256; k++) { uint32_t e = s->pal ? s->pal->entries[k] : (uint32_t)k * 0x010101u;
        lut[k] = 0xff000000u | (e & 0xff) << 16 | (e & 0xff00) | (e >> 16 & 0xff); }
    for (uint32_t y = 0; y < s->h; y++) {
        const uint8_t *r = src + y * s->pitch; uint32_t *o = dst + y * s->w;
        for (uint32_t x = 0; x < s->w; x++) {
            uint32_t v = bp == 1 ? r[x] : bp == 2 ? (uint32_t)(r[2 * x] | r[2 * x + 1] << 8) : (uint32_t)(r[4 * x] | r[4 * x + 1] << 8 | r[4 * x + 2] << 16 | (uint32_t)r[4 * x + 3] << 24);
            uint32_t px = bp == 1 ? lut[v] : chan(v, am) << 24 | chan(v, rm) << 16 | chan(v, gm) << 8 | chan(v, bm);
            if (s->has_ck && v >= s->ck_lo && v <= s->ck_hi) px &= 0x00ffffffu;
            o[x] = px;
        }
    }
}
/* Direct3D's smooth textures (DF_TEXTURES=smooth): filtered texels near a cut-out's edge blend in the transparent ones,
 * so those take the colour of their visible neighbours (two texels deep) instead of the key's black */
static int smooth_textures(void) { static int v = -1; if (v < 0) v = getenv("DF_TEXTURES") && !strcmp(getenv("DF_TEXTURES"), "smooth"); return v; }
static void bleed_edges(uint32_t *px, uint32_t w, uint32_t h)
{
    uint8_t *done = malloc((size_t)w * h);
    for (uint32_t k = 0; k < w * h; k++) done[k] = px[k] >> 24 != 0;
    for (int pass = 1; pass <= 2; pass++)
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            uint32_t k = y * w + x; if (done[k]) continue;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
            for (int j = 0; j < 4; j++) {
                int64_t xx = (int64_t)x + dx[j], yy = (int64_t)y + dy[j];
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                uint32_t q = (uint32_t)yy * w + (uint32_t)xx; if (!done[q] || done[q] > pass) continue;
                r += px[q] >> 16 & 0xff; g += px[q] >> 8 & 0xff; b += px[q] & 0xff; n++;
            }
            if (n) { px[k] = (r / n) << 16 | (g / n) << 8 | (b / n); done[k] = (uint8_t)(pass + 1); }   /* still transparent */
        }
    free(done);
}
static void gpu_upload(PSurf *s)
{
    if (s->gid && s->gkind == 2) {                                   /* a depth buffer the game filled itself: its values, 0..1 */
        uint32_t n = s->w * s->h, *p = cmd_begin(DSR_DEPTH_UPLOAD, 12 + n * 4); float *z = (float *)(p + 3);
        p[0] = s->gid; p[1] = s->w; p[2] = s->h;
        for (uint32_t y = 0; y < s->h; y++) {
            const uint8_t *row = G_MEM + s->mem + y * s->pitch;
            if (s->bpp == 16) for (uint32_t x = 0; x < s->w; x++) z[y * s->w + x] = (float)(row[2 * x] | row[2 * x + 1] << 8) * (1.0f / 65535.0f);
            else for (uint32_t x = 0; x < s->w; x++) { uint32_t v; memcpy(&v, row + 4 * x, 4); z[y * s->w + x] = (float)((double)v * (1.0 / 4294967295.0)); }
        }
        cmd_end(); s->gpu_newer = 0; return;
    }
    if (!s->gid) return;
    uint32_t *p = cmd_begin(DSR_SURFACE_UPLOAD, 24 + s->w * s->h * 4);
    p[0] = s->gid; p[1] = 0; p[2] = 0; p[3] = s->w; p[4] = s->h; p[5] = s->w * 4;
    surf_to_bgra(s, p + 6);
    if (s->gkind == 3 && smooth_textures()) bleed_edges(p + 6, s->w, s->h);
    cmd_end(); s->gpu_newer = 0;
}
/* the GPU's pixels back into the CPU copy (a render target the game locks), in the surface's own format */
static void gpu_readback(PSurf *s)
{
    if (!s->gid || !s->gpu_newer || s->gkind == 2) return;
    uint32_t *tmp = malloc((size_t)s->w * s->h * 4); int32_t r[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    if (cmd_readback(s->gid, r, (uint8_t *)tmp, s->w * 4, 4)) {
        uint32_t rm = s->pf[4], gm = s->pf[5], bm = s->pf[6];
        for (uint32_t y = 0; y < s->h; y++) for (uint32_t x = 0; x < s->w; x++) {
            uint32_t c = tmp[y * s->w + x], v = 0, cc[3] = {c >> 16 & 255, c >> 8 & 255, c & 255}, m[3] = {rm, gm, bm};
            for (int k = 0; k < 3; k++) if (m[k]) { int sh = __builtin_ctz(m[k]), bits = __builtin_popcount(m[k]); v |= ((cc[k] >> (8 - (bits > 8 ? 8 : bits))) << sh) & m[k]; }
            uint8_t *d = G_MEM + s->mem + y * s->pitch + x * (s->bpp / 8);
            if (s->bpp == 16) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); } else if (s->bpp == 32) memcpy(d, &v, 4);
        }
    }
    free(tmp); s->gpu_newer = 0;
}
static void surf_free(PSurf *s)
{
    if (s->dd && s->dd->primary == s) s->dd->primary = 0;
    uint32_t gs[6] = {s->g[0], s->g[1], s->g[2], s->g[3], s->g_gamma, s->g_tex};
    pthread_mutex_lock(&lk);
    for (int v = 0; v < 6; v++) { uint32_t k = gs[v] ? rt_r32(G_MEM, gs[v] + 4) : 0; if (k && k < MAXO && objs[k].type) { objs[k].type = 0; objs[k].p = 0; free_obj[nfree_obj++] = k; } }
    pthread_mutex_unlock(&lk);
    if (s->mem) heap_free(w32_process_heap, s->mem);
    gpu_detach(s);
    free(s);
}
static uint32_t surf_release(PSurf *s)
{
    int r = __atomic_sub_fetch(&s->ref, 1, __ATOMIC_SEQ_CST);
    if (r == 0) {
        PSurf *b = s->back;
        if (b && b->front == s) { b->front = 0; surf_free(b); }      /* the flip chain goes with its primary */
        surf_free(s);
    }
    return (uint32_t)(r < 0 ? 0 : r);
}
static void desc_out(const PSurf *s, uint32_t d, int lock_ptr)       /* fill the game's DDSURFACEDESC(2) */
{
    uint32_t size = rt_r32(G_MEM, d); if (size != 108 && size != 124) size = 108;
    memset(GP(d), 0, size); rt_w32(G_MEM, d, size);
    rt_w32(G_MEM, d + SD_FLAGS, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | (lock_ptr ? DDSD_LPSURFACE : 0) | (s->back && s->front == 0 ? DDSD_BACKBUFFERCOUNT : 0));
    rt_w32(G_MEM, d + SD_HEIGHT, s->h); rt_w32(G_MEM, d + SD_WIDTH, s->w); rt_w32(G_MEM, d + SD_PITCH, s->pitch);
    if (s->back && !s->front) rt_w32(G_MEM, d + SD_BACKBUFFERS, 1);
    if (lock_ptr) rt_w32(G_MEM, d + SD_SURFACE, s->mem);
    for (int k = 0; k < 8; k++) rt_w32(G_MEM, d + SD_PF + 4 * (uint32_t)k, s->pf[k]);
    rt_w32(G_MEM, d + SD_CAPS, s->caps);
    if (s->has_ck) { rt_w32(G_MEM, d + SD_CKSRCBLT, s->ck_lo); rt_w32(G_MEM, d + SD_CKSRCBLT + 4, s->ck_hi); rt_w32(G_MEM, d + SD_FLAGS, rt_r32(G_MEM, d + SD_FLAGS) | DDSD_CKSRCBLT); }
}
static int vidx_of_surf_iid(uint32_t iid)            /* IDirectDrawSurface 1/2/3 -> 0..1, 4 -> 2, 7 -> 3; -1 other */
{
    switch (iid) { case 0x6c14db81u: return 0; case 0x57805885u: case 0xda044e00u: return 1; case 0x0b2b8630u: return 2; case 0x06675a80u: return 3; }
    return -1;
}
M(s_QueryInterface, 3) ME(PSurf, T_SURF)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2); int v = vidx_of_surf_iid(iid);
    if (v >= 0 || iid == 0) { rt_w32(G_MEM, out, me->g[v < 0 ? 0 : v]); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    if (iid == 0x69c11c3eu) { rt_w32(G_MEM, out, me->g_gamma); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    if (iid == 0x93281502u) {                                         /* IDirect3DTexture2: any surface has one (a system-memory one is a Load source) */
        if (!me->g_tex) me->g_tex = iface(vt_tex, T_TEX, me);
        rt_w32(G_MEM, out, me->g_tex); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3);
    }
    dlog("surface QI: interface %08x not available", iid); rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(s_AddRef, 1) ME(PSurf, T_SURF) RET(__atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST), 1); END
M(s_Release, 1) ME(PSurf, T_SURF) RET(surf_release(me), 1); END
/* copies and fills */
static void rect_in(uint32_t r, const PSurf *s, int32_t *o)
{
    if (r) for (int k = 0; k < 4; k++) o[k] = (int32_t)rt_r32(G_MEM, r + 4 * (uint32_t)k);
    else { o[0] = o[1] = 0; o[2] = (int32_t)s->w; o[3] = (int32_t)s->h; }
}
static int clip_to(int32_t *r, const PSurf *s)
{
    if (r[0] < 0) r[0] = 0; if (r[1] < 0) r[1] = 0;
    if (r[2] > (int32_t)s->w) r[2] = (int32_t)s->w; if (r[3] > (int32_t)s->h) r[3] = (int32_t)s->h;
    return r[2] > r[0] && r[3] > r[1];
}
static uint32_t px_get(const PSurf *s, uint32_t x, uint32_t y)
{
    const uint8_t *p = G_MEM + s->mem + y * s->pitch + x * (s->bpp / 8);
    return s->bpp == 8 ? p[0] : s->bpp == 16 ? (uint32_t)(p[0] | p[1] << 8) : (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
}
static void px_put(const PSurf *s, uint32_t x, uint32_t y, uint32_t v)
{
    uint8_t *p = G_MEM + s->mem + y * s->pitch + x * (s->bpp / 8);
    if (s->bpp == 8) p[0] = (uint8_t)v; else if (s->bpp == 16) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); } else memcpy(p, &v, 4);
}
static uint32_t convert(const PSurf *from, const PSurf *to, uint32_t v)   /* a pixel between formats (8-bit through the palette) */
{
    if (from->bpp == to->bpp) return v;
    uint32_t r, g, b;
    if (from->bpp == 8) { uint32_t e = from->pal ? from->pal->entries[v & 0xff] : v * 0x010101u; r = e & 0xff; g = e >> 8 & 0xff; b = e >> 16 & 0xff; }
    else if (from->bpp == 16) { r = (v >> 11) * 255 / 31; g = (v >> 5 & 63) * 255 / 63; b = (v & 31) * 255 / 31; }
    else { r = v >> 16 & 0xff; g = v >> 8 & 0xff; b = v & 0xff; }
    if (to->bpp == 16) return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    if (to->bpp == 32) return r << 16 | g << 8 | b;
    return 0;                                                       /* (to 8 bits: not done) */
}
static void copy_rect(PSurf *d, const int32_t *dr, PSurf *s, const int32_t *sr, int use_key, uint32_t klo, uint32_t khi)
{
    int32_t dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    if (!use_key && d->bpp == s->bpp && dw == sw && dh == sh) {        /* the common case: rows of bytes */
        uint32_t bp = d->bpp / 8, row = (uint32_t)dw * bp;
        for (int32_t y = 0; y < dh; y++)
            memmove(G_MEM + d->mem + (uint32_t)(dr[1] + y) * d->pitch + (uint32_t)dr[0] * bp, G_MEM + s->mem + (uint32_t)(sr[1] + y) * s->pitch + (uint32_t)sr[0] * bp, row);
        return;
    }
    for (int32_t y = 0; y < dh; y++) {
        uint32_t sy = (uint32_t)(sr[1] + (int64_t)y * sh / dh);
        for (int32_t x = 0; x < dw; x++) {
            uint32_t sx = (uint32_t)(sr[0] + (int64_t)x * sw / dw), v = px_get(s, sx, sy);
            if (use_key && v >= klo && v <= khi) continue;
            px_put(d, (uint32_t)(dr[0] + x), (uint32_t)(dr[1] + y), convert(s, d, v));
        }
    }
}
static void fill_rect(PSurf *d, const int32_t *r, uint32_t v)
{
    for (int32_t y = r[1]; y < r[3]; y++) {
        if (d->bpp == 8) memset(G_MEM + d->mem + (uint32_t)y * d->pitch + (uint32_t)r[0], (int)(v & 0xff), (size_t)(r[2] - r[0]));
        else for (int32_t x = r[0]; x < r[2]; x++) px_put(d, (uint32_t)x, (uint32_t)y, v);
    }
}
static PSurf *visible_of(PSurf *s) { return (s->caps & DDSCAPS_PRIMARYSURFACE) ? s : 0; }
static void shown_after_copy(Ctx *c, PSurf *d, const int32_t *dr)
{
    /* a copy onto a primary without a flip chain is what the player sees now (in a window, the area copied); full
     * screen with a back buffer, the picture comes at Flip instead */
    if (!visible_of(d) || d->back) return;
    show(c, d, (uint32_t)dr[0], (uint32_t)dr[1], (uint32_t)(dr[2] - dr[0]), (uint32_t)(dr[3] - dr[1]));
}
M(s_Blt, 6) ME(PSurf, T_SURF)                                       /* (dst rect, src, src rect, flags, DDBLTFX *) */
    PSurf *src = the(ARG(2), T_SURF); uint32_t flags = ARG(4), fx = ARG(5);
    int32_t dr[4], sr[4]; rect_in(ARG(1), me, dr);
    if (me->gid && (me->gkind == 1 || me->gkind == 2)) {             /* onto a render target: done on the GPU */
        { static int n; if (n++ < 8) dlog("Blt onto GPU %u (%ux%u caps %x) from %s %ux%u flags %x", me->gid, me->w, me->h, me->caps, src ? (src->gid ? "GPU" : "CPU") : "fill", src ? src->w : 0, src ? src->h : 0, flags); }
        if (src) rect_in(ARG(3), src, sr);
        if (src && !src->gid) { gpu_attach(src, 4); gpu_upload(src); }
        else if (src) gpu_readback(src), gpu_upload(src);
        uint32_t fill = 0;
        if (flags & DDBLT_COLORFILL) { uint32_t v = fx ? rt_r32(G_MEM, fx + 80) : 0; fill = 0xff000000u | chan(v, me->pf[4]) << 16 | chan(v, me->pf[5]) << 8 | chan(v, me->pf[6]); }
        uint32_t *p = cmd_begin(DSR_BLT, 48); p[0] = me->gid; memcpy(p + 1, dr, 16); p[5] = src ? src->gid : 0;
        if (src) memcpy(p + 6, sr, 16); else memset(p + 6, 0, 16);
        p[10] = flags; p[11] = fill; cmd_end();
        me->gpu_newer = 1; RET(DD_OK, 6);
    }
    if (flags & DDBLT_COLORFILL) {
        if (!clip_to(dr, me)) RET(DD_OK, 6);
        fill_rect(me, dr, fx ? rt_r32(G_MEM, fx + 80) : 0);
        shown_after_copy(c, me, dr); RET(DD_OK, 6);
    }
    if (!src) { dlog("Blt flags %x without a source", flags); RET(DD_OK, 6); }
    rect_in(ARG(3), src, sr);
    int key = 0; uint32_t klo = 0, khi = 0;
    if ((flags & DDBLT_KEYSRCOVERRIDE) && fx) { key = 1; klo = rt_r32(G_MEM, fx + 92); khi = rt_r32(G_MEM, fx + 96); }
    else if ((flags & DDBLT_KEYSRC) && src->has_ck) { key = 1; klo = src->ck_lo; khi = src->ck_hi; }
    if (dr[2] - dr[0] == sr[2] - sr[0] && dr[3] - dr[1] == sr[3] - sr[1]) {   /* unscaled: clip both rectangles together */
        int32_t ox = sr[0] - dr[0], oy = sr[1] - dr[1];
        if (!clip_to(dr, me)) RET(DD_OK, 6);
        sr[0] = dr[0] + ox; sr[1] = dr[1] + oy; sr[2] = dr[2] + ox; sr[3] = dr[3] + oy;
        if (!clip_to(sr, src)) RET(DD_OK, 6);
        dr[0] = sr[0] - ox; dr[1] = sr[1] - oy; dr[2] = sr[2] - ox; dr[3] = sr[3] - oy;
    } else if (!clip_to(dr, me) || !clip_to(sr, src)) RET(DD_OK, 6);
    copy_rect(me, dr, src, sr, key, klo, khi);
    if (me->gid) gpu_upload(me);                                      /* a texture filled by copying */
    shown_after_copy(c, me, dr); RET(DD_OK, 6);
END
M(s_BltFast, 6) ME(PSurf, T_SURF)                                   /* (x, y, src, src rect, trans) */
    PSurf *src = the(ARG(3), T_SURF); if (!src) RET(DDERR_INVALIDPARAMS, 6);
    int32_t sr[4]; rect_in(ARG(4), src, sr);
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    int32_t dr[4] = {x, y, x + sr[2] - sr[0], y + sr[3] - sr[1]}, ox = sr[0] - dr[0], oy = sr[1] - dr[1];
    if (!clip_to(dr, me)) RET(DD_OK, 6);
    sr[0] = dr[0] + ox; sr[1] = dr[1] + oy; sr[2] = dr[2] + ox; sr[3] = dr[3] + oy;
    if (!clip_to(sr, src)) RET(DD_OK, 6);
    dr[0] = sr[0] - ox; dr[1] = sr[1] - oy; dr[2] = sr[2] - ox; dr[3] = sr[3] - oy;
    int key = (ARG(5) & DDBLTFAST_SRCCOLORKEY) && src->has_ck;
    gpu_readback(src); copy_rect(me, dr, src, sr, key, src->ck_lo, src->ck_hi);
    if (me->gid) gpu_upload(me);
    shown_after_copy(c, me, dr); RET(DD_OK, 6);
END
M(s_Flip, 3) ME(PSurf, T_SURF)                                      /* (target override, flags): the back buffer becomes visible */
    PSurf *b = the(ARG(1), T_SURF); if (!b) b = me->back;
    if (!b) RET(DD_OK, 3);
    { static int n; if (n++ < 6) dlog("Flip %ux%u (GPU %u, back %u)", me->w, me->h, me->gid, b->gid); }
    if (b->gid) {                                                     /* Direct3D: the back buffer as rendered */
        {   /* tests (DS_TEST, W32_BURST_DIR=<dir>): when <dir>/go appears, the next 12 frames as raw BGRA, f<n>-<ms>.bgra */
            static int burst = -1, on = -1; if (on < 0) on = getenv("DS_TEST") && getenv("W32_BURST_DIR");
            if (on) { char f[600]; const char *bd = getenv("W32_BURST_DIR"); snprintf(f, sizeof f, "%s/go", bd);
                if (burst < 0 && access(f, 0) == 0) { burst = 0; unlink(f); }
                if (burst >= 0) { uint32_t *px = malloc((size_t)b->w * b->h * 4); int32_t r[4] = {0, 0, (int32_t)b->w, (int32_t)b->h};
                    if (px && cmd_readback(b->gid, r, (uint8_t *)px, b->w * 4, 4)) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                        snprintf(f, sizeof f, "%s/f%02d-%ld.bgra", bd, burst, (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
                        FILE *o = fopen(f, "wb"); if (o) { fwrite(px, 4, (size_t)b->w * b->h, o); fclose(o); } }
                    free(px); if (++burst == 12) burst = -1; } } }
        ddraw_pal_frames++; dsr_present(c, b->gid); RET(DD_OK, 3);
    }
    uint32_t t = me->mem; me->mem = b->mem; b->mem = t;              /* the surfaces keep their identities; their pixels swap */
    show(c, me, 0, 0, me->w, me->h);
    RET(DD_OK, 3);
END
M(s_GetAttachedSurface, 3) ME(PSurf, T_SURF)                        /* (DDSCAPS(2) *, out) */
    uint32_t want = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    PSurf *a = (want & DDSCAPS_BACKBUFFER) || (want & DDSCAPS_FLIP) ? me->back : (want & DDSCAPS_FRONTBUFFER) ? me->front : me->back;
    if (!a) { rt_w32(G_MEM, out, 0); RET(DDERR_NOTFOUND, 3); }
    int v = 0; uint32_t mine = rt_r32(G_MEM, ARG(0)); for (int k = 0; k < 4; k++) if (vt_surf[k] == mine) v = k;
    __atomic_add_fetch(&a->ref, 1, __ATOMIC_SEQ_CST); rt_w32(G_MEM, out, a->g[v]); RET(DD_OK, 3);
END
M(s_GetCaps, 2) ME(PSurf, T_SURF) rt_w32(G_MEM, ARG(1), me->caps); RET(DD_OK, 2); END
M(s_GetSurfaceDesc, 2) ME(PSurf, T_SURF) desc_out(me, ARG(1), 0); RET(DD_OK, 2); END
M(s_GetPixelFormat, 2) ME(PSurf, T_SURF) for (int k = 0; k < 8; k++) rt_w32(G_MEM, ARG(1) + 4 * (uint32_t)k, me->pf[k]); RET(DD_OK, 2); END
M(s_Lock, 5) ME(PSurf, T_SURF)                                      /* (rect, desc, flags, event) */
    uint32_t d = ARG(2); { static int n; if (me->gid && me->gkind != 3 && n++ < 8) dlog("Lock of GPU %u (%ux%u caps %x kind %d)", me->gid, me->w, me->h, me->caps, me->gkind); }
    gpu_readback(me); desc_out(me, d, 1);
    if (ARG(1)) { int32_t r[4]; rect_in(ARG(1), me, r); rt_w32(G_MEM, d + SD_SURFACE, me->mem + (uint32_t)r[1] * me->pitch + (uint32_t)r[0] * (me->bpp / 8)); }
    me->locked++; RET(DD_OK, 5);
END
M(s_Unlock, 2) ME(PSurf, T_SURF)
    if (me->locked) me->locked--;
    if (me->gid) {                                                    /* a texture or render target: the GPU's copy follows */
        gpu_upload(me);
        if (visible_of(me) && !me->back) { ddraw_pal_frames++; dsr_present(c, me->gid); }   /* drawn straight onto a single primary (cutscenes) */
        RET(DD_OK, 2);
    }
    if (visible_of(me) && !me->back) show(c, me, 0, 0, me->w, me->h);   /* drawn straight onto a single primary */
    RET(DD_OK, 2);
END
M(s_IsLost, 1) RET(DD_OK, 1); END
M(s_Restore, 1) RET(DD_OK, 1); END
M(s_SetPalette, 2) ME(PSurf, T_SURF)
    PPal *p = the(ARG(1), T_PAL);
    if (p) __atomic_add_fetch(&p->ref, 1, __ATOMIC_SEQ_CST);
    me->pal = p; if (me->back) me->back->pal = p;
    if (me->gid && me->gkind == 3) gpu_upload(me);
    RET(DD_OK, 2);
END
M(s_GetPalette, 2) ME(PSurf, T_SURF)
    if (!me->pal) { rt_w32(G_MEM, ARG(1), 0); RET(DDERR_NOPALETTEATTACHED, 2); }
    __atomic_add_fetch(&me->pal->ref, 1, __ATOMIC_SEQ_CST); rt_w32(G_MEM, ARG(1), me->pal->g); RET(DD_OK, 2);
END
M(s_SetClipper, 2) ME(PSurf, T_SURF) me->clip = the(ARG(1), T_CLIP); RET(DD_OK, 2); END
M(s_GetClipper, 2) ME(PSurf, T_SURF)
    if (!me->clip) { rt_w32(G_MEM, ARG(1), 0); RET(DDERR_NOCLIPPERATTACHED, 2); }
    rt_w32(G_MEM, ARG(1), me->clip->g); RET(DD_OK, 2);
END
M(s_SetColorKey, 3) ME(PSurf, T_SURF)                               /* (flags, DDCOLORKEY *) */
    if (ARG(1) & DDCKEY_SRCBLT) {
        if (ARG(2)) { me->ck_lo = rt_r32(G_MEM, ARG(2)); me->ck_hi = rt_r32(G_MEM, ARG(2) + 4); me->has_ck = 1; } else me->has_ck = 0;
        if (me->gid && me->gkind == 3) gpu_upload(me);
    }
    RET(DD_OK, 3);
END
M(s_GetColorKey, 3) ME(PSurf, T_SURF)
    if (!(ARG(1) & DDCKEY_SRCBLT) || !me->has_ck) RET(DDERR_NOCOLORKEY, 3);
    rt_w32(G_MEM, ARG(2), me->ck_lo); rt_w32(G_MEM, ARG(2) + 4, me->ck_hi); RET(DD_OK, 3);
END
M(s_GetDDInterface, 2) ME(PSurf, T_SURF) rt_w32(G_MEM, ARG(1), me->dd ? me->dd->g[0] : 0); RET(DD_OK, 2); END
M(s_GetBltStatus, 2) RET(DD_OK, 2); END
M(s_AddAttachedSurface, 2) ME(PSurf, T_SURF)                         /* (surface): a depth buffer for a render target */
    PSurf *z = the(ARG(1), T_SURF); if (!z) RET(DDERR_INVALIDPARAMS, 2);
    if (z->caps & 0x20000) { __atomic_add_fetch(&z->ref, 1, __ATOMIC_SEQ_CST); me->zbuf = z; if (me->dd && me->dd->d3d) gpu_attach(z, 2); RET(DD_OK, 2); }
    dlog("AddAttachedSurface: caps %x not a depth buffer", z->caps); RET(DDERR_UNSUPPORTED, 2);
END
M(s_GetFlipStatus, 2) RET(DD_OK, 2); END
M(s_GetUniquenessValue, 2) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
NOT_M(s_AddOverlayDirtyRect, 2) NOT_M(s_BltBatch, 4) NOT_M(s_DeleteAttachedSurface, 3)
NOT_M(s_EnumAttachedSurfaces, 3) NOT_M(s_EnumOverlayZOrders, 4) NOT_M(s_GetDC, 2) NOT_M(s_ReleaseDC, 2)
NOT_M(s_GetOverlayPosition, 3) NOT_M(s_Initialize, 3) NOT_M(s_SetOverlayPosition, 3) NOT_M(s_UpdateOverlay, 6)
NOT_M(s_UpdateOverlayDisplay, 2) NOT_M(s_UpdateOverlayZOrder, 3) OK_M(s_PageLock, 2) OK_M(s_PageUnlock, 2) NOT_M(s_SetSurfaceDesc, 3)
NOT_M(s_SetPrivateData, 5) NOT_M(s_GetPrivateData, 4) NOT_M(s_FreePrivateData, 2) OK_M(s_ChangeUniquenessValue, 1)
NOT_M(s_SetPriority, 2) NOT_M(s_GetPriority, 2) NOT_M(s_SetLOD, 2) NOT_M(s_GetLOD, 2)

/* gamma: the game's brightness setting as the renderer's ramp */
static uint16_t gamma_ramp[3][256]; static int gamma_set;
M(g_QueryInterface, 3) ME(PSurf, T_GAMMA)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (iid == 0x69c11c3eu) { rt_w32(G_MEM, out, me->g_gamma); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    int v = vidx_of_surf_iid(iid); if (v >= 0) { rt_w32(G_MEM, out, me->g[v]); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(g_AddRef, 1) ME(PSurf, T_GAMMA) RET(__atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST), 1); END
M(g_Release, 1) ME(PSurf, T_GAMMA) RET(surf_release(me), 1); END
M(g_GetGammaRamp, 3)
    if (!gamma_set) for (int k = 0; k < 256; k++) gamma_ramp[0][k] = gamma_ramp[1][k] = gamma_ramp[2][k] = (uint16_t)(k * 0x101);
    memcpy(GP(ARG(2)), gamma_ramp, 1536); RET(DD_OK, 3);
END
M(g_SetGammaRamp, 3)
    memcpy(gamma_ramp, GP(ARG(2)), 1536); gamma_set = 1;
    uint32_t *p = cmd_begin(DSR_GAMMA, 1536); memcpy(p, gamma_ramp, 1536); cmd_end(); RET(DD_OK, 3);
END

/* ---- palettes ---- */
M(p_QueryInterface, 3) ME(PPal, T_PAL)
    uint32_t iid = rt_r32(G_MEM, ARG(1));
    if (iid == 0x6c14db84u || iid == 0) { rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(E_NOINTERFACE, 3);
END
M(p_AddRef, 1) ME(PPal, T_PAL) RET(__atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST), 1); END
M(p_Release, 1) ME(PPal, T_PAL) int r = __atomic_sub_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(r < 0 ? 0 : r, 1); END   /* (kept: surfaces may still point at it) */
M(p_GetCaps, 2) ME(PPal, T_PAL) rt_w32(G_MEM, ARG(1), me->caps); RET(DD_OK, 2); END
M(p_GetEntries, 5) ME(PPal, T_PAL)                                  /* (flags, base, count, out) */
    uint32_t b = ARG(2), n = ARG(3), out = ARG(4);
    for (uint32_t k = 0; k < n && b + k < 256; k++) rt_w32(G_MEM, out + 4 * k, me->entries[b + k]);
    RET(DD_OK, 5);
END
M(p_SetEntries, 5) ME(PPal, T_PAL)                                  /* (flags, start, count, entries) */
    uint32_t s = ARG(2), n = ARG(3), in = ARG(4);
    for (uint32_t k = 0; k < n && s + k < 256; k++) me->entries[s + k] = rt_r32(G_MEM, in + 4 * k);
    RET(DD_OK, 5);
END
OK_M(p_Initialize, 4)

/* ---- clipper ---- */
M(cl_QueryInterface, 3) ME(PClip, T_CLIP) rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); END
M(cl_AddRef, 1) ME(PClip, T_CLIP) RET((uint32_t)++me->ref, 1); END
M(cl_Release, 1) ME(PClip, T_CLIP) RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END
NOT_M(cl_GetClipList, 4)
M(cl_GetHWnd, 2) ME(PClip, T_CLIP) rt_w32(G_MEM, ARG(1), me->hwnd); RET(DD_OK, 2); END
OK_M(cl_Initialize, 3)
M(cl_IsClipListChanged, 2) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 2); END
OK_M(cl_SetClipList, 3)
M(cl_SetHWnd, 3) ME(PClip, T_CLIP) me->hwnd = ARG(2); RET(DD_OK, 3); END

/* ---- IDirectDraw (1, 2, 4, 7) ---- */
static int vidx_of_dd_iid(uint32_t iid)
{
    switch (iid) { case 0x6c14db80u: return 0; case 0xb3a6f3e0u: return 1; case 0x9c59509au: return 2; case 0x15e65ec0u: return 3; }
    return -1;
}
M(dd_QueryInterface, 3) ME(PDraw, T_DD)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2); int v = vidx_of_dd_iid(iid);
    dlog("IDirectDraw QI %08x -> %s", iid, v >= 0 || iid == 0 ? "ok" : "no");
    if (v >= 0 || iid == 0) { rt_w32(G_MEM, out, me->g[v < 0 ? 0 : v]); me->ref++; RET(DD_OK, 3); }
    if (iid == 0xbb223240u) { me->d3d = 1; if (!me->g_d3d) me->g_d3d = iface(vt_d3d, T_D3D, me); rt_w32(G_MEM, out, me->g_d3d); me->ref++; RET(DD_OK, 3); }   /* IDirect3D3 */
    rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(dd_AddRef, 1) ME(PDraw, T_DD) RET((uint32_t)++me->ref, 1); END
M(dd_Release, 1) ME(PDraw, T_DD) RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END
OK_M(dd_Compact, 1)
M(dd_CreateClipper, 4) ME(PDraw, T_DD)
    PClip *k = calloc(1, sizeof *k); k->ref = 1; k->g = iface(vt_clip, T_CLIP, k);
    rt_w32(G_MEM, ARG(2), k->g); RET(DD_OK, 4);
END
M(dd_CreatePalette, 5) ME(PDraw, T_DD)                              /* (flags, entries, out, outer) */
    PPal *p = calloc(1, sizeof *p); p->ref = 1; p->caps = ARG(1); p->g = iface(vt_pal, T_PAL, p);
    if (ARG(2)) for (int k = 0; k < 256; k++) p->entries[k] = rt_r32(G_MEM, ARG(2) + 4 * (uint32_t)k);
    rt_w32(G_MEM, ARG(3), p->g); RET(DD_OK, 5);
END
static int vidx_of_this(Ctx *c, const uint32_t *tab) { uint32_t mine = rt_r32(G_MEM, ARG(0)); for (int k = 0; k < 4; k++) if (tab[k] == mine) return k; return 0; }
M(dd_CreateSurface, 4) ME(PDraw, T_DD)                              /* (desc, out, outer) */
    uint32_t d = ARG(1), fl = rt_r32(G_MEM, d + SD_FLAGS), caps = (fl & DDSD_CAPS) ? rt_r32(G_MEM, d + SD_CAPS) : 0;
    uint32_t w = (fl & DDSD_WIDTH) ? rt_r32(G_MEM, d + SD_WIDTH) : 0, h = (fl & DDSD_HEIGHT) ? rt_r32(G_MEM, d + SD_HEIGHT) : 0;
    uint32_t bpp = (fl & DDSD_PIXELFORMAT) ? rt_r32(G_MEM, d + SD_PF + PF_BITS) : me->mode_bpp;
    int v = vidx_of_this(c, vt_dd); int sv = v >= 2 ? v : (v == 1 ? 1 : 0);
    if (caps & DDSCAPS_PRIMARYSURFACE) { w = me->mode_w; h = me->mode_h; bpp = me->mode_bpp; }
    if (!w || !h || (bpp != 8 && bpp != 16 && bpp != 32)) { dlog("CreateSurface %ux%ux%u caps %x refused", w, h, bpp, caps); RET(DDERR_INVALIDPARAMS, 4); }
    uint32_t sc = caps | ((caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
    if (caps & DDSCAPS_PRIMARYSURFACE) sc |= DDSCAPS_VISIBLE | ((caps & DDSCAPS_FLIP) ? DDSCAPS_FRONTBUFFER : 0);
    PSurf *s = surf_new(me, w, h, bpp, sc);
    if ((fl & DDSD_PIXELFORMAT) && !(caps & DDSCAPS_PRIMARYSURFACE)) for (int k = 1; k < 8; k++) s->pf[k] = rt_r32(G_MEM, d + SD_PF + 4 * (uint32_t)k);
    if (fl & DDSD_CKSRCBLT) { s->ck_lo = rt_r32(G_MEM, d + SD_CKSRCBLT); s->ck_hi = rt_r32(G_MEM, d + SD_CKSRCBLT + 4); s->has_ck = 1; }
    if ((caps & DDSCAPS_FLIP) && (fl & DDSD_BACKBUFFERCOUNT) && rt_r32(G_MEM, d + SD_BACKBUFFERS)) {
        PSurf *b = surf_new(me, w, h, bpp, (sc & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
        s->back = b; b->front = s; b->back = s;
    }
    if (me->d3d) {                                                    /* Direct3D: textures, render targets, depth on the GPU */
        if (caps & 0x1000) { gpu_attach(s, 3); s->g_tex = iface(vt_tex, T_TEX, s); }          /* DDSCAPS_TEXTURE */
        else if (caps & 0x20000) gpu_attach(s, 2);                                             /* DDSCAPS_ZBUFFER */
        else if (caps & (0x2000 | DDSCAPS_PRIMARYSURFACE)) { gpu_attach(s, 1); if (s->back) gpu_attach(s->back, 1); }   /* DDSCAPS_3DDEVICE */
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) me->primary = s;
    dlog("CreateSurface %ux%ux%u caps %x%s", w, h, bpp, caps, s->back ? " (with a back buffer)" : "");
    rt_w32(G_MEM, ARG(2), s->g[sv]); RET(DD_OK, 4);
END
NOT_M(dd_DuplicateSurface, 3)
static const uint16_t modes[][2] = {{640, 480}, {800, 600}, {1024, 768}};
M(dd_EnumDisplayModes, 5) ME(PDraw, T_DD)                           /* (flags, desc filter, ctx, callback) */
    uint32_t filter = ARG(2), ctx = ARG(3), cb = ARG(4), v = (uint32_t)vidx_of_this(c, vt_dd), size = v >= 2 ? 124 : 108;
    uint32_t d = heap_alloc(w32_process_heap, 8, 124);
    static const uint32_t depths[] = {8, 16, 32};
    for (int di = 0; di < 3; di++) for (size_t k = 0; k < sizeof modes / sizeof *modes; k++) {
        uint32_t w = modes[k][0], h = modes[k][1], bpp = depths[di];
        if (filter) { uint32_t ff = rt_r32(G_MEM, filter + SD_FLAGS);
            if ((ff & DDSD_WIDTH) && rt_r32(G_MEM, filter + SD_WIDTH) != w) continue;
            if ((ff & DDSD_HEIGHT) && rt_r32(G_MEM, filter + SD_HEIGHT) != h) continue;
            if ((ff & DDSD_PIXELFORMAT) && rt_r32(G_MEM, filter + SD_PF + PF_BITS) != bpp) continue; }
        memset(GP(d), 0, 124); rt_w32(G_MEM, d, size);
        rt_w32(G_MEM, d + SD_FLAGS, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
        rt_w32(G_MEM, d + SD_WIDTH, w); rt_w32(G_MEM, d + SD_HEIGHT, h); rt_w32(G_MEM, d + SD_PITCH, w * bpp / 8); rt_w32(G_MEM, d + SD_REFRESH, 60);
        uint32_t pf[8]; pf_for(pf, bpp); for (int j = 0; j < 8; j++) rt_w32(G_MEM, d + SD_PF + 4 * (uint32_t)j, pf[j]);
        uint32_t a[2] = {d, ctx};
        if (w32_callback(c, cb, 2, a) == 0) goto done;
    }
done:
    heap_free(w32_process_heap, d); RET(DD_OK, 5);
END
NOT_M(dd_EnumSurfaces, 5)
OK_M(dd_FlipToGDISurface, 1)
M(dd_GetCaps, 3)                                                    /* (driver caps, emulation caps): DDCAPS, as much as the caller's size takes */
    for (int k = 1; k <= 2; k++) {
        uint32_t p = ARG(k); if (!p) continue;
        uint32_t sz = rt_r32(G_MEM, p); if (sz < 68 || sz > 512) continue;
        memset(GP(p + 4), 0, sz - 4);
        rt_w32(G_MEM, p + 4, 0x1 | 0x40 | 0x200 | 0x8000 | 0x400000 | 0x4000000 | 0x80000000u);   /* 3D, BLT, BLTSTRETCH, PALETTE, COLORKEY, BLTCOLORFILL, CANBLTSYSMEM */
        if (sz >= 108) rt_w32(G_MEM, p + 104, 0x8 | 0x10 | 0x200 | 0x1000 | 0x2000 | 0x4000 | 0x20000 | 0x10000000);   /* surfaces: complex, flip, primary, texture, 3D device, video memory, z-buffer, local */
        rt_w32(G_MEM, p + 12, 0x200);                               /* DDCKEYCAPS_SRCBLT */
        rt_w32(G_MEM, p + 24, 4 | 0x10 | 0x400);                    /* DDPCAPS_8BIT, PRIMARYSURFACE, ALLOW256 */
        rt_w32(G_MEM, p + 60, 256u << 20); rt_w32(G_MEM, p + 64, 240u << 20);
    }
    RET(DD_OK, 3);
END
M(dd_GetDisplayMode, 2) ME(PDraw, T_DD)
    uint32_t d = ARG(1), size = rt_r32(G_MEM, d); if (size != 124) size = 108;
    memset(GP(d), 0, size); rt_w32(G_MEM, d, size);
    rt_w32(G_MEM, d + SD_FLAGS, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    rt_w32(G_MEM, d + SD_WIDTH, me->mode_w); rt_w32(G_MEM, d + SD_HEIGHT, me->mode_h); rt_w32(G_MEM, d + SD_PITCH, me->mode_w * me->mode_bpp / 8);
    rt_w32(G_MEM, d + SD_REFRESH, 60);
    uint32_t pf[8]; pf_for(pf, me->mode_bpp); for (int j = 0; j < 8; j++) rt_w32(G_MEM, d + SD_PF + 4 * (uint32_t)j, pf[j]);
    RET(DD_OK, 2);
END
M(dd_GetFourCCCodes, 3) if (ARG(1)) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 3); END
M(dd_GetGDISurface, 2) ME(PDraw, T_DD)
    if (!me->primary) { rt_w32(G_MEM, ARG(1), 0); RET(DDERR_NOTFOUND, 2); }
    __atomic_add_fetch(&me->primary->ref, 1, __ATOMIC_SEQ_CST); rt_w32(G_MEM, ARG(1), me->primary->g[0]); RET(DD_OK, 2);
END
M(dd_GetMonitorFrequency, 2) rt_w32(G_MEM, ARG(1), 60); RET(DD_OK, 2); END
M(dd_GetScanLine, 2) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 2); END
M(dd_GetVerticalBlankStatus, 2) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
OK_M(dd_Initialize, 2)
M(dd_RestoreDisplayMode, 1) ME(PDraw, T_DD) me->mode_w = (uint32_t)w32_screen_w; me->mode_h = (uint32_t)w32_screen_h; me->mode_bpp = 32; RET(DD_OK, 1); END
M(dd_SetCooperativeLevel, 3) ME(PDraw, T_DD)
    dlog("SetCooperativeLevel hwnd %08x flags %08x", ARG(1), ARG(2));
    if (ARG(1)) me->hwnd = ARG(1);
    me->coop = ARG(2); RET(DD_OK, 3);
END
static uint32_t set_mode(Ctx *c, PDraw *me, uint32_t w, uint32_t h, uint32_t bpp)
{
    dlog("SetDisplayMode %ux%ux%u", w, h, bpp);
    if (bpp != 8 && bpp != 16 && bpp != 32) return DDERR_UNSUPPORTED;
    me->mode_w = w; me->mode_h = h; me->mode_bpp = bpp; dsr_d3d9_w = w; dsr_d3d9_h = h;
    if (me->hwnd && (me->coop & DDSCL_EXCLUSIVE)) w32_set_window_pos(c, me->hwnd, 0, 0, (int)w, (int)h, 0x40 | 0x10);
    uint32_t *p = cmd_begin(DSR_MODE, 8); p[0] = w; p[1] = h; cmd_end();
    return DD_OK;
}
M(dd_SetDisplayMode1, 4) ME(PDraw, T_DD) RET(set_mode(c, me, ARG(1), ARG(2), ARG(3)), 4); END   /* IDirectDraw: (w, h, bpp) */
M(dd_SetDisplayMode, 6) ME(PDraw, T_DD) RET(set_mode(c, me, ARG(1), ARG(2), ARG(3)), 6); END    /* later: (w, h, bpp, refresh, flags) */
OK_M(dd_WaitForVerticalBlank, 3)
M(dd_GetAvailableVidMem, 4) if (ARG(2)) rt_w32(G_MEM, ARG(2), 256u << 20); if (ARG(3)) rt_w32(G_MEM, ARG(3), 240u << 20); RET(DD_OK, 4); END
NOT_M(dd_GetSurfaceFromDC, 3) OK_M(dd_RestoreAllSurfaces, 1) OK_M(dd_TestCooperativeLevel, 1)
M(dd_GetDeviceIdentifier, 3)
    uint32_t p = ARG(1); memset(GP(p), 0, 1060);
    strcpy((char *)GP(p), "display"); strcpy((char *)GP(p + 512), "Apple Metal (Diablo II native)");
    RET(DD_OK, 3);
END
NOT_M(dd_StartModeTest, 4) NOT_M(dd_EvaluateMode, 3)

/* ---- Direct3D 3 (DirectX 6): the game's Direct3D renderer. It projects in its own code and draws pre-transformed,
 * textured triangles; here they become the Metal renderer's draws into the 3D back buffer (gid), which Flip presents.
 * The device's DirectX 5/6 states that Direct3D 7 dropped (texture blend mode, filters, colour key, texture handle)
 * become texture-stage states. ---- */
uint32_t fvf_stride(uint32_t fvf);                                       /* ddraw.c */
extern uint32_t dsr_d3d9_w, dsr_d3d9_h;
typedef struct PVp { int ref; uint32_t g; uint32_t data[11]; int set; } PVp;   /* D3DVIEWPORT2 */
typedef struct PDev {
    int ref; uint32_t g; PDraw *dd; PSurf *rt; PVp *vp; PVp *vps[8]; int nvp;
    uint32_t rs[256], tss[8][32]; PSurf *tex[8];
    uint32_t bound_rt, bound_z, bound_tex[8];                             /* what the renderer has */
} PDev;
static uint32_t surf_release(PSurf *s);
static void dev_set_tex(PDev *d, int stage, PSurf *t)                    /* the device holds a reference to what it has bound */
{
    if (d->tex[stage] == t) return;
    if (t) __atomic_add_fetch(&t->ref, 1, __ATOMIC_SEQ_CST);
    PSurf *old = d->tex[stage]; d->tex[stage] = t;
    if (old) { if (d->bound_tex[stage] == old->gid && old->ref == 1) { uint32_t *p = cmd_begin(DSR_TEXTURE, 8); p[0] = (uint32_t)stage; p[1] = 0; cmd_end(); d->bound_tex[stage] = 0; }
               surf_release(old); }
}
static void rs_emit(uint32_t st, uint32_t v) { uint32_t *p = cmd_begin(DSR_RENDER_STATE, 8); p[0] = st; p[1] = v; cmd_end(); }
static void tss_emit(uint32_t stage, uint32_t t, uint32_t v)
{
    if ((t == 16 || t == 17) && smooth_textures()) v = 2;                 /* smooth textures: bilinear, whatever the game asks */
    uint32_t *p = cmd_begin(DSR_TSS, 12); p[0] = stage; p[1] = t; p[2] = v; cmd_end();
}
static void dev_alpha_test(PDev *d)                                     /* the game's alpha test, or the colour key's */
{
    uint32_t half = smooth_textures() ? 0x7f : 0;                     /* smooth textures: a cut-out's edge where it is half covered */
    if (d->rs[15]) { uint32_t f = d->rs[25] ? d->rs[25] : 8; rs_emit(15, 1); rs_emit(24, (f == 5 || f == 7) && d->rs[24] < half ? half : d->rs[24]); rs_emit(25, f); }
    else if (d->rs[41]) { rs_emit(15, 1); rs_emit(24, half); rs_emit(25, 5); }  /* colour-keyed texels are transparent: alpha > 0 */
    else rs_emit(15, 0);
}
static void dev_texture_blend(PDev *d)                                  /* D3DRENDERSTATE_TEXTUREMAPBLEND as stage 0 operations */
{
    enum { DIS = 1, ARG1 = 2, ARG2 = 3, MOD = 4, ADD = 7, BLENDTEX = 13, TA_DIFFUSE = 0, TA_TEXTURE = 2 };
    uint32_t m = d->rs[21], cop = MOD, aop = MOD;
    PSurf *t = d->tex[0]; int talpha = t && ((t->pf[1] & 1) || t->has_ck);
    switch (m) {
    case 1: case 7: cop = ARG1; aop = ARG1; break;                       /* DECAL, COPY */
    case 2: cop = MOD; aop = talpha ? ARG1 : ARG2; break;                /* MODULATE: the texture's alpha if it has one */
    case 3: cop = BLENDTEX; aop = ARG2; break;                           /* DECALALPHA */
    case 4: cop = MOD; aop = MOD; break;                                 /* MODULATEALPHA */
    case 8: cop = ADD; aop = ARG2; break;                                /* ADD */
    default: return;
    }
    if (!t) { cop = ARG2; aop = ARG2; }
    tss_emit(0, 1, cop); tss_emit(0, 2, TA_TEXTURE); tss_emit(0, 3, TA_DIFFUSE);
    tss_emit(0, 4, aop); tss_emit(0, 5, TA_TEXTURE); tss_emit(0, 6, TA_DIFFUSE);
    tss_emit(1, 1, DIS); tss_emit(1, 4, DIS);
}
static void dev_bind(PDev *d)                                          /* the render target, depth and textures, before a draw */
{
    uint32_t rt = d->rt ? d->rt->gid : 0, z = d->rt && d->rt->zbuf && d->rs[7] ? d->rt->zbuf->gid : 0;
    if (rt != d->bound_rt || z != d->bound_z) { uint32_t *p = cmd_begin(DSR_SET_RT, 8); p[0] = rt; p[1] = z; cmd_end(); d->bound_rt = rt; d->bound_z = z; }
    for (int k = 0; k < 2; k++) {
        if (d->tex[k] && !d->tex[k]->gid) { gpu_attach(d->tex[k], 3); gpu_upload(d->tex[k]); }   /* bound before it had a GPU copy */
        uint32_t id = d->tex[k] ? d->tex[k]->gid : 0;
        if (id != d->bound_tex[k]) { uint32_t *p = cmd_begin(DSR_TEXTURE, 8); p[0] = (uint32_t)k; p[1] = id; cmd_end(); d->bound_tex[k] = id; }
    }
}
static void dev_viewport(PDev *d)
{
    if (!d->vp || !d->vp->set) return;
    uint32_t *p = cmd_begin(DSR_VIEWPORT, 24); memcpy(p, d->vp->data + 1, 16); memcpy(p + 4, d->vp->data + 9, 8); cmd_end();
}
static uint32_t legacy_fvf(uint32_t vt)                                  /* D3DVT_VERTEX, LVERTEX, TLVERTEX, or already an FVF */
{
    return vt == 1 ? 0x112 : vt == 2 ? 0x1e2 : vt == 3 ? 0x1c4 : vt;
}
static void dev_draw(PDev *d, uint32_t prim, uint32_t vt, uint32_t verts, uint32_t nv, uint32_t idx, uint32_t ni)
{
    if (dd_log < 0) dd_log = getenv("DSR_LOG") != 0 || getenv("W32_DDLOG") != 0;
    if (dd_log) {   /* W32_DDLOG: each distinct combination of states the game draws with, once */
        static uint64_t seen[256]; static int ns; PSurf *t = d->tex[0];
        uint64_t key = (uint64_t)d->rs[27] | (uint64_t)d->rs[19] << 4 | (uint64_t)d->rs[20] << 8 | (uint64_t)d->rs[21] << 12 | (uint64_t)d->rs[15] << 16 |
                       (uint64_t)d->rs[41] << 17 | (uint64_t)(t ? t->bpp : 0) << 20 | (uint64_t)(t ? t->pf[7] & 0xffff : 0) << 28 | (uint64_t)(t && t->has_ck) << 44 |
                       (uint64_t)d->tss[0][1] << 45 | (uint64_t)d->tss[0][4] << 50 | (uint64_t)prim << 55 | (uint64_t)(vt & 0xf) << 60;
        int k = 0; while (k < ns && seen[k] != key) k++;
        if (k == ns && ns < 256) { seen[ns++] = key;
            dlog("draw state: fvf %x prim %u blend %u %u/%u texblend %u atest %u ckey %u | tex %ux%u bpp %u a-mask %x key %d | tss0 c%u(%u,%u) a%u(%u,%u) | %u verts", vt, prim, d->rs[27], d->rs[19], d->rs[20],
                 d->rs[21], d->rs[15], d->rs[41], t ? t->w : 0, t ? t->h : 0, t ? t->bpp : 0, t ? t->pf[7] : 0, t ? t->has_ck : 0, d->tss[0][1], d->tss[0][2], d->tss[0][3],
                 d->tss[0][4], d->tss[0][5], d->tss[0][6], nv); }
    }
    if (!d->rt || !d->rt->gid || !nv) return;
    uint32_t fvf = legacy_fvf(vt), stride = fvf_stride(fvf), vbytes = nv * stride, ibytes = (ni * 2 + 3) & ~3u;
    dev_bind(d);
    uint32_t *p = cmd_begin(DSR_DRAW, 16 + vbytes + ibytes);
    p[0] = prim; p[1] = fvf; p[2] = nv; p[3] = ni;
    memcpy(p + 4, G_MEM + verts, vbytes);
    if (ni) { memset((uint8_t *)(p + 4) + vbytes, 0, ibytes); memcpy((uint8_t *)(p + 4) + vbytes, G_MEM + idx, ni * 2); }
    cmd_end(); d->rt->gpu_newer = 1;
}
static void fill_caps(uint32_t p)                                       /* a D3DDEVICEDESC (DirectX 6, 252 bytes) */
{
    uint32_t size = rt_r32(G_MEM, p); if (size < 172 || size > 252) size = 252;
    memset(GP(p), 0, size); rt_w32(G_MEM, p, size);
    rt_w32(G_MEM, p + 4, 0x7ff);                                        /* every D3DDD_ field */
    rt_w32(G_MEM, p + 8, 2);                                            /* D3DCOLOR_RGB */
    rt_w32(G_MEM, p + 12, 0x1 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x80000);
    rt_w32(G_MEM, p + 16, 8); rt_w32(G_MEM, p + 20, 1);                  /* transform caps: clip */
    rt_w32(G_MEM, p + 24, 1);                                           /* clipping */
    rt_w32(G_MEM, p + 28, 16); rt_w32(G_MEM, p + 32, 7); rt_w32(G_MEM, p + 36, 1); rt_w32(G_MEM, p + 40, 8);
    for (uint32_t pc = p + 44; pc <= p + 100; pc += 56) {               /* line and triangle caps (D3DPRIMCAPS) */
        uint32_t v[14] = {56, 0x2 | 0x10 | 0x20 | 0x40, 0x1 | 0x10 | 0x20 | 0x80, 0xff, 0x1fff, 0x1fff, 0xff, 0x7ffff,
                          0x1 | 0x4 | 0x8 | 0x40 | 0x400, 0x0303033f, 0xff, 0xf, 0, 0};          /* textures: perspective, alpha, transparency; not pow2 or square only */
        for (int k = 0; k < 14; k++) rt_w32(G_MEM, pc + 4 * (uint32_t)k, v[k]);
    }
    rt_w32(G_MEM, p + 156, 0x400 | 0x100); rt_w32(G_MEM, p + 160, 0x400 | 0x100);   /* render and depth: 16 and 32 bits */
    rt_w32(G_MEM, p + 168, 65535);
    if (size >= 188) { rt_w32(G_MEM, p + 172, 1); rt_w32(G_MEM, p + 176, 1); rt_w32(G_MEM, p + 180, 4096); rt_w32(G_MEM, p + 184, 4096); }
    if (size >= 252) {
        rt_w32(G_MEM, p + 204, 4096); rt_w32(G_MEM, p + 208, 4096); rt_w32(G_MEM, p + 212, 1);
        rt_w32(G_MEM, p + 240, 2); rt_w32(G_MEM, p + 244, 0x3fff); rt_w16(G_MEM, p + 248, 2); rt_w16(G_MEM, p + 250, 2);
    }
}
static const uint8_t HAL_GUID[16] = {0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11, 0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e};
/* IDirect3DVertexBuffer: vertices in guest memory */
typedef struct PVB { int ref; uint32_t g, fvf, n, stride, mem; } PVB;
M(vb_QueryInterface, 3) ME(PVB, T_VB) rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); END
M(vb_AddRef, 1) ME(PVB, T_VB) RET((uint32_t)++me->ref, 1); END
M(vb_Release, 1) ME(PVB, T_VB)
    int r = --me->ref;
    if (r <= 0) { uint32_t k = rt_r32(G_MEM, me->g + 4); pthread_mutex_lock(&lk); objs[k].type = 0; free_obj[nfree_obj++] = k; pthread_mutex_unlock(&lk);
                  heap_free(w32_process_heap, me->mem); free(me); RET(0, 1); }
    RET((uint32_t)r, 1);
END
M(vb_Lock, 4) ME(PVB, T_VB)                                              /* (flags, data *, size *) */
    rt_w32(G_MEM, ARG(2), me->mem); if (ARG(3)) rt_w32(G_MEM, ARG(3), me->n * me->stride); RET(DD_OK, 4);
END
OK_M(vb_Unlock, 1)
M(vb_GetVertexBufferDesc, 2) ME(PVB, T_VB)
    uint32_t d = ARG(1); rt_w32(G_MEM, d, 16); rt_w32(G_MEM, d + 4, 0); rt_w32(G_MEM, d + 8, me->fvf); rt_w32(G_MEM, d + 12, me->n); RET(DD_OK, 2);
END
OK_M(vb_Optimize, 3) NOT_M(vb_ProcessVertices, 8)
/* IDirect3D3 */
M(d3_QueryInterface, 3) ME(PDraw, T_D3D)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2); int v = vidx_of_dd_iid(iid);
    if (iid == 0xbb223240u) { rt_w32(G_MEM, out, me->g_d3d); me->ref++; RET(DD_OK, 3); }
    if (v >= 0) { rt_w32(G_MEM, out, me->g[v]); me->ref++; RET(DD_OK, 3); }
    dlog("IDirect3D3 QI %08x: no", iid); rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(d3_AddRef, 1) ME(PDraw, T_D3D) RET((uint32_t)++me->ref, 1); END
M(d3_Release, 1) ME(PDraw, T_D3D) RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END
M(d3_EnumDevices, 3)                                                    /* (callback(guid, description, name, hal, hel, ctx), ctx) */
    uint32_t cb = ARG(1), ctx = ARG(2), b = heap_alloc(w32_process_heap, 8, 16 + 64 + 32 + 2 * 252);
    memcpy(GP(b), HAL_GUID, 16); strcpy((char *)GP(b + 16), "Apple Metal"); strcpy((char *)GP(b + 80), "Direct3D HAL");
    uint32_t hal = b + 112, hel = b + 112 + 252; rt_w32(G_MEM, hal, 252); fill_caps(hal); rt_w32(G_MEM, hel, 252);
    uint32_t a[6] = {b, b + 16, b + 80, hal, hel, ctx}; w32_callback(c, cb, 6, a);
    heap_free(w32_process_heap, b); RET(DD_OK, 3);
END
M(d3_CreateViewport, 3)
    PVp *v = calloc(1, sizeof *v); v->ref = 1; v->g = iface(vt_vp, T_VP, v); rt_w32(G_MEM, ARG(1), v->g); RET(DD_OK, 3);
END
M(d3_CreateDevice, 5) ME(PDraw, T_D3D)                                   /* (device guid, render target, out, outer) */
    PSurf *rt = the(ARG(2), T_SURF);
    if (!rt) RET(DDERR_INVALIDPARAMS, 5);
    if (!rt->gid) gpu_attach(rt, 1);
    PDev *d = calloc(1, sizeof *d); d->ref = 1; d->dd = me; d->rt = rt; d->g = iface(vt_dev, T_DEV, d);
    d->rs[7] = rt->zbuf ? 1 : 0; d->rs[14] = 1; d->rs[21] = 2; d->rs[22] = 3; d->rs[23] = 4; d->rs[25] = 8; d->rs[19] = 2; d->rs[20] = 1;
    for (int k = 0; k < 256; k++) if (d->rs[k]) rs_emit((uint32_t)k, d->rs[k]);
    rs_emit(22, 1);                                                   /* the game's 2D triangles come in either winding */
    for (uint32_t k = 0; k < 2; k++) { tss_emit(k, 16, 1); tss_emit(k, 17, 1); }   /* point sampling (or smooth textures' bilinear) */
    dsr_d3d9_w = rt->w; dsr_d3d9_h = rt->h;
    dlog("CreateDevice on %ux%ux%u (GPU %u)%s", rt->w, rt->h, rt->bpp, rt->gid, rt->zbuf ? " with depth" : "");
    rt_w32(G_MEM, ARG(3), d->g); RET(DD_OK, 5);
END
M(d3_EnumZBufferFormats, 4)                                              /* (device, callback(DDPIXELFORMAT, ctx), ctx) */
    uint32_t cb = ARG(2), ctx = ARG(3), pf = heap_alloc(w32_process_heap, 8, 32);
    static const uint32_t depths[] = {16, 32};
    for (int k = 0; k < 2; k++) {
        memset(GP(pf), 0, 32); rt_w32(G_MEM, pf, 32); rt_w32(G_MEM, pf + 4, 0x400); rt_w32(G_MEM, pf + 12, depths[k]);
        rt_w32(G_MEM, pf + 20, depths[k] == 16 ? 0xffff : 0xffffffffu);
        uint32_t a[2] = {pf, ctx}; if (!w32_callback(c, cb, 2, a)) break;
    }
    heap_free(w32_process_heap, pf); RET(DD_OK, 4);
END
OK_M(d3_EvictManagedTextures, 1)
NOT_M(d3_CreateLight, 3) NOT_M(d3_CreateMaterial, 3)
M(d3_FindDevice, 3)                                                     /* (D3DFINDDEVICESEARCH *, D3DFINDDEVICERESULT *): the HAL device */
    uint32_t r = ARG(2), size = rt_r32(G_MEM, r), desc = size > 20 ? (size - 20) / 2 : 0;   /* dwSize, GUID, hardware desc, software desc */
    if (desc < 172 || desc > 252) desc = 252;
    memcpy(GP(r + 4), HAL_GUID, 16);
    rt_w32(G_MEM, r + 20, desc); fill_caps(r + 20);
    memset(GP(r + 20 + desc), 0, desc); rt_w32(G_MEM, r + 20 + desc, desc);
    dlog("FindDevice: the HAL (descriptions %u bytes)", desc); RET(DD_OK, 3);
END
M(d3_CreateVertexBuffer, 5)                                              /* (D3DVERTEXBUFFERDESC, out, flags, outer) */
    uint32_t d = ARG(1); PVB *v = calloc(1, sizeof *v); v->ref = 1;
    v->fvf = legacy_fvf(rt_r32(G_MEM, d + 8)); v->n = rt_r32(G_MEM, d + 12); v->stride = fvf_stride(v->fvf);
    v->mem = heap_alloc(w32_process_heap, 8, v->n * v->stride + 16); v->g = iface(vt_vb, T_VB, v);
    dlog("CreateVertexBuffer: %u vertices, FVF %x (stride %u)", v->n, v->fvf, v->stride);
    rt_w32(G_MEM, ARG(2), v->g); RET(DD_OK, 5);
END
/* IDirect3DDevice3 */
M(dv_QueryInterface, 3) ME(PDev, T_DEV)
    uint32_t iid = rt_r32(G_MEM, ARG(1));
    if (iid == 0xb0ab3b60u || iid == 0) { rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); }
    dlog("IDirect3DDevice3 QI %08x: no", iid); rt_w32(G_MEM, ARG(2), 0); RET(E_NOINTERFACE, 3);
END
M(dv_AddRef, 1) ME(PDev, T_DEV) RET((uint32_t)++me->ref, 1); END
M(dv_Release, 1) ME(PDev, T_DEV)
    if (me->ref > 0 && --me->ref == 0) for (int k = 0; k < 8; k++) dev_set_tex(me, k, 0);   /* the textures it held */
    RET((uint32_t)me->ref, 1);
END
M(dv_GetCaps, 3) if (ARG(1)) fill_caps(ARG(1)); if (ARG(2)) { uint32_t sz = rt_r32(G_MEM, ARG(2)); memset(GP(ARG(2) + 4), 0, sz > 4 && sz <= 252 ? sz - 4 : 248); } RET(DD_OK, 3); END
M(dv_GetStats, 2) uint32_t sz = rt_r32(G_MEM, ARG(1)); if (sz >= 4 && sz <= 64) memset(GP(ARG(1) + 4), 0, sz - 4); RET(DD_OK, 2); END
M(dv_AddViewport, 2) ME(PDev, T_DEV)
    PVp *v = the(ARG(1), T_VP); if (!v) RET(DDERR_INVALIDPARAMS, 2);
    if (me->nvp < 8) me->vps[me->nvp++] = v; v->ref++; RET(DD_OK, 2);
END
M(dv_DeleteViewport, 2) ME(PDev, T_DEV)
    PVp *v = the(ARG(1), T_VP);
    for (int k = 0; k < me->nvp; k++) if (me->vps[k] == v) { me->vps[k] = me->vps[--me->nvp]; if (me->vp == v) me->vp = 0; RET(DD_OK, 2); }
    RET(DDERR_INVALIDPARAMS, 2);
END
M(dv_EnumTextureFormats, 3)                                              /* (callback(DDPIXELFORMAT, ctx), ctx) */
    uint32_t cb = ARG(1), ctx = ARG(2), pf = heap_alloc(w32_process_heap, 8, 32);
    static const uint32_t fmts[][6] = {                                  /* flags, bits, r, g, b, a */
        {0x40, 16, 0xf800, 0x07e0, 0x001f, 0}, {0x41, 16, 0x7c00, 0x03e0, 0x001f, 0x8000},
        {0x41, 16, 0x0f00, 0x00f0, 0x000f, 0xf000}, {0x41, 32, 0xff0000, 0xff00, 0xff, 0xff000000u}};
    for (int k = 0; k < 4; k++) {
        memset(GP(pf), 0, 32); rt_w32(G_MEM, pf, 32);
        for (int j = 0; j < 6; j++) rt_w32(G_MEM, pf + (j ? 8 + 4 * (uint32_t)j : 4), fmts[k][j]);
        uint32_t a[2] = {pf, ctx}; if (!w32_callback(c, cb, 2, a)) break;
    }
    heap_free(w32_process_heap, pf); RET(DD_OK, 3);
END
OK_M(dv_BeginScene, 1) OK_M(dv_EndScene, 1)
M(dv_GetDirect3D, 2) ME(PDev, T_DEV) rt_w32(G_MEM, ARG(1), me->dd->g_d3d); me->dd->ref++; RET(DD_OK, 2); END
M(dv_SetCurrentViewport, 2) ME(PDev, T_DEV) me->vp = the(ARG(1), T_VP); dev_viewport(me); RET(DD_OK, 2); END
M(dv_GetCurrentViewport, 2) ME(PDev, T_DEV) if (!me->vp) RET(DDERR_INVALIDPARAMS, 2); rt_w32(G_MEM, ARG(1), me->vp->g); me->vp->ref++; RET(DD_OK, 2); END
M(dv_SetRenderTarget, 3) ME(PDev, T_DEV)
    PSurf *t = the(ARG(1), T_SURF); if (!t) RET(DDERR_INVALIDPARAMS, 3);
    if (!t->gid) gpu_attach(t, 1);
    me->rt = t; RET(DD_OK, 3);
END
M(dv_GetRenderTarget, 2) ME(PDev, T_DEV) rt_w32(G_MEM, ARG(1), me->rt ? me->rt->g[2] : 0); if (me->rt) __atomic_add_fetch(&me->rt->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 2); END
M(dv_GetRenderState, 3) ME(PDev, T_DEV) rt_w32(G_MEM, ARG(2), ARG(1) < 256 ? me->rs[ARG(1)] : 0); RET(DD_OK, 3); END
M(dv_SetRenderState, 3) ME(PDev, T_DEV)
    uint32_t st = ARG(1), v = ARG(2); if (st >= 256) RET(DD_OK, 3);
    me->rs[st] = v;
    switch (st) {
    case 1: { PSurf *t = 0; for (uint32_t k = 1; k < nobjs && v; k++) if (objs[k].type == T_TEX && ((PSurf *)objs[k].p)->g_tex == v) t = objs[k].p;
              dev_set_tex(me, 0, t); dev_texture_blend(me); break; }               /* TEXTUREHANDLE */
    case 3: tss_emit(0, 12, v); break;                                    /* TEXTUREADDRESS */
    case 44: tss_emit(0, 13, v); break; case 45: tss_emit(0, 14, v); break;
    case 17: tss_emit(0, 16, v == 2 ? 2 : 1); break;                      /* TEXTUREMAG: D3DFILTER_ -> D3DTFG_ */
    case 18: tss_emit(0, 17, v == 1 || v == 3 ? 1 : 2); tss_emit(0, 18, v >= 3 ? (v == 3 || v == 5 ? 1 : 2) : 0); break;   /* TEXTUREMIN */
    case 21: dev_texture_blend(me); break;                                /* TEXTUREMAPBLEND */
    case 15: case 24: case 25: case 41: dev_alpha_test(me); break;        /* alpha test, colour key */
    case 22: rs_emit(22, 1); break;                                       /* (culling: the game's quads come in either winding) */
    case 2: case 4: case 9: case 10: case 11: case 12: case 13: case 16: case 26: case 29: case 30: case 31: case 32: case 33: case 39: case 40: break;
    default: rs_emit(st, v); break;
    }
    RET(DD_OK, 3);
END
OK_M(dv_SetLightState, 3) M(dv_GetLightState, 3) rt_w32(G_MEM, ARG(2), 0); RET(DD_OK, 3); END
M(dv_SetTransform, 3) { static int n; if (n++ < 6) dlog("SetTransform %u", ARG(1)); } RET(DD_OK, 3); END
M(dv_GetTransform, 3) memset(GP(ARG(2)), 0, 64); for (int k = 0; k < 4; k++) rt_w32(G_MEM, ARG(2) + 20 * (uint32_t)k, 0x3f800000u); RET(DD_OK, 3); END
OK_M(dv_MultiplyTransform, 3)
M(dv_DrawPrimitive, 6) ME(PDev, T_DEV) dev_draw(me, ARG(1), ARG(2), ARG(3), ARG(4), 0, 0); RET(DD_OK, 6); END
M(dv_DrawIndexedPrimitive, 8) ME(PDev, T_DEV) dev_draw(me, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)); RET(DD_OK, 8); END
OK_M(dv_SetClipStatus, 2) M(dv_GetClipStatus, 2) memset(GP(ARG(1)), 0, 36); RET(DD_OK, 2); END
M(dv_GetTexture, 3) ME(PDev, T_DEV) PSurf *t = ARG(1) < 8 ? me->tex[ARG(1)] : 0; rt_w32(G_MEM, ARG(2), t ? t->g_tex : 0); if (t) __atomic_add_fetch(&t->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); END
M(dv_SetTexture, 3) ME(PDev, T_DEV)                                      /* (stage, IDirect3DTexture2) */
    if (ARG(1) >= 8) RET(DDERR_INVALIDPARAMS, 3);
    dev_set_tex(me, (int)ARG(1), the(ARG(2), T_TEX));
    if (ARG(1) == 0 && me->rs[21]) dev_texture_blend(me);
    RET(DD_OK, 3);
END
M(dv_GetTextureStageState, 4) ME(PDev, T_DEV) rt_w32(G_MEM, ARG(3), ARG(1) < 8 && ARG(2) < 32 ? me->tss[ARG(1)][ARG(2)] : 0); RET(DD_OK, 4); END
M(dv_SetTextureStageState, 4) ME(PDev, T_DEV)
    if (ARG(1) < 8 && ARG(2) < 32) { me->tss[ARG(1)][ARG(2)] = ARG(3); me->rs[21] = 0; tss_emit(ARG(1), ARG(2), ARG(3)); }   /* (the stage states take over from the blend mode) */
    RET(DD_OK, 4);
END
M(dv_ValidateDevice, 2) if (ARG(1)) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
NOT_M(dv_NextViewport, 4) NOT_M(dv_Begin, 4) NOT_M(dv_BeginIndexed, 6) NOT_M(dv_Vertex, 2) NOT_M(dv_Index, 2) NOT_M(dv_End, 2)
NOT_M(dv_DrawPrimitiveStrided, 6) NOT_M(dv_DrawIndexedPrimitiveStrided, 8) NOT_M(dv_ComputeSphereVisibility, 6)
M(dv_DrawPrimitiveVB, 6) ME(PDev, T_DEV)                                  /* (prim, vertex buffer, first, count, flags) */
    PVB *v = the(ARG(2), T_VB); if (!v) RET(DDERR_INVALIDPARAMS, 6);
    dev_draw(me, ARG(1), v->fvf, v->mem + ARG(3) * v->stride, ARG(4), 0, 0); RET(DD_OK, 6);
END
M(dv_DrawIndexedPrimitiveVB, 6) ME(PDev, T_DEV)                           /* (prim, vertex buffer, indices, count, flags) */
    PVB *v = the(ARG(2), T_VB); if (!v) RET(DDERR_INVALIDPARAMS, 6);
    dev_draw(me, ARG(1), v->fvf, v->mem, v->n, ARG(3), ARG(4)); RET(DD_OK, 6);
END
/* IDirect3DViewport3 */
M(vp_QueryInterface, 3) ME(PVp, T_VP) rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); END
M(vp_AddRef, 1) ME(PVp, T_VP) RET((uint32_t)++me->ref, 1); END
M(vp_Release, 1) ME(PVp, T_VP) RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END
OK_M(vp_Initialize, 2)
static PDev *dev_of_vp(PVp *v) { for (uint32_t k = 1; k < nobjs; k++) if (objs[k].type == T_DEV) { PDev *d = objs[k].p; for (int j = 0; j < d->nvp; j++) if (d->vps[j] == v) return d; } return 0; }
M(vp_SetViewport2, 2) ME(PVp, T_VP)                                      /* D3DVIEWPORT2 (or D3DVIEWPORT: the same x, y, w, h, z range) */
    for (int k = 0; k < 11; k++) me->data[k] = rt_r32(G_MEM, ARG(1) + 4 * (uint32_t)k);
    me->set = 1; PDev *d = dev_of_vp(me); if (d && d->vp == me) dev_viewport(d);
    RET(DD_OK, 2);
END
M(vp_GetViewport2, 2) ME(PVp, T_VP) for (int k = 1; k < 11; k++) rt_w32(G_MEM, ARG(1) + 4 * (uint32_t)k, me->data[k]); RET(DD_OK, 2); END
static void vp_clear(PVp *v, uint32_t n, uint32_t rects, uint32_t flags, uint32_t color, uint32_t z)
{
    PDev *d = dev_of_vp(v); if (!d || !d->rt || !d->rt->gid) return;
    dev_bind(d);
    uint32_t *p = cmd_begin(DSR_CLEAR, 20 + 16 * n);
    p[0] = flags; p[1] = color; p[2] = z; p[3] = 0; p[4] = n; if (n) memcpy(p + 5, G_MEM + rects, 16 * n);
    cmd_end(); d->rt->gpu_newer = 1;
}
M(vp_Clear, 4) ME(PVp, T_VP) vp_clear(me, ARG(1), ARG(2), ARG(3), 0xff000000u, 0x3f800000u); RET(DD_OK, 4); END
M(vp_Clear2, 7) ME(PVp, T_VP) vp_clear(me, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)); RET(DD_OK, 7); END
OK_M(vp_SetBackground, 2) OK_M(vp_SetBackgroundDepth, 2) OK_M(vp_SetBackgroundDepth2, 2)
NOT_M(vp_TransformVertices, 5) NOT_M(vp_LightElements, 3) NOT_M(vp_GetBackground, 3) NOT_M(vp_GetBackgroundDepth, 3)
NOT_M(vp_AddLight, 2) NOT_M(vp_DeleteLight, 2) NOT_M(vp_NextLight, 4) NOT_M(vp_GetBackgroundDepth2, 3)
/* IDirect3DTexture2: the texture surface's other face */
M(tx_QueryInterface, 3) ME(PSurf, T_TEX)
    uint32_t iid = rt_r32(G_MEM, ARG(1)); int v = vidx_of_surf_iid(iid);
    if (iid == 0x93281502u) { rt_w32(G_MEM, ARG(2), me->g_tex); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    if (v >= 0) { rt_w32(G_MEM, ARG(2), me->g[v]); __atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); RET(DD_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(E_NOINTERFACE, 3);
END
M(tx_AddRef, 1) ME(PSurf, T_TEX) RET(__atomic_add_fetch(&me->ref, 1, __ATOMIC_SEQ_CST), 1); END
M(tx_Release, 1) ME(PSurf, T_TEX) RET(surf_release(me), 1); END
M(tx_GetHandle, 3) ME(PSurf, T_TEX) rt_w32(G_MEM, ARG(2), me->g_tex); RET(DD_OK, 3); END
M(tx_PaletteChanged, 3) ME(PSurf, T_TEX) gpu_upload(me); RET(DD_OK, 3); END
M(tx_Load, 2) ME(PSurf, T_TEX)                                           /* (source texture): its pixels into this one */
    PSurf *src = the(ARG(1), T_TEX); if (!src) RET(DDERR_INVALIDPARAMS, 2);
    uint32_t row = (src->w < me->w ? src->w : me->w) * (me->bpp / 8), h = src->h < me->h ? src->h : me->h;
    for (uint32_t y = 0; y < h; y++) memcpy(G_MEM + me->mem + y * me->pitch, G_MEM + src->mem + y * src->pitch, row);
    if (src->pal && !me->pal) me->pal = src->pal;
    if (src->has_ck) { me->has_ck = 1; me->ck_lo = src->ck_lo; me->ck_hi = src->ck_hi; }
    gpu_upload(me); RET(DD_OK, 2);
END

/* ---- vtables ---- */
typedef void (*Fn)(Ctx *);
static uint32_t vtable(const char *name, const Fn *fns, int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "ddraw!%s.%d", name, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
static void init_vtables(void)
{
    static int done; if (done) return; done = 1;
    Fn dd[30] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
        dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
        dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus,
        dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode1, dd_WaitForVerticalBlank,
        dd_GetAvailableVidMem, dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier,
        dd_StartModeTest, dd_EvaluateMode };
    vt_dd[0] = vtable("IDirectDraw", dd, 23);
    dd[21] = dd_SetDisplayMode;
    vt_dd[1] = vtable("IDirectDraw2", dd, 24); vt_dd[2] = vtable("IDirectDraw4", dd, 28); vt_dd[3] = vtable("IDirectDraw7", dd, 30);
    static const Fn surf[49] = { s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt, s_BltBatch, s_BltFast,
        s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip, s_GetAttachedSurface, s_GetBltStatus,
        s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus, s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat,
        s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
        s_SetOverlayPosition, s_SetPalette, s_Unlock, s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder,
        s_GetDDInterface, s_PageLock, s_PageUnlock, s_SetSurfaceDesc, s_SetPrivateData, s_GetPrivateData, s_FreePrivateData,
        s_GetUniquenessValue, s_ChangeUniquenessValue, s_SetPriority, s_GetPriority, s_SetLOD, s_GetLOD };
    vt_surf[0] = vtable("IDirectDrawSurface", surf, 36); vt_surf[1] = vtable("IDirectDrawSurface3", surf, 40);
    vt_surf[2] = vtable("IDirectDrawSurface4", surf, 45); vt_surf[3] = vtable("IDirectDrawSurface7", surf, 49);
    static const Fn pal[] = { p_QueryInterface, p_AddRef, p_Release, p_GetCaps, p_GetEntries, p_Initialize, p_SetEntries };
    vt_pal = vtable("IDirectDrawPalette", pal, 7);
    static const Fn clip[] = { cl_QueryInterface, cl_AddRef, cl_Release, cl_GetClipList, cl_GetHWnd, cl_Initialize, cl_IsClipListChanged, cl_SetClipList, cl_SetHWnd };
    vt_clip = vtable("IDirectDrawClipper", clip, 9);
    static const Fn gam[] = { g_QueryInterface, g_AddRef, g_Release, g_GetGammaRamp, g_SetGammaRamp };
    vt_gamma = vtable("IDirectDrawGammaControl", gam, 5);
    static const Fn d3[] = { d3_QueryInterface, d3_AddRef, d3_Release, d3_EnumDevices, d3_CreateLight, d3_CreateMaterial, d3_CreateViewport,
        d3_FindDevice, d3_CreateDevice, d3_CreateVertexBuffer, d3_EnumZBufferFormats, d3_EvictManagedTextures };
    static const Fn dv[] = { dv_QueryInterface, dv_AddRef, dv_Release, dv_GetCaps, dv_GetStats, dv_AddViewport, dv_DeleteViewport, dv_NextViewport,
        dv_EnumTextureFormats, dv_BeginScene, dv_EndScene, dv_GetDirect3D, dv_SetCurrentViewport, dv_GetCurrentViewport, dv_SetRenderTarget,
        dv_GetRenderTarget, dv_Begin, dv_BeginIndexed, dv_Vertex, dv_Index, dv_End, dv_GetRenderState, dv_SetRenderState, dv_GetLightState,
        dv_SetLightState, dv_SetTransform, dv_GetTransform, dv_MultiplyTransform, dv_DrawPrimitive, dv_DrawIndexedPrimitive, dv_SetClipStatus,
        dv_GetClipStatus, dv_DrawPrimitiveStrided, dv_DrawIndexedPrimitiveStrided, dv_DrawPrimitiveVB, dv_DrawIndexedPrimitiveVB,
        dv_ComputeSphereVisibility, dv_GetTexture, dv_SetTexture, dv_GetTextureStageState, dv_SetTextureStageState, dv_ValidateDevice };
    static const Fn vp[] = { vp_QueryInterface, vp_AddRef, vp_Release, vp_Initialize, vp_GetViewport2, vp_SetViewport2, vp_TransformVertices,
        vp_LightElements, vp_SetBackground, vp_GetBackground, vp_SetBackgroundDepth, vp_GetBackgroundDepth, vp_Clear, vp_AddLight,
        vp_DeleteLight, vp_NextLight, vp_GetViewport2, vp_SetViewport2, vp_SetBackgroundDepth2, vp_GetBackgroundDepth2, vp_Clear2 };
    static const Fn tx[] = { tx_QueryInterface, tx_AddRef, tx_Release, tx_GetHandle, tx_PaletteChanged, tx_Load };
    _Static_assert(sizeof d3 / sizeof *d3 == 12 && sizeof dv / sizeof *dv == 42 && sizeof vp / sizeof *vp == 21 && sizeof tx / sizeof *tx == 6, "DirectX 6 vtables");
    static const Fn vb[] = { vb_QueryInterface, vb_AddRef, vb_Release, vb_Lock, vb_Unlock, vb_ProcessVertices, vb_GetVertexBufferDesc, vb_Optimize };
    vt_vb = vtable("IDirect3DVertexBuffer", vb, 8);
    vt_d3d = vtable("IDirect3D3", d3, 12); vt_dev = vtable("IDirect3DDevice3", dv, 42); vt_vp = vtable("IDirect3DViewport3", vp, 21); vt_tex = vtable("IDirect3DTexture2", tx, 6);
}

/* ---- exports ---- */
IMPL(ddraw, DirectDrawCreate)                                       /* (GUID *, out, outer) */
{
    init_vtables();
    PDraw *dd = calloc(1, sizeof *dd); dd->ref = 1;
    dd->mode_w = (uint32_t)w32_screen_w; dd->mode_h = (uint32_t)w32_screen_h; dd->mode_bpp = 32;
    for (int v = 0; v < 4; v++) dd->g[v] = iface(vt_dd[v], T_DD, dd);
    dlog("DirectDrawCreate");
    rt_w32(G_MEM, ARG(1), dd->g[0]); RET(DD_OK, 3);
}
IMPL(ddraw, DirectDrawEnumerateA)                                   /* (callback(guid, description, name, ctx), ctx): the display */
{
    uint32_t buf = heap_alloc(w32_process_heap, 8, 64);
    strcpy((char *)GP(buf), "Primary Display Driver"); strcpy((char *)GP(buf + 32), "display");
    uint32_t a[4] = {0, buf, buf + 32, ARG(1)};
    w32_callback(c, ARG(0), 4, a);
    heap_free(w32_process_heap, buf); RET(DD_OK, 2);
}
