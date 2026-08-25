#!/usr/bin/env bash
# What this machine is missing, before a stage is spent finding out.
#
# Exits non-zero if anything REQUIRED is absent. Optional things are reported
# and forgiven, with the stage they matter to.
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/New Bottle}"
GAME="$BOTTLE/drive_c/Program Files (x86)/Steam/steamapps/common/Titan Quest Anniversary Edition"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
fail=0

ok()   { printf '  \033[32mok\033[0m    %s\n' "$1"; }
bad()  { printf '  \033[31mMISS\033[0m  %s\n' "$1"; fail=1; }
note() { printf '  \033[33m--\033[0m    %s\n' "$1"; }

echo "toolchain"
if command -v i686-w64-mingw32-g++ >/dev/null; then
  ok "i686-w64-mingw32-g++ $(i686-w64-mingw32-g++ -dumpversion)  (32-bit: this is the one we need)"
else
  bad "i686-w64-mingw32-g++ - brew install mingw-w64"
fi
command -v i686-w64-mingw32-objdump >/dev/null \
  && ok "i686-w64-mingw32-objdump" || bad "i686-w64-mingw32-objdump"
command -v node >/dev/null && ok "node $(node --version)" || bad "node"

echo "bottle"
[ -d "$BOTTLE" ] && ok "$BOTTLE" || bad "bottle not found: $BOTTLE  (set TQ_BOTTLE)"
[ -x "$CX/bin/cxstart" ] && ok "cxstart  (per-run env injection, docs/rev/observed.md O15)" \
                         || bad "cxstart not found under $CX"

W="$BOTTLE/drive_c/windows/syswow64/winmm.dll"
if [ -f "$W" ]; then
  if i686-w64-mingw32-objdump -f "$W" 2>/dev/null | grep -q pei-i386; then
    n=$(i686-w64-mingw32-objdump -p "$W" | awk '/\[Ordinal\/Name Pointer\] Table/,0' \
        | awk '/^\t\[/ {print $NF}' | wc -l | tr -d ' ')
    ok "syswow64/winmm.dll  32-bit, $n exports  (the proxy is generated from this)"
  else
    bad "syswow64/winmm.dll is not 32-bit x86"
  fi
else
  bad "no syswow64/winmm.dll - on an ARM64 bottle system32's is Aarch64, we need syswow64's"
fi

echo "game"
if [ -f "$GAME/TQ.exe" ]; then
  if i686-w64-mingw32-objdump -f "$GAME/TQ.exe" 2>/dev/null | grep -q pei-i386; then
    ok "TQ.exe  PE32 (32-bit - the single most consequential fact in the repo)"
  else
    bad "TQ.exe is not PE32 - every assumption in CLAUDE.md needs rechecking"
  fi
else
  bad "TQ.exe not found under $GAME"
fi
[ -f "$GAME/Direct3D11.dll" ] && ok "Direct3D11.dll  (Stage 3 waits for this module)" \
                              || bad "Direct3D11.dll not found"
if [ -f "$GAME/winmm.dll" ]; then
  note "winmm.dll IS INSTALLED in the game directory - scripts/uninstall-dll.sh removes it"
else
  ok "no winmm.dll in the game directory (clean)"
fi

[ -x node_modules/.bin/tsx ] && ok "node_modules present (npm run log / frames / typecheck)" \
                              || note "no node_modules - npm install, or npm run log/frames/typecheck will not run"

echo "measurement  (docs/rev/observed.md O12 - the instrument)"
command -v ffmpeg >/dev/null && ok "ffmpeg $(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')" \
                             || note "ffmpeg absent - needed to count frames in a recording"
if [ -f "$GAME/dxmt.conf" ]; then
  if grep -qE '^\s*d3d11\.preferredMaxFrameRate' "$GAME/dxmt.conf"; then
    note "dxmt.conf has an ACTIVE frame-rate cap - measuring mode, not normal play"
  else
    ok "dxmt.conf present, cap commented out (normal play)"
  fi
else
  note "no dxmt.conf - fine; add one to cap the frame rate for measuring"
fi

if [ -x cache/venv/bin/python ] && cache/venv/bin/python -c 'import numpy' 2>/dev/null; then
  ok "cache/venv has numpy (tools/recording.py)"
else
  note "no cache/venv with numpy - python3 -m venv cache/venv && cache/venv/bin/pip install numpy  (tools/recording.py needs it)"
fi
G_FRAMES="$BOTTLE/drive_c/users/crossover/AppData/Local/Temp/tqflicker-frames.log"
[ -f "$G_FRAMES" ] && note "a frames table exists from the last run - npm run keep-log -- <label> before launching again (Risk 16)"

echo "the critical path  (docs/rev/observed.md O30)"
# Not optional any more: O30 showed the game issues the draw, so the fault is on
# the Metal side and Stage 1's capture is the only instrument that can see it.
if [ -d /Applications/Xcode.app ]; then
  ok "Xcode - Stage 1 (Metal frame capture) is UNBLOCKED and is the critical path"
else
  # A note, not a failure: this is a DECISION, not a missing dependency. Shipping
  # the bug report instead (Stage 6) is a legitimate outcome, and a doctor that
  # exits non-zero forever would make every session start on a false alarm.
  note "no Xcode - since O30 this is the project's one open decision: the fault is on the"
  note "      Metal side, and a .gputrace has no viewer here. Install Xcode (~15GB; Command"
  note "      Line Tools are not enough) for Stage 1, or ship the bug report (Stage 6)"
fi

echo
[ $fail -eq 0 ] && echo "doctor: ready" || echo "doctor: something REQUIRED is missing (above)"
exit $fail
