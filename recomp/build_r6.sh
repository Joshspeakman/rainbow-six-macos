#!/bin/bash
# build_r6.sh <game folder> [outdir]
# Recompile the user's own Tom Clancy's Rainbow Six (GOG 1.04: RainbowSix.exe) with the game-folder DLLs it loads
# (SMACKW32.DLL, Smacker video; VCT32150.dll, the Voxware voice codec) into C and link it with the native runtime
# (tools/build_runtime.sh: Metal on macOS, SDL/OpenGL on Linux). Everything generated stays local (work/ is git-ignored).
#
# Any RainbowSix.exe is accepted, including one with a mod patched in: a new executable PE section (any name) that
# jmp/call hooks in .text reach. tools/analyze.py then covers every executable section (the gaps between them are
# int3), seeds a function at the start of each extra section, follows direct calls into it as functions and direct
# jumps into it (and back) as part of the hooked function, and scans it for pointer tables. Code the mod reaches only
# through pointers it builds at run time can be listed in R6_SEEDS=<file> (hex addresses, one per line). The stock
# GOG executable's SHA-256 is printed; R6_EXE_SHA=<sha> makes the build refuse any other file.
#
# Not lifted: GOG's winmm.dll (a CD-audio-to-Ogg shim) and the libvorbis/libogg DLLs behind it; runtime/win32/winmm.c
# implements the MCI CD-audio commands and plays MUSIC/TrackNN.ogg itself.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$(cd "${1:?usage: build_r6.sh <Rainbow Six folder> [outdir]}" && pwd)"
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi | cut -d' ' -f1; }
EXE_NAME=RainbowSix.exe
STOCK_SHA=$(cat "$HERE/r6_stock.sha256" 2>/dev/null || true)
[ -f "$GAME/$EXE_NAME" ] || { echo "no $EXE_NAME in $GAME"; exit 1; }
GOT=$(sha "$GAME/$EXE_NAME")
if [ -n "${R6_EXE_SHA:-}" ] && [ "$GOT" != "$R6_EXE_SHA" ]; then echo "$EXE_NAME is $GOT, not R6_EXE_SHA"; exit 1; fi
[ "$GOT" = "$STOCK_SHA" ] && echo "$EXE_NAME: stock GOG 1.04" || echo "$EXE_NAME: $GOT (not the stock GOG 1.04 file: patched?)"
OUT="${2:-$HERE/work/r6}"
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
python3 "$HERE/tools/compiler_compat.py" "${CC:-clang}" "$OUT/compiler-flags.txt"
cp "$GAME/$EXE_NAME" "$OUT/$EXE_NAME"; chmod u+w "$OUT/$EXE_NAME"; EXE="$OUT/$EXE_NAME"
if [[ -n "${R6_RELINK:-}" && -d "$OUT/obj" ]]; then              # runtime changes only: keep the lifted objects
  echo "== runtime and link (R6_RELINK)"; exec "$HERE/tools/build_runtime.sh" "$OUT"
fi
echo "== analysis"
"$PY" "$HERE/tools/analyze.py" "$EXE" "$OUT/analysis.json" > "$OUT/analysis.log"
sed -n '/^functions /p; /^seed sources/p; /^decode failures:/p; /^jump tables /p; /^SEH:/p' "$OUT/analysis.log"
echo "== lifting"
"$PY" "$HERE/tools/lift.py" "$EXE" "$OUT/analysis.json" "$OUT" --game r6 | grep -Ev '^unsupported: \{\}$'
# the game's own DLLs, loaded by name: both want 0x10000000, so VCT32150 is lifted from a copy relocated to 0x11000000
# (tools/rebase_pe.py); loader.c applies the same relocations to the original at run time.
NIMP=$("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/image.json")
IMGS=("=")
for spec in SMACKW32.DLL VCT32150.dll@0x11000000; do
  rel=${spec%@*}; tag=$(echo "$rel" | tr . _ | tr A-Z a-z); src="$GAME/$rel"
  [ -f "$src" ] || { echo "missing $src"; exit 1; }
  if [[ $spec == *@* ]]; then "$PY" "$HERE/tools/rebase_pe.py" "$src" "$OUT/$rel" "${spec#*@}"; src="$OUT/$rel"; fi
  "$PY" "$HERE/tools/analyze.py" "$src" "$OUT/$tag.json" | head -1
  "$PY" "$HERE/tools/lift.py" "$src" "$OUT/$tag.json" "$OUT" --tag "$tag" --imp-base "$NIMP" | grep -E 'lifted|internal'
  NIMP=$((NIMP + $("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/${tag}_image.json")))
  IMGS+=("$tag=$rel")
done
"$PY" "$HERE/tools/link_tables.py" "$OUT" "${IMGS[@]}"
echo "== compiling"
rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
JOBS="${R6_BUILD_JOBS:-16}"
printf '%s\n' "$OUT"/lift_*.c "$OUT"/*_lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "$JOBS" "$HERE/tools/cc1.sh" "$OUT"
echo "== $(ls "$OUT/obj" | wc -l | tr -d ' ') object files; runtime and link"
"$HERE/tools/build_runtime.sh" "$OUT"
