"""Resolve the verified BW executable's one-time code decryption at build time."""
import re
import struct
from pe import PE


def decrypt_functions(path):
    from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                                  UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_EDI,
                                  UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EFLAGS,
                                  UC_X86_REG_GDTR, UC_X86_REG_CS, UC_X86_REG_SS,
                                  UC_X86_REG_DS, UC_X86_REG_ES)
    pe = PE(path)
    entries = []
    for _, va, _, ra, rs, flags in pe.sections:
        if not flags & 0x20000000:
            continue
        for match in re.finditer(rb'\xe8(....)', pe.data[ra:ra + rs], re.S):
            target = (va + match.end() + struct.unpack('<i', match[1])[0]) & 0xffffffff
            if target == 0x11e1d78:
                entries.append(va + match.start())
    entries.sort()
    if len(entries) != 21 or entries[0] != 0x17ffd7e or entries[-1] != 0x180c1f5:
        raise ValueError('Unexpected Broken World encrypted-function layout.')
    registers = (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX,
                 UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP,
                 UC_X86_REG_EFLAGS)
    result, patches, records = bytearray(pe.data), {}, []
    for index, entry in enumerate(entries):
        upper = entries[index + 1] if index + 1 < len(entries) else 0x1834000
        probes = []
        for variant in range(2):
            uc = Uc(UC_ARCH_X86, UC_MODE_32)
            uc.mem_map(0, 0x10000)
            uc.mem_write(0x1000, struct.pack('<3Q', 0, 0x00cf9b000000ffff, 0x00cf93000000ffff))
            uc.reg_write(UC_X86_REG_GDTR, (0, 0x1000, 23, 0))
            for reg, value in ((UC_X86_REG_CS, 8), (UC_X86_REG_SS, 16),
                               (UC_X86_REG_DS, 16), (UC_X86_REG_ES, 16)):
                uc.reg_write(reg, value)
            uc.mem_map(pe.base, 0x2000000)
            for _, va, _, ra, rs, _ in pe.sections:
                uc.mem_write(va, pe.data[ra:ra + rs])
            uc.mem_map(0x30000000, 0x100000)
            uc.mem_map(0x70000000, 0x100000)
            values = [0x30001000 + variant * 0x10000 + i * 0x1000 for i in range(6)]
            values += [0x70009000 + variant * 0x20000, 0x70008000 + variant * 0x20000,
                       0x202 if not variant else 0xad7]
            for reg, value in zip(registers, values):
                uc.reg_write(reg, value)
            uc.mem_write(values[7], struct.pack('<I', 0x401000))
            visits, writes = 0, set()

            def returned(u, address, size, _):
                nonlocal visits
                visits += 1
                if visits == 2:
                    u.emu_stop()

            def wrote(u, access, address, size, value, _):
                writes.update(range(address, address + size))

            uc.hook_add(UC_HOOK_CODE, returned, begin=entry, end=entry)
            uc.hook_add(UC_HOOK_MEM_WRITE, wrote, begin=pe.base, end=0x23ffffff)
            try:
                uc.emu_start(entry, 0, count=200000000)
            except UcError as error:
                raise ValueError(f'Could not decrypt function {entry:#x}: {error}') from error
            if visits != 2 or [uc.reg_read(r) for r in registers] != values:
                raise ValueError(f'Decryption at {entry:#x} did not preserve the CPU state.')
            changes = {}
            for address in sorted(writes):
                byte = bytes(uc.mem_read(address, 1))
                if byte == pe.bytes_at(address, 1):
                    continue
                if 0x1b20000 <= address < 0x1b21000 or 0x1a58000 <= address < 0x1a59000:
                    continue  # private VM scratch and generated synchronization routines
                marker = (0x1874000 <= address < 0x1881000 and
                          byte == b'\x01' and pe.bytes_at(address, 1) == b'\x00')
                if not (entry <= address < upper or marker):
                    raise ValueError(f'Decryption at {entry:#x} altered unexpected memory {address:#x}.')
                changes[address] = byte[0]
            probes.append(changes)
        if not probes[0] or probes[0] != probes[1]:
            raise ValueError(f'Decryption at {entry:#x} depends on the incoming CPU state.')
        changes = probes[0]
        for address, byte in changes.items():
            if address in patches and patches[address] != byte:
                raise ValueError(f'Decrypted functions disagree at {address:#x}.')
            offset = pe.off(address)
            if offset is None:
                raise ValueError(f'Decrypted byte {address:#x} is outside the image.')
            patches[address] = byte
            result[offset] = byte
        end = max(a for a in changes if entry <= a < upper) + 1
        records.append(dict(entry=entry, end=end))
    return result, records
