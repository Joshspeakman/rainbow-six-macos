#!/bin/bash
# make_suite.sh <version tag> <id>=<game folder> ...: builds the drag-and-drop app and disk image for a repository with
# several games in one app (GTA Classics), from setup/suite.json. Each <id> names one of its "games" (setup/<id>.json),
# built by make_release.sh into the same app (SUITE_APP): the game's own binary, its settings as setup/<binary>.json and
# its address lists. The app's own executable is the suite's launch window ("launcher" sources); Play on a game that
# isn't set up runs that game's binary, which shows its setup window and comes back to the launch window when done
# (setup_ui.m: DS_SUITE). Every game must be given: the release check runs over all of them.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"; S="$HERE/setup"; SJ="$S/suite.json"
TAG="${1:?usage: make_suite.sh <version tag> <id>=<game folder> ...}"; shift
j() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1])); v=v.get(sys.argv[2],sys.argv[3] if len(sys.argv)>3 else ""); print(" ".join(map(str,v)) if isinstance(v,list) else v)' "$SJ" "$@"; }
NAME="$(j name)"; BIN="$(j binary)"; SLUG="${NAME// /-}"
REPO_NAME="$(basename "$ROOT")"; REL="$HOME/ProjectBuilds/$REPO_NAME/release"; mkdir -p "$REL"
IDS="$(python3 -c 'import json,sys; print(" ".join(g["json"][:-5] for g in json.load(open(sys.argv[1]))["games"]))' "$SJ")"
declare -a FOLDERS=()
for id in $IDS; do
  f=""; for a in "$@"; do [ "${a%%=*}" = "$id" ] && f="${a#*=}"; done
  [ -n "$f" ] && [ -d "$f" ] || { echo "make_suite: give $id=<its game folder> (every game: the release check covers them all)"; exit 1; }
  FOLDERS+=("$(cd "$f" && pwd)")
done
A="$REL/$NAME.app"; rm -rf "${A:?}"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources/setup"
k=0; for id in $IDS; do
  echo "==== $id"
  SUITE_APP="$A" GAME_JSON="$S/$id.json" bash "$S/make_release.sh" "${FOLDERS[$k]}" "$TAG" | grep -E '^==|lists:|release check|error'
  k=$((k + 1))
done
echo "==== the launch window"
FW=(); for f in $(j launcher_frameworks); do FW+=(-framework "$f"); done
SRC=(); for f in $(j launcher); do SRC+=("$HERE/$f"); done
clang -O2 -fobjc-arc -Wall "${FW[@]}" -o "$A/Contents/MacOS/$BIN" "${SRC[@]}"
for f in $(j resources); do cp "$HERE/$f" "$A/Contents/Resources/"; done
python3 -c 'import json,sys; [print(h["name"] + "\t" + h["dir"] + "\t" + h["build"]) for h in json.load(open(sys.argv[1])).get("helpers", [])]' "$SJ" |
while IFS=$'\t' read -r hn hd hb; do   # small tools the launch window runs, built here (the player's Mac has no compiler): Resources/bin
  mkdir -p "$A/Contents/Resources/bin"; (cd "$HERE/$hd"; OUT="$A/Contents/Resources/bin/$hn" bash -c "$hb")
done
sed -e "s|@REPO@|$(j release_repo)|; s|@ASSET@|$SLUG.zip|" "$S/update.sh" > "$A/Contents/MacOS/update"; chmod +x "$A/Contents/MacOS/update"
echo "== icon and bundle"
I="$(mktemp -d)"; clang -fobjc-arc -framework AppKit -framework CoreImage "$S/make_art.m" -I "$S" -o "$I/mk"; "$I/mk" icon "$(j style)" "$(j icon_label)" "$I/i.png"; mkdir "$I/i.iconset"
for n in 16 32 128 256 512; do sips -z $n $n "$I/i.png" --out "$I/i.iconset/icon_${n}x${n}.png" >/dev/null; sips -z $((n*2)) $((n*2)) "$I/i.png" --out "$I/i.iconset/icon_${n}x${n}@2x.png" >/dev/null; done
iconutil -c icns "$I/i.iconset" -o "$A/Contents/Resources/icon.icns"
"$I/mk" dmg "$(j style)" "$(j title)" "$REL/bg.png" 1; "$I/mk" dmg "$(j style)" "$(j title)" "$REL/bg@2x.png" 2; tiffutil -cathidpicheck "$REL/bg.png" "$REL/bg@2x.png" -out "$REL/background.tiff" 2>/dev/null; rm -f "${REL:?}/bg.png" "${REL:?}/bg@2x.png"; rm -rf "${I:?}"
cat > "$A/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string><key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>$(j bundle_id)</string><key>CFBundleExecutable</key><string>$BIN</string>
  <key>CFBundleIconFile</key><string>icon</string><key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${TAG#v}</string><key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>GCSupportsGameMode</key><true/><key>LSMinimumSystemVersion</key><string>13.0</string><key>NSHighResolutionCapable</key><true/>
  <key>NSLocalNetworkUsageDescription</key><string>$(j local_network)</string>
  <key>CFBundleDocumentTypes</key><array><dict><key>CFBundleTypeName</key><string>Game Folder</string><key>CFBundleTypeRole</key><string>Viewer</string>
    <key>LSHandlerRank</key><string>Alternate</string><key>LSItemContentTypes</key><array><string>public.folder</string></array></dict></array>
</dict></plist>
PLIST
echo "== release check (every game)"
k=0; for id in $IDS; do python3 "$S/check_release.py" "$A" "${FOLDERS[$k]}" "$S/$id.json"; k=$((k + 1)); done
codesign --force --deep -s - "$A"; codesign -v "$A"
echo "== packaging"
"$HERE/.venv/bin/python" -c 'import dmgbuild' 2>/dev/null || "$HERE/.venv/bin/python" -m pip install -q dmgbuild
D="$(mktemp -d)"; sed "s/@NAME@/$NAME/g" "$S/ReadMe.txt" > "$D/Read Me First.txt"
rm -f "${REL:?}/${SLUG:?}.dmg" "${REL:?}/${SLUG:?}.zip"
cat > "$D/settings.py" <<PY
files = ["$A", "$D/Read Me First.txt"]; symlinks = {"Applications": "/Applications"}; format = "UDZO"; filesystem = "HFS+"
badge_icon = None; icon = "$A/Contents/Resources/icon.icns"; background = "$REL/background.tiff"; show_status_bar = False; show_tab_view = False; show_toolbar = False; show_pathbar = False; show_sidebar = False
window_rect = ((200, 120), (660, 420)); default_view = "icon-view"; icon_size = 112; text_size = 13
icon_locations = {"$NAME.app": (170, 230), "Applications": (490, 230), "Read Me First.txt": (330, 372)}
PY
"$HERE/.venv/bin/dmgbuild" -s "$D/settings.py" "$NAME" "$REL/$SLUG.dmg" 2>&1 | tail -1; rm -rf "${D:?}"
(cd "$REL"; ditto -c -k --keepParent "$NAME.app" "$SLUG.zip")
du -sh "$A" "$REL/$SLUG.dmg" | sed "s|$REL/||"
