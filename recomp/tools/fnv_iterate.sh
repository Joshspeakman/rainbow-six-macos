#!/bin/bash
# fnv_iterate.sh [relink] : rebuild (everything, or FNV_RELINK for runtime-only changes) and run a short windowed
# test session of Fallout NV; prints the end of the log. GAME (the extracted app folder) and SECS can be set.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
GAME="${GAME:-$HOME/Games/FalloutNV-GOG/app}"; SECS="${SECS:-60}"; N=$(date +%H%M%S)
[[ "${1:-}" == relink ]] && export FNV_RELINK=1
./build_fnv.sh "$GAME/FalloutNV.exe" > work/build.log 2>&1 || { echo "build failed"; grep -E "error|undefined" work/build.log | head -20; exit 1; }
DS_TEST=1 W32_AUDIO_MUTE=1 DS_EXIT_AFTER=$SECS DS_RESOLUTION=${DS_RESOLUTION:-1280x720} timeout -s KILL $((SECS + 15)) setsid \
  work/fnv/app/FalloutNVLinux --exe work/fnv/FalloutNV.exe --game "$GAME" --data "${FNV_DATA:-$HOME/.local/share/fallout-nv-linux-dev}" \
  > work/run-$N.log 2>&1 < /dev/null
echo "run rc=$? log work/run-$N.log"
grep -v "GetProcAddress(kernel32, \(Encode\|Decode\|Fls\)" work/run-$N.log | tail -${TAIL:-30} | cut -c1-300
