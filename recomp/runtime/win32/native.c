/* Native versions of the game's hottest inner loops. Each one computes exactly what the recompiled original does (the
 * same double-precision steps rounded by the x87's precision control, the same float stores and the same integer
 * conversion under the current rounding mode); DS2_NATIVE=0 turns them off, DS2_NATIVE_CHECK=1 runs the original as
 * well and reports any difference. */
#include "w32.h"
#include "edition.h"

static int native_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("DS2_NATIVE"); on = !(e && !strcmp(e, "0")); } return on; }
static int native_check(void) { static int on = -1; if (on < 0) on = getenv("DS2_NATIVE_CHECK") != 0; return on; }

/* 0x5385f0 (cdecl dst, src, s): *dst += *src * s / 256 per colour channel, each channel saturating at 255; dst's
 * alpha is kept. The same integer steps as the original (packed red/blue, green apart). */
static uint32_t color_add_scaled(uint32_t d, uint32_t src, uint32_t s)
{
    uint32_t rb = (src & 0xff00ffu) * s, g = ((src >> 8) & 0xffu) * s;
    rb += 0x800080u;
    uint32_t a = ((((rb >> 8) & 0xff00ffu) + rb) >> 8) & 0xff00ffu;
    g += 0x80u;
    a += d & 0xff00ffu;
    uint32_t gg = ((((g >> 8) & 0xffu) + g) & 0xff00u) + (d & 0xff00ff00u);
    if (a & 0xff000000u) a = (a & 0xffffu) | 0xff0000u;
    if (a & 0xff00u) a = (a & 0xff0000u) | 0xffu;
    if (gg & 0xff0000u) gg = (gg & 0xff00ff00u) | 0xff00u;
    return gg + a;
}

/* The vertex lighting of a directional light (inside 0x544fd0, the loop at 0x544df0..0x544e61; about a third of the
 * game thread). For each of ecx vertices: the light direction (ebp-0x30..-0x28) dotted with the vertex normal (esi,
 * 12 bytes apart); when that is above [0xa8f338] it is scaled by the light's intensity (ebp-0xc), clamped to 1 at
 * [0xa8ec14], and 0x538690 adds the light's colour ([ebp+8]+4) times it to the vertex colour (edx+0xc, 24 bytes
 * apart): 0x538690 converts the factor with fistp of (float)(factor * [0xa8e71c]) and calls 0x5385f0. */
static struct { int pending; uint32_t dst, n, ebx, esi, edi; uint32_t col[4096]; float last; int have_last; } chk;
#include "platform.h"
static void light_dir_loop_(Ctx *c);
static void light_dir_loop(Ctx *c)
{
    static int prof = -1; if (prof < 0) prof = getenv("DS2_NATIVE_PROF") != 0;
    if (!prof) { light_dir_loop_(c); return; }
    static uint64_t t_sum, calls, verts, last; uint64_t t0 = ds_clock_ns(); uint32_t n = c->ecx;
    light_dir_loop_(c);
    uint64_t t1 = ds_clock_ns(); t_sum += t1 - t0; calls++; verts += n;
    if (!last) last = t1;
    if (t1 - last > 1000000000ull * 5) {   /* every ~5 s (nanosecond timebase) */
        fprintf(stderr, "native: light loop %llu calls, %llu vertices, %.2f ms in 5 s\n", (unsigned long long)calls, (unsigned long long)verts, t_sum / 1000000.0);
        t_sum = calls = verts = 0; last = t1;
    }
}
static void light_dir_loop_(Ctx *c)
{
    uint32_t n = c->ecx, ebp = c->ebp, fcw = c->fcw; if (!n) return;
    const uint8_t *M = G_MEM;
    double lx = rt_rf32(M, ebp - 0x30), ly = rt_rf32(M, ebp - 0x2c), lz = rt_rf32(M, ebp - 0x28), in = rt_rf32(M, ebp - 0xc);
    double k0 = rt_rf32(M, DS2_LIGHT_ZERO), k1 = rt_rf32(M, DS2_LIGHT_ONE), k2 = rt_rf32(M, DS2_LIGHT_SCALE);
    uint32_t src = rt_r32(M, rt_r32(M, ebp + 8) + 4);                                       /* the light's colour */
    uint32_t nrm = c->esi + 8, dst = c->edx + 0xc, end_esi = nrm + 12 * n, end_edi = dst + 0x18 * n;
    int check = native_check() && n <= 4096; float last = 0; int have_last = 0;
    /* with single precision selected (as Direct3D leaves the FPU) and the host's rounding mode following the x87's, float
     * arithmetic rounds exactly as the x87 does whenever a result is a normal float (or zero): such vertices take the
     * float path; any other result (subnormal, infinite, NaN), or another precision setting, takes the exact one */
    int fast = (fcw & 0x300u) == 0;
    float flx = (float)lx, fly = (float)ly, flz = (float)lz, fin = (float)in, fk0 = (float)k0, fk1 = (float)k1, fk2 = (float)k2;
    #define OKF(x) ((x) == 0.0f || (__builtin_fabsf(x) >= 1.17549435e-38f && __builtin_fabsf(x) <= 3.40282347e+38f))
    for (uint32_t k = 0; k < n; k++, nrm += 12, dst += 0x18) {
        float v, t = 0; int lit = -1;                         /* -1: not decided by the float path */
        if (fast) {
            float yy = fly * rt_rf32(M, nrm - 4), xx = flx * rt_rf32(M, nrm - 8), d1 = yy + xx, zz = flz * rt_rf32(M, nrm), d = d1 + zz;
            if (OKF(yy) && OKF(xx) && OKF(d1) && OKF(zz) && OKF(d)) {
                if (!(d > fk0)) lit = 0;
                else {
                    float f = d * fin;
                    if (OKF(f)) { v = f < fk1 ? f : 1.0f; t = v * fk2; if (OKF(t)) lit = 1; }
                }
            }
        }
        if (lit < 0) {                                        /* the exact steps */
            double yy = rt_pc(fcw, ly * (double)rt_rf32(M, nrm - 4)), xx = rt_pc(fcw, lx * (double)rt_rf32(M, nrm - 8));
            double d = rt_pc(fcw, yy + xx);
            d = rt_pc(fcw, d + rt_pc(fcw, lz * (double)rt_rf32(M, nrm)));
            if (!(d > k0)) lit = 0;                           /* fcom: below, equal or unordered */
            else {
                double f = rt_pc(fcw, d * in);
                v = f < k1 ? rt_f32(fcw, f) : 1.0f;           /* (unordered clamps too) */
                t = rt_f32(fcw, rt_pc(fcw, (double)v * k2)); lit = 1;
            }
        }
        if (!lit) { if (check) chk.col[k] = rt_r32(M, dst); continue; }
        last = v; have_last = 1;
        uint32_t out = color_add_scaled(rt_r32(M, dst), src, rt_fist32(c, (double)t));
        if (check) chk.col[k] = out; else rt_w32(G_MEM, dst, out);
    }
    #undef OKF
    if (check) {                       /* let the original run, then compare at 0x544ee5 */
        chk.pending = 1; chk.dst = c->edx + 0xc; chk.n = n; chk.esi = end_esi; chk.edi = end_edi; chk.last = last; chk.have_last = have_last;
        return;
    }
    if (have_last) rt_wf32(G_MEM, ebp - 0x10, last);
    c->ebx = 0; c->esi = end_esi; c->edi = end_edi;
    c->ecx = 0;                        /* the loop's own entry test (test ecx, ecx; jbe 0x544ee5) now skips it */
}
static void light_dir_check(Ctx *c)
{
    if (!chk.pending) return;
    chk.pending = 0;
    static long runs, bad;
    int diff = 0; uint32_t first = 0;
    for (uint32_t k = 0; k < chk.n; k++) if (rt_r32(G_MEM, chk.dst + 0x18 * k) != chk.col[k]) { if (!diff++) first = k; }
    if (chk.have_last && rt_rf32(G_MEM, c->ebp - 0x10) != chk.last) diff++;
    if (!runs++) fprintf(stderr, "native: light loop check running (first run: %u vertices, %s)\n", chk.n, diff ? "differs" : "identical");
    if (diff) { bad++; fprintf(stderr, "native: light loop differs in %d of %u vertices (first %u: original %08x, native %08x)\n", diff, chk.n, first,
                                rt_r32(G_MEM, chk.dst + 0x18 * first), chk.col[first]); }
    if (runs % 20000 == 0) fprintf(stderr, "native: light loop checked %ld runs, %ld differ\n", runs, bad);
}

/* 0x67aeb6 (thiscall, ecx: a 3x3 matrix, rows of 3 floats; out, in): out = (in . row0, in . row1, in . row2), the
 * transform of the terrain's vertices (about a seventh of the game thread, called for every vertex). The x87 order:
 * out0 = (v1 m1 + v0 m0) + v2 m2, out1 = (v1 m4 + v0 m3) + v2 m5, out2 = v2 m8 + (v1 m7 + v0 m6), each product and sum
 * rounded by precision control, then stored as floats. */
#if defined(FNV_EDITION) || defined(SIMS_EDITION) || defined(DF_EDITION)
static void fnv_no_mat3(Ctx *c) { (void)c; }                 /* no native hot loops for Fallout NV (yet) */
#else
void DS2_MAT3_FN(Ctx *c);                                 /* the recompiled original (check mode) */
#endif
static int mat3_bypass; static long mat3_runs, mat3_bad;
static int mat3_apply(Ctx *c)
{
    if (mat3_bypass) return 0;
    const uint8_t *M = G_MEM; uint32_t m = c->ecx, out = rt_r32(M, c->esp + 4), in = rt_r32(M, c->esp + 8), fcw = c->fcw;
    float v0 = rt_rf32(M, in), v1 = rt_rf32(M, in + 4), v2 = rt_rf32(M, in + 8), r[3];
    float m0 = rt_rf32(M, m), m1 = rt_rf32(M, m + 4), m2 = rt_rf32(M, m + 8), m3 = rt_rf32(M, m + 12), m4 = rt_rf32(M, m + 16),
          m5 = rt_rf32(M, m + 20), m6 = rt_rf32(M, m + 24), m7 = rt_rf32(M, m + 28), m8 = rt_rf32(M, m + 32);
    int done = 0;
    if ((fcw & 0x300u) == 0) {                          /* float arithmetic where it rounds exactly as the x87 (see above) */
        #define OKF(x) ((x) == 0.0f || (__builtin_fabsf(x) >= 1.17549435e-38f && __builtin_fabsf(x) <= 3.40282347e+38f))
        float a = v0 * m0, b = v1 * m1, cc = v2 * m2, d = v0 * m3, e = v1 * m4, f = v2 * m5, g = v0 * m6, h = v1 * m7, i = v2 * m8;
        float ab = b + a, x = ab + cc, de = e + d, y = de + f, gh = h + g, z = i + gh;
        if (OKF(a) && OKF(b) && OKF(cc) && OKF(d) && OKF(e) && OKF(f) && OKF(g) && OKF(h) && OKF(i) && OKF(ab) && OKF(x) && OKF(de) && OKF(y) && OKF(gh) && OKF(z)) {
            r[0] = x; r[1] = y; r[2] = z; done = 1;
        }
        #undef OKF
    }
    if (!done) {
        #define P(e) rt_pc(fcw, (e))
        double a = P((double)v0 * m0), b = P((double)v1 * m1), cc = P((double)v2 * m2), d = P((double)v0 * m3), e = P((double)v1 * m4),
               f = P((double)v2 * m5), g = P((double)v0 * m6), h = P((double)v1 * m7), i = P((double)v2 * m8);
        r[0] = rt_f32(fcw, P(P(b + a) + cc)); r[1] = rt_f32(fcw, P(P(e + d) + f)); r[2] = rt_f32(fcw, P(i + P(h + g)));
        #undef P
    }
    if (native_check()) {                               /* run the original here and compare with its result */
        mat3_bypass = 1; DS2_MAT3_FN(c); mat3_bypass = 0; mat3_runs++;
        if (rt_rf32(M, out) != r[0] || rt_rf32(M, out + 4) != r[1] || rt_rf32(M, out + 8) != r[2]) {
            if (mat3_bad++ < 20) fprintf(stderr, "native: matrix differs: original %.9g %.9g %.9g, native %.9g %.9g %.9g\n",
                                         rt_rf32(M, out), rt_rf32(M, out + 4), rt_rf32(M, out + 8), r[0], r[1], r[2]);
        }
        if (mat3_runs % 1000000 == 0) fprintf(stderr, "native: matrix checked %ld calls, %ld differ\n", mat3_runs, mat3_bad);
        return 1;                                       /* the original did the call */
    }
    rt_wf32(G_MEM, out, r[0]); rt_wf32(G_MEM, out + 4, r[1]); rt_wf32(G_MEM, out + 8, r[2]);
    c->eax = in; c->edx = m;                            /* as the original leaves them (ebx is kept; ecx unchanged) */
    c->esp += 12;                                       /* ret 8 */
    return 1;
}

int native_override(Ctx *c, uint32_t addr)
{
    if (!native_on()) return 0;
    switch (addr) {
    case DS2_MAT3: return mat3_apply(c);
    }
    return 0;
}

int native_hook(Ctx *c, uint32_t addr)
{
    if (!native_on()) return 0;
    switch (addr) {
    case DS2_LIGHT_LOOP: light_dir_loop(c); return 1;
    case DS2_LIGHT_AFTER: if (native_check()) light_dir_check(c); return 1;
    }
    return 0;
}
