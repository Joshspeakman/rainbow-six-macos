"""Conservative discovery of computed branches in the verified BW executable.

This never changes instructions. It follows bounded, constant stack/register
expressions and proposes code entries for the normal lifter. Unknown values
remain unknown, and conditional branches explore both possible paths.
"""
from capstone import x86 as X
from capstone import CS_AC_WRITE


def stack_targets(lifter, start):
    pending = [(start, 0, {}, {})]
    visited, targets = set(), set()
    budget = 512
    while pending and budget:
        pc, sp, regs, memory = pending.pop()
        while budget:
            budget -= 1
            state = (pc, sp, tuple(sorted(regs.items())), tuple(sorted(memory.items())))
            if state in visited or len(memory) > 256 or abs(sp) > 4096:
                break
            visited.add(state)
            i = lifter.ins(pc)
            if i is None:
                break
            mn, ops, nxt = i.mnemonic, i.operands, pc + i.size

            def address(op):
                m = op.mem
                return sp + m.disp if m.base == X.X86_REG_ESP and not m.index and not m.segment else None

            def read_memory(at, size):
                if at is None or any(at + k not in memory for k in range(size)):
                    return None
                return sum(memory[at + k] << (8 * k) for k in range(size))

            def write_memory(at, size, value):
                if at is not None:
                    for k in range(size):
                        if value is None:
                            memory.pop(at + k, None)
                        else:
                            memory[at + k] = (value >> (8 * k)) & 255

            def read(op):
                if op.type == X.X86_OP_IMM:
                    return op.imm & ((1 << (8 * op.size)) - 1)
                if op.type == X.X86_OP_REG:
                    return regs.get(op.reg)
                return read_memory(address(op), op.size)

            def write(op, value):
                if op.type == X.X86_OP_MEM:
                    write_memory(address(op), op.size, value)
                elif op.type == X.X86_OP_REG:
                    # Partial register writes invalidate their containing GPR.
                    name = i.reg_name(op.reg)
                    parent = {'al':'eax','ah':'eax','ax':'eax','bl':'ebx','bh':'ebx','bx':'ebx',
                              'cl':'ecx','ch':'ecx','cx':'ecx','dl':'edx','dh':'edx','dx':'edx',
                              'si':'esi','di':'edi','bp':'ebp','sp':'esp'}.get(name)
                    if parent:
                        regs.pop(getattr(X, 'X86_REG_' + parent.upper()), None)
                        value = None
                    if value is None:
                        regs.pop(op.reg, None)
                    else:
                        regs[op.reg] = value & ((1 << (8 * op.size)) - 1)

            if mn in ('call', 'int', 'int3', 'ud2', 'hlt', 'retf'):
                break
            if mn == 'ret':
                target = read_memory(sp, 4)
                if target is not None:
                    targets.add(target)
                break
            if mn == 'jmp':
                target = read(ops[0])
                if target is None:
                    break
                targets.add(target)
                pc = target
                continue
            if mn.startswith('j') or mn.startswith('loop'):
                if mn.startswith('loop'):
                    regs.pop(X.X86_REG_ECX, None)
                if ops and ops[0].type == X.X86_OP_IMM:
                    pending.append((ops[0].imm & 0xffffffff, sp, regs.copy(), memory.copy()))
            elif mn in ('push', 'pushfd'):
                value = read(ops[0]) if mn == 'push' and ops[0].size == 4 else None
                sp -= 4
                write_memory(sp, 4, value)
            elif mn in ('pop', 'popfd'):
                value = read_memory(sp, 4)
                sp += 4
                if mn == 'pop':
                    if ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP:
                        break
                    write(ops[0], value)
            elif mn == 'lea' and ops[0].reg == X.X86_REG_ESP:
                at = address(ops[1])
                if at is None:
                    break
                sp = at
            elif mn in ('add', 'sub') and ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP:
                amount = read(ops[1])
                if amount is None:
                    break
                if amount >= 0x80000000:
                    amount -= 0x100000000
                sp += amount if mn == 'add' else -amount
            elif mn in ('mov', 'movzx'):
                if ops[0].type == X.X86_OP_REG and ops[0].reg == X.X86_REG_ESP:
                    break
                write(ops[0], read(ops[1]))
            elif mn == 'xchg':
                if any(op.type == X.X86_OP_REG and op.reg == X.X86_REG_ESP for op in ops):
                    break
                a, b = read(ops[0]), read(ops[1])
                write(ops[0], b)
                write(ops[1], a)
            elif mn in ('add', 'sub', 'xor', 'and', 'or'):
                a, b = read(ops[0]), read(ops[1])
                value = None
                if a is not None and b is not None:
                    value = {'add':a+b, 'sub':a-b, 'xor':a^b, 'and':a&b, 'or':a|b}[mn]
                write(ops[0], value)
            elif mn in ('shld', 'shrd') and ops[-1].type == X.X86_OP_IMM and not (ops[-1].imm & 31):
                pass  # common no-op padding in return trampolines
            elif mn not in ('nop', 'cmp', 'test', 'clc', 'stc', 'cmc'):
                _, writes = i.regs_access()
                if X.X86_REG_ESP in writes:
                    break
                for register in writes:
                    regs.pop(register, None)
                if ops and ops[0].access & CS_AC_WRITE:
                    write(ops[0], None)
            pc = nxt
    return {t for t in targets if lifter.TLO <= t < lifter.THI and lifter.code_pointer(t)}
