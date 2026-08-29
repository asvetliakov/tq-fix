#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
GAME="${TQ_GAME:-$BOTTLE/drive_c/GOG Games/Titan Quest - Anniversary Edition}"

if [ -f "$GAME/winmm.dll.tqflicker-installed" ]; then
  rm -f "$GAME/winmm.dll" "$GAME/winmm.dll.tqflicker-installed"
  if [ -f "$GAME/winmm.dll.orig" ]; then
    mv "$GAME/winmm.dll.orig" "$GAME/winmm.dll"
  fi
fi

REG="$BOTTLE/drive_c/tqflicker-unoverride.reg"
printf '%s\n' \
  'REGEDIT4' \
  '' \
  '[-HKEY_CURRENT_USER\Software\Wine\AppDefaults\TQ.exe\DllOverrides]' > "$REG"
"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  -- regedit 'C:\tqflicker-unoverride.reg' >/dev/null 2>&1 || true
sleep 2
rm -f "$REG"

echo "uninstalled Titan Quest DX11 flicker fix"
