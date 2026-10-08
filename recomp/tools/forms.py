"""forms.py <exe> <analysis.json> : every distinct instruction form (mnemonic + operand shapes) used by the functions."""
import sys, os, json, collections, capstone
from capstone import x86 as X
sys.path.insert(0, os.path.dirname(__file__)); from pe import PE
pe = PE(sys.argv[1]); t = pe.section('.text'); TLO = t[1]; code = pe.bytes_at(TLO, t[2])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
funcs = json.load(open(sys.argv[2]))['funcs']
def shape(i, op):
    if op.type == X.X86_OP_REG:
        n = i.reg_name(op.reg)
        return n if n.startswith('st') or n in ('cl', 'al', 'ax', 'eax', 'dx') or n.startswith('xmm') else 'r%d' % (op.size * 8)
    if op.type == X.X86_OP_IMM: return 'i%d' % (op.size * 8)
    seg = i.reg_name(op.mem.segment) if op.mem.segment else ''
    return (seg + ':' if seg in ('fs', 'gs') else '') + 'm%d' % (op.size * 8)
forms = collections.Counter(); ex = {}
seen = set()
for f in funcs:
    stack = [f]
    while stack:
        va = stack.pop()
        while va not in seen and TLO <= va < TLO + len(code):
            ins = list(md.disasm(code[va - TLO:va - TLO + 16], va, 1))
            if not ins: break
            i = ins[0]; seen.add(va)
            k = (i.mnemonic,) + tuple(shape(i, o) for o in i.operands)
            forms[k] += 1; ex.setdefault(k, '%x %s %s' % (va, i.mnemonic, i.op_str))
            mn = i.mnemonic
            if mn == 'jmp' or mn in ('ret', 'int3', 'hlt'):
                if mn == 'jmp' and i.operands[0].type == X.X86_OP_IMM: stack.append(i.operands[0].imm)
                break
            if mn[0] == 'j' or mn.startswith('loop'): stack.append(i.operands[0].imm)
            va += i.size
for k, n in sorted(forms.items(), key=lambda kv: kv[0]):
    print('%-40s %7d   %s' % (' '.join(k), n, ex[k]))
print(len(forms), 'forms')
