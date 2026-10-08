#!/bin/bash
# Play from a local build; all writes go to DS_NATIVE_DATA, never the source game folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="${1:?usage: run.sh <Dungeon Siege II game folder> [build output]}"
OUT="${2:-$HERE/work/full}"
OUT="$(cd "$OUT" && pwd)"
if [[ $(uname -s) == Linux ]]; then
    DATA="${DS_NATIVE_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/dungeon-siege-2-linux}"
    exec python3 "$HERE/host/linux_launcher/launcher.py" --exe "$OUT/DungeonSiege2.exe" \
        --game "$GAME" --data "$DATA" --binary "$OUT/app/DungeonSiege2Linux"
fi
DATA="${DS_NATIVE_DATA:-$HOME/Games/DungeonSiege2Native}"
mkdir -p "$DATA/drive_c/Users/player/Documents"
exec "$OUT/app/DungeonSiegeNative" --exe "$OUT/DungeonSiege2.exe" --game "$GAME" --data "$DATA"
