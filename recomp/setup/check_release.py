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


def code_samples(path):
    """A few 48-byte stretches of the file's first executable section."""
    d = open(path, "rb").read()
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
