/* Win32 heaps in guest memory. Deterministic (same addresses on every run): boundary-tag blocks, binned free lists,
 * segments from vm_alloc. Block header (16 bytes, guest memory): size|INUSE, prev_size, requested size, owner heap.
 * Free blocks keep next/prev free links in their first payload words. Payloads are 16-byte aligned. */
#include "w32.h"

#define INUSE     1u
#define HDR       16u
#define MINBLK    32u
#define NBINS     96
#define SEG_MIN   (1u << 20)
#define BIG       (4u << 20)            /* allocations above this get their own segment */
enum { HEAP_ZERO_MEMORY = 0x08, HEAP_REALLOC_IN_PLACE_ONLY = 0x10 };

typedef struct Heap {
    uint32_t handle; pthread_mutex_t lock;
    uint32_t bins[NBINS];               /* guest address of the first free block, 0 = empty */
    uint32_t *segs; int nsegs, capsegs; /* segment bases (first block at base + 16) */
} Heap;
static Heap *heaps[256]; static int nheaps;
static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t w32_process_heap;

#define SZ(b)   (R32x((b)) & ~15u)
static inline uint32_t R32x(uint32_t a) { return rt_r32(G_MEM, a); }
static inline void W32x(uint32_t a, uint32_t v) { rt_w32(G_MEM, a, v); }

static int bin_of(uint32_t sz)
{
    if (sz <= 1024) return (int)(sz >> 4) - 2;          /* 32..1024: bins 0..62 exact */
    int b = 63 + (31 - __builtin_clz(sz)) - 10;        /* 2^10.. : one bin per power of two */
    return b < NBINS ? b : NBINS - 1;
}
static void unlink_free(Heap *h, uint32_t b)
{
    uint32_t nx = R32x(b + HDR), pv = R32x(b + HDR + 4);
    if (pv) W32x(pv + HDR, nx); else h->bins[bin_of(SZ(b))] = nx;
    if (nx) W32x(nx + HDR + 4, pv);
}
static void link_free(Heap *h, uint32_t b)
{
    int k = bin_of(SZ(b)); uint32_t head = h->bins[k];
    W32x(b + HDR, head); W32x(b + HDR + 4, 0);
    if (head) W32x(head + HDR + 4, b);
    h->bins[k] = b;
}
static void set_block(uint32_t b, uint32_t size, uint32_t inuse, uint32_t prev, uint32_t req, uint32_t owner)
{ W32x(b, size | inuse); W32x(b + 4, prev); W32x(b + 8, req); W32x(b + 12, owner); }

static uint32_t add_segment(Heap *h, uint32_t need)
{
    uint32_t sz = need + 2 * HDR > SEG_MIN ? (need + 2 * HDR + 0xffff) & ~0xffffu : SEG_MIN;
    uint32_t base = vm_alloc(0, sz, 0x3000, 4);
    if (!base) return 0;
    if (h->nsegs == h->capsegs) { h->capsegs = h->capsegs ? h->capsegs * 2 : 8; h->segs = realloc(h->segs, h->capsegs * sizeof *h->segs); }
    h->segs[h->nsegs++] = base;
    /* [16-byte segment header][free block ...][end sentinel: size 0, in use] */
    uint32_t b = base + HDR, n = sz - 2 * HDR - HDR;
    set_block(b, n, 0, 0, 0, h->handle);
    set_block(b + n, 0, INUSE, n, 0, h->handle);
    link_free(h, b);
    return b;
}
static Heap *H(uint32_t handle)
{
    for (int k = 0; k < nheaps; k++) if (heaps[k] && heaps[k]->handle == handle) return heaps[k];
    return 0;
}
uint32_t heap_create(uint32_t initial, uint32_t max)
{
    (void)initial; (void)max;
    Heap *h = calloc(1, sizeof *h);
    pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h->lock, &a);
    h->handle = 1;                                  /* placeholder until the first segment exists */
    uint32_t b = add_segment(h, 0);
    if (!b) { free(h); return 0; }
    h->handle = h->segs[0];
    W32x(b + 12, h->handle); W32x(b + SZ(b) + 12, h->handle);
    pthread_mutex_lock(&heaps_lock); heaps[nheaps++] = h; pthread_mutex_unlock(&heaps_lock);
    return h->handle;
}
int heap_destroy(uint32_t handle)
{
    Heap *h = H(handle);
    if (!h || handle == w32_process_heap) return 0;
    for (int k = 0; k < h->nsegs; k++) vm_free(h->segs[k], 0, 0x8000);
    pthread_mutex_lock(&heaps_lock);
    for (int k = 0; k < nheaps; k++) if (heaps[k] == h) heaps[k] = 0;
    pthread_mutex_unlock(&heaps_lock);
    free(h->segs); free(h); return 1;
}
static uint32_t take(Heap *h, uint32_t b, uint32_t need, uint32_t req)
{
    uint32_t sz = SZ(b), prev = R32x(b + 4);
    unlink_free(h, b);
    if (sz - need >= MINBLK) {                       /* split: the rest stays free */
        uint32_t r = b + need, rs = sz - need, nx = b + sz;
        set_block(r, rs, 0, need, 0, h->handle); W32x(nx + 4, rs); link_free(h, r); sz = need;
    }
    set_block(b, sz, INUSE, prev, req, h->handle);
    return b + HDR;
}
static uint32_t alloc_locked(Heap *h, uint32_t size)
{
    uint32_t need = (size + HDR + 15) & ~15u;
    if (need < MINBLK) need = MINBLK;
    if (size > 0x7ff00000u) return 0;
    for (int k = bin_of(need); k < NBINS; k++)
        for (uint32_t b = h->bins[k]; b; b = R32x(b + HDR))
            if (SZ(b) >= need) return take(h, b, need, size);
    uint32_t b = add_segment(h, need);
    return b ? take(h, b, need, size) : 0;
}
uint32_t heap_alloc(uint32_t handle, uint32_t flags, uint32_t size)
{
    Heap *h = H(handle); if (!h) return 0;
    pthread_mutex_lock(&h->lock);
    uint32_t p = alloc_locked(h, size);
    pthread_mutex_unlock(&h->lock);
    if (p && (flags & HEAP_ZERO_MEMORY)) memset(G_MEM + p, 0, size);
    return p;
}
static int valid(Heap *h, uint32_t p)
{ return p >= 0x10000 && (R32x(p - HDR) & INUSE) && R32x(p - HDR + 12) == h->handle && SZ(p - HDR); }
static void free_locked(Heap *h, uint32_t b)
{
    uint32_t sz = SZ(b), prev = R32x(b + 4), nx = b + sz;
    if (!(R32x(nx) & INUSE)) { unlink_free(h, nx); sz += SZ(nx); }
    if (prev && !(R32x(b - prev) & INUSE)) { b -= prev; unlink_free(h, b); sz += SZ(b); prev = R32x(b + 4); }
    set_block(b, sz, 0, prev, 0, h->handle); W32x(b + sz + 4, sz);
    link_free(h, b);
}
int heap_free(uint32_t handle, uint32_t p)
{
    Heap *h = H(handle);
    if (!p) return 1;
    if (!h || !valid(h, p)) return 0;
    pthread_mutex_lock(&h->lock); free_locked(h, p - HDR); pthread_mutex_unlock(&h->lock);
    return 1;
}
uint32_t heap_size(uint32_t handle, uint32_t p)
{
    Heap *h = H(handle);
    return h && valid(h, p) ? R32x(p - HDR + 8) : 0xffffffffu;
}
uint32_t heap_realloc(uint32_t handle, uint32_t flags, uint32_t p, uint32_t size)
{
    Heap *h = H(handle);
    if (!h || !valid(h, p)) return 0;
    pthread_mutex_lock(&h->lock);
    uint32_t b = p - HDR, sz = SZ(b), old = R32x(b + 8), need = (size + HDR + 15) & ~15u, r = 0;
    if (need < MINBLK) need = MINBLK;
    uint32_t nx = b + sz;
    if (need > sz && !(R32x(nx) & INUSE) && sz + SZ(nx) >= need) {       /* grow into the next free block */
        unlink_free(h, nx); uint32_t tot = sz + SZ(nx);
        set_block(b, tot, INUSE, R32x(b + 4), old, h->handle); W32x(b + tot + 4, tot); sz = tot;
    }
    if (need <= sz) {
        if (sz - need >= MINBLK) {                                           /* shrink: free the tail */
            uint32_t t = b + need, ts = sz - need;
            set_block(b, need, INUSE, R32x(b + 4), size, h->handle);
            set_block(t, ts, INUSE, need, 0, h->handle); W32x(t + ts + 4, ts); free_locked(h, t);
        } else W32x(b + 8, size);
        r = p;
    } else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY)) {
        r = alloc_locked(h, size);
        if (r) { memcpy(G_MEM + r, G_MEM + p, old < size ? old : size); free_locked(h, b); }
    }
    pthread_mutex_unlock(&h->lock);
    if (r && (flags & HEAP_ZERO_MEMORY) && size > old) memset(G_MEM + r + old, 0, size - old);
    return r;
}
