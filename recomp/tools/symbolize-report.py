#!/usr/bin/env python3
"""symbolize-report.py <report.txt> [DungeonSiegeNative binary]: map a crash/hang report's host addresses onto exact x86
instructions (the recompiled code carries `#line <x86 address>` debug info; atos reads it from the build's object
files, so run this on the machine that built the app)."""
import re, subprocess, sys, os
rep = open(sys.argv[1]).read()
binary = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/Applications/Dungeon Siege Native.app/Contents/MacOS/DungeonSiegeNative')
m = re.search(r'HOST ADDRESSES \(load address (0x[0-9a-f]+)', rep)
if not m: sys.exit('no HOST ADDRESSES section')
load = m.group(1)
sec = rep[m.end():].split('\n\n')[0]
for line in sec.splitlines()[1:]:
    if ':' not in line: continue
    name, addrs = line.strip().split(':', 1)
    pcs = addrs.split()
    if not pcs: continue
    q = [pcs[0]] + ['%#x' % (int(p, 16) - 1) for p in pcs[1:]]          # return addresses: the call instruction
    out = subprocess.run(['atos', '-o', binary, '-l', load] + q, capture_output=True, text=True).stdout.splitlines()
    print('[%s]' % name)
    for o in out:
        x = re.search(r'\(x86:(\d+)\)', o)
        fn = o.split(' (in ')[0]
        if x: print('   %-28s x86 %08x' % (fn, int(x.group(1))))
        elif not fn.startswith('0x'): print('   %s' % o)
