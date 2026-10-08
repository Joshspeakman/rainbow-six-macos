/* Direct3D 9 on the Metal renderer (src/renderer): the IDirect3D9 object, the device and its resources, as Dungeon
 * Siege II uses them. Drawing turns into the same command stream as the DS1 port's Direct3D 7 layer (ddraw.c): fixed-
 * function draws with an FVF vertex layout, render/texture-stage states (D3D7 numbering; sampler states are mapped
 * onto the D3D7 texture-stage ones), transforms, surfaces by renderer id. Textures keep a CPU copy in the game's own
 * format (what LockRect hands out) and are converted to BGRA8 when unlocked (d3d9_fmt.c). Vertex declarations are
 * turned into an FVF layout (gathered from their streams at draw time). Shaders: created and kept, not run yet (draws
 * with a vertex shader are skipped; with only a pixel shader they are drawn fixed-function).
 * D3D9_LOG=1: every method's first calls; any method not written yet says so once. */
#include "d3d9_priv.h"
#include "edition.h"
#include "../../../src/dsr/dsr_shm.h"
#include <pthread.h>
#include "display_modes.h"

static Dev9 *the_dev;
extern int w32_screen_w, w32_screen_h, w32_screen_hz;
static void surf_destroy(Surf9 *s);

/* ---- surfaces ---- */
/* the renderer's surface formats: 1 ARGB8, 2 XRGB8, 3 depth/stencil, 4-9 R16F, G16R16F, A16B16G16R16F, R32F, G32R32F, A32B32G32R32F */
static int fmt_code(uint32_t fmt) { return d9_fmt_depth(fmt) ? 3 : d9_fmt_float(fmt) ? 4 + (int)(fmt - 111) : d9_fmt_alpha(fmt) ? 1 : 2; }
/* samples for the next surf_new (render targets, depth buffers, the back buffer): D3DMULTISAMPLE_TYPE n (2-16) is n
 * samples; the renderer draws into multisampled storage and resolves when the surface is read */
static _Thread_local uint32_t new_samples;   /* per thread: loader threads create textures meanwhile */
static uint32_t ms_samples(uint32_t type) { return type >= 2 && type <= 16 ? type : 0; }
static Surf9 *surf_new(Dev9 *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool, int kind, Tex9 *tex, uint32_t level, uint32_t face, uint32_t parent_id, uint32_t levels)
{
    uint32_t samples = new_samples; new_samples = 0;
    Surf9 *s = calloc(1, sizeof *s); s->ref = 1; s->dev = d; s->tex = tex; s->level = level; s->face = face;
    s->w = w ? w : 1; s->h = h ? h : 1; s->fmt = fmt; s->usage = usage; s->pool = pool; s->kind = kind;
    s->pitch = d9_pitch(fmt, s->w); s->size = s->pitch * d9_rows(fmt, s->h); s->mem = gmem(s->size);
    s->g = make_iface(O9_SURF, s); s->id = next_object_id(); s->samples = samples;
    if (pool != 2 && pool != 3) {                       /* SYSTEMMEM / SCRATCH surfaces have no GPU copy */
        uint32_t *p = cmd_begin(DSR_SURFACE_CREATE, 40);
        p[0] = s->id; p[1] = s->w; p[2] = s->h; p[3] = (uint32_t)kind; p[4] = (uint32_t)fmt_code(fmt); p[5] = parent_id; p[6] = level; p[7] = levels;
        p[8] = face; p[9] = (tex && tex->type == O9_CUBE ? 1 : 0) | samples << 8;
        cmd_end();
    }
    return s;
}
static void surf_destroy(Surf9 *s)
{
    if (s->pool != 2 && s->pool != 3) { uint32_t *p = cmd_begin(DSR_SURFACE_DESTROY, 4); p[0] = s->id; cmd_end(); }
    surface_id_free(s->id); gfree_(s->mem, s->size); obj_drop(s->g); free(s);
}
/* the CPU copy's rows [y0, y1) to the GPU (converted to BGRA8, in bands that fit the command ring) */
static void surf_upload(Surf9 *s, uint32_t y0, uint32_t y1)
{
    if (s->pool == 2 || s->pool == 3 || s->kind == 2 || !d9_fmt_known(s->fmt) || d9_fmt_float(s->fmt)) return;
    if (d9_fmt_block(s->fmt)) { y0 &= ~3u; y1 = (y1 + 3) & ~3u; }
    if (y1 > s->h) y1 = s->h;
    uint32_t row = s->w * 4, band = (8u << 20) / row; if (band < 4) band = 4; band &= ~3u;
    for (uint32_t a = y0; a < y1; a += band) {
        uint32_t b = a + band < y1 ? a + band : y1;
        uint32_t *p = cmd_begin(DSR_SURFACE_UPLOAD, 24 + row * (b - a));
        p[0] = s->id; p[1] = 0; p[2] = a; p[3] = s->w; p[4] = b - a; p[5] = row;
        d9_to_bgra(s->fmt, G_MEM + s->mem, s->pitch, s->w, a, b, (uint8_t *)(p + 6));
        cmd_end();
    }
}
static void surf_readback(Surf9 *s)                    /* GPU -> CPU copy (render targets the game locks) */
{
    if (!s->gpu_newer || d9_fmt_bytes(s->fmt) != 4) return;
    int32_t r[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    cmd_readback(s->id, r, G_MEM + s->mem, s->pitch, 4);
    if (s->fmt == 22) for (uint32_t y = 0; y < s->h; y++) for (uint32_t x = 0; x < s->w; x++) G_MEM[s->mem + y * s->pitch + 4 * x + 3] = 0xff;
    s->gpu_newer = 0;
}
static uint32_t tex_release(Tex9 *t);
static uint32_t surf_addref(Surf9 *s) { if (s->tex) return (uint32_t)++s->tex->ref; return (uint32_t)++s->ref; }
static uint32_t surf_release(Surf9 *s)
{
    if (s->tex) return tex_release(s->tex);
    uint32_t r = (uint32_t)--s->ref; if (!r) surf_destroy(s); return r;
}
static uint32_t res_type(int t) { return t == O9_CUBE ? RT_CUBETEXTURE : t == O9_VOLTEX ? RT_VOLUMETEXTURE : RT_TEXTURE; }
static Tex9 *any_tex(uint32_t g) { Tex9 *t = obj(g, O9_TEX); if (!t) t = obj(g, O9_CUBE); if (!t) t = obj(g, O9_VOLTEX); return t; }

M(sf_QueryInterface, 3) ME(Surf9, O9_SURF) rt_w32(G_MEM, ARG(2), ARG(0)); surf_addref(me); RET(D3D_OK, 3); END
M(sf_AddRef, 1) ME(Surf9, O9_SURF) RET(surf_addref(me), 1); END
M(sf_Release, 1) ME(Surf9, O9_SURF) RET(surf_release(me), 1); END
M(sf_GetDevice, 2) ME(Surf9, O9_SURF) rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
OK_M(sf_SetPrivateData, 5)
M(sf_GetPrivateData, 4) RET(D3DERR_NOTFOUND, 4); END
OK_M(sf_FreePrivateData, 2)
M(sf_SetPriority, 2) RET(0, 2); END
M(sf_GetPriority, 1) RET(0, 1); END
OK_M(sf_PreLoad, 1)
M(sf_GetType, 1) RET(RT_SURFACE, 1); END
M(sf_GetContainer, 3) ME(Surf9, O9_SURF)
    if (me->tex) { rt_w32(G_MEM, ARG(2), me->tex->g); me->tex->ref++; } else { rt_w32(G_MEM, ARG(2), me->dev->g); me->dev->ref++; }
    RET(D3D_OK, 3);
END
static void surf_desc(Surf9 *s, uint32_t out)          /* D3DSURFACE_DESC */
{
    uint32_t v[8] = {s->fmt, RT_SURFACE, s->usage, s->pool, 0, 0, s->w, s->h}; memcpy(GP(out), v, 32);
}
M(sf_GetDesc, 2) ME(Surf9, O9_SURF) surf_desc(me, ARG(1)); RET(D3D_OK, 2); END
static uint32_t surf_lock(Surf9 *s, uint32_t locked, uint32_t rect, uint32_t flags)
{
    int32_t r[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    if (rect) { memcpy(r, GP(rect), 16); if (r[0] < 0 || r[1] < 0 || r[2] > (int32_t)s->w || r[3] > (int32_t)s->h || r[2] <= r[0] || r[3] <= r[1]) return D3DERR_INVALIDCALL; }
    { static int lg = -1; if (lg < 0) lg = getenv("D3D9_LOCKLOG") ? 200 : 0;
      if (lg > 0 && (s->usage & USAGE_RENDERTARGET)) { lg--; fprintf(stderr, "d3d9: lock render target %u %ux%u format %#x flags %#x (gpu newer %d)\n", s->id, s->w, s->h, s->fmt, flags, s->gpu_newer); } }
    if (!(flags & LOCK_DISCARD)) surf_readback(s);
    uint32_t off = d9_fmt_block(s->fmt) ? (uint32_t)(r[1] / 4) * s->pitch + (uint32_t)(r[0] / 4) * (uint32_t)d9_fmt_block(s->fmt)
                                        : (uint32_t)r[1] * s->pitch + (uint32_t)r[0] * (uint32_t)(d9_fmt_bytes(s->fmt) ? d9_fmt_bytes(s->fmt) : 4);
    rt_w32(G_MEM, locked, s->pitch); rt_w32(G_MEM, locked + 4, s->mem + off);
    s->locked = 1; s->lflags = flags; s->ly0 = (uint32_t)r[1]; s->ly1 = (uint32_t)r[3];
    return D3D_OK;
}
static void tex_autogen(Tex9 *t, int face);
static uint32_t surf_unlock(Surf9 *s)
{
    if (!s->locked) return D3DERR_INVALIDCALL;
    s->locked = 0;
    if (!(s->lflags & LOCK_READONLY)) {
        surf_upload(s, s->ly0, s->ly1);
        if (s->tex && s->level == 0 && (s->tex->usage & USAGE_AUTOGENMIPMAP)) tex_autogen(s->tex, (int)s->face);
    }
    return D3D_OK;
}
M(sf_LockRect, 4) ME(Surf9, O9_SURF) RET(surf_lock(me, ARG(1), ARG(2), ARG(3)), 4); END
M(sf_UnlockRect, 1) ME(Surf9, O9_SURF) RET(surf_unlock(me), 1); END
TODO_M(sf_GetDC, 2, D3DERR_INVALIDCALL)
TODO_M(sf_ReleaseDC, 2, D3DERR_INVALIDCALL)

/* ---- textures (2D, cube, volume) ---- */
static uint32_t tex_release(Tex9 *t)
{
    uint32_t r = (uint32_t)--t->ref; if (r) return r;
    for (int f = 0; f < 6; f++) for (uint32_t l = 0; l < 16; l++) if (t->lv[f][l]) surf_destroy(t->lv[f][l]);
    obj_drop(t->g); free(t); return 0;
}
static uint32_t full_levels(uint32_t w, uint32_t h) { uint32_t n = 1; while (w > 1 || h > 1) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; n++; } return n; }
static Tex9 *tex_new(Dev9 *d, int type, uint32_t w, uint32_t h, uint32_t dep, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    Tex9 *t = calloc(1, sizeof *t); t->ref = 1; t->dev = d; t->type = type; t->w = w; t->h = h; t->d = dep; t->usage = usage; t->fmt = fmt; t->pool = pool;
    uint32_t full = full_levels(w, h);
    t->levels = (usage & USAGE_AUTOGENMIPMAP) ? full : (!levels || levels > full ? full : levels);
    if (t->levels > 16) t->levels = 16;
    int faces = type == O9_CUBE ? 6 : 1, kind = (usage & USAGE_DEPTHSTENCIL) ? 2 : 3;
    for (int f = 0; f < faces; f++) {
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t root = l ? t->lv[f][0]->id : f ? t->lv[0][0]->id : 0;   /* cube faces share face 0's texture (one slice each) */
            t->lv[f][l] = surf_new(d, lw, lh, fmt, usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL), pool, kind, t, l, (uint32_t)f, root, l ? 0 : t->levels);
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
    }
    t->g = make_iface(type, t);
    return t;
}
static void tex_autogen(Tex9 *t, int face)            /* D3DUSAGE_AUTOGENMIPMAP: the smaller levels by 2x2 averaging (BGRA8) */
{
    Surf9 *s0 = t->lv[face][0]; uint32_t w = s0->w, h = s0->h;
    uint8_t *cur = malloc((size_t)w * h * 4); d9_to_bgra(s0->fmt, G_MEM + s0->mem, s0->pitch, w, 0, h, cur);
    for (uint32_t l = 1; l < t->levels; l++) {
        uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1; uint8_t *nx = malloc((size_t)nw * nh * 4);
        for (uint32_t y = 0; y < nh; y++) for (uint32_t x = 0; x < nw; x++) for (int k = 0; k < 4; k++) {
            uint32_t x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : h - 1;
            nx[(y * nw + x) * 4 + k] = (uint8_t)((cur[(y0 * w + x0) * 4 + k] + cur[(y0 * w + x1) * 4 + k] + cur[(y1 * w + x0) * 4 + k] + cur[(y1 * w + x1) * 4 + k] + 2) / 4);
        }
        Surf9 *s = t->lv[face][l]; uint32_t row = nw * 4;
        uint32_t *p = cmd_begin(DSR_SURFACE_UPLOAD, 24 + row * nh);
        p[0] = s->id; p[1] = 0; p[2] = 0; p[3] = nw; p[4] = nh; p[5] = row; memcpy(p + 6, nx, (size_t)row * nh); cmd_end();
        free(cur); cur = nx; w = nw; h = nh;
    }
    free(cur);
}
#define TEX_ME Tex9 *me = any_tex(ARG(0)); if (!me) { fprintf(stderr, "d3d9: bad texture %08x in %s\n", ARG(0), __func__); RET(D3DERR_INVALIDCALL, nargs_); }
M(tx_QueryInterface, 3) TEX_ME rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(tx_AddRef, 1) TEX_ME RET((uint32_t)++me->ref, 1); END
M(tx_Release, 1) TEX_ME RET(tex_release(me), 1); END
M(tx_GetDevice, 2) TEX_ME rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
OK_M(tx_SetPrivateData, 5)
M(tx_GetPrivateData, 4) RET(D3DERR_NOTFOUND, 4); END
OK_M(tx_FreePrivateData, 2)
M(tx_SetPriority, 2) RET(0, 2); END
M(tx_GetPriority, 1) RET(0, 1); END
OK_M(tx_PreLoad, 1)
M(tx_GetType, 1) TEX_ME RET(res_type(me->type), 1); END
M(tx_SetLOD, 2) TEX_ME uint32_t o = me->lod; me->lod = ARG(1); RET(o, 2); END
M(tx_GetLOD, 1) TEX_ME RET(me->lod, 1); END
M(tx_GetLevelCount, 1) TEX_ME RET((me->usage & USAGE_AUTOGENMIPMAP) ? 1 : me->levels, 1); END
OK_M(tx_SetAutoGenFilterType, 2)
M(tx_GetAutoGenFilterType, 1) RET(2, 1); END
M(tx_GenerateMipSubLevels, 1) TEX_ME if (me->levels > 1) tex_autogen(me, 0); RET(0, 1); END
M(tx_GetLevelDesc, 3) TEX_ME
    if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 3);
    surf_desc(me->lv[0][ARG(1)], ARG(2)); rt_w32(G_MEM, ARG(2) + 4, RT_SURFACE); RET(D3D_OK, 3);
END
M(tx_GetSurfaceLevel, 3) TEX_ME
    if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 3);
    rt_w32(G_MEM, ARG(2), me->lv[0][ARG(1)]->g); me->ref++; RET(D3D_OK, 3);
END
M(tx_LockRect, 5) TEX_ME if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 5); RET(surf_lock(me->lv[0][ARG(1)], ARG(2), ARG(3), ARG(4)), 5); END
M(tx_UnlockRect, 2) TEX_ME if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 2); RET(surf_unlock(me->lv[0][ARG(1)]), 2); END
OK_M(tx_AddDirtyRect, 2)
/* cube textures: (face, level) */
M(cb_GetCubeMapSurface, 4) TEX_ME
    if (ARG(1) >= 6 || ARG(2) >= me->levels) RET(D3DERR_INVALIDCALL, 4);
    rt_w32(G_MEM, ARG(3), me->lv[ARG(1)][ARG(2)]->g); me->ref++; RET(D3D_OK, 4);
END
M(cb_LockRect, 6) TEX_ME if (ARG(1) >= 6 || ARG(2) >= me->levels) RET(D3DERR_INVALIDCALL, 6); RET(surf_lock(me->lv[ARG(1)][ARG(2)], ARG(3), ARG(4), ARG(5)), 6); END
M(cb_UnlockRect, 3) TEX_ME if (ARG(1) >= 6 || ARG(2) >= me->levels) RET(D3DERR_INVALIDCALL, 3); RET(surf_unlock(me->lv[ARG(1)][ARG(2)]), 3); END
OK_M(cb_AddDirtyRect, 3)
/* volume textures: one slice kept (LockBox hands out the first slice's memory; enough for creation from files) */
M(vt_GetLevelDesc, 3) TEX_ME
    if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 3);
    Surf9 *s = me->lv[0][ARG(1)]; uint32_t v[8] = {s->fmt, RT_VOLUME, s->usage, s->pool, s->w, s->h, me->d, 0}; memcpy(GP(ARG(2)), v, 32); RET(D3D_OK, 3);
END
TODO_M(vt_GetVolumeLevel, 3, D3DERR_INVALIDCALL)
M(vt_LockBox, 5) TEX_ME
    if (ARG(1) >= me->levels) RET(D3DERR_INVALIDCALL, 5);
    Surf9 *s = me->lv[0][ARG(1)]; rt_w32(G_MEM, ARG(2), s->pitch); rt_w32(G_MEM, ARG(2) + 4, s->pitch * s->h); rt_w32(G_MEM, ARG(2) + 8, s->mem);
    RET(D3D_OK, 5);
END
OK_M(vt_UnlockBox, 2)
OK_M(vt_AddDirtyBox, 2)

/* ---- vertex and index buffers: guest memory the game locks; read at draw time ---- */
#define BUF_ME Buf9 *me = obj(ARG(0), O9_VB); if (!me) me = obj(ARG(0), O9_IB); if (!me) { fprintf(stderr, "d3d9: bad buffer %08x in %s\n", ARG(0), __func__); RET(D3DERR_INVALIDCALL, nargs_); }
/* The Linux renderer keeps a copy of each buffer (DSR_VBUF_DATA) and shader draws name buffers and element types
 * (DSR_DRAW_PROG_VB) instead of copying every vertex as float4: what a Lock covered is sent again before the next draw
 * that reads the buffer. D3D9_GPU_VB=0 turns it off. */
#ifdef __linux__
static int gpu_vb(void) { static int on = -1; if (on < 0) { const char *e = getenv("D3D9_GPU_VB"); on = !(e && *e == '0'); } return on; }
#else
static int gpu_vb(void) { return 0; }
#endif
static pthread_mutex_t vb_lock = PTHREAD_MUTEX_INITIALIZER;    /* buffers are filled on the game's loader threads too */
static uint32_t vbid_free[DSR_VBUFS], vbid_nfree, vbid_next = 1;
static uint32_t vbid_alloc(void)
{
    pthread_mutex_lock(&vb_lock); uint32_t id = vbid_nfree ? vbid_free[--vbid_nfree] : vbid_next < DSR_VBUFS ? vbid_next++ : 0;
    pthread_mutex_unlock(&vb_lock); return id;
}
static void range_add(uint32_t *lo, uint32_t *hi, uint32_t a, uint32_t b) { if (*lo >= *hi) { *lo = a; *hi = b; } else { if (a < *lo) *lo = a; if (b > *hi) *hi = b; } }
static void buf_locked(Buf9 *b, uint32_t off, uint32_t n)       /* Lock: the range the game may write */
{
    if (off > b->len) return; if (!n || n > b->len - off) n = b->len - off;
    pthread_mutex_lock(&vb_lock); range_add(&b->llo, &b->lhi, off, off + n); pthread_mutex_unlock(&vb_lock);
}
static void buf_unlocked(Buf9 *b)                                /* Unlock: what was locked is written, to be sent */
{
    pthread_mutex_lock(&vb_lock); if (b->llo < b->lhi) range_add(&b->dlo, &b->dhi, b->llo, b->lhi); b->llo = b->lhi = 0; pthread_mutex_unlock(&vb_lock);
}
static void buf_sync_locked(Buf9 *b)                  /* send what was written since the last draw (4 MB per command) */
{
    while (b->dlo < b->dhi) {
        uint32_t n = b->dhi - b->dlo; if (n > (4u << 20)) n = 4u << 20;
        uint32_t *p = cmd_begin(DSR_VBUF_DATA, 16 + ((n + 3) & ~3u));
        p[0] = b->gid; p[1] = b->len; p[2] = b->dlo; p[3] = n; memcpy(p + 4, G_MEM + b->mem + b->dlo, n); cmd_end();
        b->dlo += n;
    }
    b->dlo = b->dhi = 0;
}
static void buf_sync(Buf9 *b) { pthread_mutex_lock(&vb_lock); buf_sync_locked(b); pthread_mutex_unlock(&vb_lock); }
static uint32_t buf_release(Buf9 *b)
{
    uint32_t r = (uint32_t)--b->ref;
    if (!r) {
        if (b->gid) { uint32_t *p = cmd_begin(DSR_VBUF_FREE, 4); p[0] = b->gid; cmd_end();
            pthread_mutex_lock(&vb_lock); if (vbid_nfree < DSR_VBUFS) vbid_free[vbid_nfree++] = b->gid; pthread_mutex_unlock(&vb_lock); }
        gfree_(b->mem, b->len); obj_drop(b->g); free(b);
    }
    return r;
}
M(bf_QueryInterface, 3) BUF_ME rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(bf_AddRef, 1) BUF_ME RET((uint32_t)++me->ref, 1); END
M(bf_Release, 1) BUF_ME RET(buf_release(me), 1); END
M(bf_GetDevice, 2) BUF_ME rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
OK_M(bf_SetPrivateData, 5)
M(bf_GetPrivateData, 4) RET(D3DERR_NOTFOUND, 4); END
OK_M(bf_FreePrivateData, 2)
M(bf_SetPriority, 2) RET(0, 2); END
M(bf_GetPriority, 1) RET(0, 1); END
OK_M(bf_PreLoad, 1)
M(bf_GetType, 1) BUF_ME RET(me->index ? RT_INDEXBUFFER : RT_VERTEXBUFFER, 1); END
M(bf_Lock, 5) BUF_ME                                   /* (offset, size, void **data, flags) */
    uint32_t off = ARG(1); if (off > me->len) RET(D3DERR_INVALIDCALL, 5);
    if (me->gid) buf_locked(me, off, ARG(2));
    rt_w32(G_MEM, ARG(3), me->mem + off); RET(D3D_OK, 5);
END
M(bf_Unlock, 1) BUF_ME if (me->gid) buf_unlocked(me); RET(D3D_OK, 1); END
M(bf_GetDesc, 2) BUF_ME                                /* D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC */
    uint32_t v[6];
    if (me->index) { v[0] = me->fmt; v[1] = RT_INDEXBUFFER; v[2] = me->usage; v[3] = me->pool; v[4] = me->len; memcpy(GP(ARG(1)), v, 20); }
    else { v[0] = 21; v[1] = RT_VERTEXBUFFER; v[2] = me->usage; v[3] = me->pool; v[4] = me->len; v[5] = me->fmt; memcpy(GP(ARG(1)), v, 24); }
    RET(D3D_OK, 2);
END

/* ---- vertex declarations: each element gathered into the matching FVF slot at draw time ---- */
enum { DT_FLOAT1, DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_D3DCOLOR, DT_UBYTE4, DT_SHORT2, DT_SHORT4, DT_UNUSED = 17 };
enum { DU_POSITION, DU_BLENDWEIGHT, DU_BLENDINDICES, DU_NORMAL, DU_PSIZE, DU_TEXCOORD, DU_TANGENT, DU_BINORMAL, DU_TESSFACTOR, DU_POSITIONT, DU_COLOR, DU_FOG, DU_DEPTH, DU_SAMPLE };
static int dtype_size(int t) { static const int s[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0}; return t < 18 ? s[t] : 0; }
static void decl_layout(Decl9 *d)                      /* the FVF and each element's place in it; elements the fixed-function pipeline doesn't use are dropped */
{
    uint32_t fvf = 0; int tc[8] = {0}, ntc = 0; d->ok = 1;
    for (int k = 0; k < d->n; k++) {
        DElem *e = &d->e[k]; e->size = 0;
        switch (e->usage) {
        case DU_POSITION: if (e->uidx == 0 && (e->type == DT_FLOAT3 || e->type == DT_FLOAT4)) fvf |= 2; break;
        case DU_POSITIONT: if (e->type == DT_FLOAT4) fvf |= 4; break;
        case DU_NORMAL: if (e->uidx == 0 && e->type == DT_FLOAT3) fvf |= 0x10; break;
        case DU_PSIZE: if (e->type == DT_FLOAT1) fvf |= 0x20; break;
        case DU_COLOR: if (e->type == DT_D3DCOLOR && e->uidx < 2) fvf |= e->uidx ? 0x80 : 0x40; break;
        case DU_TEXCOORD: if (e->uidx < 8 && e->type <= DT_FLOAT4) { tc[e->uidx] = e->type + 1; if (e->uidx + 1 > ntc) ntc = e->uidx + 1; } break;
        }
    }
    for (int k = 0; k < ntc; k++) { if (!tc[k]) tc[k] = 2; static const uint32_t code[] = {0, 3, 0, 1, 2}; fvf |= code[tc[k]] << (16 + 2 * k); }
    fvf |= (uint32_t)ntc << 8;
    /* destination offsets in FVF order */
    uint32_t off = 0, o_pos = 0, o_nrm, o_psz, o_dif, o_spc, o_tc[8];
    off = (fvf & 4) ? 16 : (fvf & 2) ? 12 : 0; o_nrm = off; if (fvf & 0x10) off += 12; o_psz = off; if (fvf & 0x20) off += 4;
    o_dif = off; if (fvf & 0x40) off += 4; o_spc = off; if (fvf & 0x80) off += 4;
    for (int k = 0; k < ntc; k++) { o_tc[k] = off; off += 4u * (uint32_t)tc[k]; }
    for (int k = 0; k < d->n; k++) {
        DElem *e = &d->e[k];
        switch (e->usage) {
        case DU_POSITION: if ((fvf & 2) && e->uidx == 0) { e->dst = (uint16_t)o_pos; e->size = 12; } break;
        case DU_POSITIONT: if (fvf & 4) { e->dst = (uint16_t)o_pos; e->size = 16; } break;
        case DU_NORMAL: if ((fvf & 0x10) && e->uidx == 0) { e->dst = (uint16_t)o_nrm; e->size = 12; } break;
        case DU_PSIZE: if (fvf & 0x20) { e->dst = (uint16_t)o_psz; e->size = 4; } break;
        case DU_COLOR: if (e->type == DT_D3DCOLOR && e->uidx < 2) { e->dst = (uint16_t)(e->uidx ? o_spc : o_dif); e->size = 4; } break;
        case DU_TEXCOORD: if (e->uidx < 8 && e->type <= DT_FLOAT4) { e->dst = (uint16_t)o_tc[e->uidx]; e->size = (uint16_t)dtype_size(e->type); } break;
        }
    }
    if (!(fvf & 6)) d->ok = 0;                          /* no position the fixed-function pipeline understands */
    d->fvf = fvf;
}
static uint32_t decl_release(Decl9 *d) { uint32_t r = (uint32_t)--d->ref; if (!r) { gfree_(d->raw, d->nraw * 8); obj_drop(d->g); free(d); } return r; }
M(dc_QueryInterface, 3) ME(Decl9, O9_DECL) rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(dc_AddRef, 1) ME(Decl9, O9_DECL) RET((uint32_t)++me->ref, 1); END
M(dc_Release, 1) ME(Decl9, O9_DECL) RET(decl_release(me), 1); END
M(dc_GetDevice, 2) ME(Decl9, O9_DECL) rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
M(dc_GetDeclaration, 3) ME(Decl9, O9_DECL)            /* (elements, *count) */
    if (ARG(1)) memcpy(GP(ARG(1)), GP(me->raw), me->nraw * 8);
    rt_w32(G_MEM, ARG(2), me->nraw); RET(D3D_OK, 3);
END

/* ---- shaders: kept (not run yet) ---- */
#define SH_ME Shader9 *me = obj(ARG(0), O9_VS); if (!me) me = obj(ARG(0), O9_PS); if (!me) RET(D3DERR_INVALIDCALL, nargs_);
static uint32_t shader_release(Shader9 *s) { uint32_t r = (uint32_t)--s->ref; if (!r) { d9_shader_forget(s); gfree_(s->mem, s->len); obj_drop(s->g); free(s); } return r; }
M(sh_QueryInterface, 3) SH_ME rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(sh_AddRef, 1) SH_ME RET((uint32_t)++me->ref, 1); END
M(sh_Release, 1) SH_ME RET(shader_release(me), 1); END
M(sh_GetDevice, 2) SH_ME rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
M(sh_GetFunction, 3) SH_ME
    if (ARG(1)) memcpy(GP(ARG(1)), GP(me->mem), me->len);
    rt_w32(G_MEM, ARG(2), me->len); RET(D3D_OK, 3);
END
static uint32_t shader_len(uint32_t p)                  /* bytecode up to and including the end token (0x0000ffff) */
{
    uint32_t n = 1; while (n < 65536 && rt_r32(G_MEM, p + 4 * n) != 0x0000ffffu) n++; return 4 * (n + 1);
}

/* ---- queries: the GPU is always done ---- */
static uint8_t query_slot_used[DSR_QUERY_SLOTS]; static pthread_mutex_t query_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t query_release(Query9 *q)
{
    uint32_t r = (uint32_t)--q->ref;
    if (!r) { pthread_mutex_lock(&query_lock); if (q->slot) query_slot_used[q->slot] = 0; pthread_mutex_unlock(&query_lock); obj_drop(q->g); free(q); }
    return r;
}
M(qy_QueryInterface, 3) ME(Query9, O9_QUERY) rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(qy_AddRef, 1) ME(Query9, O9_QUERY) RET((uint32_t)++me->ref, 1); END
M(qy_Release, 1) ME(Query9, O9_QUERY) RET(query_release(me), 1); END
M(qy_GetDevice, 2) ME(Query9, O9_QUERY) rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
M(qy_GetType, 1) ME(Query9, O9_QUERY) RET(me->type, 1); END
M(qy_GetDataSize, 1) ME(Query9, O9_QUERY) RET(me->type == 8 ? 4 : me->type == 9 ? 4 : 0, 1); END
/* occlusion queries (type 9) run on the GPU (DSR_QUERY): GetData is S_FALSE until the renderer has published the count
 * for the last End; a game that spins on it gets the previous count after a while rather than waiting forever. Events
 * (type 8) and other types: done at once. */
volatile uint32_t *dsr_query_results(void);
M(qy_Issue, 2) ME(Query9, O9_QUERY)                    /* (flags: D3DISSUE_END 1, D3DISSUE_BEGIN 2) */
    if (me->type == 9 && me->slot) {
        uint32_t *p = cmd_begin(DSR_QUERY, 12); p[0] = me->slot; p[1] = (ARG(1) & 2) ? 1 : 0;
        if (!(ARG(1) & 2)) { me->serial++; me->polls = 0; }
        p[2] = me->serial; cmd_end();
    }
    RET(D3D_OK, 2);
END
M(qy_GetData, 4) ME(Query9, O9_QUERY)                  /* (data, size, flags) */
    if (me->type != 9 || !me->slot) { if (ARG(1) && ARG(2) >= 4) rt_w32(G_MEM, ARG(1), me->type == 9 ? 1000 : 1); RET(D3D_OK, 4); }
    volatile uint32_t *res = dsr_query_results();
    uint32_t serial = res ? __atomic_load_n(&res[DSR_QUERY_SLOTS + me->slot], __ATOMIC_ACQUIRE) : me->serial;
    if (serial == me->serial) me->last = res ? res[me->slot] : 1000;
    else if (++me->polls < 2000) RET(1 /* S_FALSE */, 4);
    if (ARG(1) && ARG(2) >= 4) rt_w32(G_MEM, ARG(1), me->last);
    RET(D3D_OK, 4);
END

/* ---- state blocks: a list of recorded settings, replayed through the device's setters ---- */
static void rec_add(SB9 *b, RecEnt e) { if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->e = realloc(b->e, (size_t)b->cap * sizeof *b->e); } b->e[b->n++] = e; }
static void rec_put(SB9 *b, RecEnt e)                   /* replace an earlier entry for the same setting */
{
    for (int k = 0; k < b->n; k++) if (b->e[k].kind == e.kind && b->e[k].a == e.a) { b->e[k] = e; return; }
    rec_add(b, e);
}
static void sb_apply(Dev9 *d, SB9 *b); static void sb_capture(Dev9 *d, SB9 *b);
static uint32_t sb_release(SB9 *b) { uint32_t r = (uint32_t)--b->ref; if (!r) { free(b->e); obj_drop(b->g); free(b); } return r; }
M(sb_QueryInterface, 3) ME(SB9, O9_SB) rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(sb_AddRef, 1) ME(SB9, O9_SB) RET((uint32_t)++me->ref, 1); END
M(sb_Release, 1) ME(SB9, O9_SB) RET(sb_release(me), 1); END
M(sb_GetDevice, 2) ME(SB9, O9_SB) rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
M(sb_Capture, 1) ME(SB9, O9_SB) sb_capture(me->dev, me); RET(D3D_OK, 1); END
M(sb_Apply, 1) ME(SB9, O9_SB) sb_apply(me->dev, me); RET(D3D_OK, 1); END

/* ---- device state: each setter records (state block) or applies and sends ---- */
static void state_seen(int kind, uint32_t a, uint32_t b, uint32_t v)   /* D3D9_STATELOG: each (state, value) the game uses, once */
{
    static int on = -1; if (on < 0) on = getenv("D3D9_STATELOG") != 0;
    if (!on) return;
    static uint64_t seen[4096]; static int n; uint64_t k = (uint64_t)kind << 60 | (uint64_t)a << 48 | (uint64_t)b << 40 | v;
    for (int i = 0; i < n; i++) if (seen[i] == k) return;
    if (n < 4096) seen[n++] = k;
    fprintf(stderr, "d3d9: state %s %u %u = %u (%08x)\n", kind == 0 ? "rs" : kind == 1 ? "tss" : "ss", a, b, v, v);
}
static void set_rs(Dev9 *d, uint32_t s, uint32_t v)
{
    if (s >= 256) return;
    state_seen(0, s, 0, v);
    if (d->rec) { RecEnt e = {REC_RS, s, v}; rec_put(d->rec, e); return; }
    d->rs[s] = v; uint32_t *p = cmd_begin(DSR_RENDER_STATE, 8); p[0] = s; p[1] = v; cmd_end();
}
static void set_tss(Dev9 *d, uint32_t st, uint32_t t, uint32_t v)
{
    if (st >= 8 || t >= 33) return;
    state_seen(1, st, t, v);
    if (d->rec) { RecEnt e = {REC_TSS, st << 8 | t, v}; rec_put(d->rec, e); return; }
    d->tss[st][t] = v;
    if (t < 32) { uint32_t *p = cmd_begin(DSR_TSS, 12); p[0] = st; p[1] = t; p[2] = v; cmd_end(); }
}
static void set_ss(Dev9 *d, uint32_t sm, uint32_t t, uint32_t v)   /* sampler states, sent as the D3D7 texture-stage ones */
{
    if (sm >= 16 || t >= 14) return;
    if (d->rec) { RecEnt e = {REC_SS, sm << 8 | t, v}; rec_put(d->rec, e); return; }
    d->ss[sm][t] = v;
    static const uint8_t map[14] = {0, 13, 14, 0, 15, 16, 17, 18, 19, 20, 21, 0, 0, 0};
    if (sm < 16 && map[t]) { uint32_t *p = cmd_begin(DSR_TSS, 12); p[0] = sm; p[1] = map[t]; p[2] = t == 7 ? v + 1 : v; cmd_end(); }   /* mip filter: D3D9 0-2, D3D7 1-3; all 16 pixel samplers */
}
static uint32_t tex_id(Tex9 *t) { return t && t->lv[0][0] ? t->lv[0][0]->id : 0; }
static void set_tex(Dev9 *d, uint32_t st, Tex9 *t)
{
    if (st >= 16) return;
    if (d->rec) { RecEnt e = {REC_TEX, st, 0}; e.p = t; if (t) t->ref++; rec_put(d->rec, e); return; }
    if (t) t->ref++;
    { static int n = -1; if (n < 0) n = getenv("D3D9_TEXLOG") ? atoi(getenv("D3D9_TEXLOG")) : 0;
      if (n > 0 && d->prog) { n--; fprintf(stderr, "d3d9: program %u: stage %u texture %08x (from %08x)\n", d->prog, st, t ? t->g : 0, (uint32_t)(uintptr_t)__builtin_return_address(0)); } }
    if (d->tex[st]) tex_release(d->tex[st]);
    d->tex[st] = t;
    if (st < 16) { uint32_t *p = cmd_begin(DSR_TEXTURE, 8); p[0] = st; p[1] = tex_id(t); cmd_end(); }   /* shader model 2/3: 16 samplers */
}
static int xf_slot(uint32_t t) { return t == 256 ? 1 : (t == 2 || t == 3) ? (int)t : (t >= 16 && t <= 23) ? (int)t : -1; }
static void set_xf(Dev9 *d, uint32_t t, const float *m)
{
    if (t >= 512) return;
    if (d->rec) { RecEnt e = {REC_XF, t, 0}; memcpy(e.m, m, 64); rec_put(d->rec, e); return; }
    memcpy(d->xf[t], m, 64);
    int k = xf_slot(t); if (k < 0) return;
    uint32_t *p = cmd_begin(DSR_TRANSFORM, 68); p[0] = (uint32_t)k; memcpy(p + 1, m, 64); cmd_end();
}
static void set_vp(Dev9 *d, const uint32_t *v)
{
    if (d->rec) { RecEnt e = {REC_VP, 0, 0}; memcpy(e.m, v, 24); rec_put(d->rec, e); return; }
    memcpy(d->vp, v, 24); uint32_t *p = cmd_begin(DSR_VIEWPORT, 24); memcpy(p, v, 24); cmd_end();
}
static void set_rt(Dev9 *d)
{
    { static int lg = -1, n; if (lg < 0) lg = getenv("D3D9_RTLOG") != 0;
      if (lg && n++ < 400) fprintf(stderr, "rtlog: rt %u %ux%u fmt %u x%u | ds %u %ux%u fmt %u x%u\n", d->rt ? d->rt->id : 0, d->rt ? d->rt->w : 0, d->rt ? d->rt->h : 0,
          d->rt ? d->rt->fmt : 0, d->rt ? d->rt->samples : 0, d->ds ? d->ds->id : 0, d->ds ? d->ds->w : 0, d->ds ? d->ds->h : 0, d->ds ? d->ds->fmt : 0, d->ds ? d->ds->samples : 0); }
    uint32_t *p = cmd_begin(DSR_SET_RT, 8); p[0] = d->rt ? d->rt->id : 0; p[1] = d->ds ? d->ds->id : 0; cmd_end();
}
static void sb_apply(Dev9 *d, SB9 *b)
{
    for (int k = 0; k < b->n; k++) {
        RecEnt *e = &b->e[k];
        switch (e->kind) {
        case REC_RS: set_rs(d, e->a, e->b); break;
        case REC_TSS: set_tss(d, e->a >> 8, e->a & 255, e->b); break;
        case REC_SS: set_ss(d, e->a >> 8, e->a & 255, e->b); break;
        case REC_TEX: set_tex(d, e->a, e->p); break;
        case REC_XF: set_xf(d, e->a, e->m); break;
        case REC_VP: set_vp(d, (const uint32_t *)e->m); break;
        case REC_FVF: d->fvf = e->a; d->decl = 0; break;
        case REC_DECL: d->decl = e->p; break;
        case REC_VS: d->vs = e->p; break;
        case REC_PS: d->ps = e->p; break;
        case REC_MAT: memcpy(d->material, e->m, 64); break;
        }
    }
}
static void sb_capture(Dev9 *d, SB9 *b)               /* the current values of the settings the block holds */
{
    for (int k = 0; k < b->n; k++) {
        RecEnt *e = &b->e[k];
        switch (e->kind) {
        case REC_RS: e->b = d->rs[e->a]; break;
        case REC_TSS: e->b = d->tss[e->a >> 8][e->a & 255]; break;
        case REC_SS: e->b = d->ss[e->a >> 8][e->a & 255]; break;
        case REC_TEX: { Tex9 *t = d->tex[e->a]; if (t) t->ref++; if (e->p) tex_release(e->p); e->p = t; break; }
        case REC_XF: memcpy(e->m, d->xf[e->a], 64); break;
        case REC_VP: memcpy(e->m, d->vp, 24); break;
        case REC_FVF: e->a = d->fvf; break;
        case REC_DECL: e->p = d->decl; break;
        case REC_VS: e->p = d->vs; break;
        case REC_PS: e->p = d->ps; break;
        }
    }
}
static SB9 *sb_new(Dev9 *d) { SB9 *b = calloc(1, sizeof *b); b->ref = 1; b->dev = d; b->g = make_iface(O9_SB, b); return b; }
static void sb_all(Dev9 *d, SB9 *b)                    /* CreateStateBlock(D3DSBT_ALL): everything the setters cover */
{
    for (uint32_t s = 1; s < 256; s++) { RecEnt e = {REC_RS, s, d->rs[s]}; rec_add(b, e); }
    for (uint32_t st = 0; st < 8; st++) for (uint32_t t = 1; t < 33; t++) { RecEnt e = {REC_TSS, st << 8 | t, d->tss[st][t]}; rec_add(b, e); }
    for (uint32_t sm = 0; sm < 16; sm++) for (uint32_t t = 1; t < 14; t++) { RecEnt e = {REC_SS, sm << 8 | t, d->ss[sm][t]}; rec_add(b, e); }
    for (uint32_t st = 0; st < 16; st++) { RecEnt e = {REC_TEX, st, 0}; e.p = d->tex[st]; if (d->tex[st]) d->tex[st]->ref++; rec_add(b, e); }
    static const uint32_t xfs[] = {2, 3, 256, 16, 17, 18, 19, 20, 21, 22, 23};
    for (unsigned k = 0; k < sizeof xfs / 4; k++) { RecEnt e = {REC_XF, xfs[k], 0}; memcpy(e.m, d->xf[xfs[k]], 64); rec_add(b, e); }
    RecEnt v = {REC_VP, 0, 0}; memcpy(v.m, d->vp, 24); rec_add(b, v);
    RecEnt f = {REC_FVF, d->fvf, 0}; rec_add(b, f);
    RecEnt dc = {REC_DECL, 0, 0}; dc.p = d->decl; rec_add(b, dc);
    RecEnt vs = {REC_VS, 0, 0}; vs.p = d->vs; rec_add(b, vs);
    RecEnt ps = {REC_PS, 0, 0}; ps.p = d->ps; rec_add(b, ps);
}

/* ---- drawing ---- */
static uint32_t prim_verts(uint32_t prim, uint32_t n)
{
    switch (prim) { case 1: return n; case 2: return 2 * n; case 3: return n + 1; case 4: return 3 * n; case 5: case 6: return n + 2; }
    return 0;
}
static uint8_t *staging(Dev9 *d, size_t n) { if (n > d->vbuf_cap) { d->vbuf_cap = n + n / 2; d->vbuf = realloc(d->vbuf, d->vbuf_cap); } return d->vbuf; }
/* vertices [first, first + nv) of the current streams into FVF layout; 0 if they can't be (out of range, unsupported) */
static const uint8_t *gather(Dev9 *d, uint32_t first, uint32_t nv, uint32_t *fvf_out, const uint8_t *up, uint32_t up_stride)
{
    static int warned_decl, warned_range;
    uint32_t fvf = d->decl ? d->decl->fvf : d->fvf;
    if (d->decl && !d->decl->ok) { if (!warned_decl++) fprintf(stderr, "d3d9: a vertex declaration without a fixed-function position (draw skipped)\n"); return 0; }
    uint32_t fs = fvf_stride(fvf); if (!fs || !nv) return 0;
    uint8_t *out = staging(d, (size_t)fs * nv);
    if (!d->decl) {
        const uint8_t *src; uint32_t stride, avail;
        if (up) { src = up; stride = up_stride; avail = 0xffffffffu; }
        else {
            Buf9 *b = d->vb[0]; if (!b) return 0;
            stride = d->vb_stride[0]; if (!stride) stride = fs;
            uint64_t start = (uint64_t)d->vb_off[0] + (uint64_t)first * stride, need = start + (uint64_t)(nv - 1) * stride + fs;
            if (need > b->len) { if (!warned_range++) fprintf(stderr, "d3d9: a draw reads past its vertex buffer (skipped)\n"); return 0; }
            src = G_MEM + b->mem + start; avail = b->len;
        }
        (void)avail;
        if (stride == fs) { memcpy(out, src, (size_t)fs * nv); }
        else for (uint32_t v = 0; v < nv; v++) memcpy(out + (size_t)v * fs, src + (size_t)v * stride, fs < stride ? fs : stride);
        *fvf_out = fvf; return out;
    }
    memset(out, 0, (size_t)fs * nv);
    for (int k = 0; k < d->decl->n; k++) {
        DElem *e = &d->decl->e[k]; if (!e->size) continue;
        const uint8_t *src; uint32_t stride; uint64_t len;
        if (up) { if (e->stream) continue; src = up; stride = up_stride; len = 0xffffffffffffull; }
        else {
            Buf9 *b = d->vb[e->stream]; if (!b) continue;
            stride = d->vb_stride[e->stream];
            uint64_t start = (uint64_t)d->vb_off[e->stream] + (uint64_t)first * stride;
            if (start + (uint64_t)(nv - 1) * stride + e->off + e->size > b->len) { if (!warned_range++) fprintf(stderr, "d3d9: a draw reads past its vertex buffer (skipped)\n"); return 0; }
            src = G_MEM + b->mem + start; len = b->len;
        }
        (void)len;
        for (uint32_t v = 0; v < nv; v++) memcpy(out + (size_t)v * fs + e->dst, src + (size_t)v * stride + e->off, e->size);
    }
    *fvf_out = fvf; return out;
}
static void emit(Dev9 *d, uint32_t prim, uint32_t fvf, const uint8_t *v, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    uint32_t st = fvf_stride(fvf);
    if ((uint64_t)st * nv + (uint64_t)ni * 2 > (16u << 20)) { static int w; if (!w++) fprintf(stderr, "d3d9: a draw too large for the command ring (skipped)\n"); return; }
    uint32_t vb = st * nv, ib = (ni * 2 + 3) & ~3u, *p = cmd_begin(DSR_DRAW, 16 + vb + ib);
    p[0] = prim; p[1] = fvf; p[2] = nv; p[3] = ni;
    memcpy(p + 4, v, vb); if (ni) memcpy((uint8_t *)(p + 4) + vb, idx, ni * 2);
    cmd_end();
    if (d->rt) d->rt->gpu_newer = 1;
}
static int can_draw(Dev9 *d)
{
    if (d->vs) { static int w; if (!w++) fprintf(stderr, "d3d9: draws with a vertex shader are skipped (shaders not run yet)\n"); return 0; }
    return 1;
}
static void draw_indexed(Dev9 *d, uint32_t prim, int32_t base, uint32_t minidx, uint32_t nv, const uint8_t *ib, int idx32, uint32_t ni, const uint8_t *up, uint32_t up_stride)
{
    if (!nv || !ni) return;
    if (nv > 65536) { static int w; if (!w++) fprintf(stderr, "d3d9: an indexed draw over more than 65536 vertices (skipped)\n"); return; }
    uint32_t fvf; const uint8_t *v = gather(d, (uint32_t)(base + (int32_t)minidx), nv, &fvf, up, up_stride); if (!v) return;
    uint16_t *ix = malloc((size_t)ni * 2);
    for (uint32_t k = 0; k < ni; k++) {
        uint32_t i = idx32 ? ((const uint32_t *)ib)[k] : ((const uint16_t *)ib)[k];
        i = i >= minidx ? i - minidx : 0; if (i >= nv) i = nv - 1;
        ix[k] = (uint16_t)i;
    }
    emit(d, prim, fvf, v, nv, ix, ni); free(ix);
}

/* ---- programmable draws: an effect pass's Metal program bound (d3dx9_fx.c) ----
 * Every vertex element goes to an attribute slot by its semantic, as a float4 (hlsl.c reads them that way):
 * 0 position, 1 normal, 2-3 colour 0-1, 4-11 texture coordinates 0-7, 12 tangent, 13 binormal, 14 blend weights,
 * 15 blend indices. */
static int sem_slot(int usage, int idx)
{
    switch (usage) {
    case DU_POSITION: case DU_POSITIONT: return idx ? -1 : 0;
    case DU_NORMAL: return idx ? -1 : 1;
    case DU_COLOR: return idx < 2 ? 2 + idx : -1;
    case DU_TEXCOORD: return idx < 8 ? 4 + idx : -1;
    case DU_TANGENT: return idx ? -1 : 12;
    case DU_BINORMAL: return idx ? -1 : 13;
    case DU_BLENDWEIGHT: return idx ? -1 : 14;
    case DU_BLENDINDICES: return idx ? -1 : 15;
    }
    return -1;
}
static float half_to_float(uint16_t h)
{
    uint32_t sg = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = sg; else { float v = ldexpf((float)m, -24); return sg ? -v : v; } }
    else if (e == 31) f = sg | 0x7f800000u | (m << 13);
    else f = sg | ((e + 112) << 23) | (m << 13);
    float r; memcpy(&r, &f, 4); return r;
}
static int elem_size(int type) { static const int s[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8}; return type < 17 ? s[type] : 0; }
static void elem_f4(int type, const uint8_t *s, float *o)      /* D3DDECLTYPE -> float4, missing components 0, 0, 0, 1 */
{
    o[0] = o[1] = o[2] = 0; o[3] = 1; int16_t sh[4]; uint16_t us[4];
    switch (type) {
    case 0: case 1: case 2: case 3: memcpy(o, s, 4u * (uint32_t)(type + 1)); break;
    case 4: o[0] = s[2] / 255.0f; o[1] = s[1] / 255.0f; o[2] = s[0] / 255.0f; o[3] = s[3] / 255.0f; break;   /* D3DCOLOR (BGRA) */
    case 5: for (int k = 0; k < 4; k++) o[k] = s[k]; break;                                                   /* UBYTE4 */
    case 6: memcpy(sh, s, 4); o[0] = sh[0]; o[1] = sh[1]; break;                                               /* SHORT2 */
    case 7: memcpy(sh, s, 8); for (int k = 0; k < 4; k++) o[k] = sh[k]; break;                                /* SHORT4 */
    case 8: for (int k = 0; k < 4; k++) o[k] = s[k] / 255.0f; break;                                          /* UBYTE4N */
    case 9: memcpy(sh, s, 4); o[0] = sh[0] / 32767.0f; o[1] = sh[1] / 32767.0f; break;                         /* SHORT2N */
    case 10: memcpy(sh, s, 8); for (int k = 0; k < 4; k++) o[k] = sh[k] / 32767.0f; break;                    /* SHORT4N */
    case 11: memcpy(us, s, 4); o[0] = us[0] / 65535.0f; o[1] = us[1] / 65535.0f; break;                        /* USHORT2N */
    case 12: memcpy(us, s, 8); for (int k = 0; k < 4; k++) o[k] = us[k] / 65535.0f; break;                    /* USHORT4N */
    case 13: { uint32_t v; memcpy(&v, s, 4); o[0] = (float)(v & 1023); o[1] = (float)((v >> 10) & 1023); o[2] = (float)((v >> 20) & 1023); break; }   /* UDEC3 */
    case 14: { int32_t v; memcpy(&v, s, 4); o[0] = (float)((v << 22) >> 22) / 511.0f; o[1] = (float)((v << 12) >> 22) / 511.0f; o[2] = (float)((v << 2) >> 22) / 511.0f; break; }   /* DEC3N */
    case 15: memcpy(us, s, 4); o[0] = half_to_float(us[0]); o[1] = half_to_float(us[1]); break;               /* FLOAT16_2 */
    case 16: memcpy(us, s, 8); for (int k = 0; k < 4; k++) o[k] = half_to_float(us[k]); break;               /* FLOAT16_4 */
    }
}
typedef struct PElem { int stream, off, type, slot; } PElem;
static int prog_layout(Dev9 *d, PElem *el, int *pretrans)      /* the bound declaration (or FVF) as attribute slots */
{
    int n = 0; *pretrans = 0;
    if (d->decl) {
        for (int k = 0; k < d->decl->n && n < 32; k++) {
            DElem *e = &d->decl->e[k]; int sl = sem_slot(e->usage, e->uidx);
            if (sl < 0 || e->type > 16) {
                static uint32_t seen[64]; static int ns; uint32_t key = (uint32_t)e->usage << 8 | (uint32_t)e->uidx; int found = 0;
                for (int j = 0; j < ns; j++) if (seen[j] == key) found = 1;
                if (!found && ns < 64) { seen[ns++] = key; fprintf(stderr, "d3d9: vertex element usage %d index %d type %d has no attribute slot (dropped)\n", e->usage, e->uidx, e->type); }
                continue;
            }
            if (e->usage == DU_POSITIONT) *pretrans = 1;
            el[n].stream = e->stream; el[n].off = e->off; el[n].type = e->type; el[n].slot = sl; n++;
        }
        return n;
    }
    uint32_t f = d->fvf, off = 0;
    if ((f & 0xe) == 4) { el[n++] = (PElem){0, 0, 3, 0}; off = 16; *pretrans = 1; } else if (f & 2) { el[n++] = (PElem){0, 0, 2, 0}; off = 12; }
    if (f & 0x10) { el[n++] = (PElem){0, (int)off, 2, 1}; off += 12; }
    if (f & 0x20) off += 4;
    if (f & 0x40) { el[n++] = (PElem){0, (int)off, 4, 2}; off += 4; }
    if (f & 0x80) { el[n++] = (PElem){0, (int)off, 4, 3}; off += 4; }
    for (uint32_t t = 0, nt = (f >> 8) & 0xf; t < nt && t < 8; t++) {
        uint32_t c = (f >> (16 + 2 * t)) & 3, comps = c == 0 ? 2 : c == 1 ? 3 : c == 2 ? 4 : 1;
        el[n++] = (PElem){0, (int)off, (int)comps - 1, 4 + (int)t}; off += 4 * comps;
    }
    return n;
}
static void prog_draw(Dev9 *d, uint32_t prim, uint32_t first, uint32_t nv, const uint16_t *idx, uint32_t ni, const uint8_t *up, uint32_t up_stride)
{
    PElem el[32]; int pretrans, n = prog_layout(d, el, &pretrans); uint32_t mask = 0;
    for (int k = 0; k < n; k++) mask |= 1u << el[k].slot;
    int ns = __builtin_popcount(mask); if (!ns || !nv) return;
    { static int nlog; if (logon() && nlog++ < 6) fprintf(stderr, "d3d9: effect draw: program %u, prim %u, %u vertices, %u indices, attributes %04x%s%s\n", d->prog, prim, nv, ni, mask, pretrans ? " (pretransformed)" : "", d->decl ? " (declaration)" : ""); }
    uint64_t bytes = (uint64_t)nv * (uint64_t)ns * 16 + (uint64_t)ni * 2;
    if (bytes > (16u << 20)) { static int w; if (!w++) fprintf(stderr, "d3d9: an effect draw too large for the command ring (skipped)\n"); return; }
    uint32_t ib = (ni * 2 + 3) & ~3u, vb = nv * (uint32_t)ns * 16, *p = cmd_begin(DSR_DRAW_PROG, 20 + vb + ib);
    p[0] = prim; p[1] = mask; p[2] = nv; p[3] = ni; p[4] = (uint32_t)pretrans;
    float *out = (float *)(p + 5);
    for (int s = 0; s < ns * (int)nv; s++) { out[4 * s] = out[4 * s + 1] = out[4 * s + 2] = 0; out[4 * s + 3] = 1; }
    int col[16]; for (int sl = 0, c = 0; sl < 16; sl++) col[sl] = (mask >> sl) & 1 ? c++ : -1;
    for (int k = 0; k < n; k++) {
        const uint8_t *src; uint32_t stride; int sz = elem_size(el[k].type);
        if (up) { if (el[k].stream) continue; src = up; stride = up_stride; }
        else {
            Buf9 *b = d->vb[el[k].stream]; if (!b) continue;
            stride = d->vb_stride[el[k].stream];
            uint64_t start = (uint64_t)d->vb_off[el[k].stream] + (uint64_t)first * stride;
            if (start + (uint64_t)(nv - 1) * stride + (uint64_t)el[k].off + (uint64_t)sz > b->len) { static int w; if (!w++) fprintf(stderr, "d3d9: an effect draw reads past its vertex buffer (skipped)\n"); cmd_end(); return; }
            src = G_MEM + b->mem + start;
        }
        for (uint32_t v = 0; v < nv; v++) elem_f4(el[k].type, src + (size_t)v * stride + el[k].off, out + 4 * ((size_t)v * (size_t)ns + (size_t)col[el[k].slot]));
    }
    { static long want = -1; static int nd; if (want < 0) { const char *e = getenv("D3D9_VDUMP"); want = e ? atol(e) : 0; }   /* D3D9_VDUMP=<program>: its first draws' vertices */
      if (want && (long)d->prog == want && (mask >> 15 & 1) && nd < 400) {        /* blend indices (slot 15): the largest per draw */
        float mx = 0, w = 0; int c15 = col[15]; for (uint32_t v = 0; v < nv; v++) for (int c = 0; c < 4; c++) { float x = out[4*(v*ns+c15)+c]; if (x > mx) mx = x; }
        (void)w; fprintf(stderr, "vdump prog %u: nv %u largest blend index component %g (x765 = %g)\n", d->prog, nv, mx, mx * 765.01f); }
      if (want && (long)d->prog == want && nd < 4) { nd++; fprintf(stderr, "vdump prog %u draw %d: nv %u ni %u mask %04x first %u stride %u\n", d->prog, nd, nv, ni, mask, first, d->vb_stride[0]);
        for (uint32_t v = 0; v < nv && v < 8; v++) { fprintf(stderr, "  v%u:", v); for (int c = 0; c < ns; c++) fprintf(stderr, " (%g %g %g %g)", out[4*(v*ns+c)], out[4*(v*ns+c)+1], out[4*(v*ns+c)+2], out[4*(v*ns+c)+3]); fprintf(stderr, "\n"); }
        if (ni) { fprintf(stderr, "  idx:"); for (uint32_t k = 0; k < ni && k < 24; k++) fprintf(stderr, " %u", idx[k]); fprintf(stderr, "\n"); } } }
    if (ni) memcpy((uint8_t *)(p + 5) + vb, idx, ni * 2);
    cmd_end();
    if (d->rt) d->rt->gpu_newer = 1;
}
static void prog_draw_indexed(Dev9 *d, uint32_t prim, int32_t base, uint32_t minidx, uint32_t nv, const uint8_t *ib, int idx32, uint32_t ni, const uint8_t *up, uint32_t up_stride)
{
    if (!nv || !ni || nv > 65536) return;
    uint16_t *ix = malloc((size_t)ni * 2);
    for (uint32_t k = 0; k < ni; k++) {
        uint32_t i = idx32 ? ((const uint32_t *)ib)[k] : ((const uint16_t *)ib)[k];
        i = i >= minidx ? i - minidx : 0; if (i >= nv) i = nv - 1;
        ix[k] = (uint16_t)i;
    }
    prog_draw(d, prim, (uint32_t)(base + (int32_t)minidx), nv, ix, ni, up, up_stride); free(ix);
}
/* a shader draw from kept buffers; 0 if it can't be (no buffer copy, a type GL can't read as D3D does, a range past a
 * buffer, a zero stride): the caller then takes the float4 path */
static int prog_draw_vb(Dev9 *d, uint32_t prim, int32_t base, uint32_t lo, uint32_t nv, uint32_t ni, uint32_t start_index)
{
    if (!gpu_vb() || !nv) return 0;
    PElem el[32]; int pretrans, n = prog_layout(d, el, &pretrans); if (!n) return 0;
    Buf9 *ib = 0; if (ni) { ib = d->ib; if (!ib || !ib->gid) return 0; }
    if ((int64_t)base + lo < 0) return 0;
    for (int k = 0; k < n; k++) {
        Buf9 *b = d->vb[el[k].stream]; uint32_t stride = d->vb_stride[el[k].stream];
        if (!b || !b->gid || !stride || el[k].type == 13 || el[k].type == 14) return 0;
        uint64_t last = (uint64_t)d->vb_off[el[k].stream] + ((uint64_t)base + lo + nv - 1) * stride + (uint64_t)el[k].off + (uint64_t)elem_size(el[k].type);
        if (last > b->len) return 0;
    }
    for (int k = 0; k < n; k++) { Buf9 *b = d->vb[el[k].stream]; if (b->dlo < b->dhi) buf_sync(b); }
    if (ib && ib->dlo < ib->dhi) buf_sync(ib);
    uint32_t *p = cmd_begin(DSR_DRAW_PROG_VB, 36 + 20 * (uint32_t)n);
    p[0] = prim; p[1] = nv; p[2] = ni; p[3] = (uint32_t)pretrans | (ib && ib->fmt == 102 ? 2 : 0); p[4] = ib ? ib->gid : 0;
    p[5] = ib ? start_index * (ib->fmt == 102 ? 4 : 2) : 0; p[6] = (uint32_t)base; p[7] = ni ? 0 : (uint32_t)base; p[8] = (uint32_t)n;
    for (int k = 0; k < n; k++) {
        uint32_t *e = p + 9 + 5 * k, s = el[k].stream;
        e[0] = (uint32_t)el[k].slot; e[1] = d->vb[s]->gid; e[2] = d->vb_off[s] + (uint32_t)el[k].off; e[3] = d->vb_stride[s]; e[4] = (uint32_t)el[k].type;
    }
    cmd_end();
    if (d->rt) d->rt->gpu_newer = 1;
    return 1;
}
static uint32_t next_prog_id = 1;
uint32_t d9_program_create(const char *msl, size_t len, uint32_t kinds)
{
    if (len > (4u << 20)) return 0;
    uint32_t id = next_prog_id++, *p = cmd_begin(DSR_PROG_CREATE, 12 + (((uint32_t)len + 4) & ~3u));
    p[0] = id; p[1] = kinds; p[2] = (uint32_t)len; memcpy(p + 3, msl, len); ((char *)(p + 3))[len] = 0; cmd_end();
    return id;
}
void d9_program_use(Dev9 *d, uint32_t prog)
{
    if (d->prog == prog) return;
    d->prog = prog; uint32_t *p = cmd_begin(DSR_PROG_USE, 4); p[0] = prog; cmd_end();
}
void d9_program_consts(Dev9 *d, const float *regs, int nregs)
{
    (void)d; if (nregs <= 0) return; if (nregs > 4096) nregs = 4096;
    uint32_t *p = cmd_begin(DSR_PROG_CONST, 4 + 16 * (uint32_t)nregs); p[0] = (uint32_t)nregs; memcpy(p + 1, regs, 16 * (size_t)nregs); cmd_end();
}
/* Only the states the technique sets are saved and put back: the game changes other states between Begin and End and
 * keeps its own record of them, which a restore of everything would contradict (the scene then drawn with a stale
 * texture factor: all white, or flashing green). */
typedef struct D9Saved { D9Mask m; uint32_t rs[256], tss[8][33], ss[16][14]; Tex9 *tex[16]; uint32_t prog; } D9Saved;
void *d9_state_save(Dev9 *d, const D9Mask *m)
{
    D9Saved *s = malloc(sizeof *s); s->m = *m; memcpy(s->rs, d->rs, sizeof s->rs); memcpy(s->tss, d->tss, sizeof s->tss); memcpy(s->ss, d->ss, sizeof s->ss);
    for (int k = 0; k < 16; k++) { s->tex[k] = m->tex[k] ? d->tex[k] : NULL; if (s->tex[k]) s->tex[k]->ref++; }
    s->prog = d->prog; return s;
}
static void set_rs(Dev9 *d, uint32_t s, uint32_t v); static void set_tss(Dev9 *d, uint32_t st, uint32_t t, uint32_t v); static void set_ss(Dev9 *d, uint32_t sm, uint32_t t, uint32_t v); static void set_tex(Dev9 *d, uint32_t st, Tex9 *t);
void d9_state_restore(Dev9 *d, void *saved)
{
    D9Saved *s = saved; if (!s) return;
    const D9Mask *m = &s->m;
    { static int lg = -1; if (lg < 0) lg = getenv("D3D9_FXSTATELOG") ? 40 : 0;    /* development: what a full restore would have undone */
      if (lg > 0) for (uint32_t k = 0; k < 256; k++) if (!m->rs[k] && d->rs[k] != s->rs[k] && lg > 0) { lg--; fprintf(stderr, "d3d9: the game changed render state %u (%08x -> %08x) inside an effect\n", k, s->rs[k], d->rs[k]); }
      if (lg > 0) for (uint32_t st = 0; st < 8; st++) for (uint32_t t = 0; t < 33; t++) if (!m->tss[st][t] && d->tss[st][t] != s->tss[st][t] && lg > 0) { lg--; fprintf(stderr, "d3d9: the game changed stage %u state %u (%u -> %u) inside an effect\n", st, t, s->tss[st][t], d->tss[st][t]); } }
    for (uint32_t k = 0; k < 256; k++) if (m->rs[k] && d->rs[k] != s->rs[k]) set_rs(d, k, s->rs[k]);
    for (uint32_t st = 0; st < 8; st++) for (uint32_t t = 0; t < 33; t++) if (m->tss[st][t] && d->tss[st][t] != s->tss[st][t]) set_tss(d, st, t, s->tss[st][t]);
    for (uint32_t sm = 0; sm < 16; sm++) for (uint32_t t = 0; t < 14; t++) if (m->ss[sm][t] && d->ss[sm][t] != s->ss[sm][t]) set_ss(d, sm, t, s->ss[sm][t]);
    for (uint32_t k = 0; k < 16; k++) if (m->tex[k]) { if (d->tex[k] != s->tex[k]) set_tex(d, k, s->tex[k]); if (s->tex[k]) tex_release(s->tex[k]); }
    if (m->prog) d9_program_use(d, s->prog);
    free(s);
}
int d9_shaders_on(void) {
#ifdef __linux__
    /* The initial OpenGL host implements the game's fixed-function path.
     * Do not advertise effects until its shader compiler/consumer is ported. */
    return 0;
#else
    static int on = -1; if (on < 0) { const char *e = getenv("DS2_SHADERS"); on = !(e && !strcmp(e, "0")); } return on;
#endif
}

/* ---- the device ---- */
#define DEV_ME ME(Dev9, O9_DEV)
static void dev_defaults(Dev9 *d);
M(dv_QueryInterface, 3) DEV_ME rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(D3D_OK, 3); END
M(dv_AddRef, 1) DEV_ME RET((uint32_t)++me->ref, 1); END
M(dv_Release, 1) DEV_ME RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END   /* the device itself is kept */
M(dv_TestCooperativeLevel, 1) RET(D3D_OK, 1); END
M(dv_GetAvailableTextureMem, 1) RET(1024u << 20, 1); END
OK_M(dv_EvictManagedResources, 1)
M(dv_GetDirect3D, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->g_d3d); RET(D3D_OK, 2); END
static void fill_caps(uint32_t out);
M(dv_GetDeviceCaps, 2) fill_caps(ARG(1)); RET(D3D_OK, 2); END
M(dv_GetDisplayMode, 3) DEV_ME
    uint32_t m[4] = {me->pp[0], me->pp[1], 60, 22}; memcpy(GP(ARG(2)), m, 16); RET(D3D_OK, 3);
END
M(dv_GetCreationParameters, 2) DEV_ME
    uint32_t v[4] = {0, 1, me->hwnd, me->behavior}; memcpy(GP(ARG(1)), v, 16); RET(D3D_OK, 2);
END
M(dv_SetCursorProperties, 4) DEV_ME                     /* (hotspot x, y, surface): the image is kept for Present */
    Surf9 *s = obj(ARG(3), O9_SURF); if (!s) RET(D3DERR_INVALIDCALL, 4);
    me->cursor = s; me->cursor_hx = (int)ARG(1); me->cursor_hy = (int)ARG(2); RET(D3D_OK, 4);
END
M(dv_SetCursorPosition, 4) DEV_ME me->cursor_x = (int)ARG(1); me->cursor_y = (int)ARG(2); RET(0, 4); END
M(dv_ShowCursor, 2) DEV_ME int o = me->cursor_show; me->cursor_show = ARG(1) != 0; RET((uint32_t)o, 2); END
TODO_M(dv_CreateAdditionalSwapChain, 3, D3DERR_INVALIDCALL)
M(dv_GetSwapChain, 3) DEV_ME if (ARG(1)) RET(D3DERR_INVALIDCALL, 3); rt_w32(G_MEM, ARG(2), me->g_swap); me->ref++; RET(D3D_OK, 3); END
M(dv_GetNumberOfSwapChains, 1) RET(1, 1); END
static uint32_t dev_reset(Dev9 *d, uint32_t pp);
M(dv_Reset, 2) DEV_ME RET(dev_reset(me, ARG(1)), 2); END
static void cursor_overlay(Dev9 *d);
static void dev_present(Ctx *c, Dev9 *d)
{
    cursor_overlay(d); dsr_present(c, d->back->id);
    static int fps = -1; if (fps < 0) fps = getenv("D3D9_FPS") != 0;   /* D3D9_FPS=1: frames per second every 2 s */
    if (fps) {
        static double t0; static int n; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double t = ts.tv_sec + ts.tv_nsec / 1e9; n++;
        if (!t0) t0 = t; else if (t - t0 >= 2) { fprintf(stderr, "d3d9: %.1f fps\n", n / (t - t0)); t0 = t; n = 0; }
    }
}
M(dv_Present, 5) DEV_ME dev_present(c, me); RET(D3D_OK, 5); END
M(dv_GetBackBuffer, 5) DEV_ME                          /* (swap chain, index, type, surface) */
    if (ARG(1) || ARG(2)) RET(D3DERR_INVALIDCALL, 5);
    rt_w32(G_MEM, ARG(4), me->back->g); surf_addref(me->back); RET(D3D_OK, 5);
END
M(dv_GetRasterStatus, 3) uint32_t v[2] = {0, 0}; memcpy(GP(ARG(2)), v, 8); RET(D3D_OK, 3); END
OK_M(dv_SetDialogBoxMode, 2)
M(dv_SetGammaRamp, 4)
    uint32_t *p = cmd_begin(DSR_GAMMA, 1536); memcpy(p, GP(ARG(3)), 1536); cmd_end(); RET(0, 4);
END
M(dv_GetGammaRamp, 3) uint16_t r[768]; for (int k = 0; k < 768; k++) r[k] = (uint16_t)((k & 255) * 257); memcpy(GP(ARG(2)), r, 1536); RET(0, 3); END
static void vol_alloc(Tex9 *t)                          /* volume textures: every slice of each level in the CPU copy */
{
    uint32_t dep = t->d;
    for (uint32_t l = 0; l < t->levels; l++) {
        Surf9 *s = t->lv[0][l]; gfree_(s->mem, s->size);
        s->size = s->pitch * d9_rows(s->fmt, s->h) * (dep ? dep : 1); s->mem = gmem(s->size);
        dep = dep > 1 ? dep / 2 : 1;
    }
}
static uint32_t create_tex(Dev9 *d, int type, uint32_t w, uint32_t h, uint32_t dep, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out)
{
    { static int lg = -1; if (lg < 0) lg = getenv("D3D9_CREATELOG") != 0;
      if (lg) fprintf(stderr, "d3d9: create %s %ux%ux%u levels %u usage %#x format %#x pool %u\n", type == O9_CUBE ? "cube" : type == O9_VOLTEX ? "volume" : "texture", w, h, dep, levels, usage, fmt, pool); }
    if (!d9_fmt_known(fmt)) { fprintf(stderr, "d3d9: texture format %#x is not supported\n", fmt); return D3DERR_INVALIDCALL; }
    if (!w || !h || w > 8192 || h > 8192) return D3DERR_INVALIDCALL;
    Tex9 *t = tex_new(d, type, w, h, dep, levels, usage, fmt, pool);
    if (type == O9_VOLTEX) vol_alloc(t);
    rt_w32(G_MEM, out, t->g); return D3D_OK;
}
M(dv_CreateTexture, 9) DEV_ME RET(create_tex(me, O9_TEX, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6), ARG(7)), 9); END
M(dv_CreateVolumeTexture, 10) DEV_ME RET(create_tex(me, O9_VOLTEX, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8)), 10); END
M(dv_CreateCubeTexture, 8) DEV_ME RET(create_tex(me, O9_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5), ARG(6)), 8); END
static uint32_t create_buf(Dev9 *d, int index, uint32_t len, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out)
{
    if (!len || len > (256u << 20)) return D3DERR_INVALIDCALL;
    Buf9 *b = calloc(1, sizeof *b); b->ref = 1; b->dev = d; b->index = index; b->len = len; b->usage = usage; b->fmt = fmt; b->pool = pool; b->mem = gmem(len);
    if (gpu_vb()) { b->gid = vbid_alloc(); b->dlo = 0; b->dhi = len; }
    b->g = make_iface(index ? O9_IB : O9_VB, b); rt_w32(G_MEM, out, b->g); return D3D_OK;
}
M(dv_CreateVertexBuffer, 7) DEV_ME RET(create_buf(me, 0, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)), 7); END
M(dv_CreateIndexBuffer, 7) DEV_ME RET(create_buf(me, 1, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)), 7); END
M(dv_CreateRenderTarget, 9) DEV_ME                     /* (w, h, format, multisample, quality, lockable, surface, shared) */
    new_samples = ms_samples(ARG(4)); Surf9 *s = surf_new(me, ARG(1), ARG(2), ARG(3), USAGE_RENDERTARGET, 0, 1, 0, 0, 0, 0, 0); rt_w32(G_MEM, ARG(7), s->g); RET(D3D_OK, 9);
END
M(dv_CreateDepthStencilSurface, 9) DEV_ME
    new_samples = ms_samples(ARG(4)); Surf9 *s = surf_new(me, ARG(1), ARG(2), ARG(3), USAGE_DEPTHSTENCIL, 0, 2, 0, 0, 0, 0, 0); rt_w32(G_MEM, ARG(7), s->g); RET(D3D_OK, 9);
END
M(dv_CreateOffscreenPlainSurface, 7) DEV_ME            /* (w, h, format, pool, surface, shared) */
    if (!d9_fmt_known(ARG(3))) RET(D3DERR_INVALIDCALL, 7);
    Surf9 *s = surf_new(me, ARG(1), ARG(2), ARG(3), 0, ARG(4), 4, 0, 0, 0, 0, 0); rt_w32(G_MEM, ARG(5), s->g); RET(D3D_OK, 7);
END
M(dv_UpdateSurface, 5)                                 /* (src, src rect, dst, dst point): whole surfaces of one format */
    Surf9 *s = obj(ARG(1), O9_SURF), *t = obj(ARG(3), O9_SURF);
    if (!s || !t || s->fmt != t->fmt) RET(D3DERR_INVALIDCALL, 5);
    if (ARG(2) || ARG(4)) { static int w; if (!w++) fprintf(stderr, "d3d9: UpdateSurface with rectangles: whole surface copied\n"); }
    memcpy(G_MEM + t->mem, G_MEM + s->mem, s->size < t->size ? s->size : t->size); surf_upload(t, 0, t->h); RET(D3D_OK, 5);
END
M(dv_UpdateTexture, 3)
    Tex9 *s = any_tex(ARG(1)), *t = any_tex(ARG(2)); if (!s || !t || s->fmt != t->fmt) RET(D3DERR_INVALIDCALL, 3);
    int faces = t->type == O9_CUBE ? 6 : 1;
    for (int f = 0; f < faces; f++) for (uint32_t l = 0; l < t->levels && l < s->levels; l++) {
        Surf9 *a = s->lv[f][l], *b = t->lv[f][l]; memcpy(G_MEM + b->mem, G_MEM + a->mem, a->size < b->size ? a->size : b->size); surf_upload(b, 0, b->h);
    }
    RET(D3D_OK, 3);
END
M(dv_GetRenderTargetData, 3)
    Surf9 *s = obj(ARG(1), O9_SURF), *t = obj(ARG(2), O9_SURF); if (!s || !t || d9_fmt_bytes(t->fmt) != 4) RET(D3DERR_INVALIDCALL, 3);
    int32_t r[4] = {0, 0, (int32_t)s->w, (int32_t)s->h}; cmd_readback(s->id, r, G_MEM + t->mem, t->pitch, 4); RET(D3D_OK, 3);
END
M(dv_GetFrontBufferData, 3) DEV_ME
    Surf9 *t = obj(ARG(2), O9_SURF); if (!t) RET(D3DERR_INVALIDCALL, 3);
    int32_t r[4] = {0, 0, (int32_t)me->back->w, (int32_t)me->back->h}; cmd_readback(me->back->id, r, G_MEM + t->mem, t->pitch, 4); RET(D3D_OK, 3);
END
static void rect_of(Surf9 *s, uint32_t g, int32_t *r) { if (g) memcpy(r, GP(g), 16); else { r[0] = 0; r[1] = 0; r[2] = (int32_t)s->w; r[3] = (int32_t)s->h; } }
M(dv_StretchRect, 6)                                   /* (src, src rect, dst, dst rect, filter) */
    Surf9 *s = obj(ARG(1), O9_SURF), *t = obj(ARG(3), O9_SURF); if (!s || !t) RET(D3DERR_INVALIDCALL, 6);
    uint32_t *p = cmd_begin(DSR_BLT, 48); p[0] = t->id; rect_of(t, ARG(4), (int32_t *)p + 1); p[5] = s->id; rect_of(s, ARG(2), (int32_t *)p + 6); p[10] = 0; p[11] = 0; cmd_end();
    t->gpu_newer = 1; RET(D3D_OK, 6);
END
M(dv_ColorFill, 4)                                     /* (surface, rect, color) */
    Surf9 *t = obj(ARG(1), O9_SURF); if (!t) RET(D3DERR_INVALIDCALL, 4);
    uint32_t *p = cmd_begin(DSR_BLT, 48); p[0] = t->id; rect_of(t, ARG(2), (int32_t *)p + 1); p[5] = 0; memset(p + 6, 0, 16); p[10] = 0x400; p[11] = ARG(3); cmd_end();
    t->gpu_newer = 1; RET(D3D_OK, 4);
END
M(dv_SetRenderTarget, 3) DEV_ME                        /* (index, surface); index 0 only; the viewport becomes the whole target */
    if (ARG(1)) { if (!ARG(2)) RET(D3D_OK, 3); static int w; if (!w++) fprintf(stderr, "d3d9: multiple render targets are not supported\n"); RET(D3DERR_INVALIDCALL, 3); }
    Surf9 *s = obj(ARG(2), O9_SURF); if (!s) RET(D3DERR_INVALIDCALL, 3);
    surf_addref(s); if (me->rt) surf_release(me->rt); me->rt = s; set_rt(me);
    uint32_t vp[6] = {0, 0, s->w, s->h, 0, u32f(1.0f)}; memcpy(me->vp, vp, 24); uint32_t *p = cmd_begin(DSR_VIEWPORT, 24); memcpy(p, vp, 24); cmd_end();
    RET(D3D_OK, 3);
END
M(dv_GetRenderTarget, 3) DEV_ME if (ARG(1) || !me->rt) RET(D3DERR_NOTFOUND, 3); rt_w32(G_MEM, ARG(2), me->rt->g); surf_addref(me->rt); RET(D3D_OK, 3); END
M(dv_SetDepthStencilSurface, 2) DEV_ME
    Surf9 *s = ARG(1) ? obj(ARG(1), O9_SURF) : 0;
    { static int lg = -1, n; if (lg < 0) lg = getenv("D3D9_RTLOG") != 0; if (lg && n++ < 200) fprintf(stderr, "rtlog: SetDepthStencilSurface(%08x) from %08x -> %s\n", ARG(1), rt_r32(G_MEM, c->esp), ARG(1) && !s ? "not a surface" : "ok"); }
    if (ARG(1) && !s) RET(D3DERR_INVALIDCALL, 2);
    if (s) surf_addref(s); if (me->ds) surf_release(me->ds); me->ds = s; set_rt(me); RET(D3D_OK, 2);
END
M(dv_GetDepthStencilSurface, 2) DEV_ME if (!me->ds) RET(D3DERR_NOTFOUND, 2); rt_w32(G_MEM, ARG(1), me->ds->g); surf_addref(me->ds); RET(D3D_OK, 2); END
OK_M(dv_BeginScene, 1)
OK_M(dv_EndScene, 1)
M(dv_Clear, 7) DEV_ME                                  /* (count, rects, flags, color, z, stencil) */
    uint32_t n = ARG(1), rects = ARG(2); if (!rects || n > 4096) n = 0;
    uint32_t *p = cmd_begin(DSR_CLEAR, 20 + 16 * n);
    p[0] = ARG(3); p[1] = ARG(4); p[2] = ARG(5); p[3] = ARG(6); p[4] = n; if (n) memcpy(p + 5, GP(rects), 16 * n);
    cmd_end(); if (me->rt) me->rt->gpu_newer = 1; RET(D3D_OK, 7);
END
M(dv_SetTransform, 3) DEV_ME float m[16]; memcpy(m, GP(ARG(2)), 64); set_xf(me, ARG(1), m); RET(D3D_OK, 3); END
M(dv_GetTransform, 3) DEV_ME if (ARG(1) >= 512) RET(D3DERR_INVALIDCALL, 3); memcpy(GP(ARG(2)), me->xf[ARG(1)], 64); RET(D3D_OK, 3); END
M(dv_MultiplyTransform, 3) DEV_ME                      /* current = matrix x current */
    if (ARG(1) >= 512) RET(D3DERR_INVALIDCALL, 3);
    float a[16], r[16]; memcpy(a, GP(ARG(2)), 64); const float *b = me->xf[ARG(1)];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j] + a[4 * i + 3] * b[12 + j];
    set_xf(me, ARG(1), r); RET(D3D_OK, 3);
END
M(dv_SetViewport, 2) DEV_ME uint32_t v[6]; memcpy(v, GP(ARG(1)), 24); set_vp(me, v); RET(D3D_OK, 2); END
M(dv_GetViewport, 2) DEV_ME memcpy(GP(ARG(1)), me->vp, 24); RET(D3D_OK, 2); END
M(dv_SetMaterial, 2) DEV_ME memcpy(me->material, GP(ARG(1)), 68); RET(D3D_OK, 2); END
M(dv_GetMaterial, 2) DEV_ME memcpy(GP(ARG(1)), me->material, 68); RET(D3D_OK, 2); END
M(dv_SetLight, 3) DEV_ME if (ARG(1) < 8) memcpy(me->light[ARG(1)], GP(ARG(2)), 104); RET(D3D_OK, 3); END
M(dv_GetLight, 3) DEV_ME if (ARG(1) >= 8) RET(D3DERR_INVALIDCALL, 3); memcpy(GP(ARG(2)), me->light[ARG(1)], 104); RET(D3D_OK, 3); END
M(dv_LightEnable, 3) DEV_ME if (ARG(1) < 8) me->light_on[ARG(1)] = ARG(2) != 0; RET(D3D_OK, 3); END
M(dv_GetLightEnable, 3) DEV_ME rt_w32(G_MEM, ARG(2), ARG(1) < 8 && me->light_on[ARG(1)] ? 128 : 0); RET(D3D_OK, 3); END
M(dv_SetClipPlane, 3) DEV_ME if (ARG(1) < 6) memcpy(me->clip[ARG(1)], GP(ARG(2)), 16); RET(D3D_OK, 3); END
M(dv_GetClipPlane, 3) DEV_ME if (ARG(1) >= 6) RET(D3DERR_INVALIDCALL, 3); memcpy(GP(ARG(2)), me->clip[ARG(1)], 16); RET(D3D_OK, 3); END
M(dv_SetRenderState, 3) DEV_ME set_rs(me, ARG(1), ARG(2)); RET(D3D_OK, 3); END
M(dv_GetRenderState, 3) DEV_ME rt_w32(G_MEM, ARG(2), ARG(1) < 256 ? me->rs[ARG(1)] : 0); RET(D3D_OK, 3); END
M(dv_CreateStateBlock, 3) DEV_ME SB9 *b = sb_new(me); sb_all(me, b); rt_w32(G_MEM, ARG(2), b->g); RET(D3D_OK, 3); END
M(dv_BeginStateBlock, 1) DEV_ME if (me->rec) RET(D3DERR_INVALIDCALL, 1); me->rec = sb_new(me); RET(D3D_OK, 1); END
M(dv_EndStateBlock, 2) DEV_ME if (!me->rec) RET(D3DERR_INVALIDCALL, 2); rt_w32(G_MEM, ARG(1), me->rec->g); me->rec = 0; RET(D3D_OK, 2); END
OK_M(dv_SetClipStatus, 2)
M(dv_GetClipStatus, 2) memset(GP(ARG(1)), 0, 8); RET(D3D_OK, 2); END
M(dv_GetTexture, 3) DEV_ME if (ARG(1) >= 16) RET(D3DERR_INVALIDCALL, 3); Tex9 *t = me->tex[ARG(1)]; rt_w32(G_MEM, ARG(2), t ? t->g : 0); if (t) t->ref++; RET(D3D_OK, 3); END
M(dv_SetTexture, 3) DEV_ME
    Tex9 *t = ARG(2) ? any_tex(ARG(2)) : 0; if (ARG(2) && !t) RET(D3DERR_INVALIDCALL, 3);
    set_tex(me, ARG(1) >= 0x100000 ? 16 : ARG(1), t); RET(D3D_OK, 3);     /* (vertex-texture samplers are ignored) */
END
M(dv_GetTextureStageState, 4) DEV_ME rt_w32(G_MEM, ARG(3), ARG(1) < 8 && ARG(2) < 33 ? me->tss[ARG(1)][ARG(2)] : 0); RET(D3D_OK, 4); END
M(dv_SetTextureStageState, 4) DEV_ME set_tss(me, ARG(1), ARG(2), ARG(3)); RET(D3D_OK, 4); END
M(dv_GetSamplerState, 4) DEV_ME rt_w32(G_MEM, ARG(3), ARG(1) < 16 && ARG(2) < 14 ? me->ss[ARG(1)][ARG(2)] : 0); RET(D3D_OK, 4); END
M(dv_SetSamplerState, 4) DEV_ME set_ss(me, ARG(1), ARG(2), ARG(3)); RET(D3D_OK, 4); END
M(dv_ValidateDevice, 2) if (ARG(1)) rt_w32(G_MEM, ARG(1), 1); RET(D3D_OK, 2); END
OK_M(dv_SetPaletteEntries, 3)
TODO_M(dv_GetPaletteEntries, 3, D3DERR_INVALIDCALL)
OK_M(dv_SetCurrentTexturePalette, 2)
M(dv_GetCurrentTexturePalette, 2) rt_w32(G_MEM, ARG(1), 0); RET(D3D_OK, 2); END
M(dv_SetScissorRect, 2) DEV_ME memcpy(me->scissor, GP(ARG(1)), 16); { uint32_t *p = cmd_begin(DSR_SCISSOR, 16); memcpy(p, me->scissor, 16); cmd_end(); } RET(D3D_OK, 2); END
M(dv_GetScissorRect, 2) DEV_ME memcpy(GP(ARG(1)), me->scissor, 16); RET(D3D_OK, 2); END
OK_M(dv_SetSoftwareVertexProcessing, 2)
M(dv_GetSoftwareVertexProcessing, 1) RET(0, 1); END
OK_M(dv_SetNPatchMode, 2)
M(dv_GetNPatchMode, 1) FPUSH(0.0); RET(0, 1); END
/* the game's own vertex shader bound: its program (d3d9_shader.c) and constants, then a programmable draw */
static int shader_bind(Dev9 *d)
{
    uint32_t id = d9_shader_program(d); if (!id) return 0;
    { static int lg = -1; if (lg < 0) lg = getenv("D3D9_SHADERLOG") != 0;     /* each program's declarations, once */
      static uint64_t seen[4096]; static int ns;
      if (lg && d->decl) { uint64_t key = (uint64_t)id << 32 | (uint32_t)(uintptr_t)d->decl; int f = 0; for (int k = 0; k < ns; k++) if (seen[k] == key) f = 1;
        if (!f && ns < 4096) { seen[ns++] = key; fprintf(stderr, "d3d9: program %u declaration:", id);
          for (int k = 0; k < d->decl->n; k++) fprintf(stderr, " [s%d +%d type %d usage %d/%d]", d->decl->e[k].stream, d->decl->e[k].off, d->decl->e[k].type, d->decl->e[k].usage, d->decl->e[k].uidx);
          fprintf(stderr, "\n"); } } }
    d9_program_use(d, id); d9_shader_consts(d, id); return 1;
}
M(dv_DrawPrimitive, 4) DEV_ME                          /* (type, start vertex, primitives) */
    if (me->vs) { if (shader_bind(me) && !prog_draw_vb(me, ARG(1), (int32_t)ARG(2), 0, prim_verts(ARG(1), ARG(3)), 0, 0)) prog_draw(me, ARG(1), ARG(2), prim_verts(ARG(1), ARG(3)), 0, 0, 0, 0); RET(D3D_OK, 4); }
    if (me->prog) { prog_draw(me, ARG(1), ARG(2), prim_verts(ARG(1), ARG(3)), 0, 0, 0, 0); RET(D3D_OK, 4); }
    if (!can_draw(me)) RET(D3D_OK, 4);
    uint32_t nv = prim_verts(ARG(1), ARG(3)), fvf; const uint8_t *v = gather(me, ARG(2), nv, &fvf, 0, 0);
    if (v) emit(me, ARG(1), fvf, v, nv, 0, 0); RET(D3D_OK, 4);
END
M(dv_DrawIndexedPrimitive, 7) DEV_ME                   /* (type, base vertex, min index, vertices, start index, primitives) */
    if (me->vs && (!me->ib || !shader_bind(me))) RET(D3D_OK, 7);
    if ((!me->prog && !can_draw(me)) || !me->ib) RET(D3D_OK, 7);
    uint32_t ni = prim_verts(ARG(1), ARG(6)), is = me->ib->fmt == 102 ? 4 : 2;
    if ((uint64_t)(ARG(5) + ni) * is > me->ib->len) { static int w; if (!w++) fprintf(stderr, "d3d9: indices past the index buffer (skipped)\n"); RET(D3D_OK, 7); }
    if (me->vs && prog_draw_vb(me, ARG(1), (int32_t)ARG(2), ARG(3), ARG(4), ni, ARG(5))) RET(D3D_OK, 7);
    (me->prog ? prog_draw_indexed : draw_indexed)(me, ARG(1), (int32_t)ARG(2), ARG(3), ARG(4), G_MEM + me->ib->mem + ARG(5) * is, is == 4, ni, 0, 0); RET(D3D_OK, 7);
END
M(dv_DrawPrimitiveUP, 5) DEV_ME                        /* (type, primitives, data, stride) */
    if (me->vs) { if (shader_bind(me)) prog_draw(me, ARG(1), 0, prim_verts(ARG(1), ARG(2)), 0, 0, G_MEM + ARG(3), ARG(4)); RET(D3D_OK, 5); }
    if (me->prog) { prog_draw(me, ARG(1), 0, prim_verts(ARG(1), ARG(2)), 0, 0, G_MEM + ARG(3), ARG(4)); RET(D3D_OK, 5); }
    if (!can_draw(me)) RET(D3D_OK, 5);
    uint32_t nv = prim_verts(ARG(1), ARG(2)), fvf; const uint8_t *v = gather(me, 0, nv, &fvf, G_MEM + ARG(3), ARG(4));
    if (v) emit(me, ARG(1), fvf, v, nv, 0, 0); RET(D3D_OK, 5);
END
M(dv_DrawIndexedPrimitiveUP, 9) DEV_ME                 /* (type, min index, vertices, primitives, indices, index format, data, stride) */
    if (me->vs && !shader_bind(me)) RET(D3D_OK, 9);
    if (!me->prog && !can_draw(me)) RET(D3D_OK, 9);
    uint32_t ni = prim_verts(ARG(1), ARG(4)), stride = ARG(8);
    (me->prog ? prog_draw_indexed : draw_indexed)(me, ARG(1), 0, ARG(2), ARG(3), G_MEM + ARG(5), ARG(6) == 102, ni, G_MEM + ARG(7) + ARG(2) * stride, stride); RET(D3D_OK, 9);
END
TODO_M(dv_ProcessVertices, 7, D3DERR_INVALIDCALL)
M(dv_CreateVertexDeclaration, 3) DEV_ME
    uint32_t e = ARG(1), n = 0; while (n < 64 && rt_r16(G_MEM, e + 8 * n) != 0xff) n++;
    Decl9 *d = calloc(1, sizeof *d); d->ref = 1; d->dev = me; d->nraw = n + 1; d->raw = gmem(8 * (n + 1)); memcpy(GP(d->raw), GP(e), 8 * (n + 1));
    for (uint32_t k = 0; k < n && d->n < 32; k++) {
        uint8_t *x = (uint8_t *)GP(e + 8 * k); DElem *de = &d->e[d->n++];
        de->stream = x[0]; de->off = (uint16_t)(x[2] | x[3] << 8); de->type = x[4]; de->usage = x[6]; de->uidx = x[7];
        if (de->stream >= 16) d->n--;
    }
    decl_layout(d); d->g = make_iface(O9_DECL, d); rt_w32(G_MEM, ARG(2), d->g); RET(D3D_OK, 3);
END
M(dv_SetVertexDeclaration, 2) DEV_ME
    Decl9 *d = ARG(1) ? obj(ARG(1), O9_DECL) : 0;
    if (me->rec) { RecEnt e = {REC_DECL, 0, 0}; e.p = d; rec_put(me->rec, e); RET(D3D_OK, 2); }
    me->decl = d; RET(D3D_OK, 2);
END
M(dv_GetVertexDeclaration, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->decl ? me->decl->g : 0); if (me->decl) me->decl->ref++; RET(D3D_OK, 2); END
M(dv_SetFVF, 2) DEV_ME
    if (me->rec) { RecEnt e = {REC_FVF, ARG(1), 0}; rec_put(me->rec, e); RET(D3D_OK, 2); }
    me->fvf = ARG(1); if (ARG(1)) me->decl = 0; RET(D3D_OK, 2);
END
M(dv_GetFVF, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->decl ? me->decl->fvf : me->fvf); RET(D3D_OK, 2); END
static uint32_t create_shader(Dev9 *d, int ps, uint32_t code, uint32_t out)
{
    if (!code) return D3DERR_INVALIDCALL;
    Shader9 *s = calloc(1, sizeof *s); s->ref = 1; s->dev = d; s->ps = ps; s->len = shader_len(code); s->mem = gmem(s->len); memcpy(GP(s->mem), GP(code), s->len);
    s->g = make_iface(ps ? O9_PS : O9_VS, s); rt_w32(G_MEM, out, s->g); return D3D_OK;
}
M(dv_CreateVertexShader, 3) DEV_ME RET(create_shader(me, 0, ARG(1), ARG(2)), 3); END
M(dv_SetVertexShader, 2) DEV_ME
    Shader9 *s = ARG(1) ? obj(ARG(1), O9_VS) : 0;
    if (me->rec) { RecEnt e = {REC_VS, 0, 0}; e.p = s; rec_put(me->rec, e); RET(D3D_OK, 2); }
    me->vs = s; d9_program_use(me, 0); RET(D3D_OK, 2);   /* the game's own shader (or none) replaces an effect's */
END
M(dv_GetVertexShader, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->vs ? me->vs->g : 0); if (me->vs) me->vs->ref++; RET(D3D_OK, 2); END
static uint32_t set_consts(float (*dst)[4], uint32_t max, uint32_t start, uint32_t data, uint32_t n, int get)
{
    if (start + n > max) return D3DERR_INVALIDCALL;
    if (get) memcpy(GP(data), dst[start], 16 * n); else memcpy(dst[start], GP(data), 16 * n);
    return D3D_OK;
}
M(dv_SetVertexShaderConstantF, 4) DEV_ME RET(set_consts(me->vsc, 256, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetVertexShaderConstantF, 4) DEV_ME RET(set_consts(me->vsc, 256, ARG(1), ARG(2), ARG(3), 1), 4); END
static uint32_t set_iconsts(int32_t (*dst)[4], uint32_t start, uint32_t data, uint32_t n, int get)
{
    if (start + n > 16) return D3DERR_INVALIDCALL;
    if (get) memcpy(GP(data), dst[start], 16 * n); else memcpy(dst[start], GP(data), 16 * n);
    return D3D_OK;
}
static uint32_t set_bconsts(uint32_t *dst, uint32_t start, uint32_t data, uint32_t n, int get)
{
    if (start + n > 16) return D3DERR_INVALIDCALL;
    if (get) memcpy(GP(data), dst + start, 4 * n); else memcpy(dst + start, GP(data), 4 * n);
    return D3D_OK;
}
M(dv_SetVertexShaderConstantI, 4) DEV_ME RET(set_iconsts(me->vsi, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetVertexShaderConstantI, 4) DEV_ME RET(set_iconsts(me->vsi, ARG(1), ARG(2), ARG(3), 1), 4); END
M(dv_SetVertexShaderConstantB, 4) DEV_ME RET(set_bconsts(me->vsb, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetVertexShaderConstantB, 4) DEV_ME RET(set_bconsts(me->vsb, ARG(1), ARG(2), ARG(3), 1), 4); END
M(dv_SetStreamSource, 5) DEV_ME                        /* (stream, buffer, offset, stride) */
    if (ARG(1) >= 16) RET(D3DERR_INVALIDCALL, 5);
    Buf9 *b = ARG(2) ? obj(ARG(2), O9_VB) : 0; if (ARG(2) && !b) RET(D3DERR_INVALIDCALL, 5);
    if (b) b->ref++; if (me->vb[ARG(1)]) buf_release(me->vb[ARG(1)]);
    me->vb[ARG(1)] = b; me->vb_off[ARG(1)] = ARG(3); me->vb_stride[ARG(1)] = ARG(4); RET(D3D_OK, 5);
END
M(dv_GetStreamSource, 5) DEV_ME
    if (ARG(1) >= 16) RET(D3DERR_INVALIDCALL, 5);
    Buf9 *b = me->vb[ARG(1)]; rt_w32(G_MEM, ARG(2), b ? b->g : 0); if (b) b->ref++;
    rt_w32(G_MEM, ARG(3), me->vb_off[ARG(1)]); rt_w32(G_MEM, ARG(4), me->vb_stride[ARG(1)]); RET(D3D_OK, 5);
END
OK_M(dv_SetStreamSourceFreq, 3)
M(dv_GetStreamSourceFreq, 3) rt_w32(G_MEM, ARG(2), 1); RET(D3D_OK, 3); END
M(dv_SetIndices, 2) DEV_ME
    Buf9 *b = ARG(1) ? obj(ARG(1), O9_IB) : 0; if (ARG(1) && !b) RET(D3DERR_INVALIDCALL, 2);
    if (b) b->ref++; if (me->ib) buf_release(me->ib); me->ib = b; RET(D3D_OK, 2);
END
M(dv_GetIndices, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->ib ? me->ib->g : 0); if (me->ib) me->ib->ref++; RET(D3D_OK, 2); END
M(dv_CreatePixelShader, 3) DEV_ME RET(create_shader(me, 1, ARG(1), ARG(2)), 3); END
M(dv_SetPixelShader, 2) DEV_ME
    Shader9 *s = ARG(1) ? obj(ARG(1), O9_PS) : 0;
    if (me->rec) { RecEnt e = {REC_PS, 0, 0}; e.p = s; rec_put(me->rec, e); RET(D3D_OK, 2); }
    me->ps = s; d9_program_use(me, 0); RET(D3D_OK, 2);
END
M(dv_GetPixelShader, 2) DEV_ME rt_w32(G_MEM, ARG(1), me->ps ? me->ps->g : 0); if (me->ps) me->ps->ref++; RET(D3D_OK, 2); END
M(dv_SetPixelShaderConstantF, 4) DEV_ME RET(set_consts(me->psc, 224, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetPixelShaderConstantF, 4) DEV_ME RET(set_consts(me->psc, 224, ARG(1), ARG(2), ARG(3), 1), 4); END
M(dv_SetPixelShaderConstantI, 4) DEV_ME RET(set_iconsts(me->psi, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetPixelShaderConstantI, 4) DEV_ME RET(set_iconsts(me->psi, ARG(1), ARG(2), ARG(3), 1), 4); END
M(dv_SetPixelShaderConstantB, 4) DEV_ME RET(set_bconsts(me->psb, ARG(1), ARG(2), ARG(3), 0), 4); END
M(dv_GetPixelShaderConstantB, 4) DEV_ME RET(set_bconsts(me->psb, ARG(1), ARG(2), ARG(3), 1), 4); END
TODO_M(dv_DrawRectPatch, 4, D3DERR_INVALIDCALL)
TODO_M(dv_DrawTriPatch, 4, D3DERR_INVALIDCALL)
TODO_M(dv_DeletePatch, 2, D3DERR_INVALIDCALL)
M(dv_CreateQuery, 3) DEV_ME                            /* event (8) and occlusion (9) queries; NULL out: "is it supported" */
    uint32_t t = ARG(1); if (t != 8 && t != 9) RET(D3DERR_NOTAVAILABLE, 3);
    if (!ARG(2)) RET(D3D_OK, 3);
    Query9 *q = calloc(1, sizeof *q); q->ref = 1; q->dev = me; q->type = t; q->last = 1000;
    if (t == 9) { pthread_mutex_lock(&query_lock); for (uint32_t k = 1; k < DSR_QUERY_SLOTS; k++) if (!query_slot_used[k]) { query_slot_used[k] = 1; q->slot = k; break; } pthread_mutex_unlock(&query_lock); }
    q->g = make_iface(O9_QUERY, q); rt_w32(G_MEM, ARG(2), q->g); RET(D3D_OK, 3);
END

/* ---- swap chain (the device's one) ---- */
M(sw_QueryInterface, 3) rt_w32(G_MEM, ARG(2), ARG(0)); RET(D3D_OK, 3); END
M(sw_AddRef, 1) RET(1, 1); END
M(sw_Release, 1) RET(1, 1); END
M(sw_Present, 6) dev_present(c, the_dev); RET(D3D_OK, 6); END
M(sw_GetFrontBufferData, 2) Surf9 *t = obj(ARG(1), O9_SURF); if (!t) RET(D3DERR_INVALIDCALL, 2);
    int32_t r[4] = {0, 0, (int32_t)the_dev->back->w, (int32_t)the_dev->back->h}; cmd_readback(the_dev->back->id, r, G_MEM + t->mem, t->pitch, 4); RET(D3D_OK, 2);
END
M(sw_GetBackBuffer, 4) if (ARG(1)) RET(D3DERR_INVALIDCALL, 4); rt_w32(G_MEM, ARG(3), the_dev->back->g); surf_addref(the_dev->back); RET(D3D_OK, 4); END
M(sw_GetRasterStatus, 2) uint32_t v[2] = {0, 0}; memcpy(GP(ARG(1)), v, 8); RET(D3D_OK, 2); END
M(sw_GetDisplayMode, 2) uint32_t m[4] = {the_dev->pp[0], the_dev->pp[1], 60, 22}; memcpy(GP(ARG(1)), m, 16); RET(D3D_OK, 2); END
M(sw_GetDevice, 2) rt_w32(G_MEM, ARG(1), the_dev->g); the_dev->ref++; RET(D3D_OK, 2); END
M(sw_GetPresentParameters, 2) memcpy(GP(ARG(1)), the_dev->pp, 56); RET(D3D_OK, 2); END

/* ---- the hardware cursor (SetCursorProperties): composited over the frame at Present ---- */
static void cursor_overlay(Dev9 *d)
{
    Surf9 *s = d->cursor; if (!d->cursor_show || !s || d9_fmt_bytes(s->fmt) != 4) return;
    int32_t x = d->cursor_x - d->cursor_hx, y = d->cursor_y - d->cursor_hy, w = (int32_t)s->w, h = (int32_t)s->h;
    int32_t x0 = x < 0 ? -x : 0, y0 = y < 0 ? -y : 0, x1 = x + w > (int32_t)d->back->w ? (int32_t)d->back->w - x : w, y1 = y + h > (int32_t)d->back->h ? (int32_t)d->back->h - y : h;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t cw = (uint32_t)(x1 - x0), ch = (uint32_t)(y1 - y0), row = cw * 4;
    uint32_t *p = cmd_begin(DSR_UPLOAD_OVER, 24 + row * ch);
    p[0] = d->back->id; p[1] = (uint32_t)(x + x0); p[2] = (uint32_t)(y + y0); p[3] = cw; p[4] = ch; p[5] = row;
    uint8_t *o = (uint8_t *)(p + 6);
    for (uint32_t r = 0; r < ch; r++) for (uint32_t k = 0; k < cw; k++) {
        uint32_t v; memcpy(&v, G_MEM + s->mem + (uint32_t)(y0 + (int32_t)r) * s->pitch + 4u * (uint32_t)(x0 + (int32_t)k), 4);
        uint32_t a = s->fmt == 22 ? 255 : v >> 24;                /* premultiplied for the "over" composite */
        uint32_t pm = a << 24 | ((v >> 16 & 255) * a / 255) << 16 | ((v >> 8 & 255) * a / 255) << 8 | (v & 255) * a / 255;
        memcpy(o + r * row + 4 * k, &pm, 4);
    }
    cmd_end();
}

/* ---- device creation and reset ---- */
static void dev_defaults(Dev9 *d)                      /* Direct3D 9's initial state, sent to the renderer */
{
    static const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    SB9 *rec = d->rec; d->rec = 0;
    struct { uint32_t s, v; } rs[] = {{7, d->pp[9] ? 1u : 0u}, {8, 3}, {9, 2}, {14, 1}, {15, 0}, {16, 1}, {19, 2}, {20, 1}, {22, 3}, {23, 4}, {24, 0}, {25, 8},
        {26, 0}, {27, 0}, {28, 0}, {29, 0}, {34, 0}, {35, 0}, {36, 0}, {37, 0x3f800000u}, {38, 0x3f800000u}, {48, 0}, {52, 0}, {53, 1}, {54, 1},
        {55, 1}, {56, 8}, {57, 0}, {58, 0xffffffffu}, {59, 0xffffffffu}, {60, 0xffffffffu}, {136, 1}, {137, 1}, {139, 0}, {140, 0}, {141, 1},
        {142, 1}, {143, 0}, {145, 1}, {146, 2}, {147, 0}, {148, 0}, {151, 0}, {152, 0}, {154, 0x3f800000u}, {168, 0xf}, {171, 1}, {174, 0}};
    for (unsigned k = 0; k < sizeof rs / sizeof *rs; k++) set_rs(d, rs[k].s, rs[k].v);
    for (uint32_t st = 0; st < 8; st++) {
        set_tss(d, st, 1, st ? 1 : 4); set_tss(d, st, 2, 2); set_tss(d, st, 3, 1);
        set_tss(d, st, 4, st ? 1 : 2); set_tss(d, st, 5, 2); set_tss(d, st, 6, 1);
        set_tss(d, st, 11, st); set_tss(d, st, 24, 0); set_tss(d, st, 26, 1); set_tss(d, st, 27, 1); set_tss(d, st, 28, 1);
    }
    for (uint32_t sm = 0; sm < 16; sm++) { set_ss(d, sm, 1, 1); set_ss(d, sm, 2, 1); set_ss(d, sm, 3, 1); set_ss(d, sm, 4, 0); set_ss(d, sm, 5, 1); set_ss(d, sm, 6, 1); set_ss(d, sm, 7, 0); set_ss(d, sm, 10, 1); }
    for (uint32_t st = 0; st < 16; st++) set_tex(d, st, 0);
    set_xf(d, 256, ident); set_xf(d, 2, ident); set_xf(d, 3, ident); for (uint32_t t = 16; t < 24; t++) set_xf(d, t, ident);
    uint32_t vp[6] = {0, 0, d->back->w, d->back->h, 0, u32f(1.0f)}; set_vp(d, vp);
    d->fvf = 0; d->decl = 0; d->vs = d->ps = 0;
    d->rec = rec;
}
static void dev_surfaces(Dev9 *d, uint32_t pp)          /* back buffer (and the automatic depth buffer) from the present parameters */
{
    memcpy(d->pp, GP(pp), 56);
    if (getenv("D3D9_RTLOG")) { fprintf(stderr, "rtlog: present parameters:"); for (int k = 0; k < 14; k++) fprintf(stderr, " %x", d->pp[k]); fprintf(stderr, "\n"); }
    if (!d->pp[0] || !d->pp[1]) { d->pp[0] = (uint32_t)w32_screen_w; d->pp[1] = (uint32_t)w32_screen_h; }
    if (!d->pp[2]) d->pp[2] = 22;
    if (!d->pp[3]) d->pp[3] = 1;
    memcpy(GP(pp), d->pp, 56);                          /* (the game reads back what was chosen) */
    dsr_d3d9_w = d->pp[0]; dsr_d3d9_h = d->pp[1];
    new_samples = ms_samples(d->pp[4]); d->back = surf_new(d, d->pp[0], d->pp[1], d->pp[2] == 21 ? 21 : 22, USAGE_RENDERTARGET, 0, 1, 0, 0, 0, 0, 0);
    if (d->pp[9]) new_samples = ms_samples(d->pp[4]);
    d->depth = d->pp[9] ? surf_new(d, d->pp[0], d->pp[1], d->pp[10] ? d->pp[10] : 75, USAGE_DEPTHSTENCIL, 0, 2, 0, 0, 0, 0, 0) : 0;
    d->rt = d->back; surf_addref(d->back); d->ds = d->depth; if (d->depth) surf_addref(d->depth);
    uint32_t *p = cmd_begin(DSR_MODE, 8); p[0] = d->pp[0]; p[1] = d->pp[1]; cmd_end();
    set_rt(d);
}
static uint32_t dev_reset(Dev9 *d, uint32_t pp)
{
    if (d->rt) surf_release(d->rt); if (d->ds) surf_release(d->ds);
    d->rt = d->ds = 0;
    if (d->back) surf_release(d->back); if (d->depth) surf_release(d->depth);
    d->back = d->depth = 0;
    dev_surfaces(d, pp); dev_defaults(d);
    return D3D_OK;
}
typedef struct D3D9 { uint32_t g; int ref; } D3D9;
M(d9_QueryInterface, 3) rt_w32(G_MEM, ARG(2), ARG(0)); RET(D3D_OK, 3); END
M(d9_AddRef, 1) ME(D3D9, O9_D3D) RET((uint32_t)++me->ref, 1); END
M(d9_Release, 1) ME(D3D9, O9_D3D) RET((uint32_t)(me->ref > 0 ? --me->ref : 0), 1); END
OK_M(d9_RegisterSoftwareDevice, 2)
M(d9_GetAdapterCount, 1) RET(1, 1); END
static uint32_t gpu_vendor(void) { const char *e = getenv("DS2_GPU"); return e ? (uint32_t)strtoul(e, 0, 16) : 0x1002; }
/* the card the game is told about: a Radeon X800 (shader model 2.0; the game's "advanced" effects on by default) while
 * effects run (d3dx9_fx.c), else a Radeon 8500 (fixed function); DS2_GPU=vendor:device overrides */
static uint32_t gpu_device(void) { const char *e = getenv("DS2_GPU"); const char *c2 = e ? strchr(e, ':') : 0; return c2 ? (uint32_t)strtoul(c2 + 1, 0, 16) : d9_shaders_on() ? 0x4a49 : 0x514c; }
M(d9_GetAdapterIdentifier, 4)                          /* D3DADAPTER_IDENTIFIER9: the reported card decides the game's defaults (system_detail.gas) */
    uint32_t o = ARG(3); memset(GP(o), 0, 1100);
    strcpy((char *)GP(o), "ati2dvag.dll"); strcpy((char *)GP(o + 512),
#ifdef __linux__
        "Native Linux (OpenGL)"
#else
        "Apple Silicon (Metal)"
#endif
    ); strcpy((char *)GP(o + 1024), "\\\\.\\DISPLAY1");
    rt_w32(G_MEM, o + 1056, 0x00020000u | 6614); rt_w32(G_MEM, o + 1060, 0x00060006u);   /* driver version 6.6.2.6614 */
    rt_w32(G_MEM, o + 1064, gpu_vendor()); rt_w32(G_MEM, o + 1068, gpu_device()); rt_w32(G_MEM, o + 1072, 0); rt_w32(G_MEM, o + 1076, 0);
    static const uint8_t guid[16] = {0x5c, 0x1e, 0x8d, 0xd7, 0x6a, 0x3b, 0x11, 0xcf, 0x86, 0x42, 0x54, 0x13, 0x8e, 0xc2, 0x3a, 0x0b};
    memcpy(GP(o + 1080), guid, 16); rt_w32(G_MEM, o + 1096, 1);
    RET(D3D_OK, 4);
END
static int modes(uint32_t (*out)[2])                    /* the standard sizes that fit this screen, then the screen's own */
{
#ifdef __linux__
    return ds_display_modes(out, (uint32_t)w32_screen_w, (uint32_t)w32_screen_h, getenv("DS_RESOLUTION"));
#else
    static const uint32_t std[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 800}, {1280, 960}, {1280, 1024}, {1366, 768},
                                      {1440, 900}, {1600, 900}, {1600, 1200}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}, {2560, 1600}};
    int n = 0; uint32_t sw = (uint32_t)w32_screen_w, sh = (uint32_t)w32_screen_h;
    for (unsigned k = 0; k < sizeof std / sizeof *std; k++) if (std[k][0] <= sw && std[k][1] <= sh && !(std[k][0] == sw && std[k][1] == sh)) { out[n][0] = std[k][0]; out[n][1] = std[k][1]; n++; }
    out[n][0] = sw; out[n][1] = sh; n++;
    return n;
#endif
}
M(d9_GetAdapterModeCount, 3) uint32_t m[DS_DISPLAY_MODE_CAP][2]; RET(ARG(2) == 22 || ARG(2) == 21 ? (uint32_t)modes(m) : 0, 3); END
M(d9_EnumAdapterModes, 5)                              /* (adapter, format, index, D3DDISPLAYMODE *) */
    uint32_t m[DS_DISPLAY_MODE_CAP][2]; int n = modes(m); if (ARG(3) >= (uint32_t)n) RET(D3DERR_INVALIDCALL, 5);
    uint32_t v[4] = {m[ARG(3)][0], m[ARG(3)][1], (uint32_t)w32_screen_hz, ARG(2)}; memcpy(GP(ARG(4)), v, 16); RET(D3D_OK, 5);
END
M(d9_GetAdapterDisplayMode, 3) uint32_t v[4] = {(uint32_t)w32_screen_w, (uint32_t)w32_screen_h, (uint32_t)w32_screen_hz, 22}; memcpy(GP(ARG(2)), v, 16); RET(D3D_OK, 3); END
M(d9_CheckDeviceType, 6) RET(ARG(3) == 22 || ARG(3) == 21 ? D3D_OK : D3DERR_NOTAVAILABLE, 6); END
M(d9_CheckDeviceFormat, 7)                             /* (adapter, type, adapter format, usage, resource type, format) */
    uint32_t usage = ARG(4), f = ARG(6);
    /* render targets: 8-bit RGB(A) and the floating-point formats the renderer draws into (asked for by FNV for its shadow
     * maps: refused, it fell back to A8R8G8B8 and stored depth in 8 bits, which flickered as lit specks on characters) */
    int ok = (usage & USAGE_DEPTHSTENCIL) ? d9_fmt_depth(f) : (usage & USAGE_RENDERTARGET) ? (f == 21 || f == 22 || d9_fmt_float(f)) : d9_fmt_known(f) && !d9_fmt_depth(f);
    if (getenv("D3D9_RTLOG")) fprintf(stderr, "rtlog: CheckDeviceFormat(usage %x, resource %u, format %u) -> %s\n", usage, ARG(5), f, ok ? "ok" : "no");
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
END
/* (adapter, type, format, windowed, multisample type, quality levels). Multisampling is offered to the game only with
 * D3D9_GAME_MSAA=1: its MSAA path then draws the scene into a multisampled target with no depth buffer bound (the depth
 * it pairs with it is never created here). Antialiasing is the renderer's instead (DSR_MSAA, DSR_FXAA). */
M(d9_CheckDeviceMultiSampleType, 7)
    uint32_t t = ARG(5); int ok = t == 0 || ((t == 2 || t == 4 || t == 8) && getenv("D3D9_GAME_MSAA"));
    if (getenv("D3D9_RTLOG")) fprintf(stderr, "rtlog: CheckDeviceMultiSampleType(devtype %u, format %u, windowed %u, type %u) -> %s\n", ARG(2), ARG(3), ARG(4), t, ok ? "ok" : "no");
    if (ARG(6)) rt_w32(G_MEM, ARG(6), ok ? 1 : 0); RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
END
M(d9_CheckDepthStencilMatch, 6) RET(d9_fmt_depth(ARG(5)) ? D3D_OK : D3DERR_NOTAVAILABLE, 6); END
M(d9_CheckDeviceFormatConversion, 5) RET(D3D_OK, 5); END
static void fill_caps(uint32_t out)                    /* D3DCAPS9: a Radeon 8500-class card (shader model 1.x), or X800-class with effects */
{
    uint32_t v[76] = {0};
    v[0] = 1; v[2] = 0x20000; v[3] = 0x20000000u | 0x40000000u | 0x20000; v[4] = 0x20 | 0x80; v[5] = 0x80000000u | 1;
    v[6] = 1; v[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x4000 | 0x10000 | 0x80000 | 0x100000;
    v[8] = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800;
    v[9] = 0x1 | 0x8 | 0x10 | 0x80 | 0x100 | 0x400 | 0x800 | 0x2000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | 0x4000000;
    v[10] = v[13] = 0xff; v[11] = v[12] = 0x1fff; v[14] = 0x8 | 0x200 | 0x4000 | 0x80000 | 0x200000;
    v[15] = 0x1 | 0x4 | 0x40 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 | 0x10000;
    v[16] = v[17] = v[18] = 0x200 | 0x100 | 0x400 | 0x10000 | 0x20000 | 0x1000000 | 0x2000000;
    v[19] = v[20] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20; v[21] = 0x2 | 0x4 | 0x8 | 0x10 | 0x20;
    v[22] = v[23] = 4096; v[24] = 512; v[25] = 8192; v[26] = 4096; v[27] = 16;
    memcpy(&v[28], &(float){1e10f}, 4); memcpy(&v[29], &(float){-1e8f}, 4); memcpy(&v[30], &(float){-1e8f}, 4); memcpy(&v[31], &(float){1e8f}, 4); memcpy(&v[32], &(float){1e8f}, 4);
    v[34] = 0x1ff; v[35] = 0x80000 | 8; v[36] = 0x3fffffff; v[37] = 8; v[38] = 8; v[39] = 0x1 | 0x2 | 0x8 | 0x10 | 0x20;
    v[40] = 8; v[41] = 6; v[42] = 4; v[43] = 0; memcpy(&v[44], &(float){256.0f}, 4); v[45] = 0xfffff; v[46] = 0xfffff; v[47] = 16; v[48] = 255;
    v[49] = 0xfffe0101u; v[50] = 256; v[51] = 0xffff0104u; memcpy(&v[52], &(float){8.0f}, 4);
    v[53] = 0x1 | 0x8 | 0x20 | 0x40; v[56] = 0; v[57] = 0; v[58] = 1; v[59] = 0x1 | 0x2; v[60] = 1; v[61] = 0x200 | 0x2000000;
    if (d9_shaders_on()) {                                 /* shader model 2.0, every declaration type */
        v[49] = 0xfffe0200u; v[51] = 0xffff0200u; v[59] = 0x3ff;
        v[62] = 0; v[63] = 0; v[64] = 12; v[65] = 1;           /* VS20Caps */
        v[66] = 0; v[67] = 0; v[68] = 12; v[69] = 0; v[70] = 96;  /* PS20Caps */
        v[72] = 65535; v[73] = 96;
    }
#ifdef FNV_EDITION
    /* Fallout NV draws with its precompiled vs/ps 1.1-3.0 shaders and picks a shader package from these: a shader model
     * 3.0 card (GeForce 7-class), one render target */
    v[49] = 0xfffe0300u; v[50] = 256; v[51] = 0xffff0300u; v[59] = 0x3ff; v[60] = 1;
    v[62] = 1; v[63] = 24; v[64] = 32; v[65] = 4;                  /* VS20Caps: predication */
    v[66] = 0x1f; v[67] = 24; v[68] = 32; v[69] = 4; v[70] = 512;  /* PS20Caps: every 2.x feature */
    v[71] = 0x200 | 0x2000000; v[72] = 65535; v[73] = 65535; v[74] = 32768; v[75] = 32768;
#endif
    memcpy(GP(out), v, 304);
}
M(d9_GetDeviceCaps, 4) fill_caps(ARG(3)); RET(D3D_OK, 4); END
M(d9_GetAdapterMonitor, 2) RET(0x0001beef, 2); END
static void init_vtables9(void);
M(d9_CreateDevice, 7) ME(D3D9, O9_D3D)                 /* (adapter, type, focus window, behaviour, present parameters, device *) */
    if (the_dev) { fprintf(stderr, "d3d9: a second device: the first one is reused\n"); dev_reset(the_dev, ARG(5)); rt_w32(G_MEM, ARG(6), the_dev->g); the_dev->ref++; RET(D3D_OK, 7); }
    Dev9 *d = calloc(1, sizeof *d); d->ref = 1; d->g_d3d = me->g; d->hwnd = ARG(3); d->behavior = ARG(4);
    d->g = make_iface(O9_DEV, d); d->g_swap = make_iface(O9_SWAP, d); the_dev = d;
    dev_surfaces(d, ARG(5)); dev_defaults(d);
    fprintf(stderr, "d3d9: device %ux%u (format %u, depth %u, %s)\n", d->pp[0], d->pp[1], d->pp[2], d->pp[9] ? d->pp[10] : 0, d->pp[8] ? "windowed" : "full screen");
    rt_w32(G_MEM, ARG(6), d->g); RET(D3D_OK, 7);
END

/* ---- vtables (Direct3D 9's method order) ---- */
typedef void (*Fn9)(Ctx *);
static uint32_t vtable9(const char *iface, const Fn9 *fns, int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "d3d9!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
static void init_vtables9(void)
{
    static int done; if (done) return; done = 1;
    static const Fn9 d3d[] = { d9_QueryInterface, d9_AddRef, d9_Release, d9_RegisterSoftwareDevice, d9_GetAdapterCount, d9_GetAdapterIdentifier,
        d9_GetAdapterModeCount, d9_EnumAdapterModes, d9_GetAdapterDisplayMode, d9_CheckDeviceType, d9_CheckDeviceFormat,
        d9_CheckDeviceMultiSampleType, d9_CheckDepthStencilMatch, d9_CheckDeviceFormatConversion, d9_GetDeviceCaps, d9_GetAdapterMonitor,
        d9_CreateDevice };
    static const Fn9 dev[] = { dv_QueryInterface, dv_AddRef, dv_Release, dv_TestCooperativeLevel, dv_GetAvailableTextureMem, dv_EvictManagedResources,
        dv_GetDirect3D, dv_GetDeviceCaps, dv_GetDisplayMode, dv_GetCreationParameters, dv_SetCursorProperties, dv_SetCursorPosition,
        dv_ShowCursor, dv_CreateAdditionalSwapChain, dv_GetSwapChain, dv_GetNumberOfSwapChains, dv_Reset, dv_Present, dv_GetBackBuffer,
        dv_GetRasterStatus, dv_SetDialogBoxMode, dv_SetGammaRamp, dv_GetGammaRamp, dv_CreateTexture, dv_CreateVolumeTexture,
        dv_CreateCubeTexture, dv_CreateVertexBuffer, dv_CreateIndexBuffer, dv_CreateRenderTarget, dv_CreateDepthStencilSurface,
        dv_UpdateSurface, dv_UpdateTexture, dv_GetRenderTargetData, dv_GetFrontBufferData, dv_StretchRect, dv_ColorFill,
        dv_CreateOffscreenPlainSurface, dv_SetRenderTarget, dv_GetRenderTarget, dv_SetDepthStencilSurface, dv_GetDepthStencilSurface,
        dv_BeginScene, dv_EndScene, dv_Clear, dv_SetTransform, dv_GetTransform, dv_MultiplyTransform, dv_SetViewport, dv_GetViewport,
        dv_SetMaterial, dv_GetMaterial, dv_SetLight, dv_GetLight, dv_LightEnable, dv_GetLightEnable, dv_SetClipPlane, dv_GetClipPlane,
        dv_SetRenderState, dv_GetRenderState, dv_CreateStateBlock, dv_BeginStateBlock, dv_EndStateBlock, dv_SetClipStatus,
        dv_GetClipStatus, dv_GetTexture, dv_SetTexture, dv_GetTextureStageState, dv_SetTextureStageState, dv_GetSamplerState,
        dv_SetSamplerState, dv_ValidateDevice, dv_SetPaletteEntries, dv_GetPaletteEntries, dv_SetCurrentTexturePalette,
        dv_GetCurrentTexturePalette, dv_SetScissorRect, dv_GetScissorRect, dv_SetSoftwareVertexProcessing, dv_GetSoftwareVertexProcessing,
        dv_SetNPatchMode, dv_GetNPatchMode, dv_DrawPrimitive, dv_DrawIndexedPrimitive, dv_DrawPrimitiveUP, dv_DrawIndexedPrimitiveUP,
        dv_ProcessVertices, dv_CreateVertexDeclaration, dv_SetVertexDeclaration, dv_GetVertexDeclaration, dv_SetFVF, dv_GetFVF,
        dv_CreateVertexShader, dv_SetVertexShader, dv_GetVertexShader, dv_SetVertexShaderConstantF, dv_GetVertexShaderConstantF,
        dv_SetVertexShaderConstantI, dv_GetVertexShaderConstantI, dv_SetVertexShaderConstantB, dv_GetVertexShaderConstantB,
        dv_SetStreamSource, dv_GetStreamSource, dv_SetStreamSourceFreq, dv_GetStreamSourceFreq, dv_SetIndices, dv_GetIndices,
        dv_CreatePixelShader, dv_SetPixelShader, dv_GetPixelShader, dv_SetPixelShaderConstantF, dv_GetPixelShaderConstantF,
        dv_SetPixelShaderConstantI, dv_GetPixelShaderConstantI, dv_SetPixelShaderConstantB, dv_GetPixelShaderConstantB,
        dv_DrawRectPatch, dv_DrawTriPatch, dv_DeletePatch, dv_CreateQuery };
    static const Fn9 surf[] = { sf_QueryInterface, sf_AddRef, sf_Release, sf_GetDevice, sf_SetPrivateData, sf_GetPrivateData, sf_FreePrivateData,
        sf_SetPriority, sf_GetPriority, sf_PreLoad, sf_GetType, sf_GetContainer, sf_GetDesc, sf_LockRect, sf_UnlockRect, sf_GetDC, sf_ReleaseDC };
#define TEXBASE tx_QueryInterface, tx_AddRef, tx_Release, tx_GetDevice, tx_SetPrivateData, tx_GetPrivateData, tx_FreePrivateData, tx_SetPriority, \
        tx_GetPriority, tx_PreLoad, tx_GetType, tx_SetLOD, tx_GetLOD, tx_GetLevelCount, tx_SetAutoGenFilterType, tx_GetAutoGenFilterType, tx_GenerateMipSubLevels
    static const Fn9 tex[] = { TEXBASE, tx_GetLevelDesc, tx_GetSurfaceLevel, tx_LockRect, tx_UnlockRect, tx_AddDirtyRect };
    static const Fn9 cube[] = { TEXBASE, tx_GetLevelDesc, cb_GetCubeMapSurface, cb_LockRect, cb_UnlockRect, cb_AddDirtyRect };
    static const Fn9 vol[] = { TEXBASE, vt_GetLevelDesc, vt_GetVolumeLevel, vt_LockBox, vt_UnlockBox, vt_AddDirtyBox };
    static const Fn9 buf[] = { bf_QueryInterface, bf_AddRef, bf_Release, bf_GetDevice, bf_SetPrivateData, bf_GetPrivateData, bf_FreePrivateData,
        bf_SetPriority, bf_GetPriority, bf_PreLoad, bf_GetType, bf_Lock, bf_Unlock, bf_GetDesc };
    static const Fn9 decl[] = { dc_QueryInterface, dc_AddRef, dc_Release, dc_GetDevice, dc_GetDeclaration };
    static const Fn9 shader[] = { sh_QueryInterface, sh_AddRef, sh_Release, sh_GetDevice, sh_GetFunction };
    static const Fn9 swap[] = { sw_QueryInterface, sw_AddRef, sw_Release, sw_Present, sw_GetFrontBufferData, sw_GetBackBuffer, sw_GetRasterStatus,
        sw_GetDisplayMode, sw_GetDevice, sw_GetPresentParameters };
    static const Fn9 query[] = { qy_QueryInterface, qy_AddRef, qy_Release, qy_GetDevice, qy_GetType, qy_GetDataSize, qy_Issue, qy_GetData };
    static const Fn9 sb[] = { sb_QueryInterface, sb_AddRef, sb_Release, sb_GetDevice, sb_Capture, sb_Apply };
    _Static_assert(sizeof d3d / sizeof *d3d == 17 && sizeof dev / sizeof *dev == 119 && sizeof surf / sizeof *surf == 17 && sizeof tex / sizeof *tex == 22 &&
                   sizeof buf / sizeof *buf == 14 && sizeof swap / sizeof *swap == 10 && sizeof query / sizeof *query == 8 && sizeof sb / sizeof *sb == 6, "d3d9 vtable sizes");
    vt[O9_D3D] = vtable9("IDirect3D9", d3d, 17); vt[O9_DEV] = vtable9("IDirect3DDevice9", dev, 119); vt[O9_SURF] = vtable9("IDirect3DSurface9", surf, 17);
    vt[O9_TEX] = vtable9("IDirect3DTexture9", tex, 22); vt[O9_CUBE] = vtable9("IDirect3DCubeTexture9", cube, 22); vt[O9_VOLTEX] = vtable9("IDirect3DVolumeTexture9", vol, 22);
    vt[O9_VB] = vtable9("IDirect3DVertexBuffer9", buf, 14); vt[O9_IB] = vtable9("IDirect3DIndexBuffer9", buf, 14);
    vt[O9_DECL] = vtable9("IDirect3DVertexDeclaration9", decl, 5); vt[O9_VS] = vtable9("IDirect3DVertexShader9", shader, 5); vt[O9_PS] = vtable9("IDirect3DPixelShader9", shader, 5);
    vt[O9_SWAP] = vtable9("IDirect3DSwapChain9", swap, 10); vt[O9_QUERY] = vtable9("IDirect3DQuery9", query, 8); vt[O9_SB] = vtable9("IDirect3DStateBlock9", sb, 6);
}

IMPL(d3d9, Direct3DCreate9)
{
    init_vtables9();
    static D3D9 *d; if (!d) { d = calloc(1, sizeof *d); d->g = make_iface(O9_D3D, d); }
    d->ref++; if (logon()) fprintf(stderr, "d3d9: Direct3DCreate9(%u)\n", ARG(0));
    RET(d->g, 1);
}

/* for d3dx9.c */
uint32_t d9_create_tex(Dev9 *d, int type, uint32_t w, uint32_t h, uint32_t dep, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out)
{ return create_tex(d, type, w, h, dep, levels, usage, fmt, pool, out); }
Tex9 *d9_any_tex(uint32_t g) { return any_tex(g); }
uint32_t d9_surf_lock(Surf9 *s, uint32_t locked, uint32_t rect, uint32_t flags) { return surf_lock(s, locked, rect, flags); }
uint32_t d9_surf_unlock(Surf9 *s) { return surf_unlock(s); }
void d9_surf_upload(Surf9 *s, uint32_t y0, uint32_t y1) { surf_upload(s, y0, y1); }
void d9_set_rs(Dev9 *d, uint32_t s, uint32_t v) { set_rs(d, s, v); }
void d9_set_tss(Dev9 *d, uint32_t stage, uint32_t t, uint32_t v) { set_tss(d, stage, t, v); }
void d9_set_ss(Dev9 *d, uint32_t sampler, uint32_t t, uint32_t v) { set_ss(d, sampler, t, v); }
void d9_tex_addref(uint32_t g) { Tex9 *t = g ? any_tex(g) : NULL; if (t) t->ref++; }
void d9_tex_release(uint32_t g) { Tex9 *t = g ? any_tex(g) : NULL; if (t) tex_release(t); }
void d9_set_texture(Dev9 *d, uint32_t stage, uint32_t tex_iface) { if (stage < 16) set_tex(d, stage, tex_iface ? any_tex(tex_iface) : NULL); }
uint32_t d9_texture_at(Dev9 *d, uint32_t stage) { return stage < 16 && d->tex[stage] ? d->tex[stage]->g : 0; }
