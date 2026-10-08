#!/usr/bin/env python3
"""steamstub.py <in.exe> <out.exe>

Undo the Steam wrapper (SteamStub variant 3.1, x86) on the player's own Steam copy at build time, so the recompiler
can read the code: the stub's header sits just before its entry point in `.bind`, XOR-chained; it holds the AES-256
key and (ECB-encrypted) IV of the CBC-encrypted code section and the first 16 bytes of that section's ciphertext
("stolen" from the file). The output is the same PE with the plain code section and the original entry point.
Nothing written here leaves the build folder (recomp/work is never committed).
"""
import struct, subprocess, sys
from pe import PE

SIG = 0xC0DEC0DF

def header(pe):
    o = pe.off(pe.entry) - 0xF0
    h = bytearray(pe.data[o:o + 0xF0]); key = struct.unpack_from('<I', h, 0)[0]
    for i in range(4, 0xF0, 4):
        v = struct.unpack_from('<I', h, i)[0]; struct.pack_into('<I', h, i, v ^ key); key = v
    if struct.unpack_from('<I', h, 4)[0] != SIG: return None
    f = lambda o: struct.unpack_from('<I', h, o)[0]
    return dict(oep=f(0x20), appid=f(0x38), flags=f(0x3C), code_rva=f(0x48), code_size=f(0x50),
                key=bytes(h[0x58:0x78]), iv=bytes(h[0x78:0x88]), stolen=bytes(h[0x88:0x98]))

def aes_decrypt(mode, key, data, iv=None):
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        m = modes.ECB() if mode == 'ecb' else modes.CBC(iv)
        d = Cipher(algorithms.AES(key), m).decryptor(); return d.update(data) + d.finalize()
    except ImportError:                                  # macOS: LibreSSL's openssl is always there
        cmd = ['openssl', 'enc', '-d', '-nopad', '-aes-256-' + mode, '-K', key.hex()]
        if iv: cmd += ['-iv', iv.hex()]
        return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout

def unwrap(src, dst):
    pe = PE(src); h = header(pe)
    if h is None: raise SystemExit(f'{src}: no SteamStub 3.1 header at the entry point')
    out = bytearray(pe.data)
    if not h['flags'] & 4:                               # 4 = NoEncryption
        name, va, vs, ra, rs, _ = next(s for s in pe.sections if s[1] - pe.base == h['code_rva'])
        assert rs == h['code_size'], (name, rs, h['code_size'])
        iv = aes_decrypt('ecb', h['key'], h['iv'])
        plain = aes_decrypt('cbc', h['key'], h['stolen'] + pe.data[ra:ra + rs], iv)
        out[ra:ra + rs] = plain[:rs]
    peo = struct.unpack_from('<I', out, 0x3c)[0]
    struct.pack_into('<I', out, peo + 24 + 16, h['oep'])          # AddressOfEntryPoint
    struct.pack_into('<I', out, peo + 24 + 64, 0)                 # CheckSum (no longer the file's)
    open(dst, 'wb').write(out)
    return h

if __name__ == '__main__':
    h = unwrap(sys.argv[1], sys.argv[2])
    print(f'steamstub: app {h["appid"]}, entry point 0x{h["oep"]:x}, code section 0x{h["code_size"]:x} bytes')
