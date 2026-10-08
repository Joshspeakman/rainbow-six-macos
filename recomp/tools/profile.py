#!/usr/bin/env python3
"""profile.py <binary> <W32_PROFILE file> [from seconds] [top N] : where the time went, per thread and per function,
counting samples taken from that many seconds after launch on. Samples are 1 ms of CPU each (runtime/win32/prof.c);
the binary must be the one that ran (its load address comes from the file's first record)."""
import sys, struct, subprocess, bisect, collections
binary, path = sys.argv[1], sys.argv[2]
skip = float(sys.argv[3]) if len(sys.argv) > 3 else 0
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
raw = open(path, 'rb').read()
samples = [struct.unpack_from('<IIQ', raw, k) for k in range(0, len(raw) - 15, 16)]
syms = []
for line in subprocess.run(['nm', '-C', '--defined-only', binary], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
assert samples[0][0] == 0xffffffff, 'no address record'
base = samples[0][2] - next(a for a, n in syms if n == 'prof_init')
samples = [(t, ms, r - base) for t, ms, r in samples[1:] if ms >= skip * 1000]
def name(rip):
    i = bisect.bisect_right(addrs, rip) - 1
    return syms[i][1] if i >= 0 else '?%x' % rip
threads = collections.Counter(t for t, _, _ in samples)
print('%d samples (%.1f CPU s)' % (len(samples), len(samples) / 1000))
for t, n in threads.most_common(6): print('  thread %d: %d samples (%.0f%%)' % (t, n, 100 * n / len(samples)))
main = threads.most_common(1)[0][0]
for label, sel in (('busiest thread', [s for s in samples if s[0] == main]), ('all threads', samples)):
    c = collections.Counter(name(r) for _, _, r in sel)
    print('\n%s: %d samples' % (label, len(sel)))
    for f, n in c.most_common(top): print('  %5.1f%%  %s' % (100 * n / len(sel), f))
