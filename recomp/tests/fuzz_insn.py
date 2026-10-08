"""Instruction-level differential fuzzing: real instances of every instruction form used by the game, each run on
random register/flag/x87/memory state in Unicorn and as recompiled C; defined flags, registers, x87 and memory compared.
  fuzz_insn.py build   -> work/fuzz/{fuzz.c,libfuzz.dylib}
  fuzz_insn.py run [trials]"""
import sys, os, json, random, struct, subprocess, collections, math, ctypes
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools')); sys.path.insert(0, HERE)
import capstone
from capstone import x86 as X
from lift import Lifter, Unsupported, JUNK
from harness import *
WORK = os.path.join(ROOT, 'work', 'fuzz')
EXE = os.environ.get('FUZZ_EXE', EXE)                         # another executable (e.g. FalloutNV.exe) and its analysis
ANALYSIS = os.environ.get('FUZZ_ANALYSIS', os.path.join(ROOT, 'work', 'analysis.json'))
SKIP = JUNK | {'jmp', 'call', 'ret', 'loop', 'loope', 'loopne', 'jecxz', 'rdtsc', 'nop', 'enter', 'fnstenv', 'fldenv',
               'fnsave', 'frstor', 'fbstp'}
def is_branch(mn): return mn[0] == 'j'

def instances(L, per_form=4):
    seen = {}; visited = set()
    for f in sorted(L.funcs):
        for va, i in L.collect(f).items():
            if va in visited: continue
            visited.add(va); mn = i.mnemonic
            base = mn.split(' ')[-1]
            if base in SKIP or is_branch(base) or mn.startswith('lock') and 'inc' not in mn and 'dec' not in mn: continue
            if any(o.type == X.X86_OP_MEM and o.mem.segment and i.reg_name(o.mem.segment) == 'fs' for o in i.operands): continue
            if any(o.type == X.X86_OP_REG and i.reg_name(o.reg) in ('es', 'ss', 'cs', 'ds', 'fs', 'gs') for o in i.operands): continue
            key = (mn,) + tuple((o.type, o.size, i.reg_name(o.reg) if o.type == X.X86_OP_REG else 0) for o in i.operands)
            lst = seen.setdefault(key, [])
            if len(lst) < per_form: lst.append(va)
    return seen

def build():
    L = Lifter(EXE, ANALYSIS); L.jt_cache = {}
    forms = instances(L)
    os.makedirs(WORK, exist_ok=True)
    vas = []; out = ['#include "rt.h"\n']
    for key, lst in forms.items():
        for va in lst:
            i = L.ins(va)
            try: body = L.emit(i, va, {va: i}, set())
            except Unsupported: continue
            out.append('void t_%08x(Ctx *c) { REGS; %s SPILL; }\n' % (va, body)); vas.append(va)
    vas.sort()
    out.append('const struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[] = {\n')
    out += ['  {0x%08xu, t_%08x},\n' % (v, v) for v in vas]
    out.append('};\nconst unsigned rt_fntab_n = %d;\n' % len(vas))
    out.append('const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[] = {{0, ""}};\nconst unsigned rt_imptab_n = 0;\n')
    out.append('const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[] = {{"", 0, 0, 0}};\nconst unsigned rt_images_n = 0;\n')
    open(os.path.join(WORK, 'fuzz.c'), 'w').write(''.join(out))
    json.dump(vas, open(os.path.join(WORK, 'vas.json'), 'w'))
    subprocess.check_call(['clang', '-O1', '-ffp-contract=off', '-shared', '-fPIC', '-I', os.path.join(ROOT, 'runtime'),
                           '-o', os.path.join(WORK, 'libfuzz.dylib'), '-fno-strict-aliasing', os.path.join(ROOT, 'runtime', 'rt.c'), os.path.join(WORK, 'fuzz.c'), '-lm'])
    print('%d forms, %d instances' % (len(forms), len(vas)))

# flags that x86 leaves undefined, per mnemonic: these are not compared
UNDEF = {'and': 0x10, 'or': 0x10, 'xor': 0x10, 'test': 0x10, 'mul': 0xd4, 'imul': 0xd4, 'div': 0x8d5, 'idiv': 0x8d5,
         'bsf': 0x8b5, 'bsr': 0x8b5, 'bt': 0x8d4, 'bts': 0x8d4, 'btr': 0x8d4, 'btc': 0x8d4}
FLAGS = 0x8d5
def undefined(mn, i, count):
    m = UNDEF.get(mn, 0)
    if mn in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd'):
        m |= 0x10
        if count != 1: m |= 0x800
        if count == 0: m = 0
    if mn in ('rol', 'ror', 'rcl', 'rcr'):
        m |= 0xd4 if False else 0
        if count != 1: m |= 0x800
    return m

def run(trials):
    H = Harness(os.path.join(WORK, 'libfuzz.dylib'), EXE)
    H.lib.rt_test_flags.restype = ctypes.c_uint32; H.lib.rt_test_flags.argtypes = [ctypes.POINTER(Ctx)]
    L = Lifter(EXE, ANALYSIS)
    vas = json.load(open(os.path.join(WORK, 'vas.json')))
    rng = random.Random(7)
    def rv():
        k = rng.random()
        if k < 0.25: return rng.choice([0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe])
        if k < 0.5: return rng.getrandbits(8)
        return rng.getrandbits(32)
    def rd():
        k = rng.random()
        if k < 0.1: return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0])
        if k < 0.2: return float(rng.randint(-100000, 100000))
        if k < 0.25: return rng.uniform(-1e12, 1e12)
        return rng.uniform(-1000, 1000) * (1 if rng.random() < 0.8 else rng.random() * 1e-6)
    stats = collections.Counter(); fails = collections.defaultdict(list)
    for va in vas:
        i = L.ins(va); mn = i.mnemonic.split(' ')[-1]
        for t in range(trials):
            H.reset()
            regs = {r: rv() for r in REGS}
            regs['esp'] = STACK + STACK_SZ // 2 + rng.randrange(0, 64) * 4
            # memory operands into scratch / stack
            for o in i.operands:
                if o.type != X.X86_OP_MEM: continue
                b = i.reg_name(o.mem.base) if o.mem.base else None; x = i.reg_name(o.mem.index) if o.mem.index else None
                if b == 'esp': continue
                tgt = SCRATCH + 0x8000 + rng.randrange(0, 0x200)
                if x and x != b: regs[x] = rng.randrange(0, 16)
                if b:
                    xi = regs[x] * o.mem.scale if x and x != b else 0
                    mult = 1 + (o.mem.scale if x == b else 0)
                    regs[b] = ((tgt - (o.mem.disp & 0xffffffff) - xi) // mult) & 0xffffffff
                    if b == 'ebp' and mn in ('leave',): pass
            if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsd', 'scasb', 'cmpsb', 'cmpsd', 'cmpsw', 'xlatb') or mn in ('leave', 'popal', 'pushal'):
                regs['esi'] = SCRATCH + 0x4000 + rng.randrange(0, 64); regs['edi'] = SCRATCH + 0x6000 + rng.randrange(0, 64)
                regs['ecx'] = rng.randrange(0, 12); regs['ebx'] = SCRATCH + 0x7000
                if mn in ('scasb', 'cmpsb', 'cmpsd', 'cmpsw') and rng.random() < 0.5:
                    H.write(SCRATCH + 0x4000, bytes(256)); H.write(SCRATCH + 0x6000, bytes(256))
                if mn == 'leave': regs['ebp'] = STACK + STACK_SZ // 2 + 0x40
            if mn in ('div', 'idiv') and rng.random() < 0.7: regs['edx'] = rng.randrange(0, 4) if rng.random() < 0.5 else 0xffffffff
            scratch = bytes(rng.getrandbits(8) for _ in range(0x400))
            H.write(SCRATCH + 0x8000, scratch); H.write(SCRATCH + 0x4000, scratch[:0x100]); H.write(SCRATCH + 0x6000, scratch[0x100:0x200])
            fl = (rng.getrandbits(12) & FLAGS) | 0x202 | (0x400 if rng.random() < 0.2 else 0)
            # x87: six valid registers, two empty
            top = rng.randrange(8); stv = [rd() for _ in range(8)]
            fcw = 0x027f | (rng.randrange(4) << 10 if mn in ('fistp', 'fist', 'frndint') else 0)
            uc = H.uc
            for r in REGS: uc.reg_write(UREG[r], regs[r])
            uc.reg_write(UC_X86_REG_EFLAGS, fl); uc.reg_write(UC_X86_REG_FPCW, fcw)
            uc.reg_write(UC_X86_REG_FPSW, top << 11)
            tag = 0
            for k in range(8):
                phys = (top + k) & 7
                if k >= 6: tag |= 3 << (2 * phys)
            uc.reg_write(UC_X86_REG_FPTAG, tag)
            for k in range(8):
                m, e = math.frexp(stv[k])
                if stv[k] == 0: mant, ex = 0, 0
                else: mant = int(math.ldexp(abs(m), 64)); ex = e - 1 + 16383
                se = ex | (0x8000 if math.copysign(1, stv[k]) < 0 else 0)
                uc.reg_write(UC_X86_REG_FP0 + k, (mant, se))
            c = Ctx()
            for r in REGS: setattr(c, r, regs[r])
            c.f.op = 0; c.f.eflags = fl; c.f.df = 1 if fl & 0x400 else 0
            for k in range(8): c.st[k] = stv[k]
            c.top = top; c.fcw = fcw; c.fsw = 0
            ufault = None
            if mn in ('fsqrt', 'fyl2x', 'fprem', 'fprem1'):          # QEMU mishandles invalid operands of these
                for k in range(2):
                    p = (top + k) & 7; stv[p] = abs(stv[p]) or 1.0; c.st[p] = stv[p]
                    m, e = math.frexp(stv[p]); uc.reg_write(UC_X86_REG_FP0 + p, (int(math.ldexp(m, 64)), e - 1 + 16383))
            try:
                uc.emu_start(va, va + i.size, count=0 if i.mnemonic.startswith('rep') else 1)
                if uc.reg_read(UC_X86_REG_EIP) != va + i.size: ufault = 'cpu exception'
            except UcError as e: ufault = str(e)
            pc = ctypes.c_uint32(); what = ctypes.c_char_p()
            rc = H.lib.rt_test_run(ctypes.byref(c), va, ctypes.byref(pc), ctypes.byref(what))
            stats['runs'] += 1
            if ufault and 'UNMAPPED' in ufault: stats['unmapped'] += 1; continue   # junk decodes / addresses outside the test map
            if ufault or rc:
                if bool(ufault) != bool(rc): fails[va].append('fault: unicorn %s, recomp rc %d' % (ufault, rc))
                continue
            bad = []
            for r in REGS:
                a, b = uc.reg_read(UREG[r]), getattr(c, r)
                if a != b: bad.append('%s %08x/%08x' % (r, a, b))
            cnt = 1
            if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
                co = i.operands[-1] if len(i.operands) > 1 else None
                cnt = 1 if co is None else (co.imm & 31 if co.type == X.X86_OP_IMM else regs['ecx'] & 31)
                if mn in ('rcl', 'rcr'): cnt %= {1: 9, 2: 17, 4: 33}[i.operands[0].size]
            mask = FLAGS & ~undefined(mn, i, cnt) | 0x400
            uf, rf_ = uc.reg_read(UC_X86_REG_EFLAGS) & mask, H.lib.rt_test_flags(ctypes.byref(c)) & mask
            if uf != rf_: bad.append('flags %03x/%03x (xor %03x)' % (uf, rf_, uf ^ rf_))
            usw = uc.reg_read(UC_X86_REG_FPSW); utop = (usw >> 11) & 7
            if utop != c.top: bad.append('top %d/%d' % (utop, c.top))
            ccm = {'fsqrt': 0, 'fyl2x': 0, 'fprem': 0x4700 if not (usw & 0x400) else 0, 'fprem1': 0x4700 if not (usw & 0x400) else 0}.get(mn, 0x4700)
            if mn in ('fprem', 'fprem1') and usw & 0x400: stats['fprem-partial'] += 1; continue
            if (usw & ccm) != (c.fsw & ccm) and mn.startswith('f'): bad.append('fsw cc %04x/%04x' % (usw & 0x4700, c.fsw & 0x4700))
            utag = uc.reg_read(UC_X86_REG_FPTAG)
            if mn.startswith('f'):
                for k in range(8):
                    phys = (c.top + k) & 7
                    if ((utag >> (2 * phys)) & 3) == 3: continue
                    m_, se = uc.reg_read(UC_X86_REG_FP0 + phys)
                    uv = f80_to_float(struct.pack('<QH', m_, se)); rv_ = c.st[phys]
                    if not (uv == rv_ or (uv != uv and rv_ != rv_)) or (uv == 0 and rv_ == 0 and math.copysign(1, uv) != math.copysign(1, rv_)):
                        bad.append('st(%d) %r/%r' % (k, uv, rv_))
            bad += H.diff_memory(2)
            if bad: fails[va].append('; '.join(bad))
    nbad = 0
    for va, lst in sorted(fails.items()):
        i = L.ins(va); nbad += 1
        print('%08x %-32s %3d/%d  e.g. %s' % (va, i.mnemonic + ' ' + i.op_str, len(lst), trials, lst[0]))
    print('%d runs over %d instances; %d instances with mismatches; skipped: %s' % (stats['runs'], len(vas), nbad, dict((k, v) for k, v in stats.items() if k != 'runs')))

if __name__ == '__main__':
    if sys.argv[1] == 'build': build()
    else: run(int(sys.argv[2]) if len(sys.argv) > 2 else 50)
