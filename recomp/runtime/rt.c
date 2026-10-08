/* Runtime core for recompiled code: guest memory, indirect calls, faults, x87 helpers. */
#include "rt.h"
#include "edition.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sched.h>
#include <sys/mman.h>
#include "platform.h"

uint8_t *G_MEM;

struct rt_fn { uint32_t addr; GuestFn fn; };
extern const struct rt_fn rt_fntab[];
extern const unsigned rt_fntab_n;
struct rt_imp { uint32_t iat; const char *name; };
extern const struct rt_imp rt_imptab[];
extern const unsigned rt_imptab_n;

/* Import thunks: the loader fills each IAT slot with RT_IMPORT_BASE + 16 * index, so code that loads an import's
 * address into a register and calls it later still lands in rt_import. */
#define RT_IMPORT_BASE 0xfff00000u

/* ---- faults: abort, or return to a test harness ---- */
__thread jmp_buf *fault_jmp;            /* set by a harness to catch faults instead of aborting */
__thread uint32_t fault_pc;
__thread const char *fault_what;
__attribute__((weak)) void rt_fault_hook(Ctx *c, uint32_t pc, const char *what) { (void)c; (void)pc; (void)what; }
static void fault(Ctx *c, uint32_t pc, const char *what)
{
    fault_pc = pc; fault_what = what;
    rt_fault_hook(c, pc, what);                               /* crash report (native app); nothing in tests */
    if (fault_jmp) longjmp(*fault_jmp, 1);
    fprintf(stderr, "recomp: fatal: %s at %08x (eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x)\n",
            what, pc, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    abort();
}
void rt_unhandled(Ctx *c, uint32_t pc, const char *what) { fault(c, pc, what); __builtin_unreachable(); }
void rt_div_fault(Ctx *c, uint32_t pc) { fault(c, pc, "integer divide fault"); __builtin_unreachable(); }

#ifdef DS2_BROKEN_WORLD
/* The verified BW VM writes acquire/release routines into its context at
 * runtime. Their code is not in the PE file. Check the emitted templates
 * before use; other generated code still faults explicitly. */
static void bw_generated_lock(Ctx *c)
{
    static const uint8_t code[] = {
        0x8b,0xbb,0x20,0x03,0,0, 0x81,0xc7,4,0,0,0, 0xf0,0xfe,0x0f,
        0x79,9, 0x80,0x3f,0, 0xf3,0x90, 0x7e,0xf9, 0xeb,0xf2, 0xc3
    };
    const uint8_t *generated = G_MEM + 0x1b200c8u;
    static const uint8_t release[] = {
        0x58, 0x8b,0xbb,0x20,3,0,0, 0x81,0xc7,4,0,0,0,
        0x8a,0x47,1, 0x86,7, 0x81,0xc3,0x24,0,0,0,
        0xc7,3,0,0,0,0, 0x9d,0x61,0xc2,8,0
    };
    if (!memcmp(generated, release, 3) &&
            !memcmp(generated + 7, release + 7, 2) &&
            !memcmp(generated + 13, release + 13, 7) &&
            !memcmp(generated + 24, release + 24, sizeof release - 24)) {
        uint32_t pointer = rt_r32(G_MEM, c->ebx + rt_r32(generated, 3));
        uint32_t address = pointer + rt_r32(generated, 9);
        /* Memory XCHG is implicitly atomic, without a LOCK prefix. */
        (void)__atomic_exchange_n(G_MEM + address, G_MEM[address + 1], __ATOMIC_SEQ_CST);
        rt_w32(G_MEM, c->ebx + rt_r32(generated, 20), 0);
        c->esp += 4;  /* discard this call's return address */
        flags_set(&c->f, rt_r32(G_MEM, c->esp), 0xcd5u | F_DF);
        c->esp += 4;
        c->edi = rt_r32(G_MEM, c->esp);
        c->esi = rt_r32(G_MEM, c->esp + 4);
        c->ebp = rt_r32(G_MEM, c->esp + 8);
        c->ebx = rt_r32(G_MEM, c->esp + 16);
        c->edx = rt_r32(G_MEM, c->esp + 20);
        c->ecx = rt_r32(G_MEM, c->esp + 24);
        c->eax = rt_r32(G_MEM, c->esp + 28);
        c->esp += 32;
        c->tc = rt_r32(G_MEM, c->esp);
        c->esp += 12;
        return;
    }
    if (memcmp(generated, code, 2) || memcmp(generated + 6, code + 6, 2) ||
            memcmp(generated + 12, code + 12, sizeof code - 12)) {
        static __thread char why[384];
        strcpy(why, "unexpected generated VM lock routine:");
        size_t used = strlen(why);
        for (unsigned i = 0; i < 64; i++)
            used += (size_t)snprintf(why + used, sizeof why - used, " %02x", generated[i]);
        rt_unhandled(c, 0x1b200c8u, why);
    }
    /* The creator selects a context slot and lock-byte offset each time. */
    uint32_t slot = rt_r32(generated, 2), offset = rt_r32(generated, 8);
    uint32_t pointer = rt_r32(G_MEM, c->ebx + slot);
    c->edi = pointer + offset;
    LFs f = c->f;
    LF(LF_ADD, 2, pointer, offset, c->edi);
    for (;;) {
        uint32_t result = __atomic_sub_fetch(G_MEM + c->edi, (uint8_t)1, __ATOMIC_SEQ_CST);
        f.cin = flag_cf(&f);
        LF(LF_DEC, 0, (result + 1u) & 255u, 1, result);
        if (!(result & 128u)) break;
        uint32_t value;
        do {
            value = __atomic_load_n(G_MEM + c->edi, __ATOMIC_RELAXED);
            LF(LF_SUB, 0, value, 0, value);
            if ((int8_t)value <= 0) sched_yield();
        } while ((int8_t)value <= 0);
    }
    c->f = f;
    c->tc = rt_r32(G_MEM, c->esp);
    c->esp += 4;
}
#endif

GuestFn rt_lookup(uint32_t addr)
{
#ifdef DS2_BROKEN_WORLD
    if (addr == 0x1b200c8u) return bw_generated_lock;
    /* Guest continuations are ordinary native blocks. A compact address hash
     * keeps dispatch cheap without a dense 4 GB guest-address lookup table. */
    extern GuestFn rt_bw_lookup(uint32_t);
    GuestFn cached = rt_bw_lookup(addr);
    if (cached) return cached;
#endif
    unsigned lo = 0, hi = rt_fntab_n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rt_fntab[mid].addr == addr) return rt_fntab[mid].fn;
        if (rt_fntab[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}
#ifdef DS2_BROKEN_WORLD
static struct rt_fn *bw_functions;
static uint32_t bw_mask;
__attribute__((constructor)) static void bw_prepare_functions(void)
{
    uint32_t n = 1;
    while (n < rt_fntab_n * 2u) n <<= 1;
    bw_functions = calloc(n, sizeof *bw_functions);
    if (!bw_functions) return;  /* the sorted table remains a correct fallback */
    bw_mask = n - 1;
    for (unsigned i = 0; i < rt_fntab_n; i++) {
        uint32_t j = (rt_fntab[i].addr * 2654435761u) & bw_mask;
        while (bw_functions[j].fn) j = (j + 1) & bw_mask;
        bw_functions[j] = rt_fntab[i];
    }
}
GuestFn rt_bw_lookup(uint32_t addr)
{
    if (!bw_functions) return 0;
    uint32_t j = (addr * 2654435761u) & bw_mask;
    while (bw_functions[j].fn) {
        if (bw_functions[j].addr == addr) return bw_functions[j].fn;
        j = (j + 1) & bw_mask;
    }
    return 0;
}
#endif
static __thread RtSehPad *seh_pads;
void rt_seh_push(RtSehPad *p) { p->next = seh_pads; seh_pads = p; }
void rt_seh_pop(RtSehPad *p)
{
    for (RtSehPad **q = &seh_pads; *q; q = &(*q)->next) if (*q == p) { *q = p->next; return; }
}
__attribute__((weak)) void w32_seh_abandon(void) {}
void rt_call(Ctx *c, uint32_t t)
{
#ifdef DS2_BROKEN_WORLD
    /* BW's relocated code uses push/ret for branches and push/jmp for calls.
     * Its lifted blocks publish the next guest PC in tc. Stop at the caller's
     * actual return address; normal C returns alone cannot represent this. */
    uint32_t ret = rt_r32(G_MEM, c->esp);
again:
#endif
    for (RtSehPad *p = seh_pads; p; p = p->next)              /* control transfer to an __except block? */
        for (const uint32_t *h = p->handlers; *h; h++) if (*h == t) {
            Ctx *dst = p->c;
            if (dst != c) { uint32_t fs = dst->fs_base; *dst = *c; dst->fs_base = fs; }   /* registers as the dispatcher left them */
            p->target = t; seh_pads = p;                      /* frames above the target are gone */
            w32_seh_abandon();
            _longjmp(p->jb, 1);
        }
    if (t >= RT_IMPORT_BASE) {
#ifdef DS2_BROKEN_WORLD
        uint32_t next = rt_r32(G_MEM, c->esp);
        rt_import(c, (t - RT_IMPORT_BASE) / 16);
        if (next && next != ret) { t = next; goto again; }
        c->tc = 0;
#else
        rt_import(c, (t - RT_IMPORT_BASE) / 16);
#endif
        return;
    }
    GuestFn fn = rt_lookup(t);
    if (!fn) {                                                /* name the image, so a report says which binary needs analysis */
        extern const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]; extern const unsigned rt_images_n;
        static __thread char m[160]; const char *img = "outside every image";
        for (unsigned k = 0; k < rt_images_n; k++) {
            uint32_t b = rt_images[k].base, sz = k ? 0x01000000u : DS2_IMAGE_END - b;   /* address windows: headers may be unmapped */
            if (t - b < sz) { img = *rt_images[k].name ? rt_images[k].name : "DungeonSiege.exe"; break; }
        }
        snprintf(m, sizeof m, "indirect call to unknown code in %s", img); fault(c, t, m);
    }
#ifdef DS2_BROKEN_WORLD
    int flow = t >= 0x400000u && t < DS2_IMAGE_END;
    uint32_t foreign_return = flow ? 0 : rt_r32(G_MEM, c->esp);
    c->tc = 0;
#endif
    fn(c);
#ifdef DS2_BROKEN_WORLD
    if (!flow && foreign_return && foreign_return != ret) { t = foreign_return; goto again; }
    if (flow && c->tc && c->tc != ret) { t = c->tc; goto again; }
    c->tc = 0;
#endif
}
/* Weak default: the platform layer (Win32 on macOS) provides the real one. */
__attribute__((weak)) void rt_import(Ctx *c, uint32_t index)
{
    static char buf[160];
    snprintf(buf, sizeof buf, "unimplemented import %s", index < rt_imptab_n ? rt_imptab[index].name : "?");
    fault(c, rt_r32(G_MEM, c->esp), buf);
}
uint64_t rt_rdtsc(void)
{
    return ds_clock_ns() * 3;   /* a nominal 3 GHz counter */
}

/* ---- x87 helpers ---- */
__thread uint32_t rt_host_rc = UINT32_MAX;                  /* the x87 rounding control this thread's FPCR is set to (rt_fpcr_sync) */
/* the x87 transcendentals through the system maths library, which assumes round-to-nearest: this thread's rounding
 * mode is set to nearest around the call (the empty asm statements tie the call to the switches, so the compiler can't
 * move it across them) */
#if defined(__aarch64__)
#define FPREG "w"
#define NEAR_BEGIN uint64_t fpcr0 = __builtin_arm_rsr64("fpcr"); int dir = (fpcr0 >> 22) & 3; \
    if (dir) __builtin_arm_wsr64("fpcr", fpcr0 & ~(3ull << 22));
#define NEAR_END   if (dir) __builtin_arm_wsr64("fpcr", fpcr0);
#else
#if defined(__x86_64__)
#define FPREG "x"
#else
#define FPREG "g"
#endif
#define NEAR_BEGIN int mode0 = fegetround(); if (mode0 != FE_TONEAREST) fesetround(FE_TONEAREST);
#define NEAR_END if (mode0 != FE_TONEAREST) fesetround(mode0);
#endif
double rt_x87_1(int op, double x)
{
    NEAR_BEGIN __asm__ volatile("" : "+" FPREG (x));
    double r = op == 0 ? expm1(x * 0.693147180559945309417) : op == 1 ? tan(x) : op == 2 ? sin(x) : cos(x);
    __asm__ volatile("" : "+" FPREG (r)); NEAR_END
    return r;
}
double rt_x87_2(int op, double y, double x)
{
    NEAR_BEGIN __asm__ volatile("" : "+" FPREG (x), "+" FPREG (y));
    double r = op == 0 ? y * log2(x) : op == 1 ? atan2(y, x) : y * log2(x + 1.0);
    __asm__ volatile("" : "+" FPREG (r)); NEAR_END
    return r;
}
double rt_f80_load(const uint8_t *M, uint32_t a)
{
    uint64_t mant; uint16_t se; memcpy(&mant, M + a, 8); memcpy(&se, M + a + 8, 2);
    int e = se & 0x7fff; double s = (se & 0x8000) ? -1.0 : 1.0;
    if (e == 0 && mant == 0) return s * 0.0;
    if (e == 0x7fff) return (mant << 1) ? NAN : s * INFINITY;
    return s * ldexp((double)mant, e - 16383 - 63);
}
void rt_f80_store(uint8_t *M, uint32_t a, double v)
{
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e; double m = frexp(fabs(v), &e);               /* v = m * 2^e, m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64); se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(M + a, &mant, 8); memcpy(M + a + 8, &se, 2);
}
void rt_fxam(Ctx *c)
{
    double v = ST(0); uint32_t s = c->fsw & ~(uint32_t)(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (signbit(v)) s |= FSW_C1;
    switch (fpclassify(v)) {
    case FP_NAN: s |= FSW_C0; break;
    case FP_INFINITE: s |= FSW_C2 | FSW_C0; break;
    case FP_ZERO: s |= FSW_C3; break;
    case FP_SUBNORMAL: s |= FSW_C3 | FSW_C2; break;
    default: s |= FSW_C2; break;
    }
    c->fsw = s;
}
void rt_fprem(Ctx *c, int ieee)
{
    double a = ST(0), b = ST(1); int q;
    double rn = remquo(a, b, &q);                            /* low quotient bits, exact, for the nearest quotient */
    double r = ieee ? rn : fmod(a, b);
    unsigned ql = (unsigned)(q < 0 ? -q : q) & 7;
    if (!ieee && r != rn) ql = (ql - 1) & 7;                 /* the truncated quotient is one less in magnitude */
    uint32_t s = c->fsw & ~(uint32_t)(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (ql & 1) s |= FSW_C1;
    if (ql & 2) s |= FSW_C3;
    if (ql & 4) s |= FSW_C0;
    c->fsw = s; ST(0) = r;
}
/* 32-bit protected-mode environment layout: fcw, fsw, tag, fip, fcs, foo, fos (7 dwords) */
void rt_fnstenv(Ctx *c, uint32_t a)
{
    uint8_t *M = G_MEM;
    rt_w32(M, a, c->fcw | 0xffff0000u); rt_w32(M, a + 4, rt_fnstsw(c) | 0xffff0000u); rt_w32(M, a + 8, 0xffffffffu);
    for (int k = 3; k < 7; k++) rt_w32(M, a + 4 * k, 0);
    c->fcw |= 0x3f;                                          /* fnstenv masks all exceptions */
}
void rt_fldenv(Ctx *c, uint32_t a)
{
    uint8_t *M = G_MEM;
    rt_fcw_set(c, rt_r16(M, a)); uint32_t sw = rt_r16(M, a + 4);
    c->fsw = sw & ~0x3800u; c->top = (sw >> 11) & 7;
}
void rt_fnsave(Ctx *c, uint32_t a)
{
    rt_fnstenv(c, a); c->fcw &= ~0x3fu;
    rt_w32(G_MEM, a, c->fcw | 0xffff0000u);
    for (int k = 0; k < 8; k++) rt_f80_store(G_MEM, a + 28 + 10 * k, ST(k));
    rt_fcw_set(c, 0x37f); c->fsw = 0; c->top = 0;            /* then FNINIT */
}
void rt_frstor(Ctx *c, uint32_t a)
{
    rt_fldenv(c, a);
    for (int k = 0; k < 8; k++) ST(k) = rt_f80_load(G_MEM, a + 28 + 10 * k);
}

/* ---- test harness entry points ---- */
uint8_t *rt_test_mem(void)
{
    if (!G_MEM) {
        void *p = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return 0;
        G_MEM = p; mprotect(G_MEM, 0x10000, PROT_NONE);       /* guest NULL page faults */
    }
    return G_MEM;
}
int rt_test_run(Ctx *c, uint32_t fn_addr, uint32_t *pc, const char **what)
{
    GuestFn fn = rt_lookup(fn_addr);
    if (!fn) { *pc = fn_addr; *what = "no such function"; return 2; }
    jmp_buf jb; fault_jmp = &jb;
    if (setjmp(jb)) { fault_jmp = 0; rt_fpcr_sync(0x27f); *pc = fault_pc; *what = fault_what; return 1; }
    rt_fpcr_sync(c->fcw);
#ifdef DS2_BROKEN_WORLD
    rt_call(c, fn_addr);
#else
    fn(c);
#endif
    fault_jmp = 0; rt_fpcr_sync(0x27f); return 0;   /* the caller's (Python's) rounding back */
}
uint32_t rt_test_flags(const Ctx *c) { return flags_get(&c->f); }

static volatile char bus_flag;
void rt_bus_lock(void) { while (__atomic_test_and_set(&bus_flag, __ATOMIC_ACQUIRE)) ; }
void rt_bus_unlock(void) { __atomic_clear(&bus_flag, __ATOMIC_RELEASE); }

/* cpuid: an Intel Pentium (P54C) without MMX, so code with CPU-specific paths takes its plain x86/x87 path */
int rt_cpuid_sse2;                              /* report SSE/SSE2 (games recompiled with tools/sse.py, e.g. Fallout NV) */
#ifdef DF_EDITION
int rt_cpuid_mmx = 1;                           /* report a Pentium MMX (Delta Force requires MMX; tools/sse.py lifts it) */
#else
int rt_cpuid_mmx;
#endif
void rt_cpuid(Ctx *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    (void)c;
    if (*eax == 0) { *eax = 1; *ebx = 0x756e6547u; *edx = 0x49656e69u; *ecx = 0x6c65746eu; return; }   /* "GenuineIntel" */
    if (*eax >= 0x80000000u) { *eax = 0; *ebx = *ecx = *edx = 0; return; }                              /* no extended leaves (no 3DNow!) */
    if (rt_cpuid_mmx) { *eax = 0x543; *ebx = 0; *ecx = 0; *edx = 0x008001bfu; return; }                /* Pentium MMX (P55C) */
    if (rt_cpuid_sse2) { *eax = 0x6f6; *ebx = 0; *ecx = 0; *edx = 0x078081bfu; return; }             /* family 6 (Core 2 era): + CMOV, MMX, FXSR, SSE, SSE2; no SSE3 */
    *eax = 0x524; *ebx = 0; *ecx = 0; *edx = 0x1bf;                                                      /* family 5: FPU, TSC, CX8... no MMX */
}
/* weak default without the Win32 layer: fatal */
__attribute__((weak)) int w32_raise_cpu_exception(Ctx *c, uint32_t pc, uint32_t code) { (void)c; (void)pc; (void)code; return 0; }
void rt_exception(Ctx *c, uint32_t pc, uint32_t code)
{
    if (w32_raise_cpu_exception(c, pc, code)) return;
    static __thread char buf[64]; snprintf(buf, sizeof buf, "unhandled CPU exception %08x", code);
    fault(c, pc, buf);
}

/* weak default without the Win32 layer: no native features */
__attribute__((weak)) void rt_hook(Ctx *c, uint32_t addr) { (void)c; (void)addr; }
__attribute__((weak)) int rt_override(Ctx *c, uint32_t addr) { (void)c; (void)addr; return 0; }
