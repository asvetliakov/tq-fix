#!/usr/bin/env bash
# Reports how many of the extracted shader corpus a DXBC transform accepts.
# Every transform targets exactly one program, so anything but 1 is a defect.
# Transforms: deferred, contact, receiver, pcf, bones.
#
# The corpus is not committed; produce it first with
#   research/shadows/tools/disassemble-shaders.sh
set -euo pipefail
cd "$(dirname "$0")/.."

TRANSFORM="${1:-deferred}"
# The full corpus by default. disassemble-shaders.sh produces only the
# shadow-bound subset, which cannot show a grass or terrain transform is unique.
CORPUS="${TQ_SHADER_BYTECODE:-build/shadow-audit/all-dxbc}"

[ -d "$CORPUS" ] || {
  echo "no shader corpus at $CORPUS" >&2
  echo "build it with research/shadows/tools/extract-all-dxbc.py" >&2
  exit 2
}

mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -o build/corpus-match \
  tools/corpus-match.cpp src/dxbc_patch.cpp \
  -I src -I tools/win_shim

build/corpus-match "$TRANSFORM" "$CORPUS"
