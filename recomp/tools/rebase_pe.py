"""rebase_pe.py <in.dll> <out.dll> <new base> : a copy of a DLL with its base relocations applied for a new image base
(what Windows' loader does when the preferred address is taken). The recompiled code is lifted from this copy; at run
time loader.c applies the same relocations to the original file from the game folder."""
import sys, struct
src, dst, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 0)
d = bytearray(open(src, 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24
nsec, optsz = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
old = struct.unpack_from('<I', d, opt + 28)[0]; delta = (base - old) & 0xffffffff
secs = [struct.unpack_from('<IIII', d, opt + optsz + 40 * k + 8) for k in range(nsec)]     # vsize, va, rawsize, rawptr
def off(rva):
    for vs, va, rs, ra in secs:
        if va <= rva < va + max(vs, rs): return ra + rva - va if rva - va < rs else None
    return None
rel, rsz = struct.unpack_from('<II', d, opt + 96 + 5 * 8)
if not rel: sys.exit('no relocations')
b, n = 0, 0
while b + 8 <= rsz:
    page, bs = struct.unpack_from('<II', d, off(rel + b))
    if bs < 8: break
    for e in range(8, bs, 2):
        v = struct.unpack_from('<H', d, off(rel + b + e))[0]
        if v >> 12 == 3:
            o = off(page + (v & 0xfff))
            if o is not None: struct.pack_into('<I', d, o, (struct.unpack_from('<I', d, o)[0] + delta) & 0xffffffff); n += 1
    b += bs
struct.pack_into('<I', d, opt + 28, base)
open(dst, 'wb').write(d)
print('%s: %#x -> %#x, %d relocations' % (src.split('/')[-1], old, base, n))
