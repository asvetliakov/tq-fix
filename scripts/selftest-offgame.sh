#!/usr/bin/env bash
# Load the proxy and prove it forwards, with no game running.
#
# Costs seconds instead of a play session, and should be run after every build.
# It catches the failures that would otherwise present as "the game does not
# start", which is the least informative symptom this project can produce.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/New Bottle}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
DRIVE="$BOTTLE/drive_c"
WORK="$DRIVE/tqflicker-selftest"
REPORT_WIN='C:\tqflicker-selftest\report.txt'
LOG_WIN='C:\tqflicker-selftest\tqflicker.log'

[ -f build/winmm.dll ] || { echo "no build/winmm.dll - run: npm run build" >&2; exit 1; }

i686-w64-mingw32-gcc -o build/selftest.exe test/selftest.c -I build/gen -O2 -Wall -DCINTERFACE -DCOBJMACROS \
  -ld3d11 -ldxgi -lgdi32 -luser32

rm -rf "$WORK"; mkdir -p "$WORK"
cp build/winmm.dll build/selftest.exe "$WORK/"

# TQFLICKER_LOG keeps this out of the developer's own %TEMP% log, so a self-test
# never pollutes the file that is meant to describe a real game run.
TQFLICKER_LOG="$LOG_WIN" TQFLICKER_D3D_HOST=selftest.exe "$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  --workdir 'C:\tqflicker-selftest' \
  -- 'C:\tqflicker-selftest\selftest.exe' 'C:\tqflicker-selftest\winmm.dll' "$REPORT_WIN" \
  >/dev/null 2>&1 || true

# cxstart returns before the guest process finishes.
for _ in $(seq 1 120); do grep -q "^RESULT" "$WORK/report.txt" 2>/dev/null && break; sleep 0.25; done

if [ ! -s "$WORK/report.txt" ]; then
  echo "FAIL: the self-test produced no report - it did not run, or could not write." >&2
  exit 1
fi

echo "--- self-test report ---"
cat "$WORK/report.txt"
if [ -f "$WORK/tqflicker.log" ]; then
  echo "--- our DLL's own log ---"
  cat "$WORK/tqflicker.log"
  T="$BOTTLE/drive_c/users/crossover/AppData/Local/Temp/tqflicker-frames.log"
  if [ -f "$T" ]; then echo "--- frames table ($T) ---"; cat "$T"; fi
else
  echo "FAIL: our DLL wrote no log - DllMain did not run, or the log path was unwritable." >&2
  exit 1
fi

# Read the whole report, not a grep for "fail" - the sibling repo records two
# real failures missed exactly that way (../grimdawn-trash/CLAUDE.md).
grep -q '^RESULT: 0 failure' "$WORK/report.txt" || { echo "SELF-TEST FAILED" >&2; exit 1; }
echo "self-test: pass"
