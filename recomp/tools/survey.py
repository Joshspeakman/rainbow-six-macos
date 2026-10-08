"""survey.py <exe> : first-pass analysis: recursive-descent function discovery from the entry point, direct call
targets and code pointers found in data; coverage of .text; instruction mix; indirect transfer census."""
import sys, collections, capstone
sys.path.insert(0, __import__('os').path.dirname(__file__)); from pe import PE
pe = PE(sys.argv[1]); t = pe.section('.text'); TLO, THI = t[1], t[1] + t[2]
code = pe.bytes_at(TLO, t[2])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
insn_at = {}            # va -> (size, mnemonic, op_str)
def decode(va):
    if va in insn_at: return insn_at[va]
    o = va - TLO
    for i in md.disasm(code[o:o + 16], va, 1):
        insn_at[va] = (i.size, i.mnemonic, i.op_str, i); return insn_at[va]
    insn_at[va] = None; return None
funcs = set(); work = []
def add_func(va):
    if TLO <= va < THI and va not in funcs: funcs.add(va); work.append(va)
add_func(pe.entry)
# code pointers in data sections (vtables, callbacks): aligned dwords pointing into .text
dataptrs = set()
for name, sva, vs, ra, rs, fl in pe.sections:
    if name in ('.rdata', '.data'):
        blob = pe.bytes_at(sva, min(vs, rs))
        for k in range(0, len(blob) - 3, 4):
            v = int.from_bytes(blob[k:k + 4], 'little')
            if TLO <= v < THI: dataptrs.add(v)
for v in dataptrs: add_func(v)
visited = set(); ind_jmp = collections.Counter(); ind_call = collections.Counter(); mix = collections.Counter(); bad = 0
while work:
    f = work.pop(); stack = [f]
    while stack:
        va = stack.pop()
        while TLO <= va < THI and va not in visited:
            r = decode(va)
            if not r: bad += 1; break
            size, mn, ops, ins = r; visited.add(va); mix[mn] += 1
            nxt = va + size
            if mn == 'call':
                if ins.operands[0].type == capstone.x86.X86_OP_IMM: add_func(ins.operands[0].imm)
                else: ind_call['mem-import' if ops.startswith('dword ptr [0x') else 'reg/mem'] += 1
            elif mn == 'jmp':
                if ins.operands[0].type == capstone.x86.X86_OP_IMM: stack.append(ins.operands[0].imm)
                else: ind_jmp['table' if '*4' in ops else ('import-thunk' if ops.startswith('dword ptr [0x') else 'other')] += 1
                break
            elif mn.startswith('j') or mn in ('loop', 'loope', 'loopne', 'jecxz'):
                stack.append(ins.operands[0].imm)
            elif mn in ('ret', 'retf', 'int3', 'hlt', 'ud2'): break
            va = nxt
covered = sum(insn_at[v][0] for v in visited)
print('entry %#x, .text %#x-%#x (%d bytes)' % (pe.entry, TLO, THI, THI - TLO))
print('functions found: %d (data code pointers: %d), instructions: %d, coverage %.1f%% of .text, decode failures %d' % (len(funcs), len(dataptrs), len(visited), 100.0 * covered / (THI - TLO), bad))
print('indirect calls:', dict(ind_call), ' indirect jumps:', dict(ind_jmp))
print('distinct mnemonics: %d' % len(mix)); print(' '.join('%s:%d' % kv for kv in mix.most_common(200)))
