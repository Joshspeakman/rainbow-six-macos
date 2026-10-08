"""Differential tests of the first recompiled routines against the original code in Unicorn.
  test_slice.py <libslice.dylib> [iterations]"""
import sys, os, random, struct
sys.path.insert(0, os.path.dirname(__file__)); from harness import *

H = Harness(sys.argv[1]); N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
rng = random.Random(1)
def rf(scale=100.0):
    k = rng.random()
    if k < 0.05: return 0.0
    if k < 0.08: return -0.0
    if k < 0.12: return float(rng.randint(-4, 4))
    return rng.uniform(-scale, scale)
def fbytes(vals): return struct.pack('<%df' % len(vals), *vals)

def check(name, fn, regs, results, extra=''):
    U, R, uf, rf_, c = H.run(fn, regs)
    bad = []
    if uf or rf_:
        if (uf is None) != (rf_ is None): bad.append('fault: unicorn %s / recomp %s' % (uf, rf_))
    for k in U:
        if U[k] != R[k]: bad.append('%s: unicorn %08x recomp %08x' % (k, U[k], R[k]))
    bad += H.diff_memory()
    results[name] = results.get(name, [0, 0]); results[name][0] += 1
    if bad:
        results[name][1] += 1
        if results[name][1] <= 5: print('MISMATCH', name, extra, '\n   ' + '\n   '.join(bad))

res = {}
for it in range(N):
    # raybox(lo, hi, o, dir, out) cdecl -> al
    H.reset()
    lo = [rf() for _ in range(3)]; hi = [l + abs(rf(50)) for l in lo]
    o = [rf(200) for _ in range(3)]; d = [rf(1) for _ in range(3)]
    if rng.random() < 0.2: o = [(l + h) / 2 for l, h in zip(lo, hi)]
    if rng.random() < 0.1: d = [0.0, 0.0, 1.0]
    base = SCRATCH
    H.write(base, fbytes(lo + hi + o + d + [0, 0, 0]))
    esp = H.push_args([base, base + 12, base + 24, base + 36, base + 48])
    check('raybox', 0x63dd91, {'esp': esp, 'ebp': 0x12345678, 'ebx': 0xabcdef01}, res)

    # cull: fastcall-ish (ecx = camera; args M[9], t[3], half[3]; ret 0xc)
    H.reset()
    cam = [rf(10) for _ in range(0x100 // 4 * 2)]
    M = [rf(1) for _ in range(9)]; t = [rf(50) for _ in range(3)]; hlf = [abs(rf(5)) for _ in range(3)]
    H.write(SCRATCH, fbytes(cam)); H.write(SCRATCH + 0x400, fbytes(M + t + hlf))
    esp = H.push_args([SCRATCH + 0x400, SCRATCH + 0x424, SCRATCH + 0x430])
    check('cull', 0x67953c, {'esp': esp, 'ecx': SCRATCH, 'ebp': 0x1111}, res)

    # slerp(a[4], b[4], t) cdecl
    H.reset()
    a = [rf(1) for _ in range(4)]; b = [rf(1) for _ in range(4)]
    if rng.random() < 0.3: b = [x + rf(0.01) for x in a]
    tt = rng.random()
    H.write(SCRATCH, fbytes(a + b))
    esp = H.push_args([SCRATCH, SCRATCH + 16, struct.unpack('<I', struct.pack('<f', tt))[0]])
    check('slerp', 0x694970, {'esp': esp}, res)

    # color_add(dst*, src, k) cdecl
    H.reset()
    H.write(SCRATCH, struct.pack('<II', rng.getrandbits(32), rng.getrandbits(32)))
    esp = H.push_args([SCRATCH, SCRATCH + 4, rng.randint(0, 255) if rng.random() < 0.9 else rng.getrandbits(32)])
    check('color_add', 0x6789de, {'esp': esp}, res)
for k, (n, bad) in res.items(): print('%-10s %6d runs, %d mismatches' % (k, n, bad))
