/* Guest virtual memory: VirtualAlloc-style regions inside the 4 GB guest space. Host pages are always readable and
 * writable (G_MEM is one anonymous mapping); this tracks reservation and commit state for VirtualQuery and returns
 * zeroed memory for new commits. */
#include "w32.h"
#include <sys/mman.h>
#include "platform.h"

typedef struct Region { uint32_t base, size, type, prot; uint8_t *commit; } Region;   /* commit: per 4 KB page */
static Region *R; static int nR, capR;
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;

extern int w32_protect_memory;
#define HP (ds_page_size())                                   /* host page (arm64 macOS); guest pages are 4 KB */
static void zero_range(uint32_t a, uint32_t n)
{
    if (!n) return;
    uint32_t lo = (a + HP - 1) & ~(HP - 1), hi = (a + n) & ~(HP - 1);
    if (lo < hi) mmap(G_MEM + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (a < lo) memset(G_MEM + a, 0, (lo < a + n ? lo : a + n) - a);
    if (hi >= lo && hi < a + n && hi >= a) memset(G_MEM + hi, 0, a + n - hi);
}
/* host protection follows guest commit state (host-page granularity: a host page is accessible if any guest page in it
 * is committed) */
static int find(uint32_t a);
static void protect(uint32_t a, uint32_t n, int rw)
{
    if (!w32_protect_memory || !n) return;
    uint32_t lo = a & ~(HP - 1), hi = (a + n + HP - 1) & ~(HP - 1);
    mprotect(G_MEM + lo, hi - lo, rw ? PROT_READ | PROT_WRITE : PROT_NONE);
    if (rw && getenv("W32_VMLOG") && ((a & (HP - 1)) || ((a + n) & (HP - 1)))) fprintf(stderr, "w32: commit %08x+%x is not host-page aligned\n", a, n);
}
static int find(uint32_t a)
{
    int lo = 0, hi = nR - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (a < R[m].base) hi = m - 1;
        else if (a >= R[m].base + R[m].size) lo = m + 1;
        else return m;
    }
    return -1;
}
static int insert(uint32_t base, uint32_t size, uint32_t type, uint32_t prot)
{
    if (nR == capR) { capR = capR ? capR * 2 : 256; R = realloc(R, capR * sizeof *R); }
    int k = 0; while (k < nR && R[k].base < base) k++;
    memmove(R + k + 1, R + k, (nR - k) * sizeof *R); nR++;
    R[k].base = base; R[k].size = size; R[k].type = type; R[k].prot = prot; R[k].commit = calloc(size >> 12, 1);
    return k;
}
static int range_free(uint32_t a, uint32_t n)
{
    for (int k = 0; k < nR; k++) {
        uint32_t e = (R[k].base + R[k].size + 0xffff) & ~0xffffu;          /* a region owns its whole last 64 KB granule */
        if (a < e && R[k].base < a + n) return 0;
    }
    return 1;
}
/* R is sorted by base. Visit occupied intervals once instead of restarting a
 * full region scan for every 64 KB candidate. Keep the same lowest/highest fit
 * and granule ownership as the original allocator, including partial granules.
 * Called with vm_lock held; wide arithmetic prevents endpoint wraparound. */
static uint32_t find_gap(uint32_t span, int top_down)
{
    if (!span || span > VM_HI - VM_LO) return 0;
    if (top_down) {
        uint64_t base = (VM_HI - span) & ~0xffffu;
        for (int k = nR - 1; k >= 0; k--) {
            uint64_t lo = R[k].base;
            uint64_t hi = (lo + R[k].size + 0xffffu) & ~0xffffull;
            if (lo >= base + span) continue;
            if (hi <= base) return (uint32_t)base;
            if (lo < (uint64_t)VM_LO + span) return 0;
            base = (lo - span) & ~0xffffull;
        }
        return (uint32_t)base;
    }
    uint64_t base = VM_LO;
    for (int k = 0; k < nR; k++) {
        uint64_t lo = R[k].base;
        uint64_t hi = (lo + R[k].size + 0xffffu) & ~0xffffull;
        if (hi <= base) continue;
        if (lo >= base + span) return (uint32_t)base;
        base = hi;
        if (base + span > VM_HI) return 0;
    }
    return (uint32_t)base;
}
void vm_register(uint32_t base, uint32_t size, uint32_t type)
{
    pthread_mutex_lock(&vm_lock);
    int k = insert(base, size, type, 4); memset(R[k].commit, 1, size >> 12);
    protect(base, size, 1);
    pthread_mutex_unlock(&vm_lock);
}
uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot)
{
    enum { MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_TOP_DOWN = 0x100000 };
    uint32_t r = 0;
    if (!size) return 0;
    pthread_mutex_lock(&vm_lock);
    if (!addr || (type & MEM_RESERVE)) {
        /* Windows: the base is 64 KB aligned, the region is page (4 KB) granular; the rest of the 64 KB is unusable */
        uint32_t base = addr & ~0xffffu, end = (addr ? addr + size : 0) , span;
        uint32_t psz = addr ? ((end + 0xfff) & ~0xfffu) - base : (size + 0xfff) & ~0xfffu;
        span = (psz + 0xffff) & ~0xffffu;
        if (!addr) {
            base = find_gap(span, type & MEM_TOP_DOWN);
            if (!base) goto out;
        } else if (!range_free(base, psz)) goto out;
        int k = insert(base, psz, 0x20000 /* MEM_PRIVATE */, prot);
        if (type & MEM_COMMIT) {
            uint32_t a = addr ? addr & ~0xfffu : base, e = addr ? (addr + size + 0xfff) & ~0xfffu : base + psz;
            for (uint32_t p = a; p < e; p += 0x1000) R[k].commit[(p - base) >> 12] = 1;
            protect(a, e - a, 1);
        }
        r = addr ? addr & ~0xfffu : base;
    } else if (type & MEM_COMMIT) {
        int k = find(addr);
        uint32_t a = addr & ~0xfffu, e = (addr + size + 0xfff) & ~0xfffu;
        if (k < 0 || e > R[k].base + R[k].size) goto out;
        for (uint32_t p = a; p < e; p += 0x1000) R[k].commit[(p - R[k].base) >> 12] = 1;
        protect(a, e - a, 1);
        r = a;
    }
out:
    pthread_mutex_unlock(&vm_lock);
    if (getenv("W32_VMLOG")) fprintf(stderr, "w32: VirtualAlloc(%08x, %x, %x, %x) -> %08x\n", addr, size, type, prot, r);
    return r;
}
int vm_free(uint32_t addr, uint32_t size, uint32_t type)
{
    enum { MEM_DECOMMIT = 0x4000, MEM_RELEASE = 0x8000 };
    int ok = 0;
    pthread_mutex_lock(&vm_lock);
    int k = find(addr);
    if (k < 0) goto out;
    if (type & MEM_RELEASE) {
        if (addr != R[k].base) goto out;
        {   uint32_t b = R[k].base, n = R[k].size;
            mmap(G_MEM + (b & ~(HP - 1)), ((b + n + HP - 1) & ~(HP - 1)) - (b & ~(HP - 1)), PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            if (w32_protect_memory) mprotect(G_MEM + (b & ~(HP - 1)), ((b + n + HP - 1) & ~(HP - 1)) - (b & ~(HP - 1)), PROT_NONE);
        }
        free(R[k].commit);
        memmove(R + k, R + k + 1, (nR - k - 1) * sizeof *R); nR--; ok = 1;
    } else if (type & MEM_DECOMMIT) {
        uint32_t a = addr & ~0xfffu, e = size ? (addr + size + 0xfff) & ~0xfffu : R[k].base + R[k].size;
        if (e > R[k].base + R[k].size) goto out;
        for (uint32_t p = a; p < e; p += 0x1000) R[k].commit[(p - R[k].base) >> 12] = 0;
        /* zero the decommitted pages: host pages with no committed guest page left get fresh pages (and are closed);
         * pages still partly committed are accessible and are cleared in place */
        for (uint32_t hp = a & ~(HP - 1); hp < e; hp += HP) {
            int any = 0;
            for (uint32_t p = hp; p < hp + HP; p += 0x1000) { int j = find(p); if (j >= 0 && R[j].commit[(p - R[j].base) >> 12]) any = 1; }
            uint32_t lo = hp > a ? hp : a, hi = hp + HP < e ? hp + HP : e;
            if (!any) {
                mmap(G_MEM + hp, HP, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
                if (w32_protect_memory) mprotect(G_MEM + hp, HP, PROT_NONE);
            } else if (w32_protect_memory || 1) memset(G_MEM + lo, 0, hi - lo);
        }
        ok = 1;
    }
out:
    pthread_mutex_unlock(&vm_lock);
    if (getenv("W32_VMLOG")) fprintf(stderr, "w32: VirtualFree(%08x, %x, %x) -> %d\n", addr, size, type, ok);
    return ok;
}
int vm_query(uint32_t addr, uint32_t out)
{
    uint32_t mbi[7] = {0};
    pthread_mutex_lock(&vm_lock);
    int k = find(addr); uint32_t page = addr & ~0xfffu;
    if (k < 0) {   /* free: up to the next region */
        uint32_t next = 0x7fff0000u;
        for (int j = 0; j < nR; j++) if (R[j].base > page) { next = R[j].base; break; }
        mbi[0] = page; mbi[3] = next > page ? next - page : 0x1000; mbi[4] = 0x10000 /* MEM_FREE */; mbi[5] = 1 /* NOACCESS */;
    } else {
        Region *g = &R[k]; uint8_t st = g->commit[(page - g->base) >> 12]; uint32_t e = page;
        while (e < g->base + g->size && g->commit[(e - g->base) >> 12] == st) e += 0x1000;
        mbi[0] = page; mbi[1] = g->base; mbi[2] = g->prot; mbi[3] = e - page;
        mbi[4] = st ? 0x1000 /* MEM_COMMIT */ : 0x2000 /* MEM_RESERVE */; mbi[5] = st ? g->prot : 0; mbi[6] = g->type;
    }
    pthread_mutex_unlock(&vm_lock);
    memcpy(G_MEM + out, mbi, sizeof mbi);
    return 28;
}
int vm_committed(uint32_t addr, uint32_t size)
{
    int ok = 1;
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = addr & ~0xfffu; p < addr + size; p += 0x1000) {
        int k = find(p);
        if (k < 0 || !R[k].commit[(p - R[k].base) >> 12]) { ok = 0; break; }
    }
    pthread_mutex_unlock(&vm_lock);
    return ok;
}
uint64_t vm_hash(void)       /* FNV-1a over all committed pages (harness state comparison) */
{
    uint64_t h = 1469598103934665603ull;
    pthread_mutex_lock(&vm_lock);
    for (int k = 0; k < nR; k++)
        for (uint32_t p = 0; p < R[k].size; p += 0x1000) {
            if (!R[k].commit[p >> 12]) continue;
            const uint64_t *w = (const uint64_t *)(G_MEM + R[k].base + p);
            for (int j = 0; j < 512; j++) { h ^= w[j]; h *= 1099511628211ull; }
        }
    pthread_mutex_unlock(&vm_lock);
    return h;
}
/* tests/replay.py: registers and every committed page, written at a hook (W32_SNAP, runtime/win32/hooks.c) so one
 * call of a function can be replayed through the original x86 in Unicorn and through the recompiled code */
void vm_snapshot(const char *path, const Ctx *c)
{
    FILE *f = fopen(path, "wb"); if (!f) { perror(path); return; }
    uint32_t hdr[12] = {0x4e534644u /* "DFSN" */, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi,
                        flags_get(&c->f), c->fs_base, c->fcw};
    fwrite(hdr, 4, 12, f);
    pthread_mutex_lock(&vm_lock);
    for (int k = 0; k < nR; k++)
        for (uint32_t p = 0; p < R[k].size; p += 0x1000) {
            if (!R[k].commit[p >> 12]) continue;
            uint32_t a = R[k].base + p; fwrite(&a, 4, 1, f); fwrite(G_MEM + a, 1, 0x1000, f);
        }
    pthread_mutex_unlock(&vm_lock);
    fclose(f); fprintf(stderr, "w32: snapshot at %08x -> %s\n", rt_r32(G_MEM, c->esp), path);
}
/* 0 = free, 1 = reserved (not committed), 2 = committed */
int vm_page_state(uint32_t a)
{
    pthread_mutex_lock(&vm_lock);
    int k = find(a), st = k < 0 ? 0 : R[k].commit[((a & ~0xfffu) - R[k].base) >> 12] ? 2 : 1;
    pthread_mutex_unlock(&vm_lock);
    return st;
}
