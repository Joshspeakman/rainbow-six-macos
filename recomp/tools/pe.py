"""Minimal PE32 reader for the game executables: sections, imports (IAT address -> 'DLL!name'), image bytes by VA."""
import struct
class PE:
    def __init__(self, path):
        d = self.data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24
        self.base = struct.unpack_from('<I', d, opt + 28)[0]
        self.entry = self.base + struct.unpack_from('<I', d, opt + 16)[0]
        nsec = struct.unpack_from('<H', d, pe + 6)[0]; so = opt + struct.unpack_from('<H', d, pe + 20)[0]
        self.sections = []
        for i in range(nsec):
            name, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, so + 40 * i)
            flags = struct.unpack_from('<I', d, so + 40 * i + 36)[0]
            if not ra: rs = 0                    # Watcom: .bss has a raw size but no file data
            vs = vs or rs                        # Watcom leaves VirtualSize 0
            self.sections.append((name.rstrip(b'\0').decode(), self.base + va, vs, ra, rs, flags))
        self.dirs = [struct.unpack_from('<II', d, opt + 96 + 8 * k) for k in range(16)]
        self.imports = {}
        rva = self.dirs[1][0]; o = self.off(self.base + rva)
        while True:
            ilt, ts, fc, name, iat = struct.unpack_from('<5I', d, o)
            if not name: break
            dll = self.cstr(self.base + name); k = 0
            while True:
                e = struct.unpack_from('<I', d, self.off(self.base + (ilt or iat)) + 4 * k)[0]
                if not e: break
                nm = ('#%d' % (e & 0xffff)) if e & 0x80000000 else self.cstr(self.base + e + 2)
                self.imports[self.base + iat + 4 * k] = dll + '!' + nm; k += 1
            o += 20
    def relocs(self):
        """VAs of the image's 32-bit absolute fixups (base relocation table, type HIGHLOW)."""
        rva, size = self.dirs[5]; out = []
        if not rva: return out
        o = self.off(self.base + rva); end = o + size
        while o + 8 <= end:
            page, bs = struct.unpack_from('<II', self.data, o)
            if bs < 8: break
            for k in range(8, bs, 2):
                e = struct.unpack_from('<H', self.data, o + k)[0]
                if e >> 12 == 3: out.append(self.base + page + (e & 0xfff))
            o += bs
        return out
    def off(self, va):
        for n, sva, vs, ra, rs, fl in self.sections:
            if sva <= va < sva + max(vs, rs): return va - sva + ra if va - sva < rs else None
        return None
    def cstr(self, va):
        o = self.off(va); return self.data[o:self.data.index(b'\0', o)].decode('latin1')
    def section(self, name):
        for s in self.sections:
            if s[0] == name: return s
    def bytes_at(self, va, n):
        o = self.off(va); return self.data[o:o + n] if o is not None else b''
    def u32(self, va):
        b = self.bytes_at(va, 4); return struct.unpack('<I', b)[0] if len(b) == 4 else None

def code_range(pe):
    """(lo, hi, bytes) of the code the recompiler covers: .text for the executable; for DLLs every executable section
    (Mss32.dll keeps its hand-written mixers in a second one, MSSMIXER), with the gaps between them zero-filled."""
    t = pe.section('.text')
    is_dll = struct.unpack_from('<H', pe.data, struct.unpack_from('<I', pe.data, 0x3c)[0] + 22)[0] & 0x2000
    if not is_dll and t and not pe.section('_TEXT') and not pe.section('.rld'): return t[1], t[1] + t[2], pe.bytes_at(t[1], t[2])
    if not is_dll and not pe.section('.rld'):
        # Delta Force: Watcom's code section is AUTO; DF2 adds MASM's _text and _TEXT after .text. Every executable
        # section but the Steam wrapper's .bind, gaps filled with int3.
        ex = [s for s in pe.sections if s[5] & 0x20000000 and s[0] != '.bind']
        lo, hi = min(s[1] for s in ex), max(s[1] + s[2] for s in ex)
        buf = bytearray(b'\xcc' * (hi - lo))
        for s in ex:
            b = pe.bytes_at(s[1], min(s[2], s[4])); buf[s[1] - lo:s[1] - lo + len(b)] = b
        return lo, hi, bytes(buf)
    if not is_dll:
        # BW retains relocated engine code and some virtualized functions. Lift
        # their original x86 handlers too; the native app never needs Unicorn.
        ex = [s for s in pe.sections if s[5] & 0x20000000]
        lo, hi = min(s[1] for s in ex), max(s[1] + s[2] for s in ex)
        buf = bytearray(b'\xcc' * (hi - lo))
        for s in ex:
            b = pe.bytes_at(s[1], min(s[2], s[4])); buf[s[1] - lo:s[1] - lo + len(b)] = b
        return lo, hi, bytes(buf)
    ex = [s for s in pe.sections if s[5] & 0x20000000]
    lo, hi = min(s[1] for s in ex), max(s[1] + s[2] for s in ex)
    buf = bytearray(hi - lo)
    for s in ex:
        b = pe.bytes_at(s[1], min(s[2], s[4])); buf[s[1] - lo:s[1] - lo + len(b)] = b
    return lo, hi, bytes(buf)
