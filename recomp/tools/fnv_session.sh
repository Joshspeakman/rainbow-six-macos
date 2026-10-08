#!/bin/bash
# fnv_session.sh <name> <seconds> [extra DS_SCRIPT steps] : one muted test session of the recompiled Fallout NV in
# work/sess-<name>/: log, Windows-call trace (TRACE_ONLY=dll list), DirectSound mix as WAV, screenshots every SHOT
# seconds (default 15). NEWGAME=1 starts a new game from the main menu, skips the intro and dismisses the DLC
# messages first (ready at about 125 s). DATA is a fresh data folder unless KEEP_DATA=1.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; cd "$HERE"
NAME="$1"; SECS="$2"; EXTRA="${3:-}"; OUT="work/sess-$NAME"; rm -rf "$OUT"; mkdir -p "$OUT"
GAME="${GAME:-$HOME/Games/FalloutNV-GOG/app}"; DATA="${DATA:-$PWD/$OUT/data}"
[[ -n "${KEEP_DATA:-}" ]] || rm -rf "$DATA"
if [[ -n "${SAVE:-}" ]]; then                      # SAVE=<file.fos>: the only save in the data folder (Continue loads it)
  mkdir -p "$DATA/drive_c/Users/player/Documents/My Games/FalloutNV/Saves"; cp "$SAVE" "$DATA/drive_c/Users/player/Documents/My Games/FalloutNV/Saves/"
fi
S=""
if [[ -n "${NEWGAME:-}" ]]; then
  S="@60:dik:208;@62:dik:208;@64:dik:28;@68:dik:203;@70:dik:28;@78:dik:1;@80:dik:57;@82:dik:1"
  for t in 100 102 104 106 108 110 112 114 116 118 120; do S="$S;@$t:dik:28"; done
fi
for ((t = ${SHOT:-15}; t < SECS; t += ${SHOT:-15})); do S="$S;@$t:shot:$PWD/$OUT/t$(printf %04d $t).png"; done
[[ -n "$EXTRA" ]] && S="$S;$EXTRA"
TR=(); [[ -n "${TRACE_ONLY:-}" ]] && TR=(W32_TRACE="$OUT/trace.txt" W32_TRACE_ONLY="$TRACE_ONLY")
env "${TR[@]}" W32_AUDIO_DUMP="$OUT/mix.wav" W32_AUDIO_MUTE=1 DS_TEST=1 DS_EXIT_AFTER="$SECS" DS_RESOLUTION="${DS_RESOLUTION:-1280x720}" \
    DS_SCRIPT="${S#;}" ${SESSION_ENV:-} timeout -s KILL $((SECS + 30)) setsid \
    "${BIN:-work/fnv/app/FalloutNVLinux}" --exe work/fnv/FalloutNV.exe --game "$GAME" --data "$DATA" > "$OUT/log.txt" 2>&1 < /dev/null
echo "rc=$? $OUT"
grep -v "^frame \|GetProcAddress" "$OUT/log.txt" | sed 's/[0-9a-f]\{8\}/X/g' | sort | uniq -c | sort -rn | head -${TAIL:-25} | cut -c1-220
grep "^frame" "$OUT/log.txt" | tail -1
ls "$DATA"/CrashReports 2>/dev/null | sed 's/^/crash: /'
