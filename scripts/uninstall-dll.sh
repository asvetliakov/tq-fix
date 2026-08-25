#!/usr/bin/env bash
# Put the game directory back exactly as it was.
#
# This is part of Stage 2's gate, not a convenience. A proxy that cannot be
# cleanly removed makes every later "is it our fault?" question unanswerable.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/New Bottle}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
GAME="$BOTTLE/drive_c/Program Files (x86)/Steam/steamapps/common/Titan Quest Anniversary Edition"

if [ -f "$GAME/winmm.dll" ]; then
  if [ -f "$GAME/winmm.dll.tqflicker-installed" ]; then
    rm -f "$GAME/winmm.dll" "$GAME/winmm.dll.tqflicker-installed"
    echo "removed our winmm.dll"
    if [ -f "$GAME/winmm.dll.orig" ]; then
      mv "$GAME/winmm.dll.orig" "$GAME/winmm.dll"
      echo "restored the original winmm.dll"
    fi
  else
    # Refuse rather than repair: an unmarked winmm.dll is not ours and deleting
    # it would be destroying something we cannot put back.
    echo "winmm.dll is present but NOT marked as ours - leaving it alone." >&2
    echo "  $GAME/winmm.dll" >&2
    exit 1
  fi
else
  echo "no winmm.dll in the game directory"
fi

REG="$BOTTLE/drive_c/tqflicker-unoverride.reg"
# The leading '-' on the key deletes it.
cat > "$REG" <<'EOF'
REGEDIT4

[-HKEY_CURRENT_USER\Software\Wine\AppDefaults\TQ.exe\DllOverrides]
EOF
"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  -- regedit 'C:\tqflicker-unoverride.reg' >/dev/null 2>&1 || true
sleep 2
rm -f "$REG"
echo "removed the TQ.exe winmm DLL override"
# wineserver keeps the registry in memory and writes user.reg lazily, so
# grepping that file immediately after this will still show the old value and
# make a working uninstall look broken. Give it a few seconds before checking.
echo "(user.reg is written lazily by wineserver - allow a few seconds before grepping it)"
