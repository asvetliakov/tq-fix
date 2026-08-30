#!/usr/bin/env bash
# Cross-compile the minimal 32-bit fix, shipped as winmm.dll.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
REAL="${TQ_REAL_WINMM:-$BOTTLE/drive_c/windows/syswow64/winmm.dll}"
NATIVE="${TQ_NATIVE_WINMM:-winmm-x32.dll}"
MANIFEST=src/winmm_exports.txt
CXX=i686-w64-mingw32-g++
OUT=build/winmm.dll

command -v "$CXX" >/dev/null || { echo "missing $CXX - run: npm run doctor" >&2; exit 1; }

rm -rf build/gen
bash scripts/gen-winmm-proxy.sh build/gen "$MANIFEST"

# Embed the exact SMAA revision and its canonical lookup tables. Keeping the
# large byte tables compressed in the source tree makes reviews and clones
# considerably smaller; build/gen is disposable.
base64 -D -i third_party/smaa/AreaTex.h.gz.b64 | gzip -dc > build/gen/AreaTex.h
base64 -D -i third_party/smaa/SearchTex.h.gz.b64 | gzip -dc > build/gen/SearchTex.h
xxd -i third_party/smaa/SMAA.hlsl > build/gen/smaa_source.h

mkdir -p build
"$CXX" -shared -o "$OUT" \
  build/gen/winmm.def \
  src/fix.cpp src/dxbc_patch.cpp src/frustum_fix.cpp src/hdr.cpp src/streaming.cpp src/visual.cpp \
  build/gen/winmm_stubs.S \
  -I src -I build/gen \
  -O2 -DNDEBUG -Wall -Wextra \
  -static -static-libgcc -static-libstdc++ \
  -fno-exceptions \
  -Wl,--strip-all,--exclude-all-symbols

# ---------------------------------------------------------------- verify
exports_of() {
  i686-w64-mingw32-objdump -p "$1" \
    | awk '/\[Ordinal\/Name Pointer\] Table/,0' \
    | awk '/^\t\[/ {print $NF}' | sort -u
}

manifest_names() {
  awk '$1 == "required" || $1 == "optional" {print $2}' "$MANIFEST" | sort -u
}

required_names() {
  awk '$1 == "required" {print $2}' "$MANIFEST" | sort -u
}

validated_references=""
validate_reference() {
  local reference="$1"
  local label="$2"
  [ -f "$reference" ] || return 0

  if ! i686-w64-mingw32-objdump -f "$reference" | grep -q 'pei-i386'; then
    echo "FAIL: $label reference is not a 32-bit x86 PE: $reference" >&2
    exit 1
  fi

  local uncovered
  uncovered="$(comm -23 <(exports_of "$reference") <(manifest_names))"
  if [ -n "$uncovered" ]; then
    echo "FAIL: manifest does not cover $label exports:" >&2
    head -20 <<<"$uncovered" >&2
    exit 1
  fi

  local missing_required
  missing_required="$(comm -23 <(required_names) <(exports_of "$reference"))"
  if [ -n "$missing_required" ]; then
    echo "FAIL: $label is missing required Titan Quest exports:" >&2
    head -20 <<<"$missing_required" >&2
    exit 1
  fi

  validated_references="${validated_references:+$validated_references, }$label"
}

if ! i686-w64-mingw32-objdump -f "$OUT" | grep -q 'pei-i386'; then
  echo "FAIL: $OUT is not a 32-bit x86 PE" >&2
  exit 1
fi

if i686-w64-mingw32-objdump -h "$OUT" | grep -q '\.debug_'; then
  echo "FAIL: $OUT contains debug sections" >&2
  exit 1
fi

diff <(manifest_names) <(exports_of "$OUT") > build/exports.diff || {
  echo "FAIL: proxy export table does not match $MANIFEST:" >&2
  head -20 build/exports.diff >&2
  exit 1
}

validate_reference "$REAL" "CrossOver"
validate_reference "$NATIVE" "native Windows x86"

n=$(exports_of "$OUT" | wc -l | tr -d ' ')
size=$(stat -f %z "$OUT")
echo "built $OUT  (${size} bytes)"
echo "  exports: $n named exports from the Windows/CrossOver union"
echo "  refs:    ${validated_references:-not available (manifest-only validation)}"
echo "  arch:    $(i686-w64-mingw32-objdump -f "$OUT" | awk '/file format/{print $NF}')"
