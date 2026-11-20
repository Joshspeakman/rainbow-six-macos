#!/usr/bin/env python3
"""check_release.py <App.app> <game folder> <game.json>: fails if anything from the game got into a release.

No game files by name, and no file in the app may contain stretches of the game's own executable code."""
import json, os, struct, sys

app, game, cfgp = sys.argv[1:4]
cfg = json.load(open(cfgp))
BAD = (".exe", ".dll", ".mpq", ".lng", ".bik", ".smk", ".wav", ".m3d", ".asi", ".tank", ".dsmap", ".dsres")
ALLOW_LIST = os.path.join("Resources", "setup", "lists")


def ci(folder, rel):
    here = folder
    for part in rel.split("/"):
        names = {n.lower(): n for n in os.listdir(here)}
        here = os.path.join(here, names[part.lower()])
    return here


def macho_text(d):
    """A Mach-O's (thin, 32- or 64-bit, or the first slice of a fat file) __TEXT,__text: (file offset, size), or None."""
    magic = struct.unpack_from(">I", d, 0)[0]
    if magic in (0xcafebabe, 0xcafebabf):                       # fat: the first architecture (its fat_arch offset field)
        off = struct.unpack_from(">I", d, 16)[0]
        t = macho_text(d[off:])
        return (t[0] + off, t[1]) if t else None
    m = struct.unpack_from("<I", d, 0)[0]
    if m not in (0xfeedface, 0xfeedfacf):
        return None
    is64 = m == 0xfeedfacf
    ncmds = struct.unpack_from("<I", d, 16)[0]
    p = 32 if is64 else 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, p)
        if cmd in (0x1, 0x19):                                   # LC_SEGMENT, LC_SEGMENT_64
            nsect = struct.unpack_from("<I", d, p + (64 if is64 else 48))[0]
            q = p + (72 if is64 else 56)
            for _ in range(nsect):
                sect, seg = d[q:q + 16].rstrip(b"\0"), d[q + 16:q + 32].rstrip(b"\0")
                if seg == b"__TEXT" and sect == b"__text":
                    if is64:
                        sz, off = struct.unpack_from("<QI", d, q + 40)
                    else:
                        sz, off = struct.unpack_from("<II", d, q + 36)
                    return off, sz
                q += 80 if is64 else 68
        p += size
    return None


def code_samples(path):
    """A few 48-byte stretches of the file's first executable section (a Windows PE, or a Mac Mach-O: Homeworld)."""
    d = open(path, "rb").read()
    t = macho_text(d)
    if t:
        raw, size = t
        return [d[raw + size * k // 9: raw + size * k // 9 + 48] for k in range(1, 9)]
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        if struct.unpack_from("<I", d, o + 36)[0] & 0x20000000:
            size, raw = struct.unpack_from("<II", d, o + 16)
            return [d[raw + size * k // 9: raw + size * k // 9 + 48] for k in range(1, 9)]
    return []


samples = [s for rel in [cfg["exe"]] + [l["file"] for l in cfg.get("libs", [])] for s in code_samples(ci(game, rel))]
samples = [s for s in samples if len(set(s)) > 12]              # real code, not padding
problems = []
for base, _, names in os.walk(app):
    if os.path.join(base, "").find(ALLOW_LIST) >= 0:
        continue
    for name in names:
        p = os.path.join(base, name)
        if name.lower().endswith(BAD) or name.startswith("lift_") or name == "libgame.dylib" and "stub" not in base:
            problems.append("game file by name: " + p)
            continue
        if os.path.islink(p):
            continue
        d = open(p, "rb").read()
        if any(s in d for s in samples):
            problems.append("contains the game's code: " + p)
for p in problems:
    print("RELEASE CHECK FAILED:", p)
print(f"release check: {len(samples)} code samples, {len(problems)} problems")
sys.exit(1 if problems else 0)
