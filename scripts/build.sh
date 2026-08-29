#!/usr/bin/env bash
# Cross-compile the minimal 32-bit fix, shipped as winmm.dll.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
REAL="${TQ_REAL_WINMM:-$BOTTLE/drive_c/windows/syswow64/winmm.dll}"
CXX=i686-w64-mingw32-g++
OUT=build/winmm.dll

command -v "$CXX" >/dev/null || { echo "missing $CXX - run: npm run doctor" >&2; exit 1; }

rm -rf build/gen
bash scripts/gen-winmm-proxy.sh build/gen "$REAL"

# Embed the exact SMAA revision and its canonical lookup tables. Keeping the
# large byte tables compressed in the source tree makes reviews and clones
# considerably smaller; build/gen is disposable.
base64 -D -i third_party/smaa/AreaTex.h.gz.b64 | gzip -dc > build/gen/AreaTex.h
base64 -D -i third_party/smaa/SearchTex.h.gz.b64 | gzip -dc > build/gen/SearchTex.h
xxd -i third_party/smaa/SMAA.hlsl > build/gen/smaa_source.h

mkdir -p build
"$CXX" -shared -o "$OUT" \
  build/gen/winmm.def \
  src/fix.cpp src/dxbc_patch.cpp src/visual.cpp \
  build/gen/winmm_stubs.S \
  -I src -I build/gen \
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
echo "built $OUT  (${size} bytes)"
echo "  exports: $n, identical to $(basename "$REAL")"
echo "  arch:    $(i686-w64-mingw32-objdump -f "$OUT" | awk '/file format/{print $NF}')"
