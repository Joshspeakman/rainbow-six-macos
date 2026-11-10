#!/bin/bash
# build_runtime.sh <outdir> : compile the runtime + Win32 layer and link with the lifted objects into libgame.dylib
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$1"; mkdir -p "$OUT/obj/rt"
if [[ $(uname -s) == Linux ]]; then exec "$HERE/tools/build_linux.sh" "$OUT"; fi
# macOS: the same edition flags and include paths as tools/build_linux.sh
EDITION_FLAGS=()
[[ -f "$OUT/Df.exe" ]] && EDITION_FLAGS=(-DDF_EDITION -DDF1_EDITION)
[[ -f "$OUT/Df2.exe" ]] && EDITION_FLAGS=(-DDF_EDITION -DDF2_EDITION)
[[ -f "$OUT/RainbowSix.exe" ]] && EDITION_FLAGS=(-DDF_EDITION -DR6_EDITION)
INC=(-I "$HERE/runtime" -I "$HERE/runtime/win32" -I "$HERE/third_party/mojoshader" -I "$HERE/third_party/minimp3" -I "$HERE/third_party/vulkan/include")
# Check every compiler status before linking; bound parallel work on macOS.
PIDS=(); JOBS="${NATIVE_BUILD_JOBS:-6}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "invalid NATIVE_BUILD_JOBS" >&2; exit 1; }
wait_jobs() {
  local failed=0 pid
  for pid in "${PIDS[@]:-}"; do [ -z "$pid" ] || wait "$pid" || failed=1; done
  PIDS=(); [ "$failed" -eq 0 ]
}
queue_job() { PIDS+=("$!"); if [ "${#PIDS[@]}" -ge "$JOBS" ]; then wait_jobs; fi; }
trap 'for pid in "${PIDS[@]:-}"; do [ -z "$pid" ] || wait "$pid" || true; done' EXIT
python3 "$HERE/tools/gen_imptab.py" "$OUT/imptab.c" "$HERE"/runtime/win32/*.c
# MojoShader (d3d9_shader.c): as on Linux, the GLSL profile only (the Delta Force games never use Direct3D 9 shaders)
MOJO=(-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0
      -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_METAL=0
      -DMOJOSHADER_FLIP_RENDERTARGET -DMOJOSHADER_DEPTH_CLIPPING -I "$HERE/third_party/mojoshader")
mkdir -p "$OUT/obj/rt/mojo"
for f in "$HERE"/third_party/mojoshader/mojoshader.c "$HERE"/third_party/mojoshader/mojoshader_common.c "$HERE"/third_party/mojoshader/profiles/*.c; do
  clang -c -O2 -w "${MOJO[@]}" -o "$OUT/obj/rt/mojo/$(basename "$f" .c).o" "$f" &
  queue_job
done
for f in "$HERE"/third_party/core-math/*.c; do   # correctly rounded tan/sin/cos/atan2 for the x87 instructions (rt.c)
  clang -c -O2 -ffp-contract=off -w -o "$OUT/obj/rt/cm_$(basename "$f" .c).o" "$f" &
  queue_job
done
for f in "$HERE"/runtime/rt.c "$HERE"/runtime/win32/*.c "$OUT/imptab.c"; do
  opt="-O1 -ffp-contract=off"; [ "$(basename "$f")" = native.c ] && opt="-O3 -ffp-contract=off"   # the native hot loops: optimised, no fused multiply-add
  clang -c $opt -g -Wall -Wno-unused-function -fno-strict-aliasing "${EDITION_FLAGS[@]}" "${INC[@]}" -o "$OUT/obj/rt/$(basename "$f" .c).o" "$f" &
  queue_job
done; wait_jobs
clang -shared -o "$OUT/libgame.dylib" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "$OUT"/obj/rt/mojo/*.o -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
"$HERE/tools/build_app.sh" "$OUT"
