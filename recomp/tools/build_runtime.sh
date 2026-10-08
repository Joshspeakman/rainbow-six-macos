#!/bin/bash
# build_runtime.sh <outdir> : compile the runtime + Win32 layer and link with the lifted objects into libgame.dylib
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$1"; mkdir -p "$OUT/obj/rt"
if [[ $(uname -s) == Linux ]]; then exec "$HERE/tools/build_linux.sh" "$OUT"; fi
# macOS (untested for Delta Force so far): the same edition flags and include paths as tools/build_linux.sh
EDITION_FLAGS=()
[[ -f "$OUT/Df.exe" ]] && EDITION_FLAGS=(-DDF_EDITION -DDF1_EDITION)
[[ -f "$OUT/Df2.exe" ]] && EDITION_FLAGS=(-DDF_EDITION -DDF2_EDITION)
INC=(-I "$HERE/runtime" -I "$HERE/runtime/win32" -I "$HERE/third_party/mojoshader" -I "$HERE/third_party/minimp3" -I "$HERE/third_party/vulkan/include")
python3 "$HERE/tools/gen_imptab.py" "$OUT/imptab.c" "$HERE"/runtime/win32/*.c
# MojoShader (d3d9_shader.c): as on Linux, the GLSL profile only (the Delta Force games never use Direct3D 9 shaders)
MOJO=(-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0
      -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_METAL=0
      -DMOJOSHADER_FLIP_RENDERTARGET -DMOJOSHADER_DEPTH_CLIPPING -I "$HERE/third_party/mojoshader")
mkdir -p "$OUT/obj/rt/mojo"
for f in "$HERE"/third_party/mojoshader/mojoshader.c "$HERE"/third_party/mojoshader/mojoshader_common.c "$HERE"/third_party/mojoshader/profiles/*.c; do
  clang -c -O2 -w "${MOJO[@]}" -o "$OUT/obj/rt/mojo/$(basename "$f" .c).o" "$f" &
done
for f in "$HERE"/runtime/rt.c "$HERE"/runtime/win32/*.c "$OUT/imptab.c"; do
  opt=-O1; [ "$(basename "$f")" = native.c ] && opt="-O3 -ffp-contract=off"   # the native hot loops: optimised, no fused multiply-add
  clang -c $opt -g -Wall -Wno-unused-function -fno-strict-aliasing "${EDITION_FLAGS[@]}" "${INC[@]}" -o "$OUT/obj/rt/$(basename "$f" .c).o" "$f" &
done; wait
clang -shared -o "$OUT/libgame.dylib" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "$OUT"/obj/rt/mojo/*.o -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
"$HERE/tools/build_app.sh" "$OUT"
