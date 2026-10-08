#!/usr/bin/env bash
# Build the shared native runtime and SDL/OpenGL host from locally lifted game objects.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=$(realpath "${1:?usage: build_linux.sh <recomp output directory>}")
CC=${CC:-clang}
python3 "$HERE/tools/compiler_compat.py" "$CC" "$OUT/compiler-flags.txt"
read -ra COMPAT_FLAGS < "$OUT/compiler-flags.txt"
EXE=$OUT/DungeonSiege2.exe; APP=DungeonSiege2Linux
[[ -f "$OUT/FalloutNV.exe" ]] && { EXE=$OUT/FalloutNV.exe; APP=FalloutNVLinux; }
[[ -f "$OUT/Sims.exe" ]] && { EXE=$OUT/Sims.exe; APP=SimsLinux; }
[[ -f "$OUT/Df.exe" ]] && { EXE=$OUT/Df.exe; APP=DeltaForceLinux; }
[[ -f "$OUT/Df2.exe" ]] && { EXE=$OUT/Df2.exe; APP=DeltaForce2Linux; }
EDITION=$(PYTHONPATH="$HERE/tools" python3 -c 'import sys; from editions import identify; print(identify(sys.argv[1], prepared=True))' "$EXE")
EDITION_FLAGS=()
[[ "$EDITION" == broken-world ]] && EDITION_FLAGS=(-DDS2_BROKEN_WORLD)
[[ "$EDITION" == fnv ]] && EDITION_FLAGS=(-DFNV_EDITION)
[[ "$EDITION" == sims ]] && EDITION_FLAGS=(-DSIMS_EDITION)
[[ "$EDITION" == df1 ]] && EDITION_FLAGS=(-DDF_EDITION -DDF1_EDITION)
[[ "$EDITION" == df2 ]] && EDITION_FLAGS=(-DDF_EDITION -DDF2_EDITION)
DEPS=(sdl2 freetype2 fontconfig sndfile glew libpng)
pkg-config --exists "${DEPS[@]}"
read -ra CFLAGS <<< "$(pkg-config --cflags "${DEPS[@]}")"
read -ra LIBS <<< "$(pkg-config --libs "${DEPS[@]}")"
mkdir -p "$OUT/obj/rt" "$OUT/obj/host" "$OUT/app"
python3 "$HERE/tools/gen_imptab.py" "$OUT/imptab.c" "$HERE"/runtime/win32/*.c
# MojoShader (Direct3D 9 shader bytecode -> GLSL for the game's own shaders, d3d9_shader.c): the GLSL profile only,
# with D3D's render-target orientation and depth range converted in every vertex shader
MOJO=(-DSUPPORT_PROFILE_D3D=0 -DSUPPORT_PROFILE_BYTECODE=0 -DSUPPORT_PROFILE_HLSL=0 -DSUPPORT_PROFILE_ARB1=0 -DSUPPORT_PROFILE_ARB1_NV=0
      -DSUPPORT_PROFILE_SPIRV=0 -DSUPPORT_PROFILE_GLSPIRV=0 -DSUPPORT_PROFILE_GLSLES=0 -DSUPPORT_PROFILE_GLSLES3=0 -DSUPPORT_PROFILE_METAL=0
      -DMOJOSHADER_FLIP_RENDERTARGET -DMOJOSHADER_DEPTH_CLIPPING -I "$HERE/third_party/mojoshader")
mkdir -p "$OUT/obj/rt/mojo"
for f in "$HERE"/third_party/mojoshader/mojoshader.c "$HERE"/third_party/mojoshader/mojoshader_common.c "$HERE"/third_party/mojoshader/profiles/*.c; do
    "$CC" "${COMPAT_FLAGS[@]}" -c -O2 -fPIC -w "${MOJO[@]}" -o "$OUT/obj/rt/mojo/$(basename "$f" .c).o" "$f"
done
for f in "$HERE/runtime/rt.c" "$HERE"/runtime/win32/*.c "$OUT/imptab.c"; do
    o="$OUT/obj/rt/$(basename "$f" .c).o"
    OPT=-O1; [[ $(basename "$f") == native.c ]] && OPT=-O3
    "$CC" "${COMPAT_FLAGS[@]}" "${EDITION_FLAGS[@]}" -c "$OPT" -g -fPIC -D_GNU_SOURCE -ffp-contract=off -fno-strict-aliasing -Wall -Wno-unused-function \
        -I "$HERE/runtime" -I "$HERE/runtime/win32" -I "$HERE/third_party/mojoshader" -I "$HERE/third_party/minimp3" -I "$HERE/third_party/vulkan/include" "${CFLAGS[@]}" -o "$o" "$f"
done
"$CC" -shared -o "$OUT/libgame.so" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "$OUT"/obj/rt/mojo/*.o "${LIBS[@]}" -pthread -ldl -lm -lz
if [[ -f "$HERE/host/linux.c" ]]; then
    "$CC" "${COMPAT_FLAGS[@]}" "${EDITION_FLAGS[@]}" -O2 -g -D_GNU_SOURCE -DDS_BUILD=\"$(git -C "$ROOT" describe --always --dirty)\" \
        -I "$HERE/runtime" -I "$HERE/third_party/vulkan/include" "${CFLAGS[@]}" -o "$OUT/app/$APP" \
        "$HERE/host/linux.c" "$ROOT/src/renderer/linux/renderer.c" \
        -Wl,--export-dynamic "$OUT"/obj/*.o "$OUT"/obj/rt/*.o "$OUT"/obj/rt/mojo/*.o "${LIBS[@]}" -pthread -ldl -lm -lz
    cp "$ROOT/src/renderer/linux/"*.glsl "$ROOT/src/dsr/dsr_snapshot.bin" "$OUT/app/"
    mkdir -p "$OUT/app/launcher"
    cp "$HERE/host/linux_launcher/"*.py "$OUT/app/launcher/"
    python3 "$HERE/tools/prepare_linux_icon.py" "$EXE" "$OUT/app" || echo "(no window icon)"
    echo "$OUT/app/$APP"
fi
