/* SSE/SSE2/MMX for the recompiled code (included by rt.h; tools/sse.py emits the calls).
 *
 * Every helper takes the destination register and a source that is either another register or a temporary loaded
 * from guest memory (rt_ldx: the operand's bytes, the rest zero). Results are computed into a temporary first, so
 * d and s may be the same register. Float lanes use the host's float/double arithmetic (no fused multiply-add:
 * the generated code is built with -ffp-contract=off). min/max follow the x86 rule: the second operand when either
 * is NaN or both are zero. Known differences from the hardware: rcp/rsqrt are exact instead of 12-bit approximations,
 * MXCSR exception flags are not accumulated, and on arm64 a NaN produced from non-NaN inputs is the host's default
 * NaN (0x7fc00000) rather than x86's 0xffc00000. */
#ifndef RT_SSE_H
#define RT_SSE_H

static inline void rt_ldx(const uint8_t *M, X128 *s, uint32_t a, unsigned n) { memset(s, 0, 16); memcpy(s, M + a, n); }
static inline void rt_stx(uint8_t *M, uint32_t a, const X128 *s, unsigned n) { memcpy(M + a, s, n); }

/* ---- float arithmetic: packed (ps/pd) and lowest lane (ss/sd, the other lanes unchanged) ---- */
#define SSE_ARITH(name, EXPR) \
static inline void sse_##name##ps(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) { float a = d->f[k], b = s->f[k]; r.f[k] = (EXPR); } *d = r; } \
static inline void sse_##name##ss(X128 *d, const X128 *s) { float a = d->f[0], b = s->f[0]; d->f[0] = (EXPR); } \
static inline void sse_##name##pd(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 2; k++) { double a = d->x[k], b = s->x[k]; r.x[k] = (EXPR); } *d = r; } \
static inline void sse_##name##sd(X128 *d, const X128 *s) { double a = d->x[0], b = s->x[0]; d->x[0] = (EXPR); }
SSE_ARITH(add, a + b)
SSE_ARITH(sub, a - b)
SSE_ARITH(mul, a * b)
SSE_ARITH(div, a / b)
SSE_ARITH(min, a < b ? a : b)
SSE_ARITH(max, a > b ? a : b)
#undef SSE_ARITH
/* unary on the source: sqrt, reciprocal, reciprocal square root */
static inline void sse_sqrtps(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) r.f[k] = sqrtf(s->f[k]); *d = r; }
static inline void sse_sqrtss(X128 *d, const X128 *s) { d->f[0] = sqrtf(s->f[0]); }
static inline void sse_sqrtpd(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 2; k++) r.x[k] = sqrt(s->x[k]); *d = r; }
static inline void sse_sqrtsd(X128 *d, const X128 *s) { d->x[0] = sqrt(s->x[0]); }
static inline void sse_rcpps(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) r.f[k] = 1.0f / s->f[k]; *d = r; }
static inline void sse_rcpss(X128 *d, const X128 *s) { d->f[0] = 1.0f / s->f[0]; }
static inline void sse_rsqrtps(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) r.f[k] = 1.0f / sqrtf(s->f[k]); *d = r; }
static inline void sse_rsqrtss(X128 *d, const X128 *s) { d->f[0] = 1.0f / sqrtf(s->f[0]); }

/* ---- bitwise (ps, pd and the integer forms are the same operation) ---- */
static inline void sse_and(X128 *d, const X128 *s)  { d->q[0] &= s->q[0]; d->q[1] &= s->q[1]; }
static inline void sse_andn(X128 *d, const X128 *s) { d->q[0] = ~d->q[0] & s->q[0]; d->q[1] = ~d->q[1] & s->q[1]; }
static inline void sse_or(X128 *d, const X128 *s)   { d->q[0] |= s->q[0]; d->q[1] |= s->q[1]; }
static inline void sse_xor(X128 *d, const X128 *s)  { d->q[0] ^= s->q[0]; d->q[1] ^= s->q[1]; }

/* ---- compares: predicate 0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord ---- */
static inline int sse_pred(int p, double a, double b)
{
    switch (p) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return a == a && b == b;
    }
}
static inline void sse_cmpps(X128 *d, const X128 *s, int p) { X128 r; for (int k = 0; k < 4; k++) r.d[k] = sse_pred(p, d->f[k], s->f[k]) ? ~0u : 0; *d = r; }
static inline void sse_cmpss(X128 *d, const X128 *s, int p) { d->d[0] = sse_pred(p, d->f[0], s->f[0]) ? ~0u : 0; }
static inline void sse_cmppd(X128 *d, const X128 *s, int p) { X128 r; for (int k = 0; k < 2; k++) r.q[k] = sse_pred(p, d->x[k], s->x[k]) ? ~0ull : 0; *d = r; }
static inline void sse_cmpsd(X128 *d, const X128 *s, int p) { d->q[0] = sse_pred(p, d->x[0], s->x[0]) ? ~0ull : 0; }
/* comiss/ucomiss/comisd/ucomisd: ZF PF CF = unordered 111, less 001, equal 100, greater 000; OF SF AF cleared */
static inline uint32_t sse_comi(double a, double b)
{
    if (a != a || b != b) return F_ZF | F_PF | F_CF;
    return a < b ? F_CF : a == b ? F_ZF : 0;
}

/* ---- shuffles and moves between lanes ---- */
static inline void sse_shufps(X128 *d, const X128 *s, int i)
{ X128 r; r.d[0] = d->d[i & 3]; r.d[1] = d->d[(i >> 2) & 3]; r.d[2] = s->d[(i >> 4) & 3]; r.d[3] = s->d[(i >> 6) & 3]; *d = r; }
static inline void sse_shufpd(X128 *d, const X128 *s, int i) { X128 r; r.q[0] = d->q[i & 1]; r.q[1] = s->q[(i >> 1) & 1]; *d = r; }
static inline void sse_pshufd(X128 *d, const X128 *s, int i)
{ X128 r; for (int k = 0; k < 4; k++) r.d[k] = s->d[(i >> (2 * k)) & 3]; *d = r; }
static inline void sse_pshuflw(X128 *d, const X128 *s, int i)
{ X128 r = *s; for (int k = 0; k < 4; k++) r.w[k] = s->w[(i >> (2 * k)) & 3]; *d = r; }
static inline void sse_pshufhw(X128 *d, const X128 *s, int i)
{ X128 r = *s; for (int k = 0; k < 4; k++) r.w[4 + k] = s->w[4 + ((i >> (2 * k)) & 3)]; *d = r; }
static inline void sse_unpcklps(X128 *d, const X128 *s) { X128 r; r.d[0] = d->d[0]; r.d[1] = s->d[0]; r.d[2] = d->d[1]; r.d[3] = s->d[1]; *d = r; }
static inline void sse_unpckhps(X128 *d, const X128 *s) { X128 r; r.d[0] = d->d[2]; r.d[1] = s->d[2]; r.d[2] = d->d[3]; r.d[3] = s->d[3]; *d = r; }
static inline void sse_unpcklpd(X128 *d, const X128 *s) { d->q[1] = s->q[0]; }
static inline void sse_unpckhpd(X128 *d, const X128 *s) { X128 r; r.q[0] = d->q[1]; r.q[1] = s->q[1]; *d = r; }
static inline void sse_movhlps(X128 *d, const X128 *s) { d->q[0] = s->q[1]; }
static inline void sse_movlhps(X128 *d, const X128 *s) { d->q[1] = s->q[0]; }
/* integer interleaves: low or high halves, element size 1/2/4/8 bytes */
static inline void sse_punpck(X128 *d, const X128 *s, int size, int high, int width)   /* width 16 (XMM) or 8 (MMX) */
{
    X128 r = {{0}}; int half = width / 2 / size, base = high ? half : 0;
    for (int k = 0; k < half; k++) {
        memcpy(r.b + 2 * k * size, d->b + (base + k) * size, size);
        memcpy(r.b + (2 * k + 1) * size, s->b + (base + k) * size, size);
    }
    *d = r;
}
static inline uint32_t sse_movmskps(const X128 *s) { uint32_t m = 0; for (int k = 0; k < 4; k++) m |= (s->d[k] >> 31) << k; return m; }
static inline uint32_t sse_movmskpd(const X128 *s) { return (uint32_t)(s->q[0] >> 63) | (uint32_t)(s->q[1] >> 63) << 1; }
static inline uint32_t sse_pmovmskb(const X128 *s, int n) { uint32_t m = 0; for (int k = 0; k < n; k++) m |= (uint32_t)(s->b[k] >> 7) << k; return m; }

/* ---- integer lanes ---- */
#define SSE_LANES(name, field, n, EXPR) \
static inline void sse_##name(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < (n); k++) { __typeof__(d->field[0]) a = d->field[k], b = s->field[k]; r.field[k] = (EXPR); } *d = r; }
SSE_LANES(paddb, b, 16, a + b)  SSE_LANES(paddw, w, 8, a + b)  SSE_LANES(paddd, d, 4, a + b)  SSE_LANES(paddq, q, 2, a + b)
SSE_LANES(psubb, b, 16, a - b)  SSE_LANES(psubw, w, 8, a - b)  SSE_LANES(psubd, d, 4, a - b)  SSE_LANES(psubq, q, 2, a - b)
SSE_LANES(paddusb, b, 16, a + b > 0xff ? 0xff : a + b)    SSE_LANES(paddusw, w, 8, a + b > 0xffff ? 0xffff : a + b)
SSE_LANES(psubusb, b, 16, a > b ? a - b : 0)               SSE_LANES(psubusw, w, 8, a > b ? a - b : 0)
SSE_LANES(paddsw, sw, 8, a + b > 32767 ? 32767 : a + b < -32768 ? -32768 : a + b)
SSE_LANES(psubsw, sw, 8, a - b > 32767 ? 32767 : a - b < -32768 ? -32768 : a - b)
SSE_LANES(pmaxsw, sw, 8, a > b ? a : b)  SSE_LANES(pminsw, sw, 8, a < b ? a : b)
SSE_LANES(pmaxub, b, 16, a > b ? a : b)  SSE_LANES(pminub, b, 16, a < b ? a : b)
SSE_LANES(pavgb, b, 16, (a + b + 1) >> 1) SSE_LANES(pavgw, w, 8, (a + b + 1) >> 1)
SSE_LANES(pcmpeqb, b, 16, a == b ? 0xff : 0)  SSE_LANES(pcmpeqw, w, 8, a == b ? 0xffff : 0)  SSE_LANES(pcmpeqd, d, 4, a == b ? ~0u : 0)
SSE_LANES(pcmpgtb, sb, 16, a > b ? -1 : 0)    SSE_LANES(pcmpgtw, sw, 8, a > b ? -1 : 0)      SSE_LANES(pcmpgtd, sd, 4, a > b ? -1 : 0)
SSE_LANES(pmullw, sw, 8, (int16_t)((int32_t)a * b))
SSE_LANES(pmulhw, sw, 8, (int16_t)(((int32_t)a * b) >> 16))
SSE_LANES(pmulhuw, w, 8, (uint16_t)(((uint32_t)a * b) >> 16))
#undef SSE_LANES
static inline void sse_pmuludq(X128 *d, const X128 *s) { X128 r; r.q[0] = (uint64_t)d->d[0] * s->d[0]; r.q[1] = (uint64_t)d->d[2] * s->d[2]; *d = r; }
static inline void sse_pmaddwd(X128 *d, const X128 *s)
{ X128 r; for (int k = 0; k < 4; k++) r.sd[k] = (int32_t)((uint32_t)(d->sw[2 * k] * s->sw[2 * k]) + (uint32_t)(d->sw[2 * k + 1] * s->sw[2 * k + 1])); *d = r; }
static inline void sse_psadbw(X128 *d, const X128 *s)
{
    X128 r = {{0}};
    for (int h = 0; h < 2; h++) { uint32_t t = 0; for (int k = 0; k < 8; k++) { int v = d->b[8 * h + k] - s->b[8 * h + k]; t += v < 0 ? -v : v; } r.q[h] = t; }
    *d = r;
}
static inline int8_t  sat8(int v)   { return v > 127 ? 127 : v < -128 ? -128 : v; }
static inline uint8_t satu8(int v)  { return v > 255 ? 255 : v < 0 ? 0 : v; }
static inline int16_t sat16(int v)  { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
static inline void sse_packsswb(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 8; k++) { r.sb[k] = sat8(d->sw[k]); r.sb[8 + k] = sat8(s->sw[k]); } *d = r; }
static inline void sse_packuswb(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 8; k++) { r.b[k] = satu8(d->sw[k]); r.b[8 + k] = satu8(s->sw[k]); } *d = r; }
static inline void sse_packssdw(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) { r.sw[k] = sat16(d->sd[k]); r.sw[4 + k] = sat16(s->sd[k]); } *d = r; }
/* the 64-bit MMX forms: the source's results follow the destination's in the low quadword */
static inline void mmx_packsswb(X128 *d, const X128 *s) { X128 r; memset(&r, 0, 16); for (int k = 0; k < 4; k++) { r.sb[k] = sat8(d->sw[k]); r.sb[4 + k] = sat8(s->sw[k]); } *d = r; }
static inline void mmx_packuswb(X128 *d, const X128 *s) { X128 r; memset(&r, 0, 16); for (int k = 0; k < 4; k++) { r.b[k] = satu8(d->sw[k]); r.b[4 + k] = satu8(s->sw[k]); } *d = r; }
static inline void mmx_packssdw(X128 *d, const X128 *s) { X128 r; memset(&r, 0, 16); for (int k = 0; k < 2; k++) { r.sw[k] = sat16(d->sd[k]); r.sw[2 + k] = sat16(s->sd[k]); } *d = r; }
/* shifts by a count (an immediate or the source's low 64 bits): counts past the lane width give 0 (sign for psra) */
static inline void sse_psllw(X128 *d, uint64_t n) { for (int k = 0; k < 8; k++) d->w[k] = n > 15 ? 0 : (uint16_t)(d->w[k] << n); }
static inline void sse_pslld(X128 *d, uint64_t n) { for (int k = 0; k < 4; k++) d->d[k] = n > 31 ? 0 : d->d[k] << n; }
static inline void sse_psllq(X128 *d, uint64_t n) { for (int k = 0; k < 2; k++) d->q[k] = n > 63 ? 0 : d->q[k] << n; }
static inline void sse_psrlw(X128 *d, uint64_t n) { for (int k = 0; k < 8; k++) d->w[k] = n > 15 ? 0 : d->w[k] >> n; }
static inline void sse_psrld(X128 *d, uint64_t n) { for (int k = 0; k < 4; k++) d->d[k] = n > 31 ? 0 : d->d[k] >> n; }
static inline void sse_psrlq(X128 *d, uint64_t n) { for (int k = 0; k < 2; k++) d->q[k] = n > 63 ? 0 : d->q[k] >> n; }
static inline void sse_psraw(X128 *d, uint64_t n) { for (int k = 0; k < 8; k++) d->sw[k] = (int16_t)(d->sw[k] >> (n > 15 ? 15 : n)); }
static inline void sse_psrad(X128 *d, uint64_t n) { for (int k = 0; k < 4; k++) d->sd[k] = d->sd[k] >> (n > 31 ? 31 : n); }
static inline void sse_pslldq(X128 *d, uint64_t n) { X128 r = {{0}}; if (n < 16) memcpy(r.b + n, d->b, 16 - n); *d = r; }
static inline void sse_psrldq(X128 *d, uint64_t n) { X128 r = {{0}}; if (n < 16) memcpy(r.b, d->b + n, 16 - n); *d = r; }

/* ---- conversions; to-integer conversions round per MXCSR.RC (bits 13-14) unless truncating, and give the
 * "integer indefinite" 0x80000000 for NaN or out-of-range values ---- */
static inline double sse_round(uint32_t mxcsr, double v)
{
    switch ((mxcsr >> 13) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline uint32_t sse_cvt_i32(double r) { return (r >= -2147483648.0 && r < 2147483648.0) ? (uint32_t)(int32_t)r : 0x80000000u; }
static inline uint32_t sse_cvtsi(uint32_t mxcsr, double v) { return sse_cvt_i32(sse_round(mxcsr, v)); }
static inline uint32_t sse_cvttsi(double v) { return sse_cvt_i32(trunc(v)); }
static inline void sse_cvtdq2ps(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) r.f[k] = (float)s->sd[k]; *d = r; }
static inline void sse_cvtdq2pd(X128 *d, const X128 *s) { X128 r; r.x[0] = s->sd[0]; r.x[1] = s->sd[1]; *d = r; }
static inline void sse_cvtps2pd(X128 *d, const X128 *s) { X128 r; r.x[0] = s->f[0]; r.x[1] = s->f[1]; *d = r; }
static inline void sse_cvtpd2ps(X128 *d, const X128 *s) { X128 r = {{0}}; r.f[0] = (float)s->x[0]; r.f[1] = (float)s->x[1]; *d = r; }
static inline void sse_cvtss2sd(X128 *d, const X128 *s) { d->x[0] = s->f[0]; }
static inline void sse_cvtsd2ss(X128 *d, const X128 *s) { d->f[0] = (float)s->x[0]; }
static inline void sse_cvttps2dq(X128 *d, const X128 *s) { X128 r; for (int k = 0; k < 4; k++) r.d[k] = sse_cvttsi(s->f[k]); *d = r; }
static inline void sse_cvtps2dq(X128 *d, const X128 *s, uint32_t mx) { X128 r; for (int k = 0; k < 4; k++) r.d[k] = sse_cvtsi(mx, s->f[k]); *d = r; }
static inline void sse_cvttpd2dq(X128 *d, const X128 *s) { X128 r = {{0}}; r.d[0] = sse_cvttsi(s->x[0]); r.d[1] = sse_cvttsi(s->x[1]); *d = r; }
static inline void sse_cvtpd2dq(X128 *d, const X128 *s, uint32_t mx) { X128 r = {{0}}; r.d[0] = sse_cvtsi(mx, s->x[0]); r.d[1] = sse_cvtsi(mx, s->x[1]); *d = r; }

#endif
