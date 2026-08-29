#!/usr/bin/env bash
# Build and package the upload-ready Nexus Mods archive.
set -euo pipefail
cd "$(dirname "$0")/.."

command -v node >/dev/null || { echo "missing node" >&2; exit 1; }
command -v zip >/dev/null || { echo "missing zip" >&2; exit 1; }
command -v unzip >/dev/null || { echo "missing unzip" >&2; exit 1; }

RELEASE_VERSION="$(node -p "require('./package.json').version")"
ARCHIVE="dist/tq-dx11-fix-v${RELEASE_VERSION}.zip"

bash scripts/build.sh
mkdir -p dist
rm -f "$ARCHIVE"
zip -j -9 "$ARCHIVE" build/winmm.dll >/dev/null

CONTENTS="$(unzip -Z1 "$ARCHIVE")"
if [ "$CONTENTS" != "winmm.dll" ]; then
  echo "FAIL: release archive contains unexpected files:" >&2
  echo "$CONTENTS" >&2
  exit 1
fi

SIZE="$(stat -f %z "$ARCHIVE")"
echo "released $ARCHIVE  (${SIZE} bytes)"
echo "  contents: winmm.dll"
