#!/bin/bash
# install.sh --game-dir <Dungeon Siege II folder> [--app-dir DIR] [--data-dir DIR]
# install.sh --gog-installer <setup_dungeon_siege_2_*.exe>   (GOG offline installer; needs: brew install innoextract)
# install.sh --cd-key                  multiplayer: asks for the product key of your own original retail copy (25
#                                      characters, on the sticker under disc 1; the Steam and GOG releases come without
#                                      one) and stores what the retail installer would have derived from it (the
#                                      "DoubleHash" the game presents to the GameSpy/OpenSpy lobby) in the data folder.
#                                      The key itself is not stored. Without it, multiplayer stops at "Could not find a
#                                      valid CDKey". DS2_CDKEY=<key> gives it without the prompt. The launch window
#                                      has the same entry (Online row), and a first install offers it at the end.
# install.sh --cd-key --reg-file <file.reg>   the same for a Windows (or Wine/Proton) player: writes a .reg file
#                                      to import there (double-click it), so a PC copy without the retail installer
#                                      can join the same online games. Nothing on this Mac changes.
# install.sh --nightly                 installs "Dungeon Siege II Nightly.app" next to the stable app: the newest
#                                      experimental work (the repository's nightly branch, fetched from GitHub and
#                                      built in build/nightly; this checkout is left as it is). It shares the data
#                                      folder (saves and settings) with the stable app. Run it again to update.
#
# With no game option, the game folder of an app already installed is used again (reinstall or update).
#
# Builds the natively recompiled Dungeon Siege II from your own copy of the GOG release (version 2.3) and installs
# "Dungeon Siege II Native.app" (no Wine, no Rosetta). The game folder is only read: settings, saves and logs go to the
# data folder (default ~/Games/DungeonSiege2Native). Needs the Xcode command line tools and python3 (the recompiler
# installs capstone with pip into recomp/.venv).
set -euo pipefail
if [[ $(uname -s) == Linux ]]; then
    exec python3 "$(dirname "$0")/recomp/tools/install_linux.py" "$@"
fi
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME=""; GOG_EXE=""; APPS="$HOME/Applications"; DATA="$HOME/Games/DungeonSiege2Native"
NIGHTLY=""; AS_NIGHTLY=""; CDKEY=""; REGFILE=""; ARGS=()
GOG_SHA=c17e1cc9e01b5d2439a92e06b905040379ad4bb8618b822582c46f5fa29db4e2
while [ $# -gt 0 ]; do
  case "$1" in --nightly) NIGHTLY=1; shift; continue;; --as-nightly) AS_NIGHTLY=1; shift; continue;; --cd-key) CDKEY=1; shift; continue;; esac
  ARGS+=("$1"); [ $# -gt 1 ] && case "$1" in --*) case "$2" in --*) ;; *) ARGS+=("$2");; esac;; esac
  case "$1" in
    --game-dir) GAME="$2"; shift 2;; --gog-installer) GOG_EXE="$2"; shift 2;; --app-dir) APPS="$2"; shift 2;; --data-dir) DATA="$2"; shift 2;;
    --reg-file) REGFILE="$2"; shift 2;;
    *) echo "unknown option $1"; exit 1;;
  esac
done
# the stable app, or (--as-nightly, used by --nightly) the nightly one next to it
APP_NAME="Dungeon Siege II Native"; APP_SHOWN="Dungeon Siege II"; APP_ID=native; APP_LOG=DungeonSiege2Native
[ -n "$AS_NIGHTLY" ] && { APP_NAME="Dungeon Siege II Nightly"; APP_SHOWN="Dungeon Siege II Nightly"; APP_ID=nightly; APP_LOG=DungeonSiege2Nightly; }
installed_game() {     # the game folder an installed app was set up with (for reinstalls and updates)
  local a L v x; for a in "Dungeon Siege II Native" "Dungeon Siege II Nightly"; do
    L="$APPS/$a.app/Contents/MacOS/launcher"; [ -f "$L" ] || continue
    v="$(grep -m1 '^DEF_GAME=' "$L" | cut -d= -f2-)"           # shell-quoted (printf %q)
    if [ -n "$v" ]; then eval "x=$v"; printf '%s\n' "$x"; return 0; fi
  done; return 0
}
# The CD key for online play: the retail installer's "DoubleHash" of the player's product key (the key's 25 characters,
# alphabet BCDFGHJKMPQRTVWXY2346789, read as a base-24 number into 15 little-endian bytes; SHA-1 of that, SHA-1 of the
# digest, the first 8 bytes, as its Setup.exe computes it). store_cdkey <key> <out> [reg]: the 16 hex digits into the
# data folder's cdkey-hash.txt, or (reg) a .reg file for a Windows player. Only the hash is ever written.
store_cdkey() {
  DS2_CDKEY="$1" python3 - "$2" "${3:-}" <<'PY'
import hashlib, os, sys
A = "BCDFGHJKMPQRTVWXY2346789"; key = os.environ["DS2_CDKEY"].upper().replace("-", "").replace(" ", "")
if len(key) != 25 or any(c not in A for c in key): sys.exit("That is not a product key: 25 characters from " + A + ".")
buf = bytearray(15); top = 0
for ch in key:
    c = A.index(ch)
    for i in range(top + 1):
        v = buf[i] * 24 + c; buf[i] = v & 255; c = v >> 8
    if c: top += 1; buf[top] = c
h = hashlib.sha1(hashlib.sha1(bytes(buf)).digest()).digest()[:8]
if sys.argv[2] == "reg":
    hx = ",".join("%02x" % b for b in h); out = ["REGEDIT4", ""]
    for k in ("SOFTWARE\\Wow6432Node\\Microsoft\\Microsoft Games\\DungeonSiege2", "SOFTWARE\\Microsoft\\Microsoft Games\\DungeonSiege2"):
        out += ["[HKEY_LOCAL_MACHINE\\" + k + "]", '"DoubleHash"=hex:' + hx, ""]
    open(sys.argv[1], "w", newline="\r\n").write("\n".join(out))
    print("== wrote " + sys.argv[1] + ": import it on the PC (double-click, or regedit /s) to play online with this key")
else:
    with open(sys.argv[1], "w") as f: f.write(h.hex() + "\n")
    os.chmod(sys.argv[1], 0o600)
    print("== CD key stored for online play (only its hash) in " + sys.argv[1])
PY
}
ask_cdkey() {   # $1: prompt prefix; reads the key without echoing it
  local K="${DS2_CDKEY:-}"
  if [ -z "$K" ]; then printf '%s' "$1" >&2; IFS= read -rs K; echo >&2; fi   # the prompt on stderr: stdout is the key
  printf '%s' "$K"
}
if [ -n "$CDKEY" ]; then
  K="$(ask_cdkey 'Product key (25 characters, dashes optional): ')"
  if [ -n "$REGFILE" ]; then store_cdkey "$K" "$REGFILE" reg || exit 1; exit 0; fi
  mkdir -p "$DATA"; store_cdkey "$K" "$DATA/cdkey-hash.txt" || exit 1
  [ -n "$GAME$GOG_EXE" ] || exit 0
fi
# --nightly: fetch the nightly branch into build/nightly and let its own install.sh install the nightly app
if [ -n "$NIGHTLY" ] && [ -z "$AS_NIGHTLY" ]; then
  command -v git >/dev/null || { echo "git is needed for --nightly"; exit 1; }
  W="$HERE/build/nightly"; echo "== fetching the nightly branch"
  git -C "$HERE" fetch -q origin nightly
  git -C "$HERE" worktree prune
  if [ -e "$W/.git" ]; then git -C "$W" checkout -q -f --detach origin/nightly
  else mkdir -p "$HERE/build"; git -C "$HERE" worktree add -q -f --detach "$W" origin/nightly; fi
  echo "== nightly: $(git -C "$W" log -1 --format='%h %s')"
  [ -e "$W/recomp/.venv" ] || [ ! -d "$HERE/recomp/.venv" ] || ln -s "$HERE/recomp/.venv" "$W/recomp/.venv"   # share the tools
  if [ -z "$GAME$GOG_EXE" ]; then G="$(installed_game)"; [ -n "$G" ] && ARGS+=(--game-dir "$G"); fi
  exec "$W/install.sh" --as-nightly ${ARGS[@]+"${ARGS[@]}"}
fi
if [ -n "$GOG_EXE" ]; then       # unpack the installer into the data folder; the app reads the game from there
  [ -f "$GOG_EXE" ] || { echo "no such file: $GOG_EXE"; exit 1; }
  command -v innoextract >/dev/null || { echo "innoextract is needed for --gog-installer: brew install innoextract"; exit 1; }
  X="$DATA/gog-extract"; rm -rf "$X"; mkdir -p "$X"; echo "== unpacking $GOG_EXE"
  innoextract --gog -s -d "$X" "$GOG_EXE"
  GAME="$(dirname "$(find "$X" -name DungeonSiege2.exe -print -quit)")"
  [ -f "$GAME/DungeonSiege2.exe" ] || { echo "DungeonSiege2.exe not found in the installer"; exit 1; }
fi
[ -n "$GAME" ] || GAME="$(installed_game)"
[ -n "$GAME" ] || { echo "usage: install.sh --game-dir <Dungeon Siege II folder> | --gog-installer <setup_dungeon_siege_2_*.exe>"; exit 1; }
GAME="$(cd "$GAME" && pwd)"
for tool in clang python3; do command -v $tool >/dev/null || { echo "$tool is missing: install the Xcode command line tools (xcode-select --install)"; exit 1; }; done
[ "$(uname -m)" = arm64 ] || { echo "the native build is for Apple Silicon Macs"; exit 1; }
EXE="$GAME/DungeonSiege2.exe"
[ -f "$EXE" ] || { echo "no DungeonSiege2.exe in $GAME"; exit 1; }
[ "$(shasum -a 256 "$EXE" | cut -d' ' -f1)" = "$GOG_SHA" ] || { echo "$EXE is not the GOG Dungeon Siege II 2.3 release"; exit 1; }
echo "== recompiling $EXE (a few minutes)"
"$HERE/recomp/build.sh" "$EXE"
"$HERE/recomp/tools/build_app.sh" "$HERE/recomp/work/full" >/dev/null

A="$APPS/$APP_NAME.app"; echo "== installing $A"
mkdir -p "$APPS"; rm -rf "$A"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources"
cp "$HERE/recomp/work/full/app/DungeonSiegeNative" "$HERE/recomp/work/full/app/shaders.metal" "$HERE/recomp/work/full/app/dsr_snapshot.bin" "$A/Contents/MacOS/"
cp "$HERE/recomp/work/full/DungeonSiege2.exe" "$A/Contents/Resources/DungeonSiege2.exe"
Q_GAME=$(printf '%q' "$GAME"); Q_DATA=$(printf '%q' "$DATA")      # shell-quoted: any folder name is only data
# updates from the launcher: the branch this app follows (main, or nightly for Nightly), the commit it was built from and
# the checkout it was installed from; its updater fetches the branch into a worktree there and rebuilds the app from it
U_BRANCH=main; U_DIR=stable; [ -n "$AS_NIGHTLY" ] && { U_BRANCH=nightly; U_DIR=nightly; }
U_REPO="$(git -C "$HERE" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"; U_REPO="${U_REPO%/.git}"
U_BUILT="$(git -C "$HERE" rev-parse HEAD 2>/dev/null || true)"
if [ -n "$U_REPO" ] && [ -n "$U_BUILT" ] && git -C "$U_REPO" remote get-url origin >/dev/null 2>&1; then
  cat > "$A/Contents/MacOS/update" <<UPDATE
#!/bin/bash
# $APP_NAME updater (generated by install.sh): "update --check" prints the newest commit of $U_BRANCH on GitHub;
# "update" fetches it into a worktree of the checkout this app was installed from and rebuilds the app from it
set -euo pipefail
REPO=$(printf '%q' "$U_REPO"); BRANCH=$U_BRANCH; W="\$REPO/build/$U_DIR"
export GIT_TERMINAL_PROMPT=0                              # never wait for a password prompt
if [ "\${1:-}" = --check ]; then git -C "\$REPO" ls-remote origin "refs/heads/\$BRANCH" | cut -f1; exit 0; fi
echo "== fetching \$BRANCH from GitHub"
git -C "\$REPO" fetch -q origin "\$BRANCH"
git -C "\$REPO" worktree prune
if [ -e "\$W/.git" ]; then git -C "\$W" checkout -q -f --detach "origin/\$BRANCH"
else mkdir -p "\$REPO/build"; git -C "\$REPO" worktree add -q -f --detach "\$W" "origin/\$BRANCH"; fi
echo "== \$BRANCH: \$(git -C "\$W" log -1 --format='%h %s')"
[ -e "\$W/recomp/.venv" ] || [ ! -d "\$REPO/recomp/.venv" ] || ln -s "\$REPO/recomp/.venv" "\$W/recomp/.venv"   # share the tools
exec "\$W/install.sh"$( [ -n "$AS_NIGHTLY" ] && echo " --as-nightly" )
UPDATE
  chmod +x "$A/Contents/MacOS/update"
fi
cat > "$A/Contents/MacOS/launcher" <<LAUNCH
#!/bin/bash
# $APP_NAME launcher (generated by install.sh)
DEF_GAME=$Q_GAME
DEF_DATA=$Q_DATA
GAME="\${DS_GAME_DIR:-\$DEF_GAME}"; DATA="\${DS_NATIVE_DATA:-\$DEF_DATA}"; DOCS="\$DATA/drive_c/Users/player/Documents"
mkdir -p "\$DOCS"
L="\$DATA/$APP_LOG.log"                                   # keep the log small: previous one as .old
[ -f "\$L" ] && [ "\$(stat -f %z "\$L")" -gt 5000000 ] && mv -f "\$L" "\$DATA/$APP_LOG.old.log"
B="\$(cd "\$(dirname "\$0")" && pwd)"
export DS_LOG_FILE="\$L"                                    # crash reports attach this app's own log
[ -x "\$B/update" ] && export DS_UPDATER="\$B/update" DS_UPDATE_BRANCH=$U_BRANCH DS_BUILT_COMMIT=$U_BUILT   # the launcher's Updates row
exec "\$B/DungeonSiegeNative" --exe "\$B/../Resources/DungeonSiege2.exe" --game "\$GAME" --data "\$DATA" 2>>"\$L"
LAUNCH
chmod +x "$A/Contents/MacOS/launcher"
cat > "$A/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_SHOWN</string>
  <key>CFBundleIdentifier</key><string>io.github.dungeon-siege-2-macos.$APP_ID</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>2.3</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSLocalNetworkUsageDescription</key><string>Dungeon Siege II looks for and hosts multiplayer games on your local network.</string>
</dict></plist>
PLIST
I="$(mktemp -d)"
if python3 "$HERE/recomp/tools/extract_icon.py" "$EXE" "$I/ds.ico" 2>/dev/null && sips -s format png "$I/ds.ico" --out "$I/ds.png" >/dev/null 2>&1; then
  sips -z 512 512 "$I/ds.png" --out "$I/ds512.png" >/dev/null
  if [ -n "$AS_NIGHTLY" ] && clang -fobjc-arc -framework AppKit "$HERE/recomp/tools/nightly_icon.m" -o "$I/nightly_icon" 2>/dev/null \
     && "$I/nightly_icon" "$I/ds512.png" "$I/n.png" && sips -z 512 512 "$I/n.png" --out "$I/ds512.png" >/dev/null; then :; fi   # dark tile, NIGHTLY band
  sips -s format icns "$I/ds512.png" --out "$A/Contents/Resources/icon.icns" >/dev/null 2>&1 || true
fi
rm -rf "$I"
codesign --force --deep --sign - "$A" >/dev/null 2>&1 || true
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$A" 2>/dev/null || true
echo "== done: $A (data in $DATA)"
# online play needs the player's own CD key: offered once, on an interactive install (not by the launcher's updater)
if [ ! -f "$DATA/cdkey-hash.txt" ] && [ -t 0 ] && [ -z "$AS_NIGHTLY$CDKEY" ]; then
  echo "== Online play needs the CD key of an original retail copy (25 characters, on the sticker under disc 1; Steam"
  echo "   and GOG copies have none). Press Return to skip;"
  echo "   you can add it later in the launch window (Online) or with ./install.sh --cd-key."
  K="$(ask_cdkey 'Product key: ')"
  [ -n "$K" ] && { store_cdkey "$K" "$DATA/cdkey-hash.txt" || echo "   (not stored; try again from the launch window)"; }
fi
