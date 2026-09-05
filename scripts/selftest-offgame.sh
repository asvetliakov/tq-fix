#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
CX="/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver"
WORK="$BOTTLE/drive_c/tqflicker-selftest"
REPORT_WIN='C:\tqflicker-selftest\report.txt'

[ -f build/winmm.dll ] || { echo "no build/winmm.dll - run: npm run build" >&2; exit 1; }

i686-w64-mingw32-g++ -o build/selftest.exe \
  test/selftest.cpp test/engine_runtime.cpp src/arc_cache.cpp src/bloom_hook.cpp src/detour.cpp src/renderer_draw.cpp src/dxbc_patch.cpp src/engine_probe.cpp src/engine_hooks.cpp src/shadow_defer.cpp src/terrain_preload.cpp src/secondary_admission.cpp src/archive_hooks.cpp src/frame_overlay.cpp src/frustum_fix.cpp src/grass.cpp src/hdr.cpp src/probe.cpp src/shadow_fix.cpp src/streaming.cpp src/upload.cpp \
  -I src -I build/gen -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
  -DTQ_SELFTEST -ld3d11
i686-w64-mingw32-g++ -shared -o build/Direct3D11.dll \
  test/device_host.cpp -I src -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
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

# Real DXGI device/swap-chain recreation through the production DLL. This host
# has executable padding at the audited Present RVAs as well as the draw sites.
recreation_case="$WORK/device-recreation"
mkdir -p "$recreation_case"
i686-w64-mingw32-g++ -shared -o "$recreation_case/Direct3D11.dll" \
  test/device_host.cpp -I src -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
  -DTQ_RECREATE_HOST -ld3d11
python3 - "$recreation_case/Direct3D11.dll" <<'PY'
from pathlib import Path
import struct, sys
path = Path(sys.argv[1])
image = bytearray(path.read_bytes())
pe = struct.unpack_from('<I', image, 0x3c)[0]
size_offset = pe + 24 + 56
assert struct.unpack_from('<I', image, size_offset)[0] <= 0x192000
# Reserve the same image span as the renderer so the production identity gate
# runs unchanged. All fixture sections already fit inside that span.
struct.pack_into('<I', image, size_offset, 0x192000)
path.write_bytes(image)
PY
cp build/winmm.dll build/selftest.exe "$WORK/tq-dxbc-PS-gamma.dxbc" "$recreation_case/"
"$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
  --no-gui --no-wait --workdir 'C:\tqflicker-selftest\device-recreation' \
  -- 'C:\tqflicker-selftest\device-recreation\selftest.exe' \
     --device-recreation >/dev/null 2>&1 || true
for _ in $(seq 1 240); do
  grep -q '^RESULT' "$recreation_case/report.txt" 2>/dev/null && break
  sleep 0.25
done
[ -s "$recreation_case/report.txt" ] || { echo "FAIL: recreation produced no report" >&2; exit 1; }
cat "$recreation_case/report.txt"
grep -q '^RESULT: 0 failure' "$recreation_case/report.txt"
python3 - "$recreation_case/tqflicker-frames.csv" <<'PY'
from pathlib import Path
import csv, io, sys, time
path = Path(sys.argv[1])
# The process-exit flush follows the report write; inspect its completed file
# from outside the process instead of forcing an extra runtime logging API.
for _ in range(50):
    data = path.read_text() if path.exists() else ''
    if '# gpu timings:' in data:
        break
    time.sleep(0.1)
assert '# gpu timings:' in data, 'recreation trace did not finish flushing'
rows = list(csv.DictReader(line for line in io.StringIO(data) if not line.startswith('#')))
assert [int(row['frame']) for row in rows] == list(range(11)), 'recreation lost frame trace rows'
print('ok    one continuous frame trace survives all device recreations')
PY

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

# Production hook activation with grass as the sole Draw-hook consumer.
# Separate processes/directories keep the one-time INI state isolated.
for grass_mode in enhanced original rollback; do
  grass_case="$WORK/grass-$grass_mode"
  mkdir -p "$grass_case"
  cp build/winmm.dll build/selftest.exe build/Direct3D11.dll "$grass_case/"
  "$CX/bin/cxstart" --bottle "$(basename "$BOTTLE")" --no-convert \
    --no-gui --no-wait \
    --workdir "C:\\tqflicker-selftest\\grass-$grass_mode" \
    -- "C:\\tqflicker-selftest\\grass-$grass_mode\\selftest.exe" \
       --grass-hooks "$grass_mode" >/dev/null 2>&1 || true
  for _ in $(seq 1 120); do
    grep -q '^RESULT' "$grass_case/report.txt" 2>/dev/null && break
    sleep 0.25
  done
  [ -s "$grass_case/report.txt" ] || { echo "FAIL: grass-$grass_mode produced no report" >&2; exit 1; }
  cat "$grass_case/report.txt"
  grep -q '^RESULT: 0 failure' "$grass_case/report.txt"
done
