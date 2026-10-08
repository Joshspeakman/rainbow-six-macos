#!/bin/bash
# iterate.sh [--full]: rediscover indirect targets in the emulator, rebuild (lifted code only with --full), rerun the
# start-up harness.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
GAME="${DS_GAME:?set DS_GAME to the Dungeon Siege game folder}"; EXE="${DS_EXE:-$GAME/DungeonSiege.exe}"
if [ "${1:-}" = --full ]; then
  tools/build_runtime.sh work/full 2>&1 | grep -E 'error|warning' || true     # newest runtime: the emulator side reaches furthest
  DISCOVER=work/observed_targets.json DISCOVER_KEEP=1 .venv/bin/python tests/startup.py work/full/libgame.dylib "$GAME" 2>&1 | grep discovery || true
  ./build.sh "$EXE" 2>&1 | grep -E '^functions|lifted|error' || true
else
  tools/build_runtime.sh work/full 2>&1 | grep -E 'error|warning' || true
fi
.venv/bin/python tests/startup.py work/full/libgame.dylib "$GAME" 2>&1 | grep -v GetProcAddress | grep -E 'w32|native|emulator|trace|identical|FIRST|  native|  emulator' | head -20
