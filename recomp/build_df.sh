#!/bin/bash
# build_df.sh <game folder> [outdir]
# Recompile the user's own Steam copy of Delta Force (app 32620: Df.exe) or Delta Force 2 (app 32630: Df2.exe), with
# the game-folder DLLs it loads (Cmprssh0.dll, the archive decompressor; Netsock.dll, LAN play), into C and link it
# with the native runtime (tools/build_runtime.sh: Metal on macOS, SDL/OpenGL on Linux). The Steam wrapper is undone
# on a copy in the build folder (tools/steamstub21.py); everything generated stays local (work/ is git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$(cd "${1:?usage: build_df.sh <Delta Force or Delta Force 2 folder> [outdir]}" && pwd)"
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi | cut -d' ' -f1; }
if [ -f "$GAME/Df2.exe" ]; then
  G=df2; EXE_NAME=Df2.exe; SHA=0765acdff5fc7eb077959179b0e980743287fc962b20ccac502dd94f272aa717   # Steam 32630, build 19312
else
  G=df1; EXE_NAME=Df.exe; SHA=307bd0956f9b05f690b872b9a2823020a66b42eb22519f6d2418cf6cfe5fd98c    # Steam 32620, build 19311
fi
[ -f "$GAME/$EXE_NAME" ] || { echo "no Df.exe or Df2.exe in $GAME"; exit 1; }
[ "$(sha "$GAME/$EXE_NAME")" = "$SHA" ] || { echo "not the supported Steam $EXE_NAME"; exit 1; }
OUT="${2:-$HERE/work/$G}"
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
python3 "$HERE/tools/compiler_compat.py" "${CC:-clang}" "$OUT/compiler-flags.txt"
"$PY" "$HERE/tools/steamstub21.py" "$GAME/$EXE_NAME" "$OUT/$EXE_NAME"; EXE="$OUT/$EXE_NAME"
if [[ -n "${DF_RELINK:-}" && -d "$OUT/obj" ]]; then              # runtime changes only: keep the lifted objects
  echo "== runtime and link (DF_RELINK)"; exec "$HERE/tools/build_runtime.sh" "$OUT"
fi
echo "== analysis"
"$PY" "$HERE/tools/analyze.py" "$EXE" "$OUT/analysis.json" > "$OUT/analysis.log"
sed -n '/^functions /p; /^decode failures:/p; /^jump tables /p; /^SEH:/p' "$OUT/analysis.log"
echo "== lifting"
"$PY" "$HERE/tools/lift.py" "$EXE" "$OUT/analysis.json" "$OUT" --game "$G" | grep -Ev '^unsupported: \{\}$'
# the game's own DLLs, loaded by name: all want 0x10000000, so Cmprssh0 and Hw_d3d are lifted from copies relocated to
# 0x11000000 and 0x12000000
# (tools/rebase_pe.py); loader.c applies the same relocations to the originals at run time.
NIMP=$("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/image.json")
IMGS=("=")
DLLS="Netsock.dll Cmprssh0.dll@0x11000000"
[ "$G" = df2 ] && DLLS="$DLLS Hw_d3d.dll@0x12000000"   # DF2's Direct3D renderer (DirectX 6), loaded when the hardware driver is chosen
for spec in $DLLS; do
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
JOBS="${DF_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
printf '%s\n' "$OUT"/lift_*.c "$OUT"/*_lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "$JOBS" "$HERE/tools/cc1.sh" "$OUT"
echo "== $(ls "$OUT/obj" | wc -l | tr -d ' ') object files; runtime and link"
"$HERE/tools/build_runtime.sh" "$OUT"
