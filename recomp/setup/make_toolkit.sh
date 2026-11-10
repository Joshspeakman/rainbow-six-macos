#!/bin/bash
# make_toolkit.sh <out.tar.xz>: the compiler and Python an app carries so setup needs nothing else on the player's Mac.
# Open source only: LLVM (Apache 2.0 with LLVM exception), CPython (PSF), Capstone (BSD).
set -euo pipefail
OUT="$1"; CACHE="${TOOLKIT_CACHE:-$HOME/ProjectBuilds/_toolkit/cache}"; mkdir -p "$CACHE"
LLVM=LLVM-23.1.5-macOS-ARM64.tar.zst; LLVM_URL=https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.5/$LLVM
PY=cpython-3.12.16+20261026-aarch64-apple-darwin-install_only_stripped.tar.gz; PY_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20261026/$PY
[ -f "$CACHE/$LLVM" ] || curl -fL -o "$CACHE/$LLVM" "$LLVM_URL"
[ -f "$CACHE/$PY" ] || curl -fL -o "$CACHE/$PY" "$PY_URL"
T="$(mktemp -d "${TMPDIR:-/tmp}/toolkit.XXXXXX")"; trap 'rm -rf "${T:?}"' EXIT
mkdir -p "$T/llvm"
zstd --long=31 -dc "$CACHE/$LLVM" | tar -xf - -C "$T/llvm" --strip-components=1 \
  --include='*/bin/clang' --include='*/bin/clang-[0-9]*' --include='*/bin/lld' --include='*/bin/ld64.lld' \
  --include='*/lib/clang/*/include/stdint.h' --include='*/lib/clang/*/include/stddef.h' --include='*/lib/clang/*/include/__stddef_*' \
  --include='*/lib/clang/*/include/stdarg.h' --include='*/lib/clang/*/include/__stdarg_*' \
  --include='*/LICENSE.TXT' 2>/dev/null || true
[ -x "$T/llvm/bin/clang" ] && [ -x "$T/llvm/bin/ld64.lld" ] || { echo "the compiler didn't unpack"; exit 1; }
tar -xf "$CACHE/$PY" -C "$T"
"$T/python/bin/python3" -m pip install -q --disable-pip-version-check --target "$T/python/lib/python3.12/site-packages" capstone==5.0.7
find "$T/python" -name '__pycache__' -type d -prune -exec rm -rf {} +
P="${T:?}/python"; rm -rf "${P:?}/lib/python3.12/test" "${P:?}/lib/python3.12/idlelib" "${P:?}/lib/python3.12/tkinter" "${P:?}/share"
find "$P/lib" -maxdepth 1 \( -name 'tcl*' -o -name 'tk*' -o -name 'itcl*' \) -exec rm -rf {} +
tar -C "$T" -cf - llvm python | xz -T0 -6 > "$OUT"
ls -la "$OUT" | awk '{printf "toolkit: %.0f MB\n", $5 / 1048576}'
