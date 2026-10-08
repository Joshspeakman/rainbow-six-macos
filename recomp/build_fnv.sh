#!/bin/bash
# build_fnv.sh <FalloutNV.exe> [outdir]
# Recompile the user's own GOG Fallout: New Vegas 1.4.0.525 FalloutNV.exe (and binkw32.dll from its folder) into C and
# link it with the native runtime (tools/build_runtime.sh: the SDL/OpenGL host on Linux). Everything generated stays
# local (work/ is git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="${1:?usage: build_fnv.sh <FalloutNV.exe> [outdir]}"; OUT="${2:-$HERE/work/fnv}"
SHA=89e7020487e80f20ddc92ea641c4433b2c6839a82f45a076e414d167d11dd0fe          # GOG 1.4.0.525 (installer 12010)
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi | cut -d' ' -f1; }
[ "$(sha "$EXE")" = "$SHA" ] || { echo "not the original GOG 1.4.0.525 FalloutNV.exe"; exit 1; }
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
GAME="$(cd "$(dirname "$EXE")" && pwd)"
python3 "$HERE/tools/compiler_compat.py" "${CC:-clang}" "$OUT/compiler-flags.txt"
cp "$EXE" "$OUT/FalloutNV.exe"; EXE="$OUT/FalloutNV.exe"
if [[ -n "${FNV_RELINK:-}" && -d "$OUT/obj" ]]; then            # runtime changes only: keep the lifted objects
  echo "== runtime and link (FNV_RELINK)"; exec "$HERE/tools/build_runtime.sh" "$OUT"
fi
echo "== analysis"
"$PY" "$HERE/tools/analyze.py" "$EXE" "$OUT/analysis.json" > "$OUT/analysis.log"
sed -n '/^functions /p; /^decode failures:/p; /^jump tables /p; /^SEH:/p' "$OUT/analysis.log"
echo "== lifting"
"$PY" "$HERE/tools/lift.py" "$EXE" "$OUT/analysis.json" "$OUT" --game fnv | grep -Ev '^unsupported: \{\}$'
# the game's own DLLs, recompiled into the same image set (as build.sh does for Miles and Bink): Bink video and the
# Ogg Vorbis decoder. Both Vorbis DLLs want 0x10000000, so libvorbisfile is lifted from a copy relocated to 0x11000000
# (tools/rebase_pe.py); loader.c applies the same relocations to the original at run time.
NIMP=$("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/image.json")
IMGS=("=")
for spec in binkw32.dll libvorbis.dll libvorbisfile.dll@0x11000000; do
  rel=${spec%@*}; tag=$(echo "$rel" | tr . _); src="$GAME/$rel"
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
JOBS="${FNV_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
printf '%s\n' "$OUT"/lift_*.c "$OUT"/*_lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "$JOBS" "$HERE/tools/cc1.sh" "$OUT"
echo "== $(ls "$OUT/obj" | wc -l | tr -d ' ') object files; runtime and link"
"$HERE/tools/build_runtime.sh" "$OUT"
