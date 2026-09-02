#!/usr/bin/env bash
# Regenerates research/streaming/generated/ from a local supported installation.
# The game-directory resolution, hash pinning, Ghidra/JDK discovery, export
# verification, and PE dumping all live in research/tools/audit.sh.
#
# The first run imports and fully analyzes Engine.dll and takes roughly 40
# minutes; later runs reuse build/streaming-audit/ghidra and only re-export,
# which takes a few minutes.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
audit="$repo/research/streaming"
game="${TQ_GAME_DIR:-}"
if [ -z "$game" ]; then
  echo 'Set TQ_GAME_DIR to the folder containing TQ.exe, Engine.dll, and Game.dll.' >&2
  exit 2
fi

export AUDIT_PROJECT_DIR="$repo/build/streaming-audit/ghidra"
export AUDIT_ANALYSIS_TIMEOUT="${AUDIT_ANALYSIS_TIMEOUT:-3600}"

"$repo/research/tools/audit.sh" tq-engine \
  "$game/Engine.dll" "$audit/seeds.txt" "$audit/generated" \
  "$game/Engine.dll=engine-pe.txt"

echo "Streaming audit generated in $audit/generated"
