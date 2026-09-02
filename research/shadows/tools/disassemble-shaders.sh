#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
audit="$repo/research/shadows"
archives="${TQ_SHADER_ARCHIVES:-$repo/build/shadow-audit/archives}"
build="$repo/build/shadow-audit/dxbc-disassembler"
bytecode="$build/bytecode"
assembly="$audit/shaders/generated"
bottle="${TQ_BOTTLE_NAME:-Titan Quest}"

wine="${TQ_WINE:-}"
if [ -z "$wine" ]; then
  preview='/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine'
  release="$HOME/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine"
  if [ -x "$preview" ]; then
    wine="$preview"
  else
    wine="$release"
  fi
fi

[ -d "$archives" ] || { echo "Missing extracted shader archives: $archives" >&2; exit 2; }
[ -x "$wine" ] || { echo "CrossOver Wine wrapper not found: $wine" >&2; exit 2; }
command -v i686-w64-mingw32-g++ >/dev/null || {
  echo 'Missing i686-w64-mingw32-g++.' >&2
  exit 2
}

mkdir -p "$build" "$bytecode" "$assembly"
i686-w64-mingw32-g++ \
  -O2 -static -static-libgcc -static-libstdc++ \
  -o "$build/disassemble-dxbc.exe" \
  "$audit/tools/disassemble-dxbc.cpp"

python3 "$audit/tools/inventory-shaders.py" \
  "$archives" "$audit/shaders/inventory.csv" \
  --extract-unique "$bytecode"

"$wine" --bottle "$bottle" --no-update --no-gui \
  "$build/disassemble-dxbc.exe" "$bytecode" "$assembly"

missing=0
for shader in "$bytecode"/*.dxbc; do
  name="$(basename "$shader" .dxbc)"
  if [ ! -s "$assembly/$name.asm" ]; then
    echo "Missing disassembly: $name.asm" >&2
    missing=1
  fi
done
[ "$missing" = 0 ] || exit 5
