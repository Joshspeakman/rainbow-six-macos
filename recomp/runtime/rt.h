/* Runtime support for statically recompiled 32-bit x86 code (Dungeon Siege recompilation).
 *
 * Execution model (recomp/README.md):
 *  - Guest memory is one 4 GB host region; a guest address is an offset into it (G_MEM). The PE image is mapped at
 *    its preferred base, so every pointer the game stores keeps its original 32-bit value.
 *  - Registers live in a per-thread Ctx; generated functions copy them into locals on entry and write them back
 *    around calls (SPILL/RELOAD). The guest stack is real: call pushes the guest return address, ret pops it, and
 *    arguments are read from guest memory exactly as the original code does.
 *  - Flags are lazy: flag-setting instructions record (op, a, b, result); consumers compute only what they need.
 *  - x87 is 8 doubles + TOP + control/status words, following the precision model measured for this game under
 *    Rosetta: double arithmetic, rounding to float only at 32-bit stores, fist/fistp honouring
 *    the rounding-control field. */
#ifndef RT_H
#define RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <fenv.h>

/* Avoid an out-of-range double-to-int conversion: x86 and ARM handle that
 * differently. +/-4096 already over/underflows every finite nonzero double. */
static inline double rt_fscale(double value, double exponent) {
    if (isnan(value) || isnan(exponent)) return value + exponent;
    if (isinf(exponent) && ((exponent > 0 && value == 0) || (exponent < 0 && isinf(value)))) return NAN;
    int n = exponent > 4096 ? 4096 : exponent < -4096 ? -4096 : (int)trunc(exponent);
    return ldexp(value, n);
}

extern uint8_t *G_MEM;                         /* guest address 0 */

typedef struct LFs { uint32_t op, a, b, r, cin, eflags, df; } LFs;
typedef union X128 {                           /* an SSE register (lanes of each width) */
    uint8_t b[16]; int8_t sb[16]; uint16_t w[8]; int16_t sw[8]; uint32_t d[4]; int32_t sd[4];
    uint64_t q[2]; int64_t sq[2]; float f[4]; double x[2];
} X128;
typedef struct Ctx {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    LFs f;
    double st[8];
    uint32_t top, fcw, fsw;                    /* fsw: condition codes and exception bits; TOP is kept in `top` */
    uint32_t fs_base;                          /* guest address of this thread's TEB */
    uint32_t tc;                               /* pending tail call inside a tail-call cycle (see lift.py) */
    uint32_t mxcsr;                            /* SSE control/status (0x1f80 at thread start) */
    X128 xmm[8];                               /* SSE registers: kept in the context, not in locals */
    uint64_t mm[8];                            /* MMX registers (not aliased onto the x87 stack in this model) */
} Ctx;

/* ---- memory (memcpy: guest accesses may be unaligned) ---- */
static inline uint32_t rt_r8 (const uint8_t *M, uint32_t a) { return M[a]; }
static inline uint32_t rt_r16(const uint8_t *M, uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t rt_r32(const uint8_t *M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t rt_r64(const uint8_t *M, uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline float    rt_rf32(const uint8_t *M, uint32_t a) { float v; memcpy(&v, M + a, 4); return v; }
static inline double   rt_rf64(const uint8_t *M, uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void rt_w8 (uint8_t *M, uint32_t a, uint32_t v) { M[a] = (uint8_t)v; }
static inline void rt_w16(uint8_t *M, uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(M + a, &x, 2); }
static inline void rt_w32(uint8_t *M, uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void rt_w64(uint8_t *M, uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline void rt_wf32(uint8_t *M, uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void rt_wf64(uint8_t *M, uint32_t a, double v) { memcpy(M + a, &v, 8); }
#define R8(a)      rt_r8(M, (uint32_t)(a))
#define R16(a)     rt_r16(M, (uint32_t)(a))
#define R32(a)     rt_r32(M, (uint32_t)(a))
#define R64(a)     rt_r64(M, (uint32_t)(a))
#define RF32(a)    rt_rf32(M, (uint32_t)(a))
#define RF64(a)    rt_rf64(M, (uint32_t)(a))
#define W8(a, v)   rt_w8(M, (uint32_t)(a), (v))
#define W16(a, v)  rt_w16(M, (uint32_t)(a), (v))
#define W32(a, v)  rt_w32(M, (uint32_t)(a), (v))
#define W64(a, v)  rt_w64(M, (uint32_t)(a), (v))
#define WF32(a, v) rt_wf32(M, (uint32_t)(a), (v))
#define WF64(a, v) rt_wf64(M, (uint32_t)(a), (v))

/* ---- registers in locals ---- */
#define REGS uint8_t *const M = G_MEM; \
    uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi; \
    LFs f = c->f
#define SPILL  (c->eax = eax, c->ecx = ecx, c->edx = edx, c->ebx = ebx, c->esp = esp, c->ebp = ebp, c->esi = esi, c->edi = edi, c->f = f)
#define RELOAD (eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, esp = c->esp, ebp = c->ebp, esi = c->esi, edi = c->edi, f = c->f)

/* ---- lazy flags ---- */
enum { LF_EXPLICIT, LF_ADD, LF_SUB, LF_ADC, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_SHL, LF_SHR, LF_SAR };
enum { F_CF = 1, F_PF = 4, F_AF = 16, F_ZF = 64, F_SF = 128, F_IF = 512, F_DF = 1024, F_OF = 2048 };
/* low two bits of op: operand size 0 = 8, 1 = 16, 2 = 32 bits */
#define LF(o, sz, A, B, Rr) (f.op = ((o) << 2) | (sz), f.a = (uint32_t)(A), f.b = (uint32_t)(B), f.r = (uint32_t)(Rr))
static inline uint32_t lf_mask(uint32_t sz) { return sz == 0 ? 0xffu : sz == 1 ? 0xffffu : 0xffffffffu; }
static inline uint32_t lf_sign(uint32_t sz) { return sz == 0 ? 0x80u : sz == 1 ? 0x8000u : 0x80000000u; }
static inline uint32_t lf_bits(uint32_t sz) { return sz == 0 ? 8 : sz == 1 ? 16 : 32; }

static inline int flag_cf(const LFs *f)
{
    uint32_t op = f->op >> 2, sz = f->op & 3, m = lf_mask(sz), a = f->a & m, b = f->b & m, r = f->r & m;
    switch (op) {
    case LF_ADD: return r < a;
    case LF_ADC: return f->cin ? r <= a : r < a;
    case LF_SUB: return a < b;
    case LF_SBB: return f->cin ? a <= b : a < b;
    case LF_LOGIC: return 0;
    case LF_INC: case LF_DEC: return f->cin & 1;
    case LF_SHL: return f->b <= lf_bits(sz) ? (int)((a >> (lf_bits(sz) - f->b)) & 1) : 0;
    case LF_SHR: return (int)((a >> (f->b - 1)) & 1);
    case LF_SAR: { int32_t sa = sz == 0 ? (int8_t)a : sz == 1 ? (int16_t)a : (int32_t)a;
                   return (int)((sa >> (f->b - 1 > 31 ? 31 : f->b - 1)) & 1); }
    }
    return f->eflags & F_CF;
}
static inline int flag_zf(const LFs *f)
{ return (f->op >> 2) == LF_EXPLICIT ? (f->eflags & F_ZF) != 0 : (f->r & lf_mask(f->op & 3)) == 0; }
static inline int flag_sf(const LFs *f)
{ return (f->op >> 2) == LF_EXPLICIT ? (f->eflags & F_SF) != 0 : (f->r & lf_sign(f->op & 3)) != 0; }
static inline int flag_pf(const LFs *f)
{ return (f->op >> 2) == LF_EXPLICIT ? (f->eflags & F_PF) != 0 : !__builtin_parity(f->r & 0xff); }
static inline int flag_of(const LFs *f)
{
    uint32_t op = f->op >> 2, sz = f->op & 3, s = lf_sign(sz), m = lf_mask(sz), a = f->a, b = f->b, r = f->r;
    switch (op) {
    case LF_ADD: case LF_ADC: return ((a ^ r) & (b ^ r) & s) != 0;
    case LF_SUB: case LF_SBB: return ((a ^ b) & (a ^ r) & s) != 0;
    case LF_LOGIC: return 0;
    case LF_INC: return (r & m) == s;
    case LF_DEC: return (r & m) == s - 1;
    case LF_SHL: return (((r & s) != 0) ^ flag_cf(f));
    case LF_SHR: return (a & s) != 0;
    case LF_SAR: return 0;
    }
    return (f->eflags & F_OF) != 0;
}
static inline int flag_af(const LFs *f)
{
    switch (f->op >> 2) {
    case LF_EXPLICIT: return (f->eflags & F_AF) != 0;
    case LF_INC: return ((f->r ^ (f->r - 1)) & 0x10) != 0;
    case LF_DEC: return ((f->r ^ (f->r + 1)) & 0x10) != 0;
    case LF_ADD: case LF_ADC: case LF_SUB: case LF_SBB: return ((f->a ^ f->b ^ f->r) & 0x10) != 0;
    }
    return 0;
}
static inline uint32_t flags_get(const LFs *f)
{
    return (f->eflags & ~(uint32_t)(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF | F_DF)) | 2 |
           (flag_cf(f) ? F_CF : 0) | (flag_pf(f) ? F_PF : 0) | (flag_af(f) ? F_AF : 0) |
           (flag_zf(f) ? F_ZF : 0) | (flag_sf(f) ? F_SF : 0) | (flag_of(f) ? F_OF : 0) | (f->df ? F_DF : 0);
}
/* flags that mask names take v's bits; the others keep their current values, computed only when they're kept (the
 * mask is a constant at nearly every use, e.g. sahf after an x87 compare, so the others drop out when inlined) */
static inline __attribute__((always_inline)) void flags_set(LFs *f, uint32_t v, uint32_t mask)
{
    uint32_t cur;
    if ((f->op >> 2) == LF_EXPLICIT) cur = f->eflags;
    else {
        cur = f->eflags & ~(uint32_t)(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
        if (!(mask & F_CF) && flag_cf(f)) cur |= F_CF;
        if (!(mask & F_PF) && flag_pf(f)) cur |= F_PF;
        if (!(mask & F_AF) && flag_af(f)) cur |= F_AF;
        if (!(mask & F_ZF) && flag_zf(f)) cur |= F_ZF;
        if (!(mask & F_SF) && flag_sf(f)) cur |= F_SF;
        if (!(mask & F_OF) && flag_of(f)) cur |= F_OF;
    }
    cur = (cur & ~(uint32_t)F_DF) | 2 | (f->df ? F_DF : 0);
    f->eflags = (cur & ~mask) | (v & mask); f->op = LF_EXPLICIT; f->df = (f->eflags & F_DF) != 0;
}
#define CC_O  flag_of(&f)
#define CC_NO (!flag_of(&f))
#define CC_B  flag_cf(&f)
#define CC_AE (!flag_cf(&f))
#define CC_E  flag_zf(&f)
#define CC_NE (!flag_zf(&f))
#define CC_BE (flag_cf(&f) || flag_zf(&f))
#define CC_A  (!flag_cf(&f) && !flag_zf(&f))
#define CC_S  flag_sf(&f)
#define CC_NS (!flag_sf(&f))
#define CC_P  flag_pf(&f)
#define CC_NP (!flag_pf(&f))
#define CC_L  (flag_sf(&f) != flag_of(&f))
#define CC_GE (flag_sf(&f) == flag_of(&f))
#define CC_LE (flag_zf(&f) || flag_sf(&f) != flag_of(&f))
#define CC_G  (!flag_zf(&f) && flag_sf(&f) == flag_of(&f))

/* ---- x87 (double model) ---- */
#define ST(i)     (c->st[(c->top + (i)) & 7])
#define FPUSH(v)  do { double _v = (v); c->top = (c->top - 1) & 7; c->st[c->top] = _v; } while (0)
#define FPOP()    (c->top = (c->top + 1) & 7)
/* a value rounded to float as the x87 does, honouring the rounding-control field (FCW bits 10-11): nearest, down,
 * up or toward zero; the game switches to toward-zero at times, and its 32-bit stores then truncate */
static inline float rt_f32(uint32_t fcw, double x)
{
    float f = (float)x;
    if (__builtin_expect((fcw & 0xc00u) == 0, 1) || (double)f == x || x != x) return f;
    uint32_t b; memcpy(&b, &f, 4);                       /* one step to the neighbouring float (what nextafterf does) */
    switch ((fcw >> 10) & 3) {
    case 1: if ((double)f > x) b = (b & 0x7fffffffu) == 0 ? 0x80000001u : (b >> 31) ? b + 1 : b - 1; break;   /* down */
    case 2: if ((double)f < x) b = (b & 0x7fffffffu) == 0 ? 0x00000001u : (b >> 31) ? b - 1 : b + 1; break;   /* up */
    default: if (fabs((double)f) > fabs(x)) b -= 1; break;                                                  /* toward zero */
    }
    memcpy(&f, &b, 4); return f;
}
/* precision control (FCW bits 8-9): with single precision selected, as Direct3D leaves it for this game
 * (DDSCL_FPUSETUP), add/sub/mul/div/sqrt results are rounded to 24 bits like the x87's; the exponent keeps the
 * x87's range (values outside float's normal range are left as they are) */
static inline double rt_pc(uint32_t fcw, double x)
{
    if (__builtin_expect((fcw & 0x300u) != 0, 0)) return x;
    double a = fabs(x);
    return (a >= 1.1754943508222875e-38 && a <= 3.4028234663852886e+38) ? (double)rt_f32(fcw, x) : x;
}
#define PC(x)     rt_pc(c->fcw, (x))
/* The host's rounding mode follows the game's x87 rounding control on every thread that runs game code (rt_fcw_set at
 * each control-word change, rt_fpcr_sync where a thread starts running game code). The game spends much of its time
 * rounding toward zero, and in the directed modes (down, up, toward zero) rounding to double and then to float gives
 * exactly the x87's single rounding, so PC() and the float stores are exact; to nearest the hardware's default is
 * kept. Library maths that assumes round-to-nearest (the x87 transcendentals) goes through rt_x87_* below. */
extern __thread uint32_t rt_host_rc;
static inline void rt_fpcr_sync(uint32_t fcw)
{
#if defined(__aarch64__)
    uint32_t rc = (fcw >> 10) & 3;
    if (__builtin_expect(rc != rt_host_rc, 0)) {
        static const uint64_t arm[4] = {0, 2ull << 22, 1ull << 22, 3ull << 22};   /* x87 nearest/down/up/zero -> ARM RMode */
        __asm__ volatile("" ::: "memory");
        __builtin_arm_wsr64("fpcr", (__builtin_arm_rsr64("fpcr") & ~(3ull << 22)) | arm[rc]);
        __asm__ volatile("" ::: "memory");
        rt_host_rc = rc;
    }
#elif defined(__x86_64__)
    uint32_t rc = (fcw >> 10) & 3;
    if (__builtin_expect(rc != rt_host_rc, 0)) {
        static const int mode[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
        fesetround(mode[rc]);
        rt_host_rc = rc;
    }
#else
    (void)fcw;
#endif
}
static inline void rt_fcw_set(Ctx *c, uint32_t v) { c->fcw = v; rt_fpcr_sync(v); }
double rt_x87_1(int op, double x);                 /* 0 f2xm1, 1 fptan, 2 fsin, 3 fcos */
double rt_x87_2(int op, double y, double x);       /* 0 fyl2x, 1 fpatan, 2 fyl2xp1 */
enum { FSW_C0 = 0x100, FSW_C1 = 0x200, FSW_C2 = 0x400, FSW_C3 = 0x4000 };
static inline void rt_fcom(Ctx *c, double a, double b)
{
    uint32_t s = c->fsw & ~(uint32_t)(FSW_C0 | FSW_C2 | FSW_C3);
    if (a != a || b != b) s |= FSW_C0 | FSW_C2 | FSW_C3;
    else if (a < b) s |= FSW_C0;
    else if (a == b) s |= FSW_C3;
    c->fsw = s;
}
static inline uint32_t rt_fnstsw(const Ctx *c) { return (c->fsw & ~0x3800u & 0xffff) | ((c->top & 7) << 11); }
static inline double rt_frnd(const Ctx *c, double v)     /* rounding per the control word's RC field */
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline uint32_t rt_fist32(const Ctx *c, double v)
{ double r = rt_frnd(c, v); return (r >= -2147483648.0 && r < 2147483648.0) ? (uint32_t)(int32_t)r : 0x80000000u; }
static inline uint32_t rt_fist16(const Ctx *c, double v)
{ double r = rt_frnd(c, v); return (r >= -32768.0 && r < 32768.0) ? (uint32_t)(uint16_t)(int16_t)r : 0x8000u; }
static inline uint64_t rt_fist64(const Ctx *c, double v)
{ double r = rt_frnd(c, v); return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (uint64_t)(int64_t)r : 0x8000000000000000ull; }
void   rt_fxam(Ctx *c);
void   rt_fprem(Ctx *c, int ieee);
double rt_f80_load(const uint8_t *M, uint32_t a);
void   rt_f80_store(uint8_t *M, uint32_t a, double v);
void   rt_fnstenv(Ctx *c, uint32_t a);
void   rt_fldenv(Ctx *c, uint32_t a);
void   rt_fnsave(Ctx *c, uint32_t a);
void   rt_frstor(Ctx *c, uint32_t a);

#include "rt_sse.h"

/* ---- SEH landing pads: functions with __try/__except register one when they install their SEH frame; when the
 * exception dispatcher transfers control to one of their __except blocks, execution resumes in that function ---- */
#include <setjmp.h>
typedef struct RtSehPad { jmp_buf jb; uint32_t frame, target; const uint32_t *handlers; struct RtSehPad *next; Ctx *c; } RtSehPad;
void rt_seh_push(RtSehPad *p);
void rt_seh_pop(RtSehPad *p);

/* ---- calls and faults ---- */
typedef void (*GuestFn)(Ctx *);
void     rt_call(Ctx *c, uint32_t target);       /* indirect call: function table or import thunk; fatal if unknown */
void     rt_import(Ctx *c, uint32_t index);      /* imported function `index` (return address on the guest stack) */
void     rt_unhandled(Ctx *c, uint32_t pc, const char *what) __attribute__((noreturn));
void     rt_div_fault(Ctx *c, uint32_t pc) __attribute__((noreturn));
uint64_t rt_rdtsc(void);
void     rt_hook(Ctx *c, uint32_t addr);                   /* lift.py HOOKS: native features (runtime/win32/hooks.c) */
int      rt_override(Ctx *c, uint32_t addr);               /* lift.py OVERRIDES: nonzero = the call was performed natively */
void     rt_cpuid(Ctx *c, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
extern int rt_cpuid_sse2;                     /* 1: cpuid reports a family 6 CPU with MMX/SSE/SSE2 (Fallout NV profile) */
/* lock-prefixed read-modify-write instructions without an atomic C equivalent: one lock for all of them */
void     rt_bus_lock(void);
void     rt_bus_unlock(void);
void     rt_exception(Ctx *c, uint32_t pc, uint32_t code);   /* raise a CPU exception (SEH); returns if a handler continues */
#endif
