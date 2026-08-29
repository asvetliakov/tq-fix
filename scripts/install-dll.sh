#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
GAME="${TQ_GAME:-$BOTTLE/drive_c/GOG Games/Titan Quest - Anniversary Edition}"

[ -f build/winmm.dll ] || { echo "run npm run build first" >&2; exit 1; }
[ -f "$GAME/TQ.exe" ] || { echo "TQ.exe not found: $GAME" >&2; exit 1; }

if [ -f "$GAME/winmm.dll" ] && [ ! -f "$GAME/winmm.dll.tqflicker-installed" ]; then
  [ -f "$GAME/winmm.dll.orig" ] || cp "$GAME/winmm.dll" "$GAME/winmm.dll.orig"
fi
cp build/winmm.dll "$GAME/winmm.dll"
: > "$GAME/winmm.dll.tqflicker-installed"

REG="$BOTTLE/drive_c/tqflicker-override.reg"
printf '%s\n' \
  'REGEDIT4' \
  '' \
  '[HKEY_CURRENT_USER\Software\Wine\AppDefaults\TQ.exe\DllOverrides]' \
  '"winmm"="native,builtin"' > "$REG"
"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  -- regedit 'C:\tqflicker-override.reg' >/dev/null 2>&1 || true
sleep 2
rm -f "$REG"

echo "installed $GAME/winmm.dll"
