#!/bin/bash
# build_app.sh <outdir> : link the recompiled game, the runtime and the macOS host into an app executable (run
# build_df.sh first). Produces <outdir>/app/DeltaForce (Df.exe) or DeltaForce2 (Df2.exe) with shaders.metal and
# dsr_snapshot.bin beside it. On Linux, tools/build_linux.sh makes the SDL/OpenGL build instead.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"; OUT="$1"; APP="$OUT/app"
if [[ $(uname -s) == Linux ]]; then exec "$HERE/tools/build_linux.sh" "$OUT"; fi
NAME=DungeonSiegeNative; EDITION_FLAGS=()
[[ -f "$OUT/Df.exe" ]] && { NAME=DeltaForce; EDITION_FLAGS=(-DDF_EDITION -DDF1_EDITION); }
[[ -f "$OUT/Df2.exe" ]] && { NAME=DeltaForce2; EDITION_FLAGS=(-DDF_EDITION -DDF2_EDITION); }
mkdir -p "$APP" "$OUT/obj/host"
BUILD="$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown)"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function "${EDITION_FLAGS[@]}" -I "$HERE/runtime" -DDS_BUILD="\"$BUILD\"" -o "$OUT/obj/host/main.o" "$HERE/host/main.m"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function -o "$OUT/obj/host/renderer.o" "$ROOT/src/renderer/renderer.m"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function "${EDITION_FLAGS[@]}" -o "$OUT/obj/host/launcher.o" "$HERE/host/launcher.m"
MOJO=(); [[ -d "$OUT/obj/rt/mojo" ]] && MOJO=("$OUT"/obj/rt/mojo/*.o)
clang -o "$APP/$NAME" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "${MOJO[@]}" "$OUT"/obj/host/*.o \
  -framework AppKit -framework Metal -framework QuartzCore -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
cp "$ROOT/src/renderer/shaders.metal" "$ROOT/src/dsr/dsr_snapshot.bin" "$APP/"
echo "$APP/$NAME"
