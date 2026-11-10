#!/bin/bash
# make_release.sh <game folder> <version tag>: builds the drag-and-drop app and disk image for a public release.
#
# The app holds our runtime and launcher, compiled here against Apple's SDK; the compiler and Python that build the game on
# the player's Mac; and address lists saying where the functions are in each exact game file. It holds no game code or data:
# check_release.py fails the release otherwise. The game piece is a separate library the executable links weakly, so the app
# runs before the game exists and binds it once setup has built it (setup_ui.m restarts itself with the library path set).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$HERE/.." && pwd)"; S="$HERE/setup"; GAME="$(cd "${1:?usage: make_release.sh <game folder> <tag>}" && pwd)"; TAG="${2:?tag}"
j() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1])); v=v.get(sys.argv[2],sys.argv[3] if len(sys.argv)>3 else ""); print(" ".join(map(str,v)) if isinstance(v,list) else v)' "$S/game.json" "$@"; }
NAME="$(j name)"; BIN="$(j binary)"; SLUG="${NAME// /-}"; OUTREL="$(j out work/full)"
REPO_NAME="$(basename "$ROOT")"; B="$HOME/ProjectBuilds/$REPO_NAME"; REL="$B/release"; mkdir -p "$REL" "$B/recomp/work"
[ -e "$HERE/work" ] || { ln -s "$B/recomp/work" "$HERE/work"; C="$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir)"; grep -qxF recomp/work "$C/info/exclude" || echo recomp/work >> "$C/info/exclude"; }
if [ -L "$HERE/.venv" ] && [ ! -e "$HERE/.venv" ]; then rm -f "${HERE:?}/.venv"; fi   # a link left dangling by a moved build folder
[ -x "$HERE/.venv/bin/python" ] || { python3 -m venv "$HERE/.venv"; "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
TK="$HOME/ProjectBuilds/_toolkit/toolkit.tar.xz"; [ -f "$TK" ] || "$S/make_toolkit.sh" "$TK"
if [ -z "${SKIP_FULL_BUILD:-}" ]; then echo "== full build (for the runtime's objects and to prove this tree builds)"; (cd "$HERE"; GAME="$GAME" bash -c "$(j build)" 2>&1 | tail -2); fi
OUT="$HERE/$OUTREL"; O="$OUT/obj/easy"; rm -rf "${O:?}"; mkdir -p "$O"
echo "== the runtime, split from the game"
clang -c $(j rt_opt -O1) -g -Wall -Wno-unused-function -fno-strict-aliasing -DRT_SPLIT $(j rt_cflags) -I "$HERE/runtime" -I "$HERE/runtime/win32" -o "$OUT/obj/rt/rt.o" "$HERE/runtime/rt.c"
clang -c -O2 -fobjc-arc -Wall -Wno-unused-function -DDS_EASY_INSTALL $(j host_cflags) -I "$HERE/runtime" -DDS_BUILD="\"$TAG\"" -o "$OUT/obj/host/main.o" "$HERE/host/main.m"
clang -c -O2 -fobjc-arc -Wall -o "$OUT/obj/host/setup_ui.o" "$S/setup_ui.m"
cat > "$O/stub.c" <<'STUB'
#include <stdint.h>
const char rt_fntab[1], rt_images[1], rt_imptab[1]; const unsigned rt_fntab_n, rt_images_n, rt_imptab_n; __thread uint32_t rt_host_rc;
STUB
comm -12 <(find "$OUT/obj/rt" "$OUT/obj/host" -name '*.o' -exec nm -u {} + | awk '/^_/{print $1}' | sort -u) <(nm -gU "$OUT"/obj/*.o 2>/dev/null | awk 'NF==3{print $3}' | sort -u) | { grep '^_f_' || true; } | sed -E 's/^_(.*)$/void \1(void) {}/' >> "$O/stub.c"   # game functions the runtime calls directly
clang -dynamiclib -arch arm64 -install_name @rpath/libgame.dylib -o "$O/libgame.dylib" "$O/stub.c"
FW=(); for f in $(j frameworks); do FW+=(-framework "$f"); done
find "$OUT/obj/rt" "$OUT/obj/host" -name '*.o' > "$O/objs.txt"
A="$REL/$NAME.app"; rm -rf "${A:?}"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources/setup"
clang -o "$A/Contents/MacOS/$BIN" -filelist "$O/objs.txt" -Wl,-weak_library,"$O/libgame.dylib" -Wl,-export_dynamic -Wl,-rpath,@executable_path/../Frameworks "${FW[@]}" -lz $(j link_flags)
cp "$OUT/app/shaders.metal" "$OUT/app/dsr_snapshot.bin" "$A/Contents/MacOS/"
sed -e "s|@REPO@|$(j release_repo)|; s|@ASSET@|$SLUG.zip|" "$S/update.sh" > "$A/Contents/MacOS/update"; chmod +x "$A/Contents/MacOS/update"
echo "== setup files"
R="$A/Contents/Resources/setup"; mkdir -p "$R/tools" "$R/runtime" "$R/lists"
cp "$S/setup.py" "$S/game_tls.c" "$R/"; cp -R "$S/include" "$R/include"; cp "$HERE"/tools/*.py "$R/tools/"; cp "$HERE"/runtime/rt*.h "$R/runtime/"; cp "$TK" "$R/toolkit.tar.xz"
CODE="$(cat "$HERE"/tools/*.py "$HERE"/runtime/rt*.h "$S"/include/* "$S/game_tls.c" | shasum -a 256 | cut -d' ' -f1)"
python3 - "$S/game.json" "$R/game.json" "$CODE" "$TAG" <<'PY'
import json, sys
c = json.load(open(sys.argv[1])); c["code"] = sys.argv[3]; c["release"] = sys.argv[4]
for k in ("build", "out", "rt_cflags", "rt_opt", "host_cflags", "frameworks", "link_flags", "binary", "icon_label", "local_network", "release_repo", "bundle_id"): c.pop(k, None)
import os
if os.environ.get("TEST_EXTRA_EXE_SHA"): c["exe_sha256"][os.environ["TEST_EXTRA_EXE_SHA"]] = "test copy"   # a test build only: a release never sets this
json.dump(c, open(sys.argv[2], "w"), indent=1)
PY
echo "== address lists"
LT="$(mktemp -d)"; "$HERE/.venv/bin/python" "$S/setup.py" --game "$GAME" --root "$LT" --setup "$R" --tools "$LT" --make-lists "$R/lists" | tail -1 >/dev/null; rm -rf "${LT:?}"
ls "$R/lists" | wc -l | awk '{print "lists:", $1}'
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
python3 "$S/check_release.py" "$A" "$GAME" "$S/game.json"
codesign --force --deep -s - "$A"; codesign -v "$A"
echo "== packaging"
"$HERE/.venv/bin/python" -c 'import dmgbuild' 2>/dev/null || "$HERE/.venv/bin/pip" install -q dmgbuild
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
