/* Engine extensions: functions that Legends of Aranna's scripts call and the base game's engine lacks.
 *
 * The game binds scripts to C++ through "FuBi", which reads the executable's own export table at start-up and derives
 * every function's class, parameters and calling convention from its decorated (mangled) name. ext_install() rebuilds
 * that table in guest memory with the original entries plus ours; each of ours points at a native implementation
 * (an internal thunk), so the Skrit compiler sees them exactly like the engine's own functions.
 *
 * The implementations (runtime/win32/loa_*.c) are written for the base engine from the behaviour the expansion's content
 * expects; nothing of the expansion's executable is used. They are only installed when the expansion's data is active. */
#include "w32.h"
#include "ext.h"
#include <sys/mman.h>

#define MAX_EXT 1024
static struct { const char *name; void (*fn)(Ctx *); } ext[MAX_EXT];
static int n_ext;

void ext_add(const char *mangled, void (*fn)(Ctx *))
{
    if (n_ext < MAX_EXT) { ext[n_ext].name = mangled; ext[n_ext].fn = fn; n_ext++; }
    else fprintf(stderr, "ext: too many functions (%s)\n", mangled);
}

/* a base-engine export by decorated name (the original table, before ext_install) */
uint32_t ext_export(const char *mangled)
{
    static uint32_t ed, nnames, aname, aord, afun;
    uint32_t base = w32_image_base;
    if (!ed) {
        uint32_t pe = base + rt_r32(G_MEM, base + 0x3c); ed = base + rt_r32(G_MEM, pe + 0x78);
        nnames = rt_r32(G_MEM, ed + 24); afun = base + rt_r32(G_MEM, ed + 28); aname = base + rt_r32(G_MEM, ed + 32); aord = base + rt_r32(G_MEM, ed + 36);
    }
    for (uint32_t k = 0; k < nnames; k++)
        if (!strcmp(GS(base + rt_r32(G_MEM, aname + 4 * k)), mangled)) return base + rt_r32(G_MEM, afun + 4 * rt_r16(G_MEM, aord + 2 * k));
    fprintf(stderr, "ext: base engine has no %s\n", mangled);
    return 0;
}

/* the export directory, rebuilt with our functions appended */
int ext_install(void)
{
    if (!n_ext) return 0;
    uint32_t base = w32_image_base, pe = base + rt_r32(G_MEM, base + 0x3c), dd = pe + 0x78;
    uint32_t rva = rt_r32(G_MEM, dd), ed = base + rva;
    uint32_t nfun = rt_r32(G_MEM, ed + 20), nnames = rt_r32(G_MEM, ed + 24);
    uint32_t afun = base + rt_r32(G_MEM, ed + 28), aname = base + rt_r32(G_MEM, ed + 32), aord = base + rt_r32(G_MEM, ed + 36);
    size_t strbytes = 0; for (int k = 0; k < n_ext; k++) strbytes += strlen(ext[k].name) + 1;
    uint32_t tf = nfun + (uint32_t)n_ext, tn = nnames + (uint32_t)n_ext;
    uint32_t size = 40 + 4 * tf + 4 * tn + 2 * tn + (uint32_t)strbytes + 16;
    uint32_t blk = vm_alloc(0, (size + 0xfff) & ~0xfffu, 0x3000, 4);
    if (!blk) { fprintf(stderr, "ext: no memory for the export table\n"); return -1; }
    uint32_t nd = blk, nf = nd + 40, nn = nf + 4 * tf, no = nn + 4 * tn, ns = no + 2 * tn;
    memcpy(GP(nd), GP(ed), 40);
    memcpy(GP(nf), GP(afun), 4 * nfun); memcpy(GP(nn), GP(aname), 4 * nnames); memcpy(GP(no), GP(aord), 2 * nnames);
    uint32_t s = ns;
    for (int k = 0; k < n_ext; k++) {
        uint32_t thunk = w32_thunk_register(ext[k].name, ext[k].fn);
        rt_w32(G_MEM, nf + 4 * (nfun + (uint32_t)k), thunk - base);              /* wraps: base + rva = thunk */
        size_t len = strlen(ext[k].name) + 1; memcpy(GP(s), ext[k].name, len);
        rt_w32(G_MEM, nn + 4 * (nnames + (uint32_t)k), s - base);
        rt_w16(G_MEM, no + 2 * (nnames + (uint32_t)k), nfun + (uint32_t)k);
        s += (uint32_t)len;
    }
    rt_w32(G_MEM, nd + 20, tf); rt_w32(G_MEM, nd + 24, tn);
    rt_w32(G_MEM, nd + 28, nf - base); rt_w32(G_MEM, nd + 32, nn - base); rt_w32(G_MEM, nd + 36, no - base);
    rt_w32(G_MEM, dd, nd - base); rt_w32(G_MEM, dd + 4, size);
    /* FuBi looks at each function's first byte (following incremental-link jumps): our thunks must be readable */
    mprotect(G_MEM + THUNK_BASE, MAX_THUNKS * 16, PROT_READ | PROT_WRITE);
    memset(G_MEM + THUNK_BASE, 0xcc, MAX_THUNKS * 16);
    if (getenv("DS_EXTLOG")) fprintf(stderr, "ext: %d engine functions added to the export table\n", n_ext);
    return n_ext;
}

/* ---- calling the game's own code ---- */
uint32_t ext_thiscall(Ctx *c, uint32_t fn, uint32_t self, int nargs, const uint32_t *args)
{
    uint32_t ecx = c->ecx; c->ecx = self;
    uint32_t r = w32_callback(c, fn, nargs, args);
    c->ecx = ecx; return r;
}
double ext_thiscall_f(Ctx *c, uint32_t fn, uint32_t self, int nargs, const uint32_t *args)
{
    uint32_t top = c->top; ext_thiscall(c, fn, self, nargs, args);
    double v = c->st[c->top & 7]; c->top = top; return v;          /* the result was pushed on the x87 stack */
}
double ext_call_f(Ctx *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t top = c->top; w32_callback(c, fn, nargs, args);
    double v = c->st[c->top & 7]; c->top = top; return v;
}
