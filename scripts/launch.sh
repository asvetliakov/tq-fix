#!/usr/bin/env bash
# Launch TQ.exe directly through cxstart with whatever environment this shell
# has (docs/rev/observed.md O15), capturing the process's stderr - which is
# where Metal's validation layers write - into cache/logs/<label>-stderr.log.
#
#   MTL_DEBUG_LAYER=1 scripts/launch.sh validation-run1 [extra TQ.exe args]
#
# GOG build, 2026-08-29: there is no Steam in this bottle, so the direct route
# is the only route and O18/O24's handoff-stub caveat no longer applies.
set -euo pipefail
cd "$(dirname "$0")/.."
LABEL="${1:?label}"; shift || true
BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
GAME="${TQ_GAME:-$BOTTLE/drive_c/GOG Games/Titan Quest - Anniversary Edition}"
mkdir -p cache/logs "$BOTTLE/drive_c/dxmtlog"
export DXMT_LOG_PATH="${DXMT_LOG_PATH:-C:\\dxmtlog}"
export DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-info}"
ERR="cache/logs/${LABEL}-$(date +%H%M%S)-stderr.log"
echo "launching $(date +%T); stderr -> $ERR"
env | grep -E '^(MTL_|DXMT_|TQFLICKER_)' | sed 's/^/  /'
# cxstart wants Windows paths (O15's recipe); a Unix path here is "Path not found".
WGAME="${TQ_WGAME:-C:\\GOG Games\\Titan Quest - Anniversary Edition}"
exec "$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  --workdir "$WGAME" -- "$WGAME\\TQ.exe" "$@" 2>"$ERR"
