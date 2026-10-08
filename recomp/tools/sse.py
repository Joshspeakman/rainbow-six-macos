"""sse.py: SSE/SSE2/MMX instructions for lift.py (helpers in runtime/rt_sse.h).

XMM registers live in the context (c->xmm[n]) and MMX registers in c->mm[n]; neither is cached in locals, so calls
and SEH need no spilling for them. A memory source is loaded into a temporary of the operand's size (the rest zero)."""
import re
from capstone import x86 as X

class Unsupported(Exception): pass

def regname(L, i, op):
    return i.reg_name(op.reg) if op.type == X.X86_OP_REG else None
def is_xmm(L, i, op): n = regname(L, i, op); return bool(n) and n.startswith('xmm')
def is_mm(L, i, op): n = regname(L, i, op); return bool(n) and n.startswith('mm')

NAMED = {'ldmxcsr', 'stmxcsr', 'emms', 'prefetchnta', 'prefetcht0', 'prefetcht1', 'prefetcht2', 'prefetchw',
         'sfence', 'lfence', 'mfence', 'movnti', 'cvtss2si', 'cvttss2si', 'cvtsd2si', 'cvttsd2si'}
def is_simd(L, i, mn):
    if mn in NAMED: return True
    return any(is_xmm(L, i, o) or is_mm(L, i, o) for o in i.operands)

ARITH = re.compile(r'(add|sub|mul|div|min|max|sqrt|rcp|rsqrt)(ps|ss|pd|sd)$')
CMP = re.compile(r'cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ps|ss|pd|sd)$')
PRED = {'eq': 0, 'lt': 1, 'le': 2, 'unord': 3, 'neq': 4, 'nlt': 5, 'nle': 6, 'ord': 7}
LOGIC = {'andps': 'and', 'andpd': 'and', 'pand': 'and', 'andnps': 'andn', 'andnpd': 'andn', 'pandn': 'andn',
         'orps': 'or', 'orpd': 'or', 'por': 'or', 'xorps': 'xor', 'xorpd': 'xor', 'pxor': 'xor'}
BIN = {'unpcklps', 'unpckhps', 'unpcklpd', 'unpckhpd', 'movhlps', 'movlhps',
       'paddb', 'paddw', 'paddd', 'paddq', 'psubb', 'psubw', 'psubd', 'psubq', 'paddusb', 'paddusw', 'psubusb',
       'psubusw', 'paddsw', 'psubsw', 'pmaxsw', 'pminsw', 'pmaxub', 'pminub', 'pavgb', 'pavgw', 'pcmpeqb', 'pcmpeqw',
       'pcmpeqd', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd', 'pmullw', 'pmulhw', 'pmulhuw', 'pmuludq', 'pmaddwd', 'psadbw',
       'packsswb', 'packuswb', 'packssdw', 'cvtdq2ps', 'cvtdq2pd', 'cvtps2pd', 'cvtpd2ps', 'cvtss2sd', 'cvtsd2ss',
       'cvttps2dq', 'cvttpd2dq'}
BIN_MX = {'cvtps2dq', 'cvtpd2dq'}                     # rounding per MXCSR
BIN_IMM = {'shufps', 'shufpd', 'pshufd', 'pshuflw', 'pshufhw'}
SHIFT = {'psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad', 'pslldq', 'psrldq'}
PUNPCK = re.compile(r'punpck([lh])(bw|wd|dq|qdq)$')
MOVE16 = {'movaps', 'movups', 'movapd', 'movupd', 'movdqa', 'movdqu', 'movntps', 'movntpd', 'movntdq', 'lddqu'}

def emit(L, i, mn):
    o = i.operands

    def src(op, size=None):
        """(prelude, pointer expression) for a source operand"""
        if is_xmm(L, i, op): return '', '&c->xmm[%s]' % regname(L, i, op)[3:]
        if is_mm(L, i, op): return 'X128 s; memset(&s, 0, 16); s.q[0] = c->mm[%s]; ' % regname(L, i, op)[2:], '&s'
        if op.type == X.X86_OP_MEM:
            return 'X128 s; rt_ldx(M, &s, %s, %d); ' % (L.addr(i, op), size or op.size), '&s'
        raise Unsupported('sse source')

    def dst(op):
        """(prelude, pointer, epilogue) for a register destination"""
        if is_xmm(L, i, op): return '', '&c->xmm[%s]' % regname(L, i, op)[3:], ''
        if is_mm(L, i, op):
            n = regname(L, i, op)[2:]
            return 'X128 dm; memset(&dm, 0, 16); dm.q[0] = c->mm[%s]; ' % n, '&dm', ' c->mm[%s] = dm.q[0];' % n
        raise Unsupported('sse destination')

    def op2(call, extra=''):
        dp, dptr, de = dst(o[0]); sp, sptr = src(o[1])
        return '{ %s%s%s(%s, %s%s);%s }' % (dp, sp, call, dptr, sptr, extra, de)

    if mn in ('emms', 'sfence', 'lfence') or mn.startswith('prefetch'): return ';'
    if mn == 'mfence': return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
    if mn == 'ldmxcsr': return 'c->mxcsr = %s;' % L.rd(i, o[0])
    if mn == 'stmxcsr': return L.wr(i, o[0], 'c->mxcsr')
    if mn == 'movnti': return L.wr(i, o[0], L.rd(i, o[1]))

    m = ARITH.match(mn)
    if m: return op2('sse_' + mn)
    m = CMP.match(mn)
    if m: return op2('sse_cmp' + m.group(2), ', %d' % PRED[m.group(1)])
    if mn in LOGIC: return op2('sse_' + LOGIC[mn])
    if mn in BIN: return op2(('mmx_' if mn.startswith('pack') and is_mm(L, i, o[0]) else 'sse_') + mn)   # MMX packs: 64-bit layout
    if mn in BIN_MX: return op2('sse_' + mn, ', c->mxcsr')
    if mn in BIN_IMM: return op2('sse_' + mn, ', %d' % (o[2].imm & 0xff))
    m = PUNPCK.match(mn)
    if m:
        width = 8 if is_mm(L, i, o[0]) else 16
        return op2('sse_punpck', ', %d, %d, %d' % ({'bw': 1, 'wd': 2, 'dq': 4, 'qdq': 8}[m.group(2)], m.group(1) == 'h', width))
    if mn in SHIFT:
        dp, dptr, de = dst(o[0])
        if o[1].type == X.X86_OP_IMM: return '{ %ssse_%s(%s, %du);%s }' % (dp, mn, dptr, o[1].imm & 0xff, de)
        sp, sptr = src(o[1])
        return '{ %s%ssse_%s(%s, (%s)->q[0]);%s }' % (dp, sp, mn, dptr, sptr, de)

    # ---- moves ----
    if mn in MOVE16:
        if o[0].type == X.X86_OP_MEM: return 'rt_stx(M, %s, &c->xmm[%s], 16);' % (L.addr(i, o[0]), regname(L, i, o[1])[3:])
        sp, sptr = src(o[1], 16)
        return '{ %sc->xmm[%s] = *%s; }' % (sp, regname(L, i, o[0])[3:], sptr)
    if mn in ('movss', 'movsd'):
        n = 4 if mn == 'movss' else 8
        if o[0].type == X.X86_OP_MEM: return 'rt_stx(M, %s, &c->xmm[%s], %d);' % (L.addr(i, o[0]), regname(L, i, o[1])[3:], n)
        d = regname(L, i, o[0])[3:]
        if o[1].type == X.X86_OP_MEM: return 'rt_ldx(M, &c->xmm[%s], %s, %d);' % (d, L.addr(i, o[1]), n)
        return 'memcpy(&c->xmm[%s], &c->xmm[%s], %d);' % (d, regname(L, i, o[1])[3:], n)
    if mn in ('movq', 'movd'):
        n = 8 if mn == 'movq' else 4
        a, b = o
        def rd64(op):                   # the operand's low n bytes as a uint64_t expression
            if is_xmm(L, i, op): return 'c->xmm[%s].q[0]' % regname(L, i, op)[3:]
            if is_mm(L, i, op): return 'c->mm[%s]' % regname(L, i, op)[2:]
            if op.type == X.X86_OP_MEM: return ('R64(%s)' if n == 8 else '(uint64_t)R32(%s)') % L.addr(i, op)
            return '(uint64_t)%s' % L.rd(i, op)
        v = rd64(b)
        if n == 4: v = '(%s & 0xffffffffull)' % v
        if is_xmm(L, i, a): return '{ uint64_t v = %s; X128 *x = &c->xmm[%s]; x->q[0] = v; x->q[1] = 0; }' % (v, regname(L, i, a)[3:])
        if is_mm(L, i, a): return 'c->mm[%s] = %s;' % (regname(L, i, a)[2:], v)
        if a.type == X.X86_OP_MEM: return ('W64(%s, %s);' if n == 8 else 'W32(%s, (uint32_t)%s);') % (L.addr(i, a), v)
        return L.wr(i, a, '(uint32_t)%s' % v)
    if mn in ('movlps', 'movlpd', 'movhps', 'movhpd'):
        h = 1 if mn[3] == 'h' else 0
        if o[0].type == X.X86_OP_MEM: return 'W64(%s, c->xmm[%s].q[%d]);' % (L.addr(i, o[0]), regname(L, i, o[1])[3:], h)
        return 'c->xmm[%s].q[%d] = R64(%s);' % (regname(L, i, o[0])[3:], h, L.addr(i, o[1]))
    if mn in ('movmskps', 'movmskpd'): return L.wr(i, o[0], 'sse_%s(&c->xmm[%s])' % (mn, regname(L, i, o[1])[3:]))
    if mn == 'pmovmskb':
        if is_mm(L, i, o[1]): return '{ X128 s; memset(&s, 0, 16); s.q[0] = c->mm[%s]; %s }' % (regname(L, i, o[1])[2:], L.wr(i, o[0], 'sse_pmovmskb(&s, 8)'))
        return L.wr(i, o[0], 'sse_pmovmskb(&c->xmm[%s], 16)' % regname(L, i, o[1])[3:])
    if mn == 'pextrw':
        if not is_xmm(L, i, o[1]): raise Unsupported('mmx pextrw')
        return L.wr(i, o[0], 'c->xmm[%s].w[%d]' % (regname(L, i, o[1])[3:], o[2].imm & 7))
    if mn == 'pinsrw':
        if not is_xmm(L, i, o[0]): raise Unsupported('mmx pinsrw')
        v = L.rd(i, o[1], 2) if o[1].type == X.X86_OP_MEM else L.rd(i, o[1])
        return 'c->xmm[%s].w[%d] = (uint16_t)(%s);' % (regname(L, i, o[0])[3:], o[2].imm & 7, v)
    if mn in ('cvtsi2ss', 'cvtsi2sd'):
        d = regname(L, i, o[0])[3:]
        v = '(int32_t)%s' % L.rd(i, o[1])
        return ('c->xmm[%s].f[0] = (float)%s;' if mn == 'cvtsi2ss' else 'c->xmm[%s].x[0] = (double)%s;') % (d, v)
    if mn in ('cvtss2si', 'cvttss2si', 'cvtsd2si', 'cvttsd2si'):
        sp, sptr = src(o[1], 4 if 'ss' in mn else 8)
        v = '(%s)->%s[0]' % (sptr, 'f' if 'ss' in mn else 'x')
        conv = 'sse_cvttsi(%s)' % v if mn.startswith('cvtt') else 'sse_cvtsi(c->mxcsr, %s)' % v
        return '{ %s%s }' % (sp, L.wr(i, o[0], conv))
    if mn in ('comiss', 'ucomiss', 'comisd', 'ucomisd'):
        sp, sptr = src(o[1]); lane = 'f' if mn.endswith('ss') else 'x'
        return '{ %sflags_set(&f, sse_comi(c->xmm[%s].%s[0], (%s)->%s[0]), F_ZF | F_PF | F_CF | F_OF | F_SF | F_AF); }' % (
            sp, regname(L, i, o[0])[3:], lane, sptr, lane)
    raise Unsupported(mn)
