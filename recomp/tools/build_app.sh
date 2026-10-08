#!/bin/bash
# build_app.sh <outdir> : link the recompiled game, the runtime and the macOS host into an app executable
# (run build.sh first). Produces <outdir>/app/DungeonSiegeNative with shaders.metal and dsr_snapshot.bin beside it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"; OUT="$1"; APP="$OUT/app"
if [[ $(uname -s) == Linux ]]; then exec "$HERE/tools/build_linux.sh" "$OUT"; fi
mkdir -p "$APP" "$OUT/obj/host"
BUILD="$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown)"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function -I "$HERE/runtime" -DDS_BUILD="\"$BUILD\"" -o "$OUT/obj/host/main.o" "$HERE/host/main.m"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function -o "$OUT/obj/host/renderer.o" "$ROOT/src/renderer/renderer.m"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function -o "$OUT/obj/host/launcher.o" "$HERE/host/launcher.m"
clang -o "$APP/DungeonSiegeNative" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "$OUT"/obj/host/*.o \
  -framework AppKit -framework Metal -framework QuartzCore -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
cp "$ROOT/src/renderer/shaders.metal" "$ROOT/src/dsr/dsr_snapshot.bin" "$APP/"
echo "$APP/DungeonSiegeNative"
