"""tracediff.py <workdir> [k] : show both start-up traces around call k (default: first difference)."""
import sys, os, struct, re
REC = struct.Struct('<II8IQQII')
w = sys.argv[1]
def load(p):
    b = open(p, 'rb').read(); return [REC.unpack_from(b, k) for k in range(0, len(b) - REC.size + 1, REC.size)]
names = {}
for line in open(os.path.join(w, '..', 'fntab.c')):
    m = re.match(r'\s*\{0x([0-9a-f]+)u, "([^"]+)"\}', line)
    if m: names[len(names)] = m.group(2).split('!')[1]
A, B = load(os.path.join(w, 'native.trace')), load(os.path.join(w, 'emu.trace'))
k = int(sys.argv[2]) if len(sys.argv) > 2 else next((i for i in range(min(len(A), len(B))) if A[i][1:11] != B[i][1:11]), 0)
for i in range(max(0, k - 8), k + 4):
    for tag, T in (('N', A), ('E', B)):
        if i < len(T):
            r = T[i]; print('%s %5d %-26s from %08x  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x' % (
                tag, i, names.get(r[1], r[1]), r[12], *r[2:10]))
