"""Differential harness: run original x86 code in Unicorn and the recompiled C (a dylib built from lift.py output)
from identical memory, then compare registers, x87 state and all writable memory."""
import sys, os, ctypes, struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError, UC_PROT_ALL
from unicorn.x86_const import *
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools')); from pe import PE

REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UREG = {'eax': UC_X86_REG_EAX, 'ecx': UC_X86_REG_ECX, 'edx': UC_X86_REG_EDX, 'ebx': UC_X86_REG_EBX,
        'esp': UC_X86_REG_ESP, 'ebp': UC_X86_REG_EBP, 'esi': UC_X86_REG_ESI, 'edi': UC_X86_REG_EDI}
class LFs(ctypes.Structure): _fields_ = [(n, ctypes.c_uint32) for n in ('op', 'a', 'b', 'r', 'cin', 'eflags', 'df')]
class Ctx(ctypes.Structure):
    _fields_ = [(r, ctypes.c_uint32) for r in REGS] + [('f', LFs), ('st', ctypes.c_double * 8), ('top', ctypes.c_uint32),
                ('fcw', ctypes.c_uint32), ('fsw', ctypes.c_uint32), ('fs_base', ctypes.c_uint32), ('tc', ctypes.c_uint32),
                ('mxcsr', ctypes.c_uint32), ('xmm', (ctypes.c_uint8 * 16) * 8), ('mm', ctypes.c_uint64 * 8)]

SENTINEL = 0x00100000            # return address: a mapped page of int3
STACK, STACK_SZ = 0x00200000, 0x00100000
SCRATCH, SCRATCH_SZ = 0x10000000, 0x00100000
EXE = os.environ.get('HARNESS_EXE') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'work', 'DungeonSiege.exe')   # the patched copy build.sh makes

def f80_to_float(b):
    mant, se = struct.unpack('<QH', b[:10]); e = se & 0x7fff; s = -1.0 if se & 0x8000 else 1.0
    if e == 0 and mant == 0: return s * 0.0
    if e == 0x7fff: return float('nan') if (mant << 1) & 0xffffffffffffffff else s * float('inf')
    import math
    try: return s * math.ldexp(mant, e - 16383 - 63)
    except OverflowError: return s * float('inf')

class Harness:
    def __init__(self, lib, exe=EXE):
        self.pe = PE(exe); self.lib = ctypes.CDLL(lib)
        self.lib.rt_test_mem.restype = ctypes.c_void_p
        self.lib.rt_test_run.argtypes = [ctypes.POINTER(Ctx), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_char_p)]
        self.G = self.lib.rt_test_mem()
        lo = self.pe.base; hi = max(s[1] + max(s[2], s[4]) for s in self.pe.sections)
        hi = (hi + 0xfff) & ~0xfff
        self.image = bytearray(hi - lo)
        self.image[0:0x1000] = self.pe.data[:0x1000]
        for n, va, vs, ra, rs, fl in self.pe.sections:
            raw = self.pe.data[ra:ra + min(vs, rs)]; self.image[va - lo:va - lo + len(raw)] = raw
        self.lo = lo
        # writable regions compared after every run
        self.writable = [(va, (max(vs, rs) + 0xfff) & ~0xfff) for n, va, vs, ra, rs, fl in self.pe.sections if fl & 0x80000000]
        self.writable += [(STACK, STACK_SZ), (SCRATCH, SCRATCH_SZ)]
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(lo, len(self.image), UC_PROT_ALL); self.uc.mem_write(lo, bytes(self.image))
        self.uc.mem_map(SENTINEL, 0x1000); self.uc.mem_write(SENTINEL, b'\xcc' * 0x1000)
        self.uc.mem_map(STACK, STACK_SZ); self.uc.mem_map(SCRATCH, SCRATCH_SZ)
        ctypes.memmove(self.G + lo, bytes(self.image), len(self.image))
        ctypes.memmove(self.G + SENTINEL, b'\xcc' * 0x1000, 0x1000)
        self.zero_stack = bytes(STACK_SZ); self.zero_scratch = bytes(SCRATCH_SZ)
    def reset(self):
        for va, sz in self.writable:
            if va >= self.lo and va < self.lo + len(self.image): b = bytes(self.image[va - self.lo:va - self.lo + sz])
            else: b = bytes(sz)
            self.uc.mem_write(va, b); ctypes.memmove(self.G + va, b, sz)
    def write(self, va, b):
        self.uc.mem_write(va, b); ctypes.memmove(self.G + va, b, len(b))
    def run(self, fn, regs, fcw=0x027f, x87=()):
        """regs: dict of initial registers (esp set to the prepared stack). Returns (unicorn_state, recomp_state, fault)"""
        uc = self.uc
        for r in REGS: uc.reg_write(UREG[r], regs.get(r, 0))
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202); uc.reg_write(UC_X86_REG_FPCW, fcw)
        uc.reg_write(UC_X86_REG_FPSW, 0); uc.reg_write(UC_X86_REG_FPTAG, 0xffff)
        c = Ctx()
        for r in REGS: setattr(c, r, regs.get(r, 0))
        c.f.eflags = 0x202; c.fcw = fcw; c.top = 0
        ufault = None
        try: uc.emu_start(fn, SENTINEL, count=50_000_000)
        except UcError as e: ufault = str(e)
        pc = ctypes.c_uint32(); what = ctypes.c_char_p()
        rc = self.lib.rt_test_run(ctypes.byref(c), fn, ctypes.byref(pc), ctypes.byref(what))
        rfault = None if rc == 0 else '%s at %08x' % (what.value.decode(), pc.value)
        U = {r: uc.reg_read(UREG[r]) for r in REGS}; Rr = {r: getattr(c, r) for r in REGS}
        utop = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7
        U['top'] = utop; Rr['top'] = c.top
        return U, Rr, ufault, rfault, c
    def diff_memory(self, limit=8):
        out = []
        for va, sz in self.writable:
            a = bytes(self.uc.mem_read(va, sz)); b = ctypes.string_at(self.G + va, sz)
            if a != b:
                k = next(j for j in range(sz) if a[j] != b[j])
                out.append('memory differs at %08x: unicorn %s recomp %s' % (va + k, a[k:k + 8].hex(), b[k:k + 8].hex()))
                if len(out) >= limit: break
        return out
    def push_args(self, args, ret=SENTINEL):
        esp = STACK + STACK_SZ - 0x1000
        for v in reversed(args): esp -= 4; self.write(esp, struct.pack('<I', v & 0xffffffff))
        esp -= 4; self.write(esp, struct.pack('<I', ret))
        return esp
