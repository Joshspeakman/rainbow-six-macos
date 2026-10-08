"""Differential fuzzing of the SSE/SSE2/MMX forms (tools/sse.py): every distinct form the executable uses, run on random
XMM/MMX/register/memory state in Unicorn and as recompiled C; XMM, MMX, registers, defined flags and memory compared.
  fuzz_sse.py <exe> <analysis.json> build   -> work/fuzz_sse/{fuzz.c,libfuzz.so}
  fuzz_sse.py <exe> <analysis.json> run [trials]"""
import sys, os, json, random, struct, subprocess, collections, ctypes
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools')); sys.path.insert(0, HERE)
from capstone import x86 as X
from lift import Lifter, Unsupported
import sse
from harness import *
EXE, ANALYSIS, CMD = sys.argv[1], sys.argv[2], sys.argv[3]
WORK = os.path.join(ROOT, 'work', 'fuzz_sse'); LIB = os.path.join(WORK, 'libfuzz.so')
APPROX = {'rcpps', 'rcpss', 'rsqrtps', 'rsqrtss'}          # hardware approximations: compared to 1.5 * 2^-12

def instances(L, per_form=3):
    seen = {}; visited = set()
    for f in sorted(L.funcs):
        for va, i in L.collect(f).items():
            if va in visited: continue
            visited.add(va); mn = i.mnemonic
            if not sse.is_simd(L, i, mn) or mn.startswith('prefetch'): continue
            if any(o.type == X.X86_OP_MEM and o.mem.segment for o in i.operands): continue
            key = (mn,) + tuple((o.type, o.size) for o in i.operands)
            lst = seen.setdefault(key, [])
            if len(lst) < per_form: lst.append(va)
    return seen

def build():
    L = Lifter(EXE, ANALYSIS); L.jt_cache = {}
    forms = instances(L); os.makedirs(WORK, exist_ok=True)
    vas = []; out = ['#include "rt.h"\n']; unsup = collections.Counter()
    for key, lst in forms.items():
        for va in lst:
            i = L.ins(va)
            try: body = L.emit(i, va, {va: i}, set())
            except Unsupported as e: unsup[str(e)] += 1; continue
            out.append('void t_%08x(Ctx *c) { REGS; %s SPILL; }\n' % (va, body)); vas.append(va)
    vas.sort()
    out.append('const struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[] = {\n')
    out += ['  {0x%08xu, t_%08x},\n' % (v, v) for v in vas]
    out.append('};\nconst unsigned rt_fntab_n = %d;\n' % len(vas))
    out.append('const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[] = {{0, ""}};\nconst unsigned rt_imptab_n = 0;\n')
    out.append('const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[] = {{"", 0, 0, 0}};\nconst unsigned rt_images_n = 0;\n')
    open(os.path.join(WORK, 'fuzz.c'), 'w').write(''.join(out))
    json.dump(vas, open(os.path.join(WORK, 'vas.json'), 'w'))
    subprocess.check_call(['clang', '-O1', '-ffp-contract=off', '-fno-strict-aliasing', '-shared', '-fPIC', '-I', os.path.join(ROOT, 'runtime'),
                           '-o', LIB, os.path.join(ROOT, 'runtime', 'rt.c'), os.path.join(WORK, 'fuzz.c'), '-lm'])
    print('%d forms, %d instances; not lifted: %s' % (len(forms), len(vas), dict(unsup)))

def run(trials):
    H = Harness(LIB, EXE)
    H.lib.rt_test_flags.restype = ctypes.c_uint32; H.lib.rt_test_flags.argtypes = [ctypes.POINTER(Ctx)]
    L = Lifter(EXE, ANALYSIS); vas = json.load(open(os.path.join(WORK, 'vas.json')))
    rng = random.Random(11)
    def rf32():
        k = rng.random()
        if k < 0.1: return struct.pack('<f', rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, float('inf'), float('-inf'), float('nan')]))
        if k < 0.6: return struct.pack('<f', rng.uniform(-1000, 1000))
        if k < 0.75: return struct.pack('<f', float(rng.randint(-1 << 31, 1 << 31)))
        return struct.pack('<I', rng.getrandbits(32))
    def rf64():
        k = rng.random()
        if k < 0.1: return struct.pack('<d', rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, float('inf'), float('nan')]))
        if k < 0.6: return struct.pack('<d', rng.uniform(-1e6, 1e6))
        if k < 0.75: return struct.pack('<d', float(rng.randint(-1 << 31, 1 << 31)))
        return struct.pack('<Q', rng.getrandbits(64))
    def rvec(mn):
        if mn.endswith('pd') or mn.endswith('sd') or 'sd2' in mn or 'pd2' in mn: return rf64() + rf64()
        if rng.random() < 0.7: return b''.join(rf32() for _ in range(4))
        return bytes(rng.getrandbits(8) for _ in range(16))
    stats = collections.Counter(); fails = collections.defaultdict(list)
    for va in vas:
        i = L.ins(va); mn = i.mnemonic
        for t in range(trials):
            H.reset()
            regs = {r: rng.getrandbits(32) if rng.random() < 0.7 else rng.randrange(0, 256) for r in REGS}
            regs['esp'] = STACK + STACK_SZ // 2 + rng.randrange(0, 64) * 4
            for o in i.operands:
                if o.type != X.X86_OP_MEM: continue
                b = i.reg_name(o.mem.base) if o.mem.base else None; x = i.reg_name(o.mem.index) if o.mem.index else None
                if b == 'esp': continue
                tgt = SCRATCH + 0x8000 + rng.randrange(0, 0x20) * 16
                if x and x != b: regs[x] = rng.randrange(0, 16)
                if b:
                    xi = regs[x] * o.mem.scale if x and x != b else 0
                    mult = 1 + (o.mem.scale if x == b else 0)
                    regs[b] = ((tgt - (o.mem.disp & 0xffffffff) - xi) // mult) & 0xffffffff
            scratch = b''.join(rvec(mn) for _ in range(0x40))
            H.write(SCRATCH + 0x8000, scratch)
            stack = b''.join(rvec(mn) for _ in range(0x10)); H.write(regs['esp'] - 0x80, stack + stack)
            xmm = [rvec(mn) for _ in range(8)]; mm = [rng.getrandbits(64) for _ in range(8)]
            fl = 0x202 | (rng.getrandbits(12) & 0x8d5)
            uc = H.uc
            for r in REGS: uc.reg_write(UREG[r], regs[r])
            uc.reg_write(UC_X86_REG_EFLAGS, fl); uc.reg_write(UC_X86_REG_MXCSR, 0x1f80)
            uc.reg_write(UC_X86_REG_FPCW, 0x027f); uc.reg_write(UC_X86_REG_FPSW, 0); uc.reg_write(UC_X86_REG_FPTAG, 0)
            for k in range(8): uc.reg_write(UC_X86_REG_FP0 + k, (mm[k], 0xffff))   # MMX aliases the x87 mantissas
            for k in range(8): uc.reg_write(UC_X86_REG_XMM0 + k, int.from_bytes(xmm[k], 'little'))
            c = Ctx()
            for r in REGS: setattr(c, r, regs[r])
            c.f.op = 0; c.f.eflags = fl; c.fcw = 0x027f; c.mxcsr = 0x1f80
            for k in range(8): ctypes.memmove(c.xmm[k], xmm[k], 16); c.mm[k] = mm[k]
            ufault = None
            try:
                uc.emu_start(va, va + i.size, count=1)
                if uc.reg_read(UC_X86_REG_EIP) != va + i.size: ufault = 'cpu exception'
            except UcError as e: ufault = str(e)
            pc = ctypes.c_uint32(); what = ctypes.c_char_p()
            rc = H.lib.rt_test_run(ctypes.byref(c), va, ctypes.byref(pc), ctypes.byref(what))
            stats['runs'] += 1
            if ufault:
                stats['unicorn fault'] += 1; continue           # e.g. a misaligned movaps (#GP), which the recompiled code allows
            if rc: fails[va].append('recomp fault %s' % what.value); continue
            bad = []
            for r in REGS:
                a, b = uc.reg_read(UREG[r]), getattr(c, r)
                if a != b: bad.append('%s %08x/%08x' % (r, a, b))
            uf, rf_ = uc.reg_read(UC_X86_REG_EFLAGS) & 0x8d5, H.lib.rt_test_flags(ctypes.byref(c)) & 0x8d5
            if uf != rf_: bad.append('flags %03x/%03x' % (uf, rf_))
            for k in range(8):
                u = uc.reg_read(UC_X86_REG_XMM0 + k).to_bytes(16, 'little'); r_ = bytes(c.xmm[k])
                if u == r_: continue
                if mn in APPROX:
                    uv = struct.unpack('<4f', u); rv = struct.unpack('<4f', r_)
                    if all(a == b or (a != a and b != b) or abs(a - b) <= abs(a) * 1.5 / 4096 for a, b in zip(uv, rv)): continue
                bad.append('xmm%d %s/%s' % (k, u.hex(), r_.hex()))
            if any(is_mm for is_mm in (sse.is_mm(L, i, o) for o in i.operands)):
                for k in range(8):
                    u = uc.reg_read(UC_X86_REG_FP0 + k)[0]
                    if u != c.mm[k]: bad.append('mm%d %016x/%016x' % (k, u, c.mm[k]))
            bad += H.diff_memory(2)
            if bad: fails[va].append('; '.join(bad))
    for va, lst in sorted(fails.items()):
        i = L.ins(va)
        print('%08x %-40s %3d/%d  e.g. %s' % (va, i.mnemonic + ' ' + i.op_str, len(lst), trials, lst[0][:300]))
    print('%d runs over %d instances; %d instances with mismatches; %s' % (stats['runs'], len(vas), len(fails), dict(stats)))

if CMD == 'build': build()
else: run(int(sys.argv[4]) if len(sys.argv) > 4 else 40)
