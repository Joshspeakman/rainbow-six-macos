"""replay.py <libgame> <snapshot> <function address> : one recorded call, run through the original x86 (Unicorn) and the
recompiled function, from the same entry state; registers and memory compared afterwards.

Record the call with a hook at the function (lift with DS_TRACE_HOOKS=0x<addr>) and W32_SNAP=<addr>[:<call number>]
W32_SNAP_FILE=<path> (runtime/win32/vm.c vm_snapshot). Calls into Windows imports are not replayed: keep to code
that computes (decoders, blitters, renderers)."""
import sys, os, struct, ctypes
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError, UC_PROT_ALL
from unicorn.x86_const import *
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from harness import Ctx, REGS, UREG

lib_p, snap_p, fn = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
d = open(snap_p, 'rb').read()
hdr = struct.unpack_from('<12I', d, 0); assert hdr[0] == 0x4e534644, 'not a snapshot'
regs = dict(zip(('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'), hdr[1:9])); eflags, fs_base, fcw = hdr[9:12]
pages = {}
o = 48
while o + 4100 <= len(d):
    a = struct.unpack_from('<I', d, o)[0]; pages[a] = d[o + 4:o + 4100]; o += 4100
ret = struct.unpack_from('<I', pages[regs['esp'] & ~0xfff], regs['esp'] & 0xfff)[0]
print('%d pages, esp %08x, return to %08x' % (len(pages), regs['esp'], ret))

# ---- original x86 ----
uc = Uc(UC_ARCH_X86, UC_MODE_32)
runs = []
for a in sorted(pages):
    if runs and runs[-1][1] == a: runs[-1][1] = a + 0x1000
    else: runs.append([a, a + 0x1000])
for lo, hi in runs: uc.mem_map(lo, hi - lo, UC_PROT_ALL)
for a, b in pages.items(): uc.mem_write(a, b)
GDT = 0xffe00000
def gdt_entry(base, limit, access, flags):
    return struct.pack('<Q', (limit & 0xffff) | ((base & 0xffffff) << 16) | (access << 40) | (((limit >> 16) & 0xf) << 48) | (flags << 52) | ((base >> 24) << 56))
uc.mem_map(GDT, 0x1000)
uc.mem_write(GDT, gdt_entry(0, 0, 0, 0) + gdt_entry(0, 0xfffff, 0x9b, 0xc) + gdt_entry(0, 0xfffff, 0x93, 0xc) + gdt_entry(fs_base, 0xfff, 0x93, 0x4))
uc.reg_write(UC_X86_REG_GDTR, (0, GDT, 31, 0))
for r in (UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES): uc.reg_write(r, 0x10)
uc.reg_write(UC_X86_REG_FS, 0x18)
for r, v in regs.items(): uc.reg_write(UREG[r], v)
uc.reg_write(UC_X86_REG_EFLAGS, eflags | 2); uc.reg_write(UC_X86_REG_FPCW, fcw)
fault = None
try: uc.emu_start(fn, ret)
except UcError as e: fault = '%s at %08x' % (e, uc.reg_read(UC_X86_REG_EIP))
print('original :', fault or 'returned')

# ---- recompiled ----
lib = ctypes.CDLL(lib_p)
lib.rt_test_mem.restype = ctypes.c_void_p
lib.rt_test_run.argtypes = [ctypes.POINTER(Ctx), ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_char_p)]
G = lib.rt_test_mem()
for a, b in pages.items(): ctypes.memmove(G + a, b, 0x1000)
c = Ctx()
for r, v in regs.items(): setattr(c, r, v)
c.fs_base = fs_base; c.fcw = fcw
pc = ctypes.c_uint32(0); what = ctypes.c_char_p()
rc = lib.rt_test_run(ctypes.byref(c), fn, ctypes.byref(pc), ctypes.byref(what))
print('recompiled:', 'returned' if rc == 0 else 'fault %s at %08x' % (what.value.decode(), pc.value))

# ---- compare ----
bad = ['%s %08x/%08x' % (r, uc.reg_read(UREG[r]), getattr(c, r)) for r in REGS if uc.reg_read(UREG[r]) != getattr(c, r)]
if bad: print('registers (original/recompiled):', ', '.join(bad))
ndiff = 0
for a in sorted(pages):
    u = bytes(uc.mem_read(a, 0x1000)); n = ctypes.string_at(G + a, 0x1000)
    if u == n: continue
    ks = [k for k in range(0x1000) if u[k] != n[k]]
    ndiff += 1
    if ndiff <= 12:
        k = ks[0]
        print('page %08x: %d bytes differ, first at %08x: original %s recompiled %s' % (a, len(ks), a + k, u[k:k + 16].hex(), n[k:k + 16].hex()))
print('%d pages differ' % ndiff if ndiff else 'memory identical')
