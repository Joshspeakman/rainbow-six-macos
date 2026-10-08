#!/usr/bin/env bash
# Checks use synthetic fixtures only; no game executable or assets required.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OUT="$ROOT/build/linux-checks"
mkdir -p "$OUT"
python3 recomp/tools/compiler_compat.py clang "$OUT/compiler-flags.txt"
read -ra COMPAT_FLAGS < "$OUT/compiler-flags.txt"
CC=(clang "${COMPAT_FLAGS[@]}")
read -ra CF <<< "$(pkg-config --cflags sdl2 freetype2 fontconfig sndfile glew libpng)"
read -ra GL <<< "$(pkg-config --libs sdl2 glew libpng)"
for f in recomp/runtime/rt.c recomp/runtime/win32/*.c recomp/host/linux.c src/renderer/linux/renderer.c; do
    "${CC[@]}" -fsyntax-only -D_GNU_SOURCE -I recomp/runtime -I recomp/third_party/mojoshader -I recomp/third_party/minimp3 "${CF[@]}" "$f"
    "${CC[@]}" -fsyntax-only -D_GNU_SOURCE -DFNV_EDITION -I recomp/runtime -I recomp/third_party/mojoshader -I recomp/third_party/minimp3 "${CF[@]}" "$f"
done
"${CC[@]}" -O1 -g -D_GNU_SOURCE -ffunction-sections -fdata-sections -Wl,--gc-sections -I recomp/runtime \
    -o "$OUT/winsock" recomp/tests/winsock_linux_test.c -pthread
"$OUT/winsock"
"${CC[@]}" -O1 -g -D_GNU_SOURCE -ffunction-sections -fdata-sections -Wl,--gc-sections -I recomp/runtime \
    -o "$OUT/interlocked" recomp/tests/interlocked_test.c -pthread
"$OUT/interlocked"
"${CC[@]}" -O1 -g -D_GNU_SOURCE -I recomp/runtime -I recomp/runtime/win32 -o "$OUT/dp8" \
    recomp/tests/dp8test.c recomp/runtime/win32/dp8proto.c -pthread
"$OUT/dp8"
"${CC[@]}" -O1 -g "${CF[@]}" -o "$OUT/renderer" recomp/tests/renderer_test.c src/renderer/linux/renderer.c "${GL[@]}" -lm
"$OUT/renderer" src/renderer/linux
for test in frame_pacer display_modes; do
    "${CC[@]}" -O1 -g -o "$OUT/$test" "recomp/tests/${test}_test.c" -lm
    "$OUT/$test"
done
python3 -m py_compile recomp/host/linux_launcher/*.py recomp/tools/check_edition.py \
    recomp/tools/install_linux.py recomp/tools/prepare_linux_icon.py
python3 recomp/tests/test_lab_test.py
python3 recomp/tests/edition_test.py
"${CC[@]}" -O1 -g -D_GNU_SOURCE -DDS2_BROKEN_WORLD -I recomp/runtime \
    -o "$OUT/guest-flow" recomp/tests/guest_flow_test.c recomp/runtime/rt.c -lm -pthread
"$OUT/guest-flow"
"${CC[@]}" -O1 -g -D_GNU_SOURCE -DDS2_BROKEN_WORLD -I recomp/runtime \
    -o "$OUT/bw-lock" recomp/tests/bw_lock_test.c recomp/runtime/rt.c -lm -pthread
"$OUT/bw-lock"
"${CC[@]}" -O1 -g -D_GNU_SOURCE -DDS2_BROKEN_WORLD -I recomp/runtime \
    -o "$OUT/bw-save-compat" recomp/tests/bw_save_compat_test.c -lm
"$OUT/bw-save-compat"

if [[ -x recomp/.venv/bin/python ]]; then
    recomp/.venv/bin/python recomp/tests/lift_flow_test.py
fi
