#!/usr/bin/env bash
set -euo pipefail

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
GAME="${TQ_GAME:-$BOTTLE/drive_c/GOG Games/Titan Quest - Anniversary Edition}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"

command -v i686-w64-mingw32-g++ >/dev/null
command -v i686-w64-mingw32-objdump >/dev/null
[ -x "$CX/bin/cxstart" ]
[ -f "$GAME/TQ.exe" ]
[ -f "$GAME/Direct3D11.dll" ]
[ -f "$BOTTLE/drive_c/windows/syswow64/winmm.dll" ]
i686-w64-mingw32-objdump -f "$GAME/TQ.exe" | grep -q pei-i386
i686-w64-mingw32-objdump -f "$BOTTLE/drive_c/windows/syswow64/winmm.dll" | grep -q pei-i386

echo "doctor: ready"
