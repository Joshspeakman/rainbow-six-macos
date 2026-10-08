#!/usr/bin/env python3
"""steamstub21.py <in.exe> <out.exe>

Undo the older Steam wrapper (SteamStub variant 2.1, x86) on the player's own Steam copy at build time, so the
recompiler can read the code. Format as documented by Steamless (atom0s): the stub's start function stores the header
address, its XOR seed and its size in dwords; the XOR-chained header points at an XOR-chained payload, which holds the
XTEA-encrypted SteamDRMP.dll. That DLL's code reads the payload fields it needs (flags, entry point, code section, AES
key/IV, stolen bytes) at offsets that vary between stub builds, so they are read from its instructions. The code
section is AES-256-CBC, its IV ECB-encrypted, and its first 16 bytes of ciphertext held in the payload.
Nothing written here leaves the build folder.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_OP_REG
from pe import PE
from steamstub import aes_decrypt

u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]

def steam_xor(b, key=0):
    b = bytearray(b); o = 0
    if key == 0: key = u32(b, 0); o = 4
    for i in range(o, len(b) & ~3, 4):
        v = u32(b, i); struct.pack_into('<I', b, i, v ^ key); key = v
    return b, key

def xtea_cbc_decrypt(data, keys):
    out = bytearray(data); p1 = p2 = 0x55555555; D = 0x9E3779B9; M = 0xFFFFFFFF
    for x in range(0, len(data) & ~7, 8):
        d1, d2 = struct.unpack_from('<II', data, x); v1, v2 = d1, d2; s = (D * 32) & M
        for _ in range(32):
            v2 = (v2 - ((((v1 << 4) ^ (v1 >> 5)) + v1) ^ (s + keys[(s >> 11) & 3]))) & M
            s = (s - D) & M
            v1 = (v1 - ((((v2 << 4) ^ (v2 >> 5)) + v2) ^ (s + keys[s & 3]))) & M
        struct.pack_into('<II', out, x, v1 ^ p1, v2 ^ p2); p1, p2 = d1, d2
    return out

def find(data, pat):
    toks = pat.split(); n = len(toks)
    fixed = [(i, int(t, 16)) for i, t in enumerate(toks) if t != '??']
    first = fixed[0]
    for o in range(len(data) - n):
        if all(data[o + i] == v for i, v in fixed): return o
    return -1

def unwrap(src, dst):
    pe = PE(src); d = pe.data; eo = pe.off(pe.entry)
    if u32(d, eo - 4) != 0xC0DEC0DE: raise SystemExit(f'{src}: no SteamStub 2.x marker before the entry point')
    md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
    hva = hsize = hkey = 0
    for ins in md.disasm(d[eo:eo + 4096], pe.entry):
        if hva and hsize and hkey: break
        if ins.mnemonic != 'mov' or len(ins.operands) != 2: continue
        a, b = ins.operands
        if a.type == X86_OP_MEM and b.type == X86_OP_IMM:
            if not hva: hva = b.imm & 0xFFFFFFFF
            else: hkey = b.imm & 0xFFFFFFFF
        elif a.type == X86_OP_REG and b.type == X86_OP_IMM: hsize = (b.imm & 0xFFFFFFFF) * 4
    ho = pe.off(hva); hdr, key = steam_xor(d[ho:ho + hsize], hkey)
    if hsize // 4 == 0xD0: f = struct.unpack_from('<12I8s3I', hdr, 0); f = (None,) + f[:5] + f[5:]   # no LoadLibraryW
    else: f = struct.unpack_from('<13I8s3I', hdr, 0)
    (_, _, _, _, _, _, bind_va, _, keymatch, pay_va, pay_size, appid, _, _, drmp_va_o, drmp_size_o, xtea_o) = f
    pay, key = steam_xor(d[pe.off(pay_va):pe.off(pay_va) + pay_size], key)
    drmp_va = u32(pay, drmp_va_o); drmp_size = u32(pay, drmp_size_o)
    keys = [u32(pay, xtea_o + 4 * i) for i in range((len(pay) - xtea_o) // 4)]
    drmp = xtea_cbc_decrypt(d[pe.off(drmp_va):pe.off(drmp_va) + drmp_size], keys)
    fallback = False
    o = find(drmp, '8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8D ?? ?? ?? ?? ?? 05')
    if o < 0: o = find(drmp, '8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B')
    if o < 0:
        o = find(drmp, '8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B')
        fallback = True
    if o < 0: raise SystemExit(f'{src}: SteamDRMP.dll field offsets not found')
    b = drmp[o:o + 128]; i32 = lambda k: struct.unpack_from('<i', b, k)[0]
    offs = [i32(2), i32(14)] + [i32(k) for k in ((25, 36, 47, 61, 72) if fallback else (26, 38, 50, 62, 67))]
    flags_o, appid_o, oep_o, code_va_o, code_size_o, aes_key_o, aes_iv_o = offs
    flags = u32(pay, flags_o); oep = u32(pay, oep_o); code_va = u32(pay, code_va_o)
    out = bytearray(d)
    if not flags & 4:                                    # 4 = NoEncryption
        aes_key = bytes(pay[aes_key_o:aes_key_o + 32]); iv = bytes(pay[aes_iv_o:aes_iv_o + 16])
        stolen = bytes(pay[aes_iv_o + 16:aes_iv_o + 32]); size = u32(pay, code_size_o)
        ra = pe.off(code_va)
        iv = aes_decrypt('ecb', aes_key, iv)
        plain = aes_decrypt('cbc', aes_key, stolen + d[ra:ra + size], iv)
        out[ra:ra + size] = plain[:size]
    else: size = 0
    peo = u32(out, 0x3c)
    struct.pack_into('<I', out, peo + 24 + 16, oep - pe.base)    # AddressOfEntryPoint
    struct.pack_into('<I', out, peo + 24 + 64, 0)                # CheckSum
    open(dst, 'wb').write(out)
    return dict(appid=u32(pay, appid_o), oep=oep, code_va=code_va, code_size=size, flags=flags)

if __name__ == '__main__':
    h = unwrap(sys.argv[1], sys.argv[2])
    print(f'steamstub 2.1: app {h["appid"]}, entry point 0x{h["oep"]:x}, flags 0x{h["flags"]:x}, '
          f'code 0x{h["code_va"]:x} +0x{h["code_size"]:x}')
