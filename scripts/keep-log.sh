#!/usr/bin/env bash
# Copy both in-game logs into cache/logs under a label, before anything can
# overwrite them.
#
#   scripts/keep-log.sh stage4-run1
#
# Risk 12 in RUNBOOK.md: a completed trace run was once destroyed by an `rm`
# meant to clear stale logs. The frames table is truncated on every device
# creation and the main log is appended forever, so the table in particular is
# one launch away from gone. Keep first, then launch.
set -euo pipefail
cd "$(dirname "$0")/.."
LABEL="${1:?label, e.g. stage4-run1}"
BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
T="$BOTTLE/drive_c/users/crossover/AppData/Local/Temp"
STAMP="$(date +%H%M%S)"
mkdir -p cache/logs
for f in tqflicker.log tqflicker-frames.log; do
  if [ -f "$T/$f" ]; then
    dst="cache/logs/${LABEL}-${STAMP}-${f%.log}.${f#tqflicker}"
    dst="cache/logs/${LABEL}-${STAMP}-${f}"
    cp "$T/$f" "$dst"
    echo "kept $dst  ($(wc -l < "$dst" | tr -d ' ') lines)"
  else
    echo "no $f in %TEMP%"
  fi
done
