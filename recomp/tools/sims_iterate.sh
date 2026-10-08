#!/bin/bash
# sims_iterate.sh: relink after runtime changes (SIMS_RELINK) and run the game once in test mode (hidden window).
#   SIMS_GAME=<game folder> (default: the Steam folder on the reference host)   DATA=<data folder>   T=<seconds, 60>   NOLINK=1
#   plus any test switch (DS_SCRIPT, W32_VK_SHOT/W32_VK_SHOT_AT, W32_TRACE, W32_MSGLOG, ...).
# Output in work/run.log; prints the exit status and the log's last lines.
cd "$(dirname "$0")/.."
G="${SIMS_GAME:-/mnt/de23fa52-e674-44dc-b668-d5ca17db46cf/SteamLibrary/steamapps/common/The Sims Legacy Collection}"
D="${DATA:-$PWD/work/data}"; mkdir -p "$D"
[ -n "$NOLINK" ] || SIMS_RELINK=1 ./build_sims.sh "$G/Sims.exe" 2>&1 | grep -E "error|SimsLinux" | head -20
DS_TEST=1 W32_AUDIO_MUTE=1 timeout "${T:-60}" work/sims/app/SimsLinux --exe work/sims/Sims.exe --game "$G" --data "$D" > work/run.log 2>&1
echo "exit $?"; grep -v "^host:\|^renderer:\|GetProcAddress\|LoadLibraryExA" work/run.log | tail -"${N:-12}"
