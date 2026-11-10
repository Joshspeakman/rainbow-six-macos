#!/bin/bash
# The app's updater (Contents/MacOS/update). "update --check" prints the newest release's tag; "update" downloads it and
# swaps it in. Saves and game files live outside the app, and the next launch rebuilds the game only if its code changed.
set -euo pipefail
REPO="@REPO@"; ASSET="@ASSET@"
API="${DS_UPDATE_API:-https://api.github.com}"; WEB="${DS_UPDATE_WEB:-https://github.com}"   # tests point these at a local server
APP="$(cd "$(dirname "$0")/../.." && pwd)"
if [ "${1:-}" = --check ]; then
  curl -fsSL -m 20 "$API/repos/$REPO/releases/latest" | /usr/bin/plutil -extract tag_name raw -o - -
  exit 0
fi
[ -w "$(dirname "$APP")" ] || { echo "== can't write to $(dirname "$APP"): drag the new version in from the download page instead"; exit 1; }
T="$(mktemp -d)"; trap 'rm -rf "${T:?}"' EXIT
echo "== downloading the newest version"
curl -fsSL -m 1800 -o "$T/new.zip" "$WEB/$REPO/releases/latest/download/$ASSET"
echo "== unpacking"
/usr/bin/ditto -x -k "$T/new.zip" "$T/x"
NEW="$(/usr/bin/find "$T/x" -maxdepth 2 -name '*.app' -type d | /usr/bin/head -1)"
[ -n "$NEW" ] && [ -x "$NEW/Contents/MacOS/update" ] || { echo "== that download doesn't look right"; exit 1; }
echo "== installing"
mv "$APP" "$T/old.app"
mv "$NEW" "$APP" || { mv "$T/old.app" "$APP"; echo "== couldn't replace the app"; exit 1; }
echo "== done"
