#!/usr/bin/env bash
# Put winmm.dll beside TQ.exe and tell Wine to prefer it.
#
# Two things are needed and the second is easy to forget: Wine prefers its own
# builtin winmm over a file in the application directory, so without a DLL
# override the proxy sits there being ignored and the symptom is *nothing at
# all* - the game runs fine and no log appears.
#
# The override is scoped to **TQ.exe only**, under AppDefaults. A bottle-wide
# `winmm=native` would also apply to Steam and to every other process here,
# which is a much larger blast radius than this needs.
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/New Bottle}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
GAME="$BOTTLE/drive_c/Program Files (x86)/Steam/steamapps/common/Titan Quest Anniversary Edition"

[ -f build/winmm.dll ] || { echo "no build/winmm.dll - run: npm run build" >&2; exit 1; }
[ -f "$GAME/TQ.exe" ]  || { echo "no TQ.exe under: $GAME" >&2; exit 1; }

# Never overwrite something that is not ours without keeping it. If the game
# ever shipped its own winmm.dll, losing it would be an unattributable break.
if [ -f "$GAME/winmm.dll" ] && [ ! -f "$GAME/winmm.dll.tqflicker-installed" ]; then
  if [ ! -f "$GAME/winmm.dll.orig" ]; then
    cp "$GAME/winmm.dll" "$GAME/winmm.dll.orig"
    echo "kept the existing winmm.dll as winmm.dll.orig"
  fi
fi

cp build/winmm.dll "$GAME/winmm.dll"
# A marker, so uninstall knows the file is ours and can remove it safely.
git rev-parse --short HEAD 2>/dev/null > "$GAME/winmm.dll.tqflicker-installed" || \
  echo nogit > "$GAME/winmm.dll.tqflicker-installed"

REG="$BOTTLE/drive_c/tqflicker-override.reg"
cat > "$REG" <<'EOF'
REGEDIT4

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\TQ.exe\DllOverrides]
"winmm"="native,builtin"
EOF
"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  -- regedit 'C:\tqflicker-override.reg' >/dev/null 2>&1 || true
sleep 2
rm -f "$REG"

echo "installed:"
echo "  $GAME/winmm.dll"
echo "  HKCU\\Software\\Wine\\AppDefaults\\TQ.exe\\DllOverrides  winmm = native,builtin"
echo
echo "the log will be at %TEMP%\\tqflicker.log inside the bottle, which is:"
echo "  $BOTTLE/drive_c/users/crossover/AppData/Local/Temp/tqflicker.log"
echo "(not users/crossover/Temp - that path does not exist here)"
echo
echo "tail it with:  npm run log"
