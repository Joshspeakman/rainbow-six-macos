/* DirectDraw 7 / Direct3D 7 for the recompiled game. It keeps the game's view of the device (capabilities and formats
 * replayed from src/dsr/dsr_snapshot.bin) and turns drawing into the command stream of src/dsr/dsr_proto.h, which the
 * Metal renderer (src/renderer) consumes on its own thread. COM interfaces are
 * small guest blocks {vtable, host object index} with vtables of thunks into the methods below; every structure is
 * read and written at its 32-bit Windows offsets in guest memory; lockable memory (surfaces, vertex buffers) lives in
 * guest memory because the game writes through the pointers Lock returns. */
#include "w32.h"
#include "dsr_producer.h"
#include <stdarg.h>
#include "platform.h"
#include <unistd.h>
#include "../../../src/dsr/dsr_proto.h"
#include "../../../src/dsr/dsr_shm.h"

/* ---- constants (ddraw.h / d3d.h) ---- */
enum { DDSD_CAPS = 1, DDSD_HEIGHT = 2, DDSD_WIDTH = 4, DDSD_PITCH = 8, DDSD_BACKBUFFERCOUNT = 0x20, DDSD_LPSURFACE = 0x800,
       DDSD_PIXELFORMAT = 0x1000, DDSD_MIPMAPCOUNT = 0x20000 };
enum { DDSCAPS_BACKBUFFER = 4, DDSCAPS_FRONTBUFFER = 0x20, DDSCAPS_PRIMARYSURFACE = 0x200, DDSCAPS_SYSTEMMEMORY = 0x800,
       DDSCAPS_TEXTURE = 0x1000, DDSCAPS_3DDEVICE = 0x2000, DDSCAPS_VIDEOMEMORY = 0x4000, DDSCAPS_VISIBLE = 0x8000,
       DDSCAPS_ZBUFFER = 0x20000, DDSCAPS_MIPMAP = 0x400000, DDSCAPS_LOCALVIDMEM = 0x10000000 };
enum { DDSCAPS2_MIPMAPSUBLEVEL = 0x10000, DDPF_ALPHAPIXELS = 1, DDPF_RGB = 0x40, DDPF_ZBUFFER = 0x400 };
enum { DDLOCK_READONLY = 0x10, DDLOCK_WRITEONLY = 0x20, DDBLT_COLORFILL = 0x400, DDBLT_DEPTHFILL = 0x2000000, DDSCL_EXCLUSIVE = 0x10 };
#define DD_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDH(n) (0x88760000u | (n))
/* DDSURFACEDESC2 (124 bytes) field offsets */
enum { SD_SIZE = 0, SD_FLAGS = 4, SD_HEIGHT = 8, SD_WIDTH = 12, SD_PITCH = 16, SD_BACKBUFFERS = 20, SD_MIPMAPS = 24,
       SD_SURFACE = 36, SD_PF = 72, SD_CAPS = 104, SD_CAPS2 = 108, SD_BYTES = 124 };
/* DDPIXELFORMAT offsets (within the 32-byte struct) */
enum { PF_SIZE = 0, PF_FLAGS = 4, PF_BITS = 12, PF_R = 16, PF_G = 20, PF_B = 24, PF_A = 28 };
#define U32(p, off) (*(uint32_t *)((uint8_t *)(p) + (off)))

/* ---- logging and snapshot ---- */
static int log_on = -1;
static void dsr_log(const char *fmt, ...)
{
    if (log_on < 0) log_on = getenv("DSR_LOG") != 0;
    if (!log_on) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "dsr: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
char dsr_snapshot_path[1024];
static uint8_t *snap; static uint32_t snap_size;
typedef struct { uint32_t tag, size; const uint8_t *data; } SnapRec;
static void snap_load(void)
{
    if (snap) return;
    const char *p = *dsr_snapshot_path ? dsr_snapshot_path : getenv("DSR_SNAPSHOT");
    FILE *f = p ? fopen(p, "rb") : 0;
    if (!f) { fprintf(stderr, "dsr: snapshot missing (%s)\n", p ? p : "set DSR_SNAPSHOT"); return; }
    fseek(f, 0, SEEK_END); snap_size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    snap = malloc(snap_size); if (fread(snap, 1, snap_size, f) != snap_size) snap_size = 0; fclose(f);
}
static uint32_t tag4(const char *t) { uint32_t v; memcpy(&v, t, 4); return v; }
static SnapRec cur_rec;
static const SnapRec *scan(uint32_t off, uint32_t tag)
{
    while (off + 8 <= snap_size) {
        uint32_t t, n; memcpy(&t, snap + off, 4); memcpy(&n, snap + off + 4, 4);
        if (t == tag) { cur_rec.tag = t; cur_rec.size = n; cur_rec.data = snap + off + 8; return &cur_rec; }
        off += 8 + n;
    }
    return 0;
}
static const SnapRec *snap_first(const char *tag) { snap_load(); return snap ? scan(0, tag4(tag)) : 0; }
static const SnapRec *snap_next(const SnapRec *r, const char *tag) { return scan((uint32_t)(r->data - snap) + r->size, tag4(tag)); }

/* ---- objects ---- */
typedef struct DSurface DSurface;
typedef struct DDraw { uint32_t g_dd, g_d3d; int ref; uint32_t hwnd, coop, mode_w, mode_h, mode_bpp; DSurface *primary; } DDraw;
struct DSurface {
    uint32_t g_surf, g_gamma; int ref; DDraw *dd;
    uint8_t desc[SD_BYTES];                     /* as the game sees it (lpSurface 0) */
    uint32_t mem; int32_t pitch;                /* guest memory: the CPU copy handed out by Lock */
    uint32_t id; DSurface *attached[4]; int nattached; DSurface *owner;
    int locked; uint32_t lock_flags; int32_t lock_rect[4]; int lock_has_rect;
    int is_rt, gpu_dirty, upload_over;
};
typedef struct DVB { uint32_t g; int ref; uint32_t desc[4]; uint32_t stride, mem; } DVB;
typedef struct DDevice {
    uint32_t g; int ref; DDraw *dd; DSurface *rt;
    uint32_t rs[256], tss[8][32]; float xf[32][16]; uint8_t vp[24]; DSurface *tex[8]; uint8_t material[68];
} DDevice;
enum { O_DD = 1, O_D3D, O_SURF, O_GAMMA, O_DEV, O_VB };
typedef struct { int type; void *p; } Obj;
static Obj objs[65536]; static uint32_t nobjs = 1;
static pthread_mutex_t obj_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t vt[64];                                /* guest vtable per interface type (d3d9.c uses types from 16) */
static uint32_t free_slot[65536], nfree_slot;   /* released slots, reused only once all have been handed out */
uint32_t make_iface(int type, void *p)
{
    pthread_mutex_lock(&obj_lock);
    /* new ids while there are any (stale pointers stay detectable), then the released ones: a long session must not
     * run out (every surface takes two) */
    uint32_t k = nobjs < 65536 ? nobjs++ : nfree_slot ? free_slot[--nfree_slot] : 0;
    objs[k].type = type; objs[k].p = p;
    pthread_mutex_unlock(&obj_lock);
    uint32_t g = heap_alloc(w32_process_heap, 8, 16);
    rt_w32(G_MEM, g, vt[type]); rt_w32(G_MEM, g + 4, k);
    return g;
}
void *obj(uint32_t g, int type)
{
    if (!g) return 0;
    uint32_t k = rt_r32(G_MEM, g + 4);
    return (k < 65536 && objs[k].type == type) ? objs[k].p : 0;
}
void obj_drop(uint32_t g)                        /* an interface's object is gone */
{
    uint32_t k = g ? rt_r32(G_MEM, g + 4) : 0;
    pthread_mutex_lock(&obj_lock);
    if (k && k < 65536 && objs[k].type) { objs[k].type = 0; objs[k].p = 0; free_slot[nfree_slot++] = k; }
    pthread_mutex_unlock(&obj_lock);
}
#define ME(T, ty) T *me = obj(ARG(0), ty); if (!me) { fprintf(stderr, "dsr: bad " #T " %08x\n", ARG(0)); RET(DDERR_INVALIDPARAMS, nargs_); }

static uint32_t obj_ids, free_sid[65536], nfree_sid;      /* the renderer's surface ids: new ones, then released ones */
static pthread_mutex_t sid_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t next_object_id(void)
{
    pthread_mutex_lock(&sid_lock); uint32_t id = obj_ids < 65535 ? ++obj_ids : nfree_sid ? free_sid[--nfree_sid] : 0; pthread_mutex_unlock(&sid_lock);
    return id;                                  /* 0 (all in use): the renderer ignores the surface */
}
void surface_id_free(uint32_t id)               /* after its DSR_SURFACE_DESTROY, so a reuse comes later in the stream */
{
    pthread_mutex_lock(&sid_lock); if (id && id < 65536 && nfree_sid < 65536) free_sid[nfree_sid++] = id; pthread_mutex_unlock(&sid_lock);
}
uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t s = 0, n;
    switch (fvf & 0xe) { case 2: s = 12; break; case 4: s = 16; break; case 6: s = 16; break; case 8: s = 20; break;
                         case 0xa: s = 24; break; case 0xc: s = 28; break; case 0xe: s = 32; break; }
    if (fvf & 0x10) s += 12;
    if (fvf & 0x40) s += 4;
    if (fvf & 0x80) s += 4;
    n = (fvf & 0xf00) >> 8;
    while (n--) { uint32_t f = (fvf >> (16 + 2 * n)) & 3; s += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4; }
    return s;
}

static uint32_t frames;
/* ---- command encoder: an in-process ring (dsr_shm.h layout); a consumer (the Metal renderer) attaches later ---- */
#define NULL_RING_SIZE (32u << 20)
static DsrShmHeader *shm; static uint8_t *shm_ring; static uint32_t wpos;
volatile uint32_t *dsr_query_results(void) { return shm ? (volatile uint32_t *)((uint8_t *)shm + DSR_QUERY_OFFSET) : 0; }   /* count[], serial[] */
static uint8_t *ring, *cur; static uint32_t ring_pos, cur_size;
static pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t dsr_readbacks, dsr_uploads;
void dsr_attach_consumer(DsrShmHeader *h) { shm = h; shm_ring = (uint8_t *)h + DSR_HEADER_SIZE; wpos = h->write_pos; }
static int ring_space(uint32_t n) { return DSR_RING_SIZE - (wpos - __atomic_load_n(&shm->read_pos, __ATOMIC_ACQUIRE)) >= n; }
static void wait_space(uint32_t n) { while (!ring_space(n)) usleep(100); }
static int cmd_drop; static uint8_t *drop_buf; static size_t drop_cap;
uint32_t *cmd_begin(uint32_t op, uint32_t payload)
{
    pthread_mutex_lock(&cmd_lock);
    if (payload > DSR_RING_SIZE / 2) {          /* larger than the ring takes: written aside and dropped (a wait would never end) */
        static int warned; if (!warned++) fprintf(stderr, "dsr: a %u-byte record (op %u) is too large for the command ring; dropped\n", payload, op);
        if (drop_cap < 8 + (size_t)payload) { free(drop_buf); drop_cap = 8 + (size_t)payload; drop_buf = malloc(drop_cap); }
        if (!drop_buf) abort();
        cmd_drop = 1; cur = drop_buf; uint32_t *h = (uint32_t *)cur; h[0] = op; h[1] = payload; return h + 2;
    }
    payload = (payload + 3) & ~3u; cur_size = 8 + payload;
    if (shm) {
        uint32_t off = wpos % DSR_RING_SIZE;
        if (off + cur_size > DSR_RING_SIZE) {
            uint32_t skip = DSR_RING_SIZE - off; wait_space(skip + cur_size);
            if (skip >= 8) { uint32_t *w = (uint32_t *)(shm_ring + off); w[0] = DSR_WRAP; w[1] = skip - 8; }
            wpos += skip; off = 0;
        } else wait_space(cur_size);
        cur = shm_ring + off;
    } else {
        if (!ring) ring = malloc(NULL_RING_SIZE);
        if (ring_pos + cur_size > NULL_RING_SIZE) ring_pos = 0;
        cur = ring + ring_pos;
    }
    uint32_t *h = (uint32_t *)cur; h[0] = op; h[1] = payload;
    return h + 2;
}
static uint32_t st_ops[32];                         /* DSR_STATS=1: per-second counts of each record type */
void cmd_end(void)
{
    if (cmd_drop) { cmd_drop = 0; pthread_mutex_unlock(&cmd_lock); return; }
    uint32_t op = *(uint32_t *)cur;
    if (op < 32) st_ops[op]++;
    if (op == DSR_SURFACE_UPLOAD || op == DSR_UPLOAD_OVER) dsr_uploads++;
    if (shm) { wpos += cur_size; __atomic_store_n(&shm->write_pos, wpos, __ATOMIC_RELEASE);
#ifdef __linux__
        if (__atomic_load_n(&shm->consumer_waiting, __ATOMIC_SEQ_CST)) dsr_wake(&shm->write_pos);
#endif
    }
    else ring_pos += cur_size;
    pthread_mutex_unlock(&cmd_lock);
}
static uint64_t now_us(void) { return ds_clock_ns() / 1000; }
/* Frame cadence (DSR_FPSCAP, default auto: 120 or 60 from the game's own work per frame). */
static double cap_period_ms = -1; static int cap_auto;
static void cmd_cap_wait(void)
{
    static uint64_t deadline, released; static double work[120]; static int nw, upgrade_since; static uint64_t last_down;
    if (w32_deterministic) return;
    if (cap_period_ms < 0) {
        const char *v = getenv("DSR_FPSCAP"); if (!v) v = "auto";
        if (!strncmp(v, "auto", 4)) { cap_auto = v[4] ? 2 : 1; cap_period_ms = 1000.0 / 60; } else cap_period_ms = atof(v) > 0 ? 1000.0 / atof(v) : 0;
    }
    uint64_t t = now_us();
    if (cap_period_ms <= 0) { released = t; return; }
    if (cap_auto && released) {
        double w = (t - released) / 1000.0, s[120], p90; int n = nw < 120 ? nw : 120;
        work[nw++ % 120] = w;
        if (n >= 60) {
            memcpy(s, work, (size_t)n * sizeof *s);
            for (int k = 1; k < n; k++) { double x = s[k]; int j = k - 1; while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; } s[j + 1] = x; }
            p90 = s[n * 9 / 10];
            { static int lg = -1; static uint64_t lt; if (lg < 0) lg = getenv("DSR_CAPLOG") != 0;
              if (lg && t - lt > 1000000) { lt = t; fprintf(stderr, "dsr: game work p50 %.2f p90 %.2f max %.2f ms (cap %.0f)\n", s[n / 2], p90, s[n - 1], 1000 / cap_period_ms); } }
            /* up when 90% of frames leave 5% of the faster period spare (down only once they overrun it); the game's
             * work with shader effects on sits about 10% under 120 Hz's 8.3 ms, which a 10% margin never let up */
            double up = p90 < 8.333 * 0.95 ? 1000.0 / 120 : (cap_auto == 2 && p90 < 12.5 * 0.95) ? 1000.0 / 80 : 1000.0 / 60;
            double down = p90 > 12.5 ? 1000.0 / 60 : p90 > 8.333 ? (cap_auto == 2 ? 1000.0 / 80 : 1000.0 / 60) : 1000.0 / 120;
            if (down > cap_period_ms + 0.1) { cap_period_ms = down; upgrade_since = 0; last_down = t; nw = 0; dsr_log("fps cap auto -> %.0f", 1000 / down); }
            else if (up < cap_period_ms - 0.1 && t - last_down > 30000000) {
                if (++upgrade_since > 10 * (int)(1000 / cap_period_ms)) { cap_period_ms = up; upgrade_since = 0; nw = 0; dsr_log("fps cap auto -> %.0f", 1000 / up); }
            } else upgrade_since = 0;
        }
    }
    uint64_t period = (uint64_t)(cap_period_ms * 1000);
    if (!deadline) deadline = t;
    if (t < deadline) { while ((t = now_us()) < deadline) { if (deadline - t > 2000) usleep(1000); } }
    else if (t - deadline > period) deadline = t;
    deadline += period;
    released = now_us();
}
static void stats_tick(void)
{
    static int on = -1; static uint64_t t0; if (on < 0) on = getenv("DSR_STATS") != 0;
    if (!on) return;
    uint64_t t = now_us(); if (!t0) t0 = t;
    if (t - t0 < 1000000) return;
    fprintf(stderr, "dsr: per s: present %u draw %u blt %u upload %u upload_over %u readback %u create %u destroy %u\n", st_ops[DSR_PRESENT], st_ops[DSR_DRAW],
            st_ops[DSR_BLT], st_ops[DSR_SURFACE_UPLOAD], st_ops[DSR_UPLOAD_OVER], st_ops[DSR_READBACK], st_ops[DSR_SURFACE_CREATE], st_ops[DSR_SURFACE_DESTROY]);
    memset(st_ops, 0, sizeof st_ops); t0 = t;
}
static void cmd_frame(void)
{
    stats_tick();
    if (!shm) return;
    frames++;
    __atomic_store_n(&shm->frames_submitted, frames, __ATOMIC_RELEASE); shm->producer_alive++;
    uint64_t t0 = now_us();
#ifdef __linux__
    /* The host owns pacing. Start the next input/simulation update only once
     * this frame is presented, instead of letting two old frames queue up. */
    for (;;) {
        uint32_t done=__atomic_load_n(&shm->frames_done,__ATOMIC_ACQUIRE);
        if(done==frames || now_us()-t0>=250000) break;
        dsr_wait(&shm->frames_done,done,1000000);
    }
#else
    while (frames - __atomic_load_n(&shm->frames_done, __ATOMIC_ACQUIRE) > 2 && now_us() - t0 < 250000) usleep(50);
#endif
}
static uint32_t rb_cookie; static pthread_mutex_t rb_lock = PTHREAD_MUTEX_INITIALIZER;
int cmd_readback(uint32_t id, const int32_t *r, uint8_t *dst, uint32_t pitch, uint32_t bpp)
{
    uint32_t w = (uint32_t)(r[2] - r[0]), h = (uint32_t)(r[3] - r[1]), row = w * bpp; int ok = 0;
    if (!shm || !w || !h || row * h > DSR_READBACK_SIZE) return 0;
    pthread_mutex_lock(&rb_lock);
    rb_cookie = (rb_cookie + 1) & 0x7fffffffu;   /* the renderer sets the high bit when it could not do it */
    uint32_t *p = cmd_begin(DSR_READBACK, 24); p[0] = id; p[1] = (uint32_t)r[0]; p[2] = (uint32_t)r[1]; p[3] = w; p[4] = h; p[5] = rb_cookie; cmd_end();
    for (uint64_t t0 = now_us(); now_us() - t0 < 3000000; ) {
        uint32_t d = __atomic_load_n(&shm->readback_done, __ATOMIC_ACQUIRE);
        if (d == rb_cookie) { ok = 1; break; }
        if (d == (rb_cookie | 0x80000000u)) break;
        usleep(50);
    }
    dsr_readbacks++;
    if (ok) { const uint8_t *src = (const uint8_t *)shm + DSR_READBACK_OFFSET; for (uint32_t y = 0; y < h; y++) memcpy(dst + y * pitch, src + y * row, row); }
    pthread_mutex_unlock(&rb_lock);
    return ok;
}

/* ---- guest scratch for callback arguments ---- */
static uint32_t scratch(uint32_t n) { return heap_alloc(w32_process_heap, 8, n); }
static void scratch_free(uint32_t a) { heap_free(w32_process_heap, a); }

/* ================= surfaces ================= */
static uint32_t bpp_of(const uint8_t *d) { return U32(d, SD_PF + PF_BITS); }      /* dwZBufferBitDepth shares the offset */
static uint32_t fmt_code(const uint8_t *d)
{
    uint32_t fl = U32(d, SD_PF + PF_FLAGS);
    if (fl & DDPF_ZBUFFER) return 3;
    if (U32(d, SD_PF + PF_BITS) == 32) return (fl & DDPF_ALPHAPIXELS) ? 1 : 2;
    dsr_log("UNSUPPORTED pixel format: flags %08x bits %u", fl, U32(d, SD_PF + PF_BITS)); return 0;
}
static void default_format(uint8_t *pf) { memset(pf, 0, 32); U32(pf, PF_SIZE) = 32; U32(pf, PF_FLAGS) = DDPF_RGB; U32(pf, PF_BITS) = 32; U32(pf, PF_R) = 0xff0000; U32(pf, PF_G) = 0xff00; U32(pf, PF_B) = 0xff; }
static DSurface *new_surface(DDraw *dd, const uint8_t *sd, uint32_t w, uint32_t h, uint32_t caps, uint32_t caps2, uint32_t kind, DSurface *parent, uint32_t level, uint32_t levels)
{
    DSurface *s = calloc(1, sizeof *s);
    s->g_surf = make_iface(O_SURF, s); s->g_gamma = make_iface(O_GAMMA, s);
    s->ref = 1; s->dd = dd; s->id = next_object_id();
    memcpy(s->desc, sd, SD_BYTES); U32(s->desc, SD_SIZE) = SD_BYTES;
    U32(s->desc, SD_FLAGS) |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_PITCH;
    U32(s->desc, SD_WIDTH) = w; U32(s->desc, SD_HEIGHT) = h; U32(s->desc, SD_CAPS) = caps; U32(s->desc, SD_CAPS2) = caps2;
    if (!(U32(sd, SD_FLAGS) & DDSD_PIXELFORMAT) && !(caps & DDSCAPS_ZBUFFER)) default_format(s->desc + SD_PF);
    if ((caps & DDSCAPS_ZBUFFER) && !(U32(sd, SD_FLAGS) & DDSD_PIXELFORMAT)) {
        uint8_t *pf = s->desc + SD_PF; memset(pf, 0, 32); U32(pf, PF_SIZE) = 32; U32(pf, PF_FLAGS) = DDPF_ZBUFFER; U32(pf, PF_BITS) = 32; U32(pf, PF_G) = 0xffffffffu;
    }
    U32(s->desc, SD_PF + PF_SIZE) = 32;
    s->pitch = (int32_t)(((w * (bpp_of(s->desc) / 8)) + 3) & ~3u); U32(s->desc, SD_PITCH) = (uint32_t)s->pitch;
    U32(s->desc, SD_SURFACE) = 0;
    uint32_t bytes = (uint32_t)s->pitch * h + 16;
    s->mem = bytes >= 0x10000 ? vm_alloc(0, bytes, 0x3000, 4) : heap_alloc(w32_process_heap, 8, bytes);
    s->is_rt = (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE)) != 0;
    uint32_t *p = cmd_begin(DSR_SURFACE_CREATE, 32);
    p[0] = s->id; p[1] = w; p[2] = h; p[3] = kind; p[4] = fmt_code(s->desc); p[5] = parent ? parent->id : 0; p[6] = level; p[7] = levels;
    cmd_end();
    return s;
}
static uint32_t surface_create(DDraw *dd, const uint8_t *sd, DSurface **out)
{
    uint32_t caps = U32(sd, SD_CAPS), caps2 = U32(sd, SD_CAPS2), w = U32(sd, SD_WIDTH), h = U32(sd, SD_HEIGHT), fl = U32(sd, SD_FLAGS);
    DSurface *s;
    if (!(caps & DDSCAPS_PRIMARYSURFACE) && (w > 16384 || h > 16384)) return DDERR_INVALIDPARAMS;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        uint32_t nback = (fl & DDSD_BACKBUFFERCOUNT) ? U32(sd, SD_BACKBUFFERS) : 0;
        w = dd->mode_w; h = dd->mode_h;
        s = new_surface(dd, sd, w, h, caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE, caps2, 1, 0, 0, 1);
        DSurface *prev = s;
        for (uint32_t k = 0; k < nback; k++) {
            uint8_t bd[SD_BYTES]; memcpy(bd, sd, SD_BYTES); U32(bd, SD_FLAGS) &= ~(uint32_t)DDSD_BACKBUFFERCOUNT;
            DSurface *b = new_surface(dd, bd, w, h, (caps & ~(uint32_t)(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) | DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, caps2, 1, 0, 0, 1);
            b->owner = s; prev->attached[prev->nattached++] = b; prev = b;
        }
        dd->primary = s;
        dsr_log("primary %ux%u with %u back buffer(s): ids %u..", w, h, nback, s->id);
    } else if (caps & DDSCAPS_ZBUFFER) {
        s = new_surface(dd, sd, w, h, caps | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, caps2, 2, 0, 0, 1);
    } else if (caps & DDSCAPS_TEXTURE) {
        uint32_t levels = 1;
        if (caps & DDSCAPS_MIPMAP) {
            if (fl & DDSD_MIPMAPCOUNT) levels = U32(sd, SD_MIPMAPS);
            else { uint32_t m = w > h ? w : h; levels = 1; while (m > 1) { m >>= 1; levels++; } }
        }
        s = new_surface(dd, sd, w, h, caps, caps2, 3, 0, 0, levels);
        U32(s->desc, SD_FLAGS) |= (caps & DDSCAPS_MIPMAP) ? DDSD_MIPMAPCOUNT : 0; U32(s->desc, SD_MIPMAPS) = levels;
        DSurface *prev = s;
        for (uint32_t l = 1; l < levels; l++) {
            uint8_t md[SD_BYTES]; memcpy(md, sd, SD_BYTES); U32(md, SD_FLAGS) &= ~(uint32_t)DDSD_MIPMAPCOUNT;
            DSurface *m = new_surface(dd, md, (w >> l) ? w >> l : 1, (h >> l) ? h >> l : 1, caps, caps2 | DDSCAPS2_MIPMAPSUBLEVEL, 3, s, l, levels);
            U32(m->desc, SD_MIPMAPS) = levels - l; m->owner = s;
            prev->attached[prev->nattached++] = m; prev = m;
        }
    } else {
        if (!(fl & DDSD_WIDTH)) { w = dd->mode_w; h = dd->mode_h; }
        s = new_surface(dd, sd, w, h, caps | ((caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY), caps2, 4, 0, 0, 1);
    }
    *out = s; return DD_OK;
}
static void surf_release(DSurface *s);
static void surface_destroy(DSurface *s)
{
    for (int k = 0; k < s->nattached; k++)
        if (s->attached[k]->owner == s || s->attached[k]->owner == (s->owner ? s->owner : s)) surf_release(s->attached[k]);
    if (s->dd && s->dd->primary == s) s->dd->primary = 0;
    uint32_t *p = cmd_begin(DSR_SURFACE_DESTROY, 4); p[0] = s->id; cmd_end();
    uint32_t bytes = (uint32_t)s->pitch * U32(s->desc, SD_HEIGHT) + 16;
    if (bytes >= 0x10000) vm_free(s->mem, 0, 0x8000); else heap_free(w32_process_heap, s->mem);
    obj_drop(s->g_surf); obj_drop(s->g_gamma); surface_id_free(s->id);
    free(s);
}
static uint32_t surf_addref(DSurface *s) { return (uint32_t)__atomic_add_fetch(&s->ref, 1, __ATOMIC_SEQ_CST); }
static void surf_release(DSurface *s) { if (__atomic_sub_fetch(&s->ref, 1, __ATOMIC_SEQ_CST) == 0) surface_destroy(s); }

#define M(name, n) static void name(Ctx *c) { enum { nargs_ = n };
#define END }
/* IUnknown */
M(s_QueryInterface, 3) ME(DSurface, O_SURF)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (iid == 0x00000000u || iid == 0x06675a80u) { rt_w32(G_MEM, out, me->g_surf); surf_addref(me); RET(DD_OK, 3); }
    if (iid == 0x69c11c3eu) { rt_w32(G_MEM, out, me->g_gamma); surf_addref(me); RET(DD_OK, 3); }
    rt_w32(G_MEM, out, 0); dsr_log("surface QI: no interface %08x", iid); RET(E_NOINTERFACE, 3);
END
M(s_AddRef, 1) ME(DSurface, O_SURF) RET(surf_addref(me), 1); END
M(s_Release, 1) ME(DSurface, O_SURF) uint32_t r = (uint32_t)__atomic_sub_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); if (!r) surface_destroy(me); RET(r, 1); END
M(s_AddAttachedSurface, 2) ME(DSurface, O_SURF)
    DSurface *t = obj(ARG(1), O_SURF); if (!t) RET(DDERR_INVALIDPARAMS, 2);
    if (me->nattached >= 4) RET(DDH(110), 2);
    me->attached[me->nattached++] = t; surf_addref(t); t->owner = me; RET(DD_OK, 2);
END
M(s_DeleteAttachedSurface, 3) ME(DSurface, O_SURF)
    uint32_t a = ARG(2);
    for (int k = 0; k < me->nattached; k++) if (!a || me->attached[k]->g_surf == a) {
        DSurface *t = me->attached[k]; memmove(&me->attached[k], &me->attached[k + 1], (size_t)(me->nattached - k - 1) * sizeof t); me->nattached--;
        surf_release(t); RET(DD_OK, 3);
    }
    RET(DDH(610), 3);
END
M(s_GetAttachedSurface, 3) ME(DSurface, O_SURF)
    uint32_t caps = ARG(1), out = ARG(2);
    uint32_t want = rt_r32(G_MEM, caps) & ~(uint32_t)(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM), want2 = rt_r32(G_MEM, caps + 4);
    for (int k = 0; k < me->nattached; k++) {
        DSurface *t = me->attached[k];
        if ((U32(t->desc, SD_CAPS) & want) == want && (U32(t->desc, SD_CAPS2) & want2) == want2) { rt_w32(G_MEM, out, t->g_surf); surf_addref(t); RET(DD_OK, 3); }
    }
    rt_w32(G_MEM, out, 0); RET(DDH(255), 3);
END
M(s_EnumAttachedSurfaces, 3) ME(DSurface, O_SURF)
    uint32_t ctx = ARG(1), cb = ARG(2), d = scratch(SD_BYTES);
    for (int k = 0; k < me->nattached; k++) {
        memcpy(GP(d), me->attached[k]->desc, SD_BYTES); surf_addref(me->attached[k]);
        uint32_t a[3] = {me->attached[k]->g_surf, d, ctx};
        if (w32_callback(c, cb, 3, a) == 0) break;                  /* DDENUMRET_CANCEL */
    }
    scratch_free(d); RET(DD_OK, 3);
END
/* blits */
static void present(Ctx *c, DSurface *s);
static void rect_or_full(const DSurface *s, uint32_t r, int32_t *o)
{ if (r) memcpy(o, GP(r), 16); else { o[0] = 0; o[1] = 0; o[2] = (int32_t)U32(s->desc, SD_WIDTH); o[3] = (int32_t)U32(s->desc, SD_HEIGHT); } }
static void emit_blt(DSurface *d, const int32_t *dr, DSurface *src, const int32_t *sr, uint32_t flags, uint32_t fill)
{
    { static int n; if (getenv("DSR_STATS") && frames > 3000 && (d->id == 4 || (src && src->id == 4) || U32(d->desc, SD_WIDTH) == 128) && n++ < 12) fprintf(stderr, "dsr: blt -> %u %ux%u caps %08x from %u %ux%u flags %08x rect %d,%d-%d,%d\n", d->id, U32(d->desc, SD_WIDTH), U32(d->desc, SD_HEIGHT), U32(d->desc, SD_CAPS),
        src ? src->id : 0, src ? U32(src->desc, SD_WIDTH) : 0, src ? U32(src->desc, SD_HEIGHT) : 0, flags, dr[0], dr[1], dr[2], dr[3]); }
    d->gpu_dirty = 1;
    uint32_t *p = cmd_begin(DSR_BLT, 48);
    p[0] = d->id; memcpy(p + 1, dr, 16); p[5] = src ? src->id : 0;
    if (src) memcpy(p + 6, sr, 16); else memset(p + 6, 0, 16);
    p[10] = flags; p[11] = fill; cmd_end();
}
M(s_Blt, 6) ME(DSurface, O_SURF)
    uint32_t dr = ARG(1), flags = ARG(4), fx = ARG(5); DSurface *src = obj(ARG(2), O_SURF); int32_t a[4], b[4];
    rect_or_full(me, dr, a);
    if (flags & DDBLT_COLORFILL) emit_blt(me, a, 0, 0, flags, fx ? rt_r32(G_MEM, fx + 80) : 0);
    else if (flags & DDBLT_DEPTHFILL) emit_blt(me, a, 0, 0, flags, fx ? rt_r32(G_MEM, fx + 80) : 0);
    else if (src) {
        rect_or_full(src, ARG(3), b); emit_blt(me, a, src, b, flags, 0);
        /* a copy into the primary surface is on screen at once (the movies show their frames this way, not by flipping):
           the whole picture copied over is the picture presented */
        if ((U32(me->desc, SD_CAPS) & DDSCAPS_PRIMARYSURFACE) && a[0] == 0 && a[1] == 0 && a[2] == (int32_t)U32(me->desc, SD_WIDTH) && a[3] == (int32_t)U32(me->desc, SD_HEIGHT) &&
            b[0] == 0 && b[1] == 0 && b[2] == (int32_t)U32(src->desc, SD_WIDTH) && b[3] == (int32_t)U32(src->desc, SD_HEIGHT)) present(c, src);
    }
    RET(DD_OK, 6);
END
M(s_BltFast, 6) ME(DSurface, O_SURF)
    DSurface *src = obj(ARG(3), O_SURF); int32_t r[4], dr[4]; if (!src) RET(DDERR_INVALIDPARAMS, 6);
    rect_or_full(src, ARG(4), r);
    dr[0] = (int32_t)ARG(1); dr[1] = (int32_t)ARG(2); dr[2] = dr[0] + (r[2] - r[0]); dr[3] = dr[1] + (r[3] - r[1]);
    emit_blt(me, dr, src, r, ARG(5) | 0x80000000u, 0);
    RET(DD_OK, 6);
END
/* flip / present */
/* test mode only (DS_TEST=1): DS_CRASHTEST=fault|hang|stall|abort|hostcrash@<seconds> (uihang: host) exercises the crash reports */
static void crash_test(Ctx *c)
{
    static int armed = -1; static double at; static char kind[16]; static uint64_t t0;
    if (armed < 0) {
        const char *e = getenv("DS_CRASHTEST"), *t = getenv("DS_TEST"); armed = 0;
        if (e && t && atoi(t) && sscanf(e, "%15[a-z]@%lf", kind, &at) == 2) { armed = 1; t0 = now_us(); }
    }
    if (!armed || (now_us() - t0) / 1e6 < at) return;
    armed = 0;
    if (!strcmp(kind, "fault")) rt_unhandled(c, rt_r32(G_MEM, c->esp), "crash test: simulated fault in game code");
    if (!strcmp(kind, "hang")) { fprintf(stderr, "crash test: game main thread stops\n"); for (;;) sleep(60); }
    if (!strcmp(kind, "stall")) { fprintf(stderr, "crash test: game main thread pauses 10 s\n"); uint64_t e = now_us() + 10000000; while (now_us() < e) usleep(100000); }
    if (!strcmp(kind, "abort")) abort();
    if (!strcmp(kind, "hostcrash")) { volatile int *p = (int *)(uintptr_t)8; *p = 1; }
}
void dsr_present(Ctx *c, uint32_t id)           /* a finished frame: pacing, then the present record (also for d3d9.c) */
{
    { void w32_crash_heartbeat(int); w32_crash_heartbeat(0); crash_test(c); }
    { static int on = -1; if (on < 0) on = getenv("W32_SMCLOG") != 0; if (on) { void w32_smc_check(void); w32_smc_check(); } }
    #ifndef __linux__
    cmd_cap_wait();
    #endif
    uint64_t q = w32_deterministic ? 0 : now_us();
    uint32_t *p = cmd_begin(DSR_PRESENT, 16); p[0] = id; p[1] = (uint32_t)q; p[2] = (uint32_t)(q >> 32); p[3] = 1000000; cmd_end();
    cmd_frame();
}
static void present(Ctx *c, DSurface *s) { dsr_present(c, s->id); }
M(s_Flip, 3) ME(DSurface, O_SURF)
    DSurface *t = obj(ARG(1), O_SURF), *back = t ? t : (me->nattached ? me->attached[0] : me);
    present(c, back);
    RET(DD_OK, 3);
END
/* lock */
M(s_Lock, 5) ME(DSurface, O_SURF)
    uint32_t r = ARG(1), sd = ARG(2), flags = ARG(3), bpp = bpp_of(me->desc) / 8; int32_t rr[4];
    if (r) {
        memcpy(rr, GP(r), 16);
        if (rr[0] < 0 || rr[1] < 0 || rr[2] <= rr[0] || rr[3] <= rr[1] || (uint32_t)rr[2] > U32(me->desc, SD_WIDTH) || (uint32_t)rr[3] > U32(me->desc, SD_HEIGHT))
            RET(DDH(150), 5);                         /* DDERR_INVALIDRECT */
    }
    if (sd) {
        uint32_t sz = rt_r32(G_MEM, sd);
        memcpy(GP(sd), me->desc, SD_BYTES); rt_w32(G_MEM, sd, sz ? sz : SD_BYTES);
        rt_w32(G_MEM, sd + SD_FLAGS, U32(me->desc, SD_FLAGS) | DDSD_LPSURFACE | DDSD_PITCH); rt_w32(G_MEM, sd + SD_PITCH, (uint32_t)me->pitch);
        rt_w32(G_MEM, sd + SD_SURFACE, me->mem + (r ? (uint32_t)rr[1] * (uint32_t)me->pitch + (uint32_t)rr[0] * bpp : 0));
    }
    me->locked++; me->lock_flags = flags; me->lock_has_rect = r != 0; if (r) memcpy(me->lock_rect, rr, 16);
    { static int n; if (getenv("DSR_STATS") && frames > 3000 && n++ < 12) fprintf(stderr, "dsr: lock surface %u %ux%u caps %08x flags %08x gpu_dirty %d\n", me->id, U32(me->desc, SD_WIDTH), U32(me->desc, SD_HEIGHT), U32(me->desc, SD_CAPS), flags, me->gpu_dirty); }
    me->upload_over = 0;
    uint32_t caps = U32(me->desc, SD_CAPS);
    if ((me->is_rt || (me->gpu_dirty && (caps & DDSCAPS_TEXTURE))) && !(flags & DDLOCK_WRITEONLY) && !(caps & DDSCAPS_ZBUFFER)) {
        DSurface *src = (caps & DDSCAPS_PRIMARYSURFACE) && me->nattached ? me->attached[0] : me;
        int32_t a[4]; if (r) memcpy(a, rr, 16); else { a[0] = 0; a[1] = 0; a[2] = (int32_t)U32(me->desc, SD_WIDTH); a[3] = (int32_t)U32(me->desc, SD_HEIGHT); }
        cmd_readback(src->id, a, G_MEM + me->mem + (uint32_t)a[1] * (uint32_t)me->pitch + (uint32_t)a[0] * bpp, (uint32_t)me->pitch, bpp);
    }
    if (me->gpu_dirty && !(caps & (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER | DDSCAPS_ZBUFFER))) {
        uint32_t x0, x1, y0, y1;
        if (r) { x0 = (uint32_t)rr[0]; x1 = (uint32_t)rr[2]; y0 = (uint32_t)rr[1]; y1 = (uint32_t)rr[3]; } else { x0 = 0; x1 = U32(me->desc, SD_WIDTH); y0 = 0; y1 = U32(me->desc, SD_HEIGHT); }
        for (uint32_t y = y0; y < y1; y++) memset(G_MEM + me->mem + y * (uint32_t)me->pitch + x0 * 4, 0, (x1 - x0) * 4);
        me->upload_over = 1;
    }
    RET(DD_OK, 5);
END
M(s_Unlock, 2) ME(DSurface, O_SURF)
    if (!me->locked) RET(DDH(584), 2);
    me->locked--;
    if (!(me->lock_flags & DDLOCK_READONLY) && !(U32(me->desc, SD_CAPS) & DDSCAPS_ZBUFFER)) {
        int32_t u[4]; uint32_t bpp = bpp_of(me->desc) / 8;
        if (me->lock_has_rect) memcpy(u, me->lock_rect, 16); else { u[0] = 0; u[1] = 0; u[2] = (int32_t)U32(me->desc, SD_WIDTH); u[3] = (int32_t)U32(me->desc, SD_HEIGHT); }
        uint32_t rowb = (uint32_t)(u[2] - u[0]) * bpp, band = rowb ? (8u << 20) / rowb : 1; if (!band) band = 1;
        for (int32_t y0 = u[1]; y0 < u[3]; y0 += (int32_t)band) {     /* in bands of at most 8 MB: a big texture fits the ring */
            int32_t y1 = y0 + (int32_t)band < u[3] ? y0 + (int32_t)band : u[3];
            uint32_t *p = cmd_begin(me->upload_over ? DSR_UPLOAD_OVER : DSR_SURFACE_UPLOAD, 24 + rowb * (uint32_t)(y1 - y0));
            p[0] = me->id; p[1] = (uint32_t)u[0]; p[2] = (uint32_t)y0; p[3] = (uint32_t)(u[2] - u[0]); p[4] = (uint32_t)(y1 - y0); p[5] = rowb;
            uint8_t *dst = (uint8_t *)(p + 6);
            for (int32_t y = y0; y < y1; y++, dst += rowb) memcpy(dst, G_MEM + me->mem + (uint32_t)y * (uint32_t)me->pitch + (uint32_t)u[0] * bpp, rowb);
            cmd_end();
        }
    }
    RET(DD_OK, 2);
END
/* descriptions & misc */
M(s_GetSurfaceDesc, 2) ME(DSurface, O_SURF)
    uint32_t sd = ARG(1), sz = rt_r32(G_MEM, sd); memcpy(GP(sd), me->desc, SD_BYTES); rt_w32(G_MEM, sd, sz); rt_w32(G_MEM, sd + SD_SURFACE, 0); RET(DD_OK, 2);
END
M(s_GetCaps, 2) ME(DSurface, O_SURF) memcpy(GP(ARG(1)), me->desc + SD_CAPS, 16); RET(DD_OK, 2); END
M(s_GetPixelFormat, 2) ME(DSurface, O_SURF) memcpy(GP(ARG(1)), me->desc + SD_PF, 32); RET(DD_OK, 2); END
M(s_GetDDInterface, 2) ME(DSurface, O_SURF) rt_w32(G_MEM, ARG(1), me->dd->g_dd); me->dd->ref++; RET(DD_OK, 2); END
M(s_GetUniquenessValue, 2) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
M(s_GetPriority, 2) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 2); END
M(s_GetLOD, 2) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 2); END
M(s_GetClipper, 2) rt_w32(G_MEM, ARG(1), 0); RET(DDH(205), 2); END
M(s_GetPalette, 2) rt_w32(G_MEM, ARG(1), 0); RET(DDH(125), 2); END
M(s_SetColorKey, 3) dsr_log("SetColorKey flags %08x (ignored)", ARG(1)); RET(DD_OK, 3); END
#define OK_M(name, n) M(name, n) RET(DD_OK, n); END
#define ERR_M(name, n, e) M(name, n) RET(e, n); END
#define UNIMPL_M(name, n) M(name, n) fprintf(stderr, "dsr: UNIMPLEMENTED %s\n", #name); RET(DDERR_UNSUPPORTED, n); END
OK_M(s_IsLost, 1) OK_M(s_Restore, 1) OK_M(s_GetBltStatus, 2) OK_M(s_GetFlipStatus, 2) OK_M(s_SetClipper, 2) OK_M(s_SetPalette, 2)
OK_M(s_EnumOverlayZOrders, 4) OK_M(s_PageLock, 2) OK_M(s_PageUnlock, 2) OK_M(s_ChangeUniquenessValue, 1) OK_M(s_SetPriority, 2) OK_M(s_SetLOD, 2)
ERR_M(s_GetColorKey, 3, DDH(215)) ERR_M(s_GetOverlayPosition, 3, DDH(580)) ERR_M(s_SetOverlayPosition, 3, DDH(580))
ERR_M(s_AddOverlayDirtyRect, 2, DDH(580)) ERR_M(s_UpdateOverlay, 6, DDH(580)) ERR_M(s_UpdateOverlayDisplay, 2, DDH(580))
ERR_M(s_UpdateOverlayZOrder, 3, DDH(580)) ERR_M(s_Initialize, 3, DDH(5)) ERR_M(s_GetPrivateData, 4, DDH(255)) ERR_M(s_FreePrivateData, 2, DDH(255))
UNIMPL_M(s_BltBatch, 4) UNIMPL_M(s_GetDC, 2) UNIMPL_M(s_ReleaseDC, 2) UNIMPL_M(s_SetSurfaceDesc, 3) UNIMPL_M(s_SetPrivateData, 5)
/* gamma */
static uint16_t gamma_ramp[3][256]; static int gamma_set;
M(g_QueryInterface, 3) ME(DSurface, O_GAMMA)
    { uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
      if (iid == 0x00000000u || iid == 0x06675a80u) { rt_w32(G_MEM, out, me->g_surf); surf_addref(me); RET(DD_OK, 3); }
      if (iid == 0x69c11c3eu) { rt_w32(G_MEM, out, me->g_gamma); surf_addref(me); RET(DD_OK, 3); }
      rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3); }
END
M(g_AddRef, 1) ME(DSurface, O_GAMMA) RET(surf_addref(me), 1); END
M(g_Release, 1) ME(DSurface, O_GAMMA) uint32_t r = (uint32_t)__atomic_sub_fetch(&me->ref, 1, __ATOMIC_SEQ_CST); if (!r) surface_destroy(me); RET(r, 1); END
M(g_GetGammaRamp, 3)
    if (!gamma_set) for (int k = 0; k < 256; k++) gamma_ramp[0][k] = gamma_ramp[1][k] = gamma_ramp[2][k] = (uint16_t)(k * 0x101);
    memcpy(GP(ARG(2)), gamma_ramp, 1536); RET(DD_OK, 3);
END
M(g_SetGammaRamp, 3)
    memcpy(gamma_ramp, GP(ARG(2)), 1536); gamma_set = 1;
    uint32_t *p = cmd_begin(DSR_GAMMA, 1536); memcpy(p, gamma_ramp, 1536); cmd_end(); RET(DD_OK, 3);
END

/* ================= IDirectDraw7 ================= */
M(dd_QueryInterface, 3) ME(DDraw, O_DD)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (iid == 0x00000000u || iid == 0x15e65ec0u) { rt_w32(G_MEM, out, me->g_dd); me->ref++; RET(DD_OK, 3); }
    if (iid == 0xf5049e77u) { rt_w32(G_MEM, out, me->g_d3d); me->ref++; RET(DD_OK, 3); }
    dsr_log("ddraw7 QI: unsupported interface %08x", iid); rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(dd_AddRef, 1) ME(DDraw, O_DD) RET((uint32_t)++me->ref, 1); END
M(dd_Release, 1) ME(DDraw, O_DD) RET((uint32_t)--me->ref, 1); END
M(dd_CreateSurface, 4) ME(DDraw, O_DD)
    DSurface *s; uint8_t sd[SD_BYTES]; memcpy(sd, GP(ARG(1)), SD_BYTES);
    uint32_t hr = surface_create(me, sd, &s);
    rt_w32(G_MEM, ARG(2), hr ? 0 : s->g_surf); RET(hr, 4);
END
/* Display modes from this Mac's screen: the standard sizes that fit, the screen's own size in points and, on Retina screens, its full pixel size. Each depth's record comes from the snapshot as a template. */
extern int w32_screen_w, w32_screen_h; extern double w32_screen_scale;
static int mode_list(uint32_t (*out)[2])
{
    static const uint16_t std[][2] = {{640,480},{800,600},{1024,768},{1152,864},{1280,720},{1280,800},{1280,960},{1280,1024},{1366,768},
        {1440,900},{1600,900},{1600,1200},{1680,1050},{1920,1080},{1920,1200},{2048,1152},{2560,1440},{2560,1600}};
    uint32_t W = (uint32_t)w32_screen_w, H = (uint32_t)w32_screen_h; int n = 0;
    for (size_t k = 0; k < sizeof std / sizeof *std; k++) if (std[k][0] <= W && std[k][1] <= H) { out[n][0] = std[k][0]; out[n][1] = std[k][1]; n++; }
    out[n][0] = W; out[n][1] = H; n++;
    if (w32_screen_scale > 1.01) { out[n][0] = (uint32_t)(W * w32_screen_scale + 0.5); out[n][1] = (uint32_t)(H * w32_screen_scale + 0.5); n++; }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)                        /* by size, without duplicates */
        if (out[b][0] * out[b][1] < out[a][0] * out[a][1] || (out[b][0] * out[b][1] == out[a][0] * out[a][1] && out[b][0] < out[a][0])) {
            uint32_t t0 = out[a][0], t1 = out[a][1]; out[a][0] = out[b][0]; out[a][1] = out[b][1]; out[b][0] = t0; out[b][1] = t1; }
    int m = 0; for (int a = 0; a < n; a++) if (!m || out[a][0] != out[m - 1][0] || out[a][1] != out[m - 1][1]) { out[m][0] = out[a][0]; out[m][1] = out[a][1]; m++; }
    return m;
}
M(dd_EnumDisplayModes, 5) ME(DDraw, O_DD)
    uint32_t filter = ARG(2), ctx = ARG(3), cb = ARG(4), d = scratch(SD_BYTES); uint8_t f[SD_BYTES];
    if (filter) memcpy(f, GP(filter), SD_BYTES);
    uint32_t modes[32][2]; int nm = mode_list(modes), stop = 0;
    static const uint32_t depths[] = {32, 16, 8};
    for (int di = 0; di < 3 && !stop; di++) {
        uint8_t tmpl[SD_BYTES]; int have = 0;
        for (const SnapRec *r = snap_first("MODE"); r; r = snap_next(r, "MODE"))
            if (U32(r->data, SD_PF + PF_BITS) == depths[di]) { memcpy(tmpl, r->data, SD_BYTES); have = 1; break; }
        if (!have) continue;
        for (int k = 0; k < nm && !stop; k++) {
            uint8_t m[SD_BYTES]; memcpy(m, tmpl, SD_BYTES);
            uint32_t w = modes[k][0], h = modes[k][1], pitch = w * depths[di] / 8;
            memcpy(m + SD_WIDTH, &w, 4); memcpy(m + SD_HEIGHT, &h, 4); memcpy(m + SD_PITCH, &pitch, 4);
            if (filter && (U32(f, SD_FLAGS) & DDSD_WIDTH) && U32(f, SD_WIDTH) != w) continue;
            if (filter && (U32(f, SD_FLAGS) & DDSD_HEIGHT) && U32(f, SD_HEIGHT) != h) continue;
            if (filter && (U32(f, SD_FLAGS) & DDSD_PIXELFORMAT) && U32(f, SD_PF + PF_BITS) != depths[di]) continue;
            memcpy(GP(d), m, SD_BYTES);
            uint32_t a[2] = {d, ctx}; const SnapRec save = cur_rec;
            uint32_t ret = w32_callback(c, cb, 2, a);
            cur_rec = save;
            if (ret == 0) stop = 1;
        }
    }
    scratch_free(d); RET(DD_OK, 5);
END
M(dd_GetCaps, 3)
    uint32_t hal = ARG(1), hel = ARG(2); const SnapRec *h = snap_first("HCAP");
    if (hal && h) { uint32_t sz = rt_r32(G_MEM, hal), n = sz < h->size ? sz : h->size; memcpy(GP(hal), h->data, n); rt_w32(G_MEM, hal, n); }
    const SnapRec *e = snap_first("ECAP");
    if (hel && e) { uint32_t sz = rt_r32(G_MEM, hel), n = sz < e->size ? sz : e->size; memcpy(GP(hel), e->data, n); rt_w32(G_MEM, hel, n); }
    RET(DD_OK, 3);
END
M(dd_GetDisplayMode, 2) ME(DDraw, O_DD)
    uint32_t sd = ARG(1); const SnapRec *r = snap_first("CURM");
    if (r) { uint32_t sz = rt_r32(G_MEM, sd); memcpy(GP(sd), r->data, sz < r->size ? sz : r->size); }
    rt_w32(G_MEM, sd + SD_WIDTH, me->mode_w); rt_w32(G_MEM, sd + SD_HEIGHT, me->mode_h); rt_w32(G_MEM, sd + SD_PF + PF_BITS, me->mode_bpp);
    rt_w32(G_MEM, sd + SD_PITCH, me->mode_w * me->mode_bpp / 8);
    RET(DD_OK, 2);
END
M(dd_GetFourCCCodes, 3) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 3); END
M(dd_GetGDISurface, 2) ME(DDraw, O_DD)
    if (!me->primary) { rt_w32(G_MEM, ARG(1), 0); RET(DDH(255), 2); }
    rt_w32(G_MEM, ARG(1), me->primary->g_surf); surf_addref(me->primary); RET(DD_OK, 2);
END
M(dd_GetMonitorFrequency, 2) rt_w32(G_MEM, ARG(1), 60); RET(DD_OK, 2); END
M(dd_GetScanLine, 2) rt_w32(G_MEM, ARG(1), 0); RET(DD_OK, 2); END
M(dd_GetVerticalBlankStatus, 2) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
extern int w32_screen_w, w32_screen_h;
M(dd_RestoreDisplayMode, 1) ME(DDraw, O_DD) me->mode_w = (uint32_t)w32_screen_w; me->mode_h = (uint32_t)w32_screen_h; RET(DD_OK, 1); END
M(dd_SetCooperativeLevel, 3) ME(DDraw, O_DD)
    dsr_log("SetCooperativeLevel hwnd %08x flags %08x", ARG(1), ARG(2));
    if (ARG(1)) me->hwnd = ARG(1);
    me->coop = ARG(2);
    RET(DD_OK, 3);
END
void w32_set_window_pos(Ctx *c, uint32_t hwnd, int x, int y, int cx, int cy, uint32_t fl);
M(dd_SetDisplayMode, 6) ME(DDraw, O_DD)
    uint32_t w = ARG(1), h = ARG(2), bpp = ARG(3);
    dsr_log("SetDisplayMode %ux%ux%u", w, h, bpp);
    me->mode_w = w; me->mode_h = h; me->mode_bpp = bpp;
    if (me->hwnd && (me->coop & DDSCL_EXCLUSIVE)) w32_set_window_pos(c, me->hwnd, 0, 0, (int)w, (int)h, 0x40 | 0x10);
    uint32_t *p = cmd_begin(DSR_MODE, 8); p[0] = w; p[1] = h; cmd_end();
    RET(DD_OK, 6);
END
M(dd_GetAvailableVidMem, 4)
    const SnapRec *r = snap_first("VMEM"); uint32_t t = 0x40000000u, f = 0x40000000u;
    if (r) { memcpy(&t, r->data + 8, 4); memcpy(&f, r->data + 12, 4); }
    if (ARG(2)) rt_w32(G_MEM, ARG(2), t);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), f);
    RET(DD_OK, 4);
END
M(dd_GetDeviceIdentifier, 3)
    const SnapRec *r = snap_first("DID0");
    if (r) memcpy(GP(ARG(1)), r->data, r->size < 1080 ? r->size : 1080);
    RET(DD_OK, 3);
END
OK_M(dd_Compact, 1) OK_M(dd_FlipToGDISurface, 1) OK_M(dd_Initialize, 2) OK_M(dd_WaitForVerticalBlank, 3) OK_M(dd_RestoreAllSurfaces, 1)
OK_M(dd_TestCooperativeLevel, 1)
UNIMPL_M(dd_CreateClipper, 4) UNIMPL_M(dd_CreatePalette, 5) UNIMPL_M(dd_DuplicateSurface, 3) UNIMPL_M(dd_EnumSurfaces, 5)
UNIMPL_M(dd_GetSurfaceFromDC, 3) UNIMPL_M(dd_StartModeTest, 4) UNIMPL_M(dd_EvaluateMode, 3)

/* ================= IDirect3D7 ================= */
M(d3d_QueryInterface, 3) ME(DDraw, O_D3D)
    uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2);
    if (iid == 0x00000000u || iid == 0x15e65ec0u) { rt_w32(G_MEM, out, me->g_dd); me->ref++; RET(DD_OK, 3); }
    if (iid == 0xf5049e77u) { rt_w32(G_MEM, out, me->g_d3d); me->ref++; RET(DD_OK, 3); }
    rt_w32(G_MEM, out, 0); RET(E_NOINTERFACE, 3);
END
M(d3d_AddRef, 1) ME(DDraw, O_D3D) RET((uint32_t)++me->ref, 1); END
M(d3d_Release, 1) ME(DDraw, O_D3D) RET((uint32_t)--me->ref, 1); END
M(d3d_EnumDevices, 3)
    uint32_t cb = ARG(1), ctx = ARG(2), buf = scratch(128 + 64 + 1024);
    SnapRec n = {0}, m = {0}, d = {0}; const SnapRec *p;
    if ((p = snap_first("DEVN"))) n = *p;
    if ((p = snap_first("DEVM"))) m = *p;
    if ((p = snap_first("DEVD"))) d = *p;
    while (n.data && m.data && d.data) {
        snprintf((char *)GP(buf), 128, "%s", (const char *)n.data); snprintf((char *)GP(buf + 128), 64, "%s", (const char *)m.data);
        memcpy(GP(buf + 192), d.data, d.size < 1024 ? d.size : 1024);
        uint32_t a[4] = {buf, buf + 128, buf + 192, ctx};
        if (w32_callback(c, cb, 4, a) == 0) break;
        p = snap_next(&n, "DEVN"); n = p ? *p : (SnapRec){0};
        p = snap_next(&m, "DEVM"); m = p ? *p : (SnapRec){0};
        p = snap_next(&d, "DEVD"); d = p ? *p : (SnapRec){0};
    }
    scratch_free(buf); RET(DD_OK, 3);
END
static uint32_t device_create(DDraw *dd, DSurface *rt);
static uint32_t vb_create(uint32_t desc);
M(d3d_CreateDevice, 4) ME(DDraw, O_D3D)
    DSurface *rt = obj(ARG(2), O_SURF); uint32_t g = device_create(me, rt);
    /* as Direct3D: unless the application asked to keep its FPU state (DDSCL_FPUPRESERVE), the calling thread's x87 is
     * left in single precision with exceptions masked (this game asks for that outright with DDSCL_FPUSETUP) */
    if (!(me->coop & 0x1000u) && !getenv("W32_NO_FPUSETUP")) rt_fcw_set(c, (c->fcw & ~0xf3fu) | 0x3fu);   /* W32_NO_FPUSETUP: test switch */
    dsr_log("device created (%08x) on surface %u", rt_r32(G_MEM, ARG(1)), rt ? rt->id : 0);
    rt_w32(G_MEM, ARG(3), g); RET(DD_OK, 4);
END
M(d3d_CreateVertexBuffer, 4) rt_w32(G_MEM, ARG(2), vb_create(ARG(1))); RET(DD_OK, 4); END
M(d3d_EnumZBufferFormats, 4)
    uint32_t guid = ARG(1), cb = ARG(2), ctx = ARG(3); const SnapRec *r; int mine = 0;
    for (r = snap_first("ZDEV"); r; r = snap_next(r, "ZDEV")) if (!memcmp(r->data, GP(guid), 16)) { mine = 1; break; }
    if (!mine) r = snap_first("ZDEV");
    if (!r) RET(DD_OK, 4);
    uint32_t pf = scratch(32);
    for (const uint8_t *p = r->data + r->size; ; ) {
        uint32_t t, n; memcpy(&t, p, 4); memcpy(&n, p + 4, 4);
        if (t != tag4("ZFMT") || p >= snap + snap_size) break;
        memcpy(GP(pf), p + 8, 32);
        uint32_t a[2] = {pf, ctx};
        if (w32_callback(c, cb, 2, a) == 0) break;
        p += 8 + n;
    }
    scratch_free(pf); RET(DD_OK, 4);
END
OK_M(d3d_EvictManagedTextures, 1)

/* ================= IDirect3DVertexBuffer7 ================= */
static uint32_t vb_create(uint32_t desc)
{
    DVB *v = calloc(1, sizeof *v); memcpy(v->desc, GP(desc), 16); v->desc[0] = 16; v->ref = 1;
    v->stride = fvf_stride(v->desc[2]);
    if (v->desc[3] > 0x100000) v->desc[3] = 0x100000;             /* D3D allows 65535 vertices; no size wrap */
    uint32_t bytes = v->desc[3] * v->stride + 64;
    v->mem = bytes >= 0x10000 ? vm_alloc(0, bytes, 0x3000, 4) : heap_alloc(w32_process_heap, 8, bytes);
    dsr_log("vertex buffer: fvf %08x stride %u verts %u caps %08x", v->desc[2], v->stride, v->desc[3], v->desc[1]);
    v->g = make_iface(O_VB, v); return v->g;
}
M(vb_QueryInterface, 3) ME(DVB, O_VB)
    uint32_t iid = rt_r32(G_MEM, ARG(1));
    if (iid == 0 || iid == 0xf5049e7du) { rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(E_NOINTERFACE, 3);
END
M(vb_AddRef, 1) ME(DVB, O_VB) RET((uint32_t)++me->ref, 1); END
M(vb_Release, 1) ME(DVB, O_VB)
    uint32_t r = (uint32_t)--me->ref;
    if (!r) { uint32_t bytes = me->desc[3] * me->stride + 64; if (bytes >= 0x10000) vm_free(me->mem, 0, 0x8000); else heap_free(w32_process_heap, me->mem);
              obj_drop(me->g); free(me); }
    RET(r, 1);
END
M(vb_Lock, 4) ME(DVB, O_VB)
    rt_w32(G_MEM, ARG(2), me->mem); if (ARG(3)) rt_w32(G_MEM, ARG(3), me->desc[3] * me->stride); RET(DD_OK, 4);
END
M(vb_GetVertexBufferDesc, 2) ME(DVB, O_VB)
    uint32_t d = ARG(1), sz = rt_r32(G_MEM, d); memcpy(GP(d), me->desc, 16); rt_w32(G_MEM, d, sz); RET(DD_OK, 2);
END
OK_M(vb_Unlock, 1) OK_M(vb_Optimize, 3) UNIMPL_M(vb_ProcessVertices, 8) UNIMPL_M(vb_ProcessVerticesStrided, 8)

/* ================= IDirect3DDevice7 ================= */
static DDevice *current_device;
static int xf_index(uint32_t t) { return (t >= 1 && t <= 3) ? (int)t : (t >= 16 && t <= 23) ? (int)t - 12 : -1; }
static void emit_xf(uint32_t t, const float *m) { uint32_t *p = cmd_begin(DSR_TRANSFORM, 68); p[0] = t; memcpy(p + 1, m, 64); cmd_end(); }
static void emit_rt(DDevice *dev)
{
    DSurface *z = 0;
    for (int k = 0; k < dev->rt->nattached; k++) if (U32(dev->rt->attached[k]->desc, SD_CAPS) & DDSCAPS_ZBUFFER) z = dev->rt->attached[k];
    uint32_t *p = cmd_begin(DSR_SET_RT, 8); p[0] = dev->rt->id; p[1] = z ? z->id : 0; cmd_end();
}
static uint32_t device_create(DDraw *dd, DSurface *rt)
{
    DDevice *dev = calloc(1, sizeof *dev); dev->ref = 1; dev->dd = dd; dev->rt = rt;
    for (const SnapRec *r = snap_first("RSDF"); r; r = snap_next(r, "RSDF")) { uint32_t p[2]; memcpy(p, r->data, 8); if (p[0] < 256) dev->rs[p[0]] = p[1]; }
    for (const SnapRec *r = snap_first("TSDF"); r; r = snap_next(r, "TSDF")) { uint32_t p[3]; memcpy(p, r->data, 12); if (p[0] < 8 && p[1] < 32) dev->tss[p[0]][p[1]] = p[2]; }
    for (int k = 0; k < 32; k++) { memset(dev->xf[k], 0, 64); dev->xf[k][0] = dev->xf[k][5] = dev->xf[k][10] = dev->xf[k][15] = 1.0f; }
    uint32_t vp[6] = {0, 0, rt ? U32(rt->desc, SD_WIDTH) : dd->mode_w, rt ? U32(rt->desc, SD_HEIGHT) : dd->mode_h, 0, 0};
    float one = 1.0f; memcpy(&vp[5], &one, 4); memcpy(dev->vp, vp, 24);
    if (rt) emit_rt(dev);
    current_device = dev;
    dev->g = make_iface(O_DEV, dev); return dev->g;
}
M(dev_QueryInterface, 3) ME(DDevice, O_DEV)
    uint32_t iid = rt_r32(G_MEM, ARG(1));
    if (iid == 0 || iid == 0xf5049e79u) { rt_w32(G_MEM, ARG(2), me->g); me->ref++; RET(DD_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(E_NOINTERFACE, 3);
END
M(dev_AddRef, 1) ME(DDevice, O_DEV) RET((uint32_t)++me->ref, 1); END
M(dev_Release, 1) ME(DDevice, O_DEV) RET((uint32_t)--me->ref, 1); END
M(dev_GetCaps, 2) const SnapRec *r = snap_first("DCAP"); if (r) memcpy(GP(ARG(1)), r->data, r->size < 1024 ? r->size : 1024); RET(DD_OK, 2); END
M(dev_EnumTextureFormats, 3)
    uint32_t cb = ARG(1), ctx = ARG(2), pf = scratch(32);
    for (const SnapRec *r = snap_first("TFMT"); r; r = snap_next(r, "TFMT")) {
        memcpy(GP(pf), r->data, 32); uint32_t a[2] = {pf, ctx}; SnapRec save = *r;
        uint32_t ret = w32_callback(c, cb, 2, a); cur_rec = save;
        if (ret == 0) break;
    }
    scratch_free(pf); RET(DD_OK, 3);
END
M(dev_BeginScene, 1) uint32_t *p = cmd_begin(DSR_SCENE, 4); p[0] = 1; cmd_end(); RET(DD_OK, 1); END
M(dev_EndScene, 1) uint32_t *p = cmd_begin(DSR_SCENE, 4); p[0] = 0; cmd_end(); RET(DD_OK, 1); END
M(dev_GetDirect3D, 2) ME(DDevice, O_DEV) rt_w32(G_MEM, ARG(1), me->dd->g_d3d); me->dd->ref++; RET(DD_OK, 2); END
M(dev_SetRenderTarget, 3) ME(DDevice, O_DEV) DSurface *s = obj(ARG(1), O_SURF); if (s) { me->rt = s; emit_rt(me); } RET(DD_OK, 3); END
M(dev_GetRenderTarget, 2) ME(DDevice, O_DEV) rt_w32(G_MEM, ARG(1), me->rt->g_surf); surf_addref(me->rt); RET(DD_OK, 2); END
M(dev_Clear, 7)
    uint32_t n = ARG(1), rects = ARG(2); if (!rects || n > 4096) n = 0;          /* D3D: no list = the whole viewport */
    uint32_t *p = cmd_begin(DSR_CLEAR, 20 + 16 * n);
    p[0] = ARG(3); p[1] = ARG(4); p[2] = ARG(5); p[3] = ARG(6); p[4] = n; if (n) memcpy(p + 5, GP(rects), 16 * n);
    cmd_end(); RET(DD_OK, 7);
END
M(dev_SetTransform, 3) ME(DDevice, O_DEV)
    int k = xf_index(ARG(1)); if (k < 0) RET(DDERR_INVALIDPARAMS, 3);
    memcpy(me->xf[k], GP(ARG(2)), 64); emit_xf(ARG(1), me->xf[k]); RET(DD_OK, 3);
END
M(dev_GetTransform, 3) ME(DDevice, O_DEV)
    int k = xf_index(ARG(1)); if (k < 0) RET(DDERR_INVALIDPARAMS, 3);
    memcpy(GP(ARG(2)), me->xf[k], 64); RET(DD_OK, 3);
END
/* same arithmetic and summation order as wined3d's multiply_matrix (new = matrix x current), in float */
static void mul(float *d, const float *a, const float *b)
{
#define A(r, c) a[(r - 1) * 4 + (c - 1)]
#define B(r, c) b[(r - 1) * 4 + (c - 1)]
    float t[16];
    for (int col = 1; col <= 4; col++) for (int row = 1; row <= 4; row++)
        t[(row - 1) * 4 + (col - 1)] = (A(1, col) * B(row, 1)) + (A(2, col) * B(row, 2)) + (A(3, col) * B(row, 3)) + (A(4, col) * B(row, 4));
    memcpy(d, t, 64);
#undef A
#undef B
}
M(dev_MultiplyTransform, 3) ME(DDevice, O_DEV)
    int k = xf_index(ARG(1)); if (k < 0) RET(DDERR_INVALIDPARAMS, 3);
    float m[16]; memcpy(m, GP(ARG(2)), 64); mul(me->xf[k], me->xf[k], m); emit_xf(ARG(1), me->xf[k]); RET(DD_OK, 3);
END
M(dev_SetViewport, 2) ME(DDevice, O_DEV) memcpy(me->vp, GP(ARG(1)), 24); uint32_t *p = cmd_begin(DSR_VIEWPORT, 24); memcpy(p, me->vp, 24); cmd_end(); RET(DD_OK, 2); END
M(dev_GetViewport, 2) ME(DDevice, O_DEV) memcpy(GP(ARG(1)), me->vp, 24); RET(DD_OK, 2); END
M(dev_SetMaterial, 2) ME(DDevice, O_DEV) memcpy(me->material, GP(ARG(1)), 68); RET(DD_OK, 2); END
M(dev_GetMaterial, 2) ME(DDevice, O_DEV) memcpy(GP(ARG(1)), me->material, 68); RET(DD_OK, 2); END
M(dev_SetRenderState, 3) ME(DDevice, O_DEV)
    uint32_t s = ARG(1), v = ARG(2); if (s < 256) me->rs[s] = v;
    uint32_t *p = cmd_begin(DSR_RENDER_STATE, 8); p[0] = s; p[1] = v; cmd_end(); RET(DD_OK, 3);
END
M(dev_GetRenderState, 3) ME(DDevice, O_DEV) rt_w32(G_MEM, ARG(2), ARG(1) < 256 ? me->rs[ARG(1)] : 0); RET(DD_OK, 3); END
static void emit_draw(uint32_t prim, uint32_t fvf, const uint8_t *v, uint32_t nv, const uint8_t *idx, uint32_t ni)
{
    uint32_t st = fvf_stride(fvf);
    if ((uint64_t)st * nv + (uint64_t)ni * 2 > DSR_RING_SIZE / 2) { static int w; if (!w++) fprintf(stderr, "dsr: a draw of %u vertices / %u indices dropped (too large)\n", nv, ni); return; }
    uint32_t vb = st * nv, ib = (ni * 2 + 3) & ~3u, *p = cmd_begin(DSR_DRAW, 16 + vb + ib);
    p[0] = prim; p[1] = fvf; p[2] = nv; p[3] = ni;
    memcpy(p + 4, v, vb); if (ni) memcpy((uint8_t *)(p + 4) + vb, idx, ni * 2);
    cmd_end();
}
M(dev_DrawPrimitive, 6) emit_draw(ARG(1), ARG(2), (const uint8_t *)GP(ARG(3)), ARG(4), 0, 0); RET(DD_OK, 6); END
M(dev_DrawIndexedPrimitive, 8) emit_draw(ARG(1), ARG(2), (const uint8_t *)GP(ARG(3)), ARG(4), (const uint8_t *)GP(ARG(5)), ARG(6)); RET(DD_OK, 8); END
M(dev_DrawPrimitiveVB, 6)
    DVB *v = obj(ARG(2), O_VB); if (!v) RET(DDERR_INVALIDPARAMS, 6);
    emit_draw(ARG(1), v->desc[2], G_MEM + v->mem + ARG(3) * v->stride, ARG(4), 0, 0); RET(DD_OK, 6);
END
M(dev_DrawIndexedPrimitiveVB, 8)
    DVB *v = obj(ARG(2), O_VB); if (!v) RET(DDERR_INVALIDPARAMS, 8);
    emit_draw(ARG(1), v->desc[2], G_MEM + v->mem + ARG(3) * v->stride, ARG(4), (const uint8_t *)GP(ARG(5)), ARG(6)); RET(DD_OK, 8);
END
M(dev_GetClipStatus, 2) memset(GP(ARG(1)), 0, 28); RET(DD_OK, 2); END
M(dev_GetTexture, 3) ME(DDevice, O_DEV)
    DSurface *s = ARG(1) < 8 ? me->tex[ARG(1)] : 0; rt_w32(G_MEM, ARG(2), s ? s->g_surf : 0); if (s) surf_addref(s); RET(DD_OK, 3);
END
M(dev_SetTexture, 3) ME(DDevice, O_DEV)
    uint32_t stage = ARG(1); DSurface *s = obj(ARG(2), O_SURF);
    if (stage >= 8) RET(DDERR_INVALIDPARAMS, 3);
    DSurface *old = me->tex[stage]; if (s) surf_addref(s); me->tex[stage] = s; if (old) surf_release(old);
    uint32_t *p = cmd_begin(DSR_TEXTURE, 8); p[0] = stage; p[1] = s ? s->id : 0; cmd_end(); RET(DD_OK, 3);
END
M(dev_GetTextureStageState, 4) ME(DDevice, O_DEV)
    uint32_t st = ARG(1), t = ARG(2); rt_w32(G_MEM, ARG(3), (st < 8 && t < 32) ? me->tss[st][t] : 0); RET(DD_OK, 4);
END
M(dev_SetTextureStageState, 4) ME(DDevice, O_DEV)
    uint32_t st = ARG(1), t = ARG(2), v = ARG(3); if (st < 8 && t < 32) me->tss[st][t] = v;
    uint32_t *p = cmd_begin(DSR_TSS, 12); p[0] = st; p[1] = t; p[2] = v; cmd_end(); RET(DD_OK, 4);
END
M(dev_ValidateDevice, 2) rt_w32(G_MEM, ARG(1), 1); RET(DD_OK, 2); END
M(dev_GetLightEnable, 3) rt_w32(G_MEM, ARG(2), 0); RET(DD_OK, 3); END
M(dev_GetInfo, 4) RET(S_FALSE, 4); END
OK_M(dev_PreLoad, 2) OK_M(dev_SetClipStatus, 2)
UNIMPL_M(dev_SetLight, 3) UNIMPL_M(dev_GetLight, 3) UNIMPL_M(dev_BeginStateBlock, 1) UNIMPL_M(dev_EndStateBlock, 2)
UNIMPL_M(dev_DrawPrimitiveStrided, 6) UNIMPL_M(dev_DrawIndexedPrimitiveStrided, 8) UNIMPL_M(dev_ComputeSphereVisibility, 6)
UNIMPL_M(dev_ApplyStateBlock, 2) UNIMPL_M(dev_CaptureStateBlock, 2) UNIMPL_M(dev_DeleteStateBlock, 2) UNIMPL_M(dev_CreateStateBlock, 3)
UNIMPL_M(dev_Load, 6) UNIMPL_M(dev_LightEnable, 3) UNIMPL_M(dev_SetClipPlane, 3) UNIMPL_M(dev_GetClipPlane, 3)

/* ---- vtables ---- */
typedef void (*Fn)(Ctx *);
static uint32_t build_vtable(const char *iface, const Fn *fns, int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "ddraw!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
static void init_vtables(void)
{
    static int done; if (done) return; done = 1;
    static const Fn dd[] = { dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
        dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
        dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus,
        dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank,
        dd_GetAvailableVidMem, dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel, dd_GetDeviceIdentifier,
        dd_StartModeTest, dd_EvaluateMode };
    static const Fn d3d[] = { d3d_QueryInterface, d3d_AddRef, d3d_Release, d3d_EnumDevices, d3d_CreateDevice, d3d_CreateVertexBuffer,
        d3d_EnumZBufferFormats, d3d_EvictManagedTextures };
    static const Fn surf[] = { s_QueryInterface, s_AddRef, s_Release, s_AddAttachedSurface, s_AddOverlayDirtyRect, s_Blt, s_BltBatch, s_BltFast,
        s_DeleteAttachedSurface, s_EnumAttachedSurfaces, s_EnumOverlayZOrders, s_Flip, s_GetAttachedSurface, s_GetBltStatus,
        s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC, s_GetFlipStatus, s_GetOverlayPosition, s_GetPalette, s_GetPixelFormat,
        s_GetSurfaceDesc, s_Initialize, s_IsLost, s_Lock, s_ReleaseDC, s_Restore, s_SetClipper, s_SetColorKey,
        s_SetOverlayPosition, s_SetPalette, s_Unlock, s_UpdateOverlay, s_UpdateOverlayDisplay, s_UpdateOverlayZOrder,
        s_GetDDInterface, s_PageLock, s_PageUnlock, s_SetSurfaceDesc, s_SetPrivateData, s_GetPrivateData, s_FreePrivateData,
        s_GetUniquenessValue, s_ChangeUniquenessValue, s_SetPriority, s_GetPriority, s_SetLOD, s_GetLOD };
    static const Fn gamma[] = { g_QueryInterface, g_AddRef, g_Release, g_GetGammaRamp, g_SetGammaRamp };
    static const Fn dev[] = { dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCaps, dev_EnumTextureFormats, dev_BeginScene, dev_EndScene,
        dev_GetDirect3D, dev_SetRenderTarget, dev_GetRenderTarget, dev_Clear, dev_SetTransform, dev_GetTransform,
        dev_SetViewport, dev_MultiplyTransform, dev_GetViewport, dev_SetMaterial, dev_GetMaterial, dev_SetLight,
        dev_GetLight, dev_SetRenderState, dev_GetRenderState, dev_BeginStateBlock, dev_EndStateBlock, dev_PreLoad,
        dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_SetClipStatus, dev_GetClipStatus, dev_DrawPrimitiveStrided,
        dev_DrawIndexedPrimitiveStrided, dev_DrawPrimitiveVB, dev_DrawIndexedPrimitiveVB, dev_ComputeSphereVisibility,
        dev_GetTexture, dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice,
        dev_ApplyStateBlock, dev_CaptureStateBlock, dev_DeleteStateBlock, dev_CreateStateBlock, dev_Load, dev_LightEnable,
        dev_GetLightEnable, dev_SetClipPlane, dev_GetClipPlane, dev_GetInfo };
    static const Fn vb[] = { vb_QueryInterface, vb_AddRef, vb_Release, vb_Lock, vb_Unlock, vb_ProcessVertices, vb_GetVertexBufferDesc,
        vb_Optimize, vb_ProcessVerticesStrided };
    _Static_assert(sizeof dd / sizeof *dd == 30 && sizeof d3d / sizeof *d3d == 8 && sizeof surf / sizeof *surf == 49 &&
                   sizeof gamma / sizeof *gamma == 5 && sizeof dev / sizeof *dev == 49 && sizeof vb / sizeof *vb == 9, "vtable sizes");
    vt[O_DD] = build_vtable("IDirectDraw7", dd, 30); vt[O_D3D] = build_vtable("IDirect3D7", d3d, 8);
    vt[O_SURF] = build_vtable("IDirectDrawSurface7", surf, 49); vt[O_GAMMA] = build_vtable("IDirectDrawGammaControl", gamma, 5);
    vt[O_DEV] = build_vtable("IDirect3DDevice7", dev, 49); vt[O_VB] = build_vtable("IDirect3DVertexBuffer7", vb, 9);
}

/* for the host backend: the current display mode (the game's coordinate space) */
static DDraw *the_dd;
uint32_t dsr_d3d9_w, dsr_d3d9_h;               /* set by d3d9.c: its back buffer is the game's coordinate space */
void dsr_mode_size(uint32_t *w, uint32_t *h)
{
    if (dsr_d3d9_w) { *w = dsr_d3d9_w; *h = dsr_d3d9_h; return; }
    *w = the_dd ? the_dd->mode_w : (uint32_t)w32_screen_w; *h = the_dd ? the_dd->mode_h : (uint32_t)w32_screen_h;
}

/* ---- exports ---- */
IMPL(ddraw, DirectDrawCreateEx)
{
    uint32_t out = ARG(1), iid = rt_r32(G_MEM, ARG(2));
    dsr_log("DirectDrawCreateEx");
    if (iid != 0x15e65ec0u) RET(DDERR_INVALIDPARAMS, 4);
    init_vtables();
    DDraw *dd = calloc(1, sizeof *dd); dd->ref = 1;
    dd->mode_w = (uint32_t)w32_screen_w; dd->mode_h = (uint32_t)w32_screen_h; dd->mode_bpp = 32;
    dd->g_dd = make_iface(O_DD, dd); dd->g_d3d = make_iface(O_D3D, dd); the_dd = dd;
    rt_w32(G_MEM, out, dd->g_dd); RET(DD_OK, 4);
}
/* the same two devices Wine reports: the display, and the named adapter (whose GUID the game passes back) */
IMPL(ddraw, DirectDrawEnumerateExA)
{
    static const uint8_t adapter_guid[16] = {0xd4, 0xcd, 0xb2, 0xae, 0x41, 0x6e, 0xea, 0x43, 0x94, 0x1c, 0x83, 0x61, 0xcc, 0x76, 0x07, 0x81};
    uint32_t cb = ARG(0), ctx = ARG(1), flags = ARG(2), buf = scratch(256);
    memcpy(GP(buf), adapter_guid, 16);
    strcpy((char *)GP(buf + 16), "DirectDraw HAL"); strcpy((char *)GP(buf + 48), "display"); strcpy((char *)GP(buf + 64), "\\\\.\\DISPLAY1");
    const SnapRec *r = snap_first("DID0");
    snprintf((char *)GP(buf + 96), 128, "%s", r ? (const char *)r->data + 512 : "NVIDIA GeForce 8800 GTX");
    uint32_t a1[5] = {0, buf + 16, buf + 48, ctx, 0};
    if (w32_callback(c, cb, 5, a1) && (flags & 1)) { uint32_t a2[5] = {buf, buf + 96, buf + 64, ctx, 1}; w32_callback(c, cb, 5, a2); }
    scratch_free(buf); RET(DD_OK, 3);
}
