#!/usr/bin/env bash
# Regenerates research/shadows/generated/ from a local supported installation.
# All of the game-directory resolution, hash pinning, Ghidra/JDK discovery,
# export verification, and PE dumping lives in research/tools/audit.sh; this
# script only names the two audits the shadow work needs.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
audit="$repo/research/shadows"
game="${TQ_GAME_DIR:-}"
if [ -z "$game" ]; then
  echo 'Set TQ_GAME_DIR to the folder containing TQ.exe, Engine.dll, and Game.dll.' >&2
  exit 2
fi

# Both programs share one Ghidra project directory, as they always have.
export AUDIT_PROJECT_DIR="$repo/build/shadow-audit/ghidra"

"$repo/research/tools/audit.sh" tq-shadow \
  "$game/Engine.dll" "$audit/seeds.txt" "$audit/generated" \
  "$game/Engine.dll=engine-pe.txt" \
  "$game/Game.dll=game-pe.txt" \
  "$game/TQ.exe=tq-pe.txt"

"$repo/research/tools/audit.sh" tq-d3d11 \
  "$game/Direct3D11.dll" "$audit/direct3d11-seeds.txt" \
  "$audit/generated/direct3d11" \
  "$game/Direct3D11.dll=pe.txt"

echo "Shadow audit generated in $audit/generated"
