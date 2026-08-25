#!/usr/bin/env bash
# Cross-compile tqflicker.dll (shipped as winmm.dll) for 32-bit Windows.
#
# The build ends by reading its own export table back and comparing it, name for
# name, against the real winmm it was generated from. That check is not optional
# polish: a proxy whose export list does not match is a game that refuses to
# start, and the plan (docs/plans/stage-2-proxy.md) says to verify it **before
# ever launching the game**. It costs a second.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/New Bottle}"
REAL="${TQ_REAL_WINMM:-$BOTTLE/drive_c/windows/syswow64/winmm.dll}"
CXX=i686-w64-mingw32-g++
OUT=build/winmm.dll
BUILD_ID="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"

command -v "$CXX" >/dev/null || { echo "missing $CXX - run: npm run doctor" >&2; exit 1; }

bash scripts/gen-winmm-proxy.sh build/gen "$REAL"
bash scripts/gen-slots.sh build/gen

mkdir -p build
"$CXX" -shared -o "$OUT" \
  build/gen/winmm.def \
  src/dllmain.cpp src/log.cpp src/winmm_proxy.cpp \
  src/patch.cpp src/modules.cpp src/device.cpp src/frames.cpp \
  build/gen/winmm_stubs.S \
  -I src -I build/gen \
  -DTQFLICKER_BUILD="\"$BUILD_ID\"" \
  -O2 -Wall -Wextra \
  -static -static-libgcc -static-libstdc++ \
  -fno-exceptions \
  -Wl,--exclude-all-symbols

# ---------------------------------------------------------------- verify
exports_of() {
  i686-w64-mingw32-objdump -p "$1" \
    | awk '/\[Ordinal\/Name Pointer\] Table/,0' \
    | awk '/^\t\[/ {print $NF}' | sort
}

if ! i686-w64-mingw32-objdump -f "$OUT" | grep -q 'pei-i386'; then
  echo "FAIL: $OUT is not a 32-bit x86 PE" >&2
  exit 1
fi

diff <(exports_of "$REAL") <(exports_of "$OUT") > build/exports.diff || {
  echo "FAIL: our export table does not match the real winmm:" >&2
  head -20 build/exports.diff >&2
  exit 1
}

n=$(exports_of "$OUT" | wc -l | tr -d ' ')
size=$(stat -f %z "$OUT")
echo "built $OUT  ($BUILD_ID, ${size} bytes)"
echo "  exports: $n, identical to $(basename "$REAL")"
echo "  arch:    $(i686-w64-mingw32-objdump -f "$OUT" | awk '/file format/{print $NF}')"
