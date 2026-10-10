#!/usr/bin/env bash
# Actual Metal checks; no game data required.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/r6-metal.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
FLAGS=(-O1 -g -fobjc-arc -fsanitize=address,undefined -framework Metal -framework Foundation -framework QuartzCore)
clang "${FLAGS[@]}" -I "$ROOT/src/renderer" "$ROOT/recomp/tests/renderer_init_test.m" "$ROOT/src/renderer/renderer.m" -o "$OUT/init"
clang "${FLAGS[@]}" "$ROOT/recomp/tests/renderer_ring_test.m" -o "$OUT/ring"
clang "${FLAGS[@]}" "$ROOT/recomp/tests/renderer_draw_ring_test.m" -o "$OUT/draw"
cd "$ROOT/src/renderer"
"$OUT/init"
"$OUT/ring"
"$OUT/draw" fixed
"$OUT/draw" program
