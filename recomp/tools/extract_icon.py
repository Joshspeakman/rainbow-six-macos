#!/usr/bin/env python3
"""extract_icon.py <exe> <out.ico> : write the largest icon group from a PE file's resources as a .ico file
(used at install time so the app icon comes from the user's own game, not from this repository)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; opt = pe + 24; nsec = struct.unpack_from('<H', d, pe + 6)[0]
secs = [struct.unpack_from('<8sIIII', d, opt + struct.unpack_from('<H', d, pe + 20)[0] + 40 * i) for i in range(nsec)]
def off(rva):
    for n, vs, va, rs, ra in secs:
        if va <= rva < va + max(vs, rs): return rva - va + ra
rsrc_rva = struct.unpack_from('<I', d, opt + 96 + 2 * 8)[0]; base = off(rsrc_rva)
def entries(o):
    nn, ni = struct.unpack_from('<HH', d, o + 12)
    for k in range(nn + ni):
        name, ptr = struct.unpack_from('<II', d, o + 16 + 8 * k); yield name, ptr
def leaf(ptr):          # follow directories down to the first data entry
    while ptr & 0x80000000: ptr = next(entries(base + (ptr & 0x7fffffff)))[1]
    rva, size = struct.unpack_from('<II', d, base + ptr); return d[off(rva):off(rva) + size]
types = dict(entries(base))
icons = {}
for name, ptr in entries(base + (types[3] & 0x7fffffff)): icons[name] = leaf(ptr)
groups = [leaf(ptr) for name, ptr in entries(base + (types[14] & 0x7fffffff))]
grp = max(groups, key=lambda g: struct.unpack_from('<H', g, 4)[0])
n = struct.unpack_from('<H', grp, 4)[0]; ents = []; blobs = []
for k in range(n):
    w, h, cc, r, planes, bc, size, iid = struct.unpack_from('<BBBBHHIH', grp, 6 + 14 * k)
    data = icons[iid]; ents.append((w, h, cc, r, planes, bc, len(data))); blobs.append(data)
out = struct.pack('<HHH', 0, 1, n); offset = 6 + 16 * n
for (w, h, cc, r, planes, bc, size), data in zip(ents, blobs):
    out += struct.pack('<BBBBHHII', w, h, cc, r, planes, bc, size, offset); offset += size
open(sys.argv[2], 'wb').write(out + b''.join(blobs))
