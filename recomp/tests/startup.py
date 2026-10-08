"""Start-up differential harness: run DungeonSiege.exe's entry point natively (recompiled) and in Unicorn (original
x86), both on the same Win32 layer, and compare registers + memory hashes at every import call.
  startup.py <libgame.dylib> <game_dir> [max_imports]"""
import sys, os, ctypes, struct, shutil, tempfile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UcError, UC_PROT_ALL, UC_ERR_FETCH_UNMAPPED
from unicorn.x86_const import *
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from harness import Ctx, REGS, UREG, EXE

THUNK_BASE, CB_RET, GDT = 0xfff00000, 0xffeff000, 0x00300000   # GDT outside every hashed region
lib, game_dir = sys.argv[1], sys.argv[2]
os.environ.setdefault('W32_MILES', 'native')      # the executable only: the emulator side does not run the Miles DLLs
work = os.path.join(os.path.dirname(os.path.abspath(lib)), 'startup'); os.makedirs(work, exist_ok=True)
# each side gets a fresh C: drive (the game folder itself is read-only; writes go to an overlay inside C:)
for side in ('drive_c_native', 'drive_c_emu'):
    shutil.rmtree(os.path.join(work, side), ignore_errors=True); os.makedirs(os.path.join(work, side))
drive_c = os.path.join(work, 'drive_c_native')
libA_p, libB_p = os.path.join(work, 'libA.dylib'), os.path.join(work, 'libB.dylib')
shutil.copy(lib, libA_p); shutil.copy(lib, libB_p)
A, B = ctypes.CDLL(libA_p), ctypes.CDLL(libB_p)
for L in (A, B):
    L.w32_harness_prepare.argtypes = [ctypes.c_char_p] * 4 + [ctypes.c_int, ctypes.POINTER(Ctx)]
    L.w32_result.restype = ctypes.c_char_p; L.w32_mem.restype = ctypes.c_void_p
    L.w32_harness_import.argtypes = [ctypes.POINTER(Ctx), ctypes.c_uint32]
trA, trB = os.path.join(work, 'native.trace'), os.path.join(work, 'emu.trace')

# ---- native ----
assert A.w32_harness_prepare(EXE.encode(), game_dir.encode(), drive_c.encode(), trA.encode(), 1, None) == 0
A.w32_harness_run_native()
print('native :', A.w32_result().decode())

# ---- emulated ----
ctx = Ctx()
drive_c = os.path.join(work, 'drive_c_emu')
assert B.w32_harness_prepare(EXE.encode(), game_dir.encode(), drive_c.encode(), trB.encode(), 1, ctypes.byref(ctx)) == 0
mem = B.w32_mem(); entry = B.w32_entry_point()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map_ptr(0x10000, 0xffe00000 - 0x10000, UC_PROT_ALL, mem + 0x10000)
def gdt_entry(base, limit, access, flags):
    return struct.pack('<Q', (limit & 0xffff) | ((base & 0xffffff) << 16) | (access << 40) | (((limit >> 16) & 0xf) << 48) | (flags << 52) | ((base >> 24) << 56))
# ring-0 flat code/data plus an FS segment based at the TEB (SS must be reloaded too once segments are in use)
gdt = gdt_entry(0, 0, 0, 0) + gdt_entry(0, 0xfffff, 0x9b, 0xc) + gdt_entry(0, 0xfffff, 0x93, 0xc) + gdt_entry(ctx.fs_base, 0xfff, 0x93, 0x4)
uc.mem_write(GDT, gdt)
uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0x0))
for r in (UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES): uc.reg_write(r, 0x10)
uc.reg_write(UC_X86_REG_FS, 0x18)
for r in REGS: uc.reg_write(UREG[r], getattr(ctx, r))
uc.reg_write(UC_X86_REG_EFLAGS, 0x202); uc.reg_write(UC_X86_REG_FPCW, 0x027f)
fs_base = ctx.fs_base
state = {'done': None, 'imports': 0}

def run(pc):
    """emulate from pc until execution reaches CB_RET; services import thunks on the way"""
    while True:
        try:
            uc.emu_start(pc, CB_RET)
            return
        except UcError as e:
            pc = uc.reg_read(UC_X86_REG_EIP)
            if e.errno != UC_ERR_FETCH_UNMAPPED or pc < THUNK_BASE:
                state['done'] = 'emulator: %s at %08x' % (e, pc); raise StopIteration
        idx = (pc - THUNK_BASE) // 16
        c = Ctx()
        for r in REGS: setattr(c, r, uc.reg_read(UREG[r]))
        c.fs_base = fs_base
        ret = struct.unpack('<I', uc.mem_read(c.esp, 4))[0]
        st = B.w32_harness_import(ctypes.byref(c), idx)
        state['imports'] += 1
        if st: state['done'] = B.w32_result().decode(); raise StopIteration
        for r in REGS: uc.reg_write(UREG[r], getattr(c, r))
        pc = ret

HOOK = ctypes.CFUNCTYPE(None, ctypes.POINTER(Ctx), ctypes.c_uint32)
def cb(cptr, fn):
    c = cptr.contents
    for r in REGS: uc.reg_write(UREG[r], getattr(c, r))
    run(fn)
    for r in REGS: setattr(c, r, uc.reg_read(UREG[r]))
hook = HOOK(cb); B.w32_set_callback_hook(hook)

# discovery: record targets of indirect calls/jumps (the last instruction of the previous block decides)
import capstone, json
from unicorn import UC_HOOK_BLOCK
DISC = os.environ.get('DISCOVER')
targets = set(); last = [0, 0]; kind_cache = {}
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
def last_is_indirect(addr, size):
    k = (addr, size)
    if k not in kind_cache:
        b = bytes(uc.mem_read(addr, size)); ins = list(md.disasm(b, addr)); r = False
        if ins:
            i = ins[-1]; o = i.op_str
            if i.mnemonic in ('call', 'jmp') and not o.startswith('0x'):
                import re as _re
                m = _re.fullmatch(r'dword ptr \[(0x[0-9a-f]+)\]', o)
                iat = m and 0x720000 <= int(m.group(1), 16) < 0x720620          # import thunk: execution resumes at the return address
                table = '*4' in o and i.mnemonic == 'jmp'                        # jump-table dispatch: internal label
                r = not iat and not table
        kind_cache[k] = r
    return kind_cache[k]
def on_block(uc_, addr, size, _):
    if last[0] and addr != last[0] + last[1] and last_is_indirect(last[0], last[1]) and addr < THUNK_BASE: targets.add(addr)   # (resuming after an import is not a target)
    last[0], last[1] = addr, size
if DISC: uc.hook_add(UC_HOOK_BLOCK, on_block)
try:
    run(entry); state['done'] = 'returned %u' % uc.reg_read(UC_X86_REG_EAX)
except StopIteration: pass
B.w32_trace_close()
if DISC:
    old = set()                                         # rebuilt each run (the run reaches further as the port grows)
    if os.path.exists(DISC) and os.environ.get('DISCOVER_KEEP'): old = set(json.load(open(DISC)))
    json.dump(sorted(old | targets), open(DISC, 'w'))
    print('discovery: %d indirect targets observed (%d new) -> %s' % (len(targets), len(targets - old), DISC))
print('emulator:', state['done'], '(%d imports)' % state['imports'])

# ---- compare traces ----
REC = struct.Struct('<II8IQQII')
def load(p):
    b = open(p, 'rb').read(); return [REC.unpack_from(b, k) for k in range(0, len(b) - REC.size + 1, REC.size)]
ta, tb = load(trA), load(trB)
names = {}
import re
for line in open(os.path.join(os.path.dirname(os.path.abspath(lib)), 'fntab.c')):
    m = re.match(r'\s*\{0x([0-9a-f]+)u, "([^"]+)"\}', line)
    if m: names[len(names)] = m.group(2)
def nm(i): return names.get(i, '#%d' % i)
n = min(len(ta), len(tb)); first = None
for k in range(n):
    a, b = ta[k], tb[k]
    if a[1] != b[1] or a[2:10] != b[2:10] or a[10] != b[10] or a[11] != b[11]: first = k; break
print('trace: native %d imports, emulator %d imports' % (len(ta), len(tb)))
if first is None:
    print('identical over the common %d import calls' % n)
else:
    a, b = ta[first], tb[first]
    print('FIRST DIFFERENCE at import call %d:' % first)
    print('  native  : %-40s regs %s img %016x vm %016x' % (nm(a[1]), ' '.join('%08x' % x for x in a[2:10]), a[10], a[11]))
    print('  emulator: %-40s regs %s img %016x vm %016x' % (nm(b[1]), ' '.join('%08x' % x for x in b[2:10]), b[10], b[11]))
    print('  before  :', ', '.join('%s(ret %08x)' % (nm(r[1]), r[2]) for r in ta[max(0, first - 8):first]))
    print('  emulator after:', ', '.join(nm(r[1]) for r in tb[first:first + 12]))
    print('  native   after:', ', '.join(nm(r[1]) for r in ta[first:first + 12]))
print('last imports:', ', '.join(nm(r[1]) for r in ta[-12:]))
