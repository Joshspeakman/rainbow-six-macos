#!/bin/bash
# dev/dropin_test.sh <app> <root under ~/Games> <tag>: development check of a built release app. Search -> Set Up on a fresh root, stop at ready.json, then boot the setup-built game in test mode
A="$1"; R="$HOME/Games/$2"; T="${DROPIN_OUT:-$HOME/Scratch}/dropin-$3"; mkdir -p "$T"
BIN="$A/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$A/Contents/Info.plist")"
[ -e "$R" ] && { echo "root exists already: $R"; exit 1; }
DS_SETUP_SHOTS="$T" DS_SETUP_SEARCH=1 DS_SETUP_SEARCHLOG=1 DS_SETUP_PRESS=1 "$BIN" > "$T/setup.txt" 2>&1 & P=$!; start=$(date +%s)
while kill -0 $P 2>/dev/null; do
  if [ -f "$R/recomp/ready.json" ]; then echo "ready after $(( $(date +%s) - start )) s"; break; fi
  [ $(( $(date +%s) - start )) -gt 3000 ] && { echo "timeout"; break; }
  sleep 3
done
kill $P 2>/dev/null; sleep 1; pkill -f "$BIN" 2>/dev/null; sleep 2
grep -a "setup:\|failed" "$T/setup.txt" | head -8 | cut -c1-200; ls "$R/recomp" "$R/art" 2>/dev/null | tr '\n' ' '; echo
[ -f "$R/recomp/ready.json" ] || exit 1
EXE=$(ls "$R/recomp" | grep -i '\.exe$' | head -1); mkdir -p "$T/boot"
DYLD_LIBRARY_PATH="$R/recomp" DS_TEST=1 W32_AUDIO_MUTE=1 DS_NO_LAUNCHER=1 DS_SCRIPT="${BOOT_SCRIPT:-@40:shot:$T/boot/shot.png}" "$BIN" --exe "$R/recomp/$EXE" --game "$R/game-files" --data "$T/boot" > "$T/boot.txt" 2>&1 & P=$!
sleep 60; kill $P 2>/dev/null; sleep 2
echo "boot: $(grep -a 'frame [0-9]* (' "$T/boot.txt" | tail -1 | sed -E 's/.*(frame [0-9]+ \([0-9x]+\) [0-9.]+ fps).*/\1/'); faults=$(grep -a -c 'unhandled' "$T/boot.txt"); crash=$(ls "$T/boot/CrashReports" 2>/dev/null | wc -l | tr -d ' '); shot=$([ -f "$T/boot/shot.png" ] && echo yes || echo no)"
