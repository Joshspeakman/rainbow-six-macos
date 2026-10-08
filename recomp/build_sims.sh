#!/bin/bash
# build_sims.sh <Sims.exe> [outdir]
# Recompile the user's own Steam copy of The Sims Legacy Collection (Sims.exe, and gimex.dll and ijl10.dll from its
# folder) into C and link it with the native runtime (tools/build_runtime.sh: Metal on macOS, SDL on Linux). The Steam
# wrapper is undone on a copy in the build folder (tools/steamstub.py); everything generated stays local (work/ is
# git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="${1:?usage: build_sims.sh <Sims.exe> [outdir]}"; OUT="${2:-$HERE/work/sims}"
SHA=b7c8765ed2383cbda8c047694d54a21d97895c8dae57dcc87716ca6e78b342e3          # Steam 3314060, build 17905164
sha() { if command -v shasum >/dev/null; then shasum -a 256 "$1"; else sha256sum "$1"; fi | cut -d' ' -f1; }
[ "$(sha "$EXE")" = "$SHA" ] || { echo "not the supported Steam Sims.exe (The Sims Legacy Collection, build 17905164)"; exit 1; }
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
GAME="$(cd "$(dirname "$EXE")" && pwd)"
python3 "$HERE/tools/compiler_compat.py" "${CC:-clang}" "$OUT/compiler-flags.txt"
"$PY" "$HERE/tools/steamstub.py" "$EXE" "$OUT/Sims.exe"; EXE="$OUT/Sims.exe"
if [[ -n "${SIMS_RELINK:-}" && -d "$OUT/obj" ]]; then           # runtime changes only: keep the lifted objects
  echo "== runtime and link (SIMS_RELINK)"; exec "$HERE/tools/build_runtime.sh" "$OUT"
fi
echo "== analysis"
"$PY" "$HERE/tools/analyze.py" "$EXE" "$OUT/analysis.json" > "$OUT/analysis.log"
sed -n '/^functions /p; /^decode failures:/p; /^jump tables /p; /^SEH:/p' "$OUT/analysis.log"
echo "== lifting"
"$PY" "$HERE/tools/lift.py" "$EXE" "$OUT/analysis.json" "$OUT" --game sims | grep -Ev '^unsupported: \{\}$'
# the game's own DLLs, loaded by name at run time: gimex (EA's image codecs) and the Intel JPEG library it imports.
# Both want 0x60000000, so ijl10 is lifted from a copy relocated to 0x61000000 (tools/rebase_pe.py); loader.c applies
# the same relocations to the original at run time.
NIMP=$("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/image.json")
IMGS=("=")
for spec in gimex.dll ijl10.dll@0x61000000; do
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
JOBS="${SIMS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
printf '%s\n' "$OUT"/lift_*.c "$OUT"/*_lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "$JOBS" "$HERE/tools/cc1.sh" "$OUT"
echo "== $(ls "$OUT/obj" | wc -l | tr -d ' ') object files; runtime and link"
"$HERE/tools/build_runtime.sh" "$OUT"
