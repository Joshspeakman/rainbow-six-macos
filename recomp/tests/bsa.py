"""bsa.py <archive.bsa> list [substring] | extract <substring> <outdir> : Bethesda BSA version 104 (Fallout 3/NV).
For tests that compare what the recompiled game plays or shows with the original assets."""
import sys, os, struct, zlib
def read(path):
    f = open(path, 'rb'); h = f.read(36)
    magic, ver, off, aflags, nfold, nfile, foldlen, filelen, fflags = struct.unpack('<4sIIIIIIII', h)
    assert magic == b'BSA\0' and ver == 104, 'not a v104 BSA'
    f.seek(off); folders = [struct.unpack('<QII', f.read(16)) for _ in range(nfold)]
    entries = []
    for fh, count, foff in folders:
        n = f.read(1)[0]; name = f.read(n).rstrip(b'\0').decode('latin1')
        for _ in range(count):
            h2, size, o = struct.unpack('<QII', f.read(16)); entries.append([name, size, o])
    names = f.read(filelen).split(b'\0')
    for e, n in zip(entries, names): e.append(n.decode('latin1'))
    return f, aflags, entries
def data(f, aflags, size, o):
    comp = bool(aflags & 4) != bool(size & 0x40000000); size &= 0x3fffffff
    f.seek(o); b = f.read(size)
    if aflags & 0x100: n = b[0]; b = b[1 + n:]                       # embedded file name
    return zlib.decompress(b[4:]) if comp else b
if __name__ == '__main__':
    f, af, ents = read(sys.argv[1]); cmd = sys.argv[2]
    pat = sys.argv[3].lower() if len(sys.argv) > 3 else ''
    for folder, size, o, name in ents:
        full = folder + '\\' + name
        if pat not in full.lower(): continue
        if cmd == 'list': print(full, size & 0x3fffffff)
        else:
            out = os.path.join(sys.argv[4], full.replace('\\', '/')); os.makedirs(os.path.dirname(out), exist_ok=True)
            open(out, 'wb').write(data(f, af, size, o))
