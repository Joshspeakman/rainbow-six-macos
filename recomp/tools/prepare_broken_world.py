#!/usr/bin/env python3
"""Resolve the verified BW build's dispatch table in a LOCAL build copy.

Unicorn is used only by the build. No interpreter, Wine or Proton is linked into
the game. Every resolved entry must preserve registers, flags and stack, and
produce the same target and initializer bytes with two different CPU states.
The installed PE copy and lifted code must always come from this same output.
"""
import argparse
import json
import re
import struct
from pathlib import Path

from editions import BROKEN_WORLD_23, digest
from pe import PE


def prepare(source, output):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    from lift import JUNK
    from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                                  UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                                  UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS,
                                  UC_X86_REG_GDTR, UC_X86_REG_CS, UC_X86_REG_SS,
                                  UC_X86_REG_DS, UC_X86_REG_ES)
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise ValueError('Preparation requires a separate build copy.')
    if digest(source) != BROKEN_WORLD_23:
        raise ValueError('This normalizer requires the verified Broken World 2.30.0.4277 executable.')
    pe = PE(source)
    slots = {}
    for _, va, vs, ra, rs, flags in pe.sections:
        if not flags & 0x20000000:
            continue
        for m in re.finditer(rb'(?=\xff[\x15\x25](....))', pe.bytes_at(va, rs), re.S):
            slot = struct.unpack('<I', m[1])[0]
            stub = pe.u32(slot)
            if stub and 0x184a000 <= stub < 0x1874200:
                slots[slot] = stub
    if len(slots) != 2416:
        raise ValueError(f'Unexpected dispatch table: {len(slots)} entries, expected 2416.')
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0, 0x10000)
    # A flat protected-mode GDT lets the original handlers preserve SS with
    # push/pop, just as they do on Windows. Unicorn's null SS cannot be popped.
    uc.mem_write(0x1000, struct.pack('<3Q', 0, 0x00cf9b000000ffff, 0x00cf93000000ffff))
    uc.reg_write(UC_X86_REG_GDTR, (0, 0x1000, 23, 0))
    for reg, value in ((UC_X86_REG_CS, 8), (UC_X86_REG_SS, 16), (UC_X86_REG_DS, 16), (UC_X86_REG_ES, 16)):
        uc.reg_write(reg, value)
    uc.mem_map(pe.base, 0x2000000)
    for _, va, vs, ra, rs, _ in pe.sections:
        uc.mem_write(va, pe.data[ra:ra + rs])
    uc.mem_map(0x30000000, 0x100000)
    uc.mem_map(0x70000000, 0x100000)
    regs = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
            UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS)
    writes = set()
    target, stop_next, slot, previous_end = 0, False, 0, None
    handler_entries = set()

    def code(u, address, size, _):
        nonlocal target, stop_next, previous_end
        if address != previous_end:
            handler_entries.add(address)
        previous_end = address + size
        if stop_next:
            target = address
            u.emu_stop()
        elif address == 0x18e0246:  # verified dispatcher epilogue: ret 4
            stop_next = True
        elif size == 6 and bytes(u.mem_read(address, 6)) == b'\xff\x25' + struct.pack('<I', slot):
            resolved = int.from_bytes(u.mem_read(slot, 4), 'little')
            if resolved != slots[slot]:
                target = resolved
                u.emu_stop()

    def write(u, access, address, size, value, _):
        if pe.base <= address < 0x2400000:
            writes.add((address, size))

    uc.hook_add(UC_HOOK_CODE, code)
    uc.hook_add(UC_HOOK_MEM_WRITE, write)
    result, records = bytearray(pe.data), []
    initialized = {}
    for slot, stub in sorted(slots.items()):
        probes = []
        for variant in range(2):
            values = [0x30001000 + variant * 0x10000 + i * 0x1000 for i in range(6)]
            values += [0x70008000 + variant * 0x20000, 0x70007000 + variant * 0x20000,
                       0x202 if not variant else 0xad7]
            for register, value in zip(regs, values):
                uc.reg_write(register, value)
            uc.mem_write(values[7], struct.pack('<I', 0x401000))
            target, stop_next, writes, previous_end = 0, False, set(), None
            try:
                uc.emu_start(stub, 0, count=10000)
            except UcError as error:
                raise ValueError(f'Could not resolve dispatch slot {slot:#x}: {error}') from error
            if not target or [uc.reg_read(r) for r in regs] != values:
                raise ValueError(f'Dispatch slot {slot:#x} did not preserve the CPU state.')
            changes = {}
            for address, size in sorted(writes):
                value = bytes(uc.mem_read(address, size))
                original = pe.bytes_at(address, size)
                if value != original:
                    if address < 0x1136000:
                        raise ValueError(f'Dispatch slot {slot:#x} altered engine memory.')
                    # The dispatcher's private scratch area is transient. The slot
                    # and its immutable per-function constants are persistent.
                    if address == slot or 0x17fc000 <= address < 0x1800000:
                        for k, byte in enumerate(value):
                            changes[address + k] = byte
                    elif not (0x1b20000 <= address < 0x1b21000 or 0x1a58000 <= address < 0x1a59000):
                        raise ValueError(f'Dispatch slot {slot:#x} altered unexpected memory at {address:#x}.')
                if original:
                    uc.mem_write(address, original)
            probes.append((target, changes))
        if probes[0] != probes[1]:
            raise ValueError(f'Dispatch slot {slot:#x} depends on the incoming CPU state.')
        target, changes = probes[0]
        for address, byte in changes.items():
            if address in initialized and initialized[address] != byte:
                raise ValueError(f'Dispatch initializers disagree at {address:#x}.')
            initialized[address] = byte
            offset = pe.off(address)
            if offset is None:
                raise ValueError(f'Initializer at {address:#x} is outside the image.')
            result[offset] = byte
        struct.pack_into('<I', result, pe.off(slot), target)
        records.append(dict(slot=slot, target=target))
    # This verified constructor has an embedded virtualized function, rather
    # than a pure dispatch wrapper. Observe its handler entries for static
    # discovery; its semantics remain in the compiled original handlers.
    for register, value in zip(regs, values):
        uc.reg_write(register, value)
    uc.mem_write(values[7], struct.pack('<III', 0x18222c3, 0x4010e0, 0))
    uc.reg_write(UC_X86_REG_EAX, 0x1155760)
    target, stop_next, previous_end = 0, False, None
    uc.emu_start(0x1155760, 0, count=20000)
    if not target:
        raise ValueError('Could not discover the embedded function handlers.')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.with_suffix('.native.json').unlink(missing_ok=True)
    output.write_bytes(result)
    from bw_decrypt import decrypt_functions
    result, decrypted = decrypt_functions(output)
    output.write_bytes(result)
    manifest = dict(format=1, edition='broken-world', source_sha256=BROKEN_WORLD_23,
                    sha256=digest(output), dispatch=records, decrypted=decrypted)
    output.with_suffix('.native.json').write_text(json.dumps(manifest, indent=2) + '\n')
    targets = {r['target'] for r in records} | handler_entries | {r['entry'] for r in decrypted}
    prepared = PE(output)
    # The embedded-function VM selects one of 256 encoded opcode handlers.
    # Its dispatch epilogue rotates a table entry right by one (CL=0x41), then
    # adds the VM base. Seed the whole verified table, including handlers that
    # the constructor/wrapper probes never execute.
    vm_handlers = set()
    for index in range(256):
        encoded = prepared.u32(0x1a55000 + 4 * index)
        target = (((encoded >> 1) | ((encoded & 1) << 31)) + 0x1b1f000) & 0xffffffff
        if target and not any(s[5] & 0x20000000 and s[1] <= target < s[1] + s[2] for s in prepared.sections):
            raise ValueError('VM handler table points outside executable code.')
        vm_handlers.add(target)
    if len(vm_handlers) != 256 or 0 not in vm_handlers:
        raise ValueError('Unexpected embedded-function VM handler table.')
    targets.update(vm_handlers - {0})
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    for name, va, vs, ra, rs, flags in prepared.sections:
        if flags & 0x20000000:
            # VM calls also reach ordinary helpers whose addresses occur only
            # in encoded bytecode. Their compiler padding marks entry points.
            # Include single-byte padding in the original .text too. A junk
            # overlapping decode can otherwise hide an ordinary destructor
            # from the analyzer's uncovered-gap sweep.
            padding = rb'\xcc+(?=[^\xcc])' if va < 0x1136000 else rb'\xcc{4,}(?=[^\xcc])'
            for match in re.finditer(padding, prepared.bytes_at(va, rs)):
                entry = va + match.end()
                for count, instruction in enumerate(decoder.disasm(prepared.bytes_at(entry, min(192, rs - match.end())), entry), 1):
                    if instruction.mnemonic in JUNK:
                        break
                    if instruction.mnemonic in ('ret', 'jmp') or count == 12:
                        targets.add(entry)
                        break
        if name == 'absurdum':
            # This short helper section has packed RET-separated functions and
            # INT3 padding. Its entries are selected through computed pointers.
            targets.add(va)
            targets.update(va + match.end() for match in re.finditer(rb'\xcc+(?=[^\xcc])', prepared.bytes_at(va, rs)))
        if flags & 0x20000000:
            # One return trampoline restores FS:[0] and computes the engine
            # epilogue from two image constants. The instruction after its
            # original JMP is junk; seed the computed destination instead.
            pattern = rb'\x52\x9c\xf8\x8b\x15(....)\x13\x15(....)\x87\x54\x24\x04\x9d\xc3'
            for match in re.finditer(pattern, prepared.bytes_at(va, rs), re.S):
                values = [prepared.u32(struct.unpack('<I', match[k])[0]) for k in (1, 2)]
                if any(value is None for value in values):
                    raise ValueError('Return trampoline references a missing image constant.')
                target = sum(values) & 0xffffffff
                if not any(s[5] & 0x20000000 and s[1] <= target < s[1] + s[2] for s in prepared.sections):
                    raise ValueError('Return trampoline targets non-executable memory.')
                targets.add(target)
            # A relocated helper constructs its entry in scratch stack memory.
            pattern = rb'\xc7\x44\x24\xfc(....)\x81\x44\x24\xfc(....)\x81\x44\x24\xfc(....)\xff\x64\x24\xfc'
            for match in re.finditer(pattern, prepared.bytes_at(va, rs), re.S):
                target = sum(struct.unpack('<I', match[k])[0] for k in (1, 2, 3)) & 0xffffffff
                if not any(s[5] & 0x20000000 and s[1] <= target < s[1] + s[2] for s in prepared.sections):
                    raise ValueError('Computed helper entry is outside executable code.')
                targets.update((va + match.start(), target))
            # The VM also selects helpers from a table of CALL-next/XOR/RET
            # thunks. The CALL materializes EIP; entries overlap the preceding
            # thunk's padding, so linear disassembly misses them.
            pattern = rb'\xe8\x00\x00\x00\x00\x81\x74\x24\x08....\x58\x33\x44\x24\x04\xc3'
            targets.update(va + match.start() for match in re.finditer(pattern, prepared.bytes_at(va, rs), re.S))
        if name not in ('.text', '.rld'):
            continue
        for m in re.finditer(rb'(?=\xff[\x15\x25](....))', prepared.bytes_at(va, rs), re.S):
            slot = struct.unpack('<I', m[1])[0]
            if slot in prepared.imports:
                continue  # disk IAT entries are import-name RVAs, not code pointers
            target = prepared.u32(slot)
            if target and (0x401000 <= target < 0xb8e000 or 0x17c4000 <= target < 0x1e8a000):
                targets.add(target)  # already-direct relocated functions also need entries
    (output.parent / 'observed_targets.json').write_text(json.dumps(sorted(targets)) + '\n')
    print(f'Prepared {len(records)} verified dispatch entries and {len(decrypted)} decrypted functions in {output}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        prepare(args.source, args.output)
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
