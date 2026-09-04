#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
WORK="$BOTTLE/drive_c/tqflicker-selftest"
REPORT_WIN='C:\tqflicker-selftest\report.txt'

[ -f build/winmm.dll ] || { echo "no build/winmm.dll - run: npm run build" >&2; exit 1; }

i686-w64-mingw32-g++ -o build/selftest.exe \
  test/selftest.cpp test/engine_runtime.cpp src/arc_cache.cpp src/bloom_hook.cpp src/detour.cpp src/dxbc_patch.cpp src/engine_probe.cpp src/engine_hooks.cpp src/shadow_defer.cpp src/terrain_preload.cpp src/secondary_admission.cpp src/archive_hooks.cpp src/frame_overlay.cpp src/frustum_fix.cpp src/grass.cpp src/hdr.cpp src/probe.cpp src/shadow_fix.cpp src/streaming.cpp src/upload.cpp \
  -I src -I build/gen -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
  -DTQ_SELFTEST -ld3d11
i686-w64-mingw32-g++ -shared -o build/Direct3D11.dll \
  test/device_host.cpp -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
  -ld3d11

rm -rf "$WORK"
mkdir -p "$WORK"
cp build/winmm.dll build/selftest.exe build/Direct3D11.dll "$WORK/"
base64 -D -i test/fixtures/tq-dxbc-PS-fxaa.b64 -o "$WORK/tq-dxbc-PS-fxaa.dxbc"
base64 -D -i test/fixtures/tq-dxbc-VS-fxaa.b64 -o "$WORK/tq-dxbc-VS-fxaa.dxbc"
base64 -D -i test/fixtures/tq-dxbc-PS-shadow.b64 -o "$WORK/tq-dxbc-PS-shadow.dxbc"
base64 -D -i test/fixtures/tq-dxbc-PS-deferred-shadow.b64 -o "$WORK/tq-dxbc-PS-deferred-shadow.dxbc"
base64 -D -i test/fixtures/tq-dxbc-PS-colorgrading.b64 -o "$WORK/tq-dxbc-PS-colorgrading.dxbc"
base64 -D -i test/fixtures/tq-dxbc-PS-gamma.b64 -o "$WORK/tq-dxbc-PS-gamma.dxbc"

SHADER_ARGS=()
for shader in test/fixtures/*.dxbc; do
  [ -f "$shader" ] || continue
  name="$(basename "$shader")"
  cp "$shader" "$WORK/$name"
  SHADER_ARGS+=("C:\\tqflicker-selftest\\$name")
done

"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  --no-gui \
  --no-wait \
  --workdir 'C:\tqflicker-selftest' \
  -- 'C:\tqflicker-selftest\selftest.exe' \
     'C:\tqflicker-selftest\winmm.dll' "$REPORT_WIN" "${SHADER_ARGS[@]}" \
  >/dev/null 2>&1 || true

for _ in $(seq 1 120); do
  grep -q '^RESULT' "$WORK/report.txt" 2>/dev/null && break
  sleep 0.25
done

[ -s "$WORK/report.txt" ] || { echo "FAIL: self-test produced no report" >&2; exit 1; }
cat "$WORK/report.txt"
grep -q '^RESULT: 0 failure' "$WORK/report.txt"
