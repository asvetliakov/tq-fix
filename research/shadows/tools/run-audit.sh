#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/../../.." && pwd)"
audit="$repo/research/shadows"
game="${TQ_GAME_DIR:-}"
if [ -z "$game" ]; then
  echo 'Set TQ_GAME_DIR to the folder containing TQ.exe, Engine.dll, and Game.dll.' >&2
  exit 2
fi

engine="$game/Engine.dll"
game_dll="$game/Game.dll"
tq="$game/TQ.exe"
d3d11="$game/Direct3D11.dll"
for file in "$engine" "$game_dll" "$tq" "$d3d11"; do
  [ -f "$file" ] || { echo "Missing required binary: $file" >&2; exit 2; }
done

expect_tq=491c72a6145285ee1cf38ab8fb8656b4adec022e79e434595356ce89b5cbe2d0
expect_engine=0aedbb1805b4a5616f74e34d4f609f392e2c2dd4561c64c118f4772ab4f694f6
expect_game=754907eacf552656945ff9eaf1763630e138506517e91b698ab28a0c3186aa86
expect_d3d11=589d636746eaad93adbbb920f192478c47fa9ea6745b56438abde06f2aa158e7
check_hash() {
  local file="$1" expected="$2" actual
  actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  [ "$actual" = "$expected" ] || {
    echo "Unsupported binary: $file" >&2
    echo "expected $expected" >&2
    echo "actual   $actual" >&2
    exit 3
  }
}
check_hash "$tq" "$expect_tq"
check_hash "$engine" "$expect_engine"
check_hash "$game_dll" "$expect_game"
check_hash "$d3d11" "$expect_d3d11"

ghidra="${GHIDRA_HOME:-$(brew --prefix ghidra)/libexec}"
headless="$ghidra/support/analyzeHeadless"
[ -x "$headless" ] || { echo "Ghidra analyzeHeadless not found: $headless" >&2; exit 4; }
if [ -z "${JAVA_HOME:-}" ]; then
  JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
  export JAVA_HOME
fi
[ -x "$JAVA_HOME/bin/java" ] || {
  echo "Java runtime not found under JAVA_HOME: $JAVA_HOME" >&2
  exit 4
}

project="$repo/build/shadow-audit/ghidra"
generated="$audit/generated"
backend_generated="$audit/generated/direct3d11"
mkdir -p "$project" "$generated" "$backend_generated"

if [ -f "$project/tq-shadow.gpr" ]; then
  "$headless" "$project" tq-shadow \
    -process Engine.dll \
    -noanalysis \
    -scriptPath "$audit/tools" \
    -postScript ExportShadowAudit.java "$generated" "$audit/seeds.txt"
else
  "$headless" "$project" tq-shadow \
    -import "$engine" \
    -analysisTimeoutPerFile 1800 \
    -scriptPath "$audit/tools" \
    -postScript ExportShadowAudit.java "$generated" "$audit/seeds.txt"
fi

if [ -f "$project/tq-d3d11.gpr" ]; then
  "$headless" "$project" tq-d3d11 \
    -process Direct3D11.dll \
    -noanalysis \
    -scriptPath "$audit/tools" \
    -postScript ExportShadowAudit.java "$backend_generated" "$audit/direct3d11-seeds.txt"
else
  "$headless" "$project" tq-d3d11 \
    -import "$d3d11" \
    -analysisTimeoutPerFile 1800 \
    -scriptPath "$audit/tools" \
    -postScript ExportShadowAudit.java "$backend_generated" "$audit/direct3d11-seeds.txt"
fi

for artifact in functions.csv calls.csv callgraph.dot disassembly.asm decompiled.c data-references.csv; do
  [ -s "$generated/$artifact" ] || {
    echo "Ghidra export failed or produced an empty artifact: $generated/$artifact" >&2
    exit 5
  }
done
for artifact in functions.csv calls.csv callgraph.dot disassembly.asm decompiled.c data-references.csv; do
  [ -s "$backend_generated/$artifact" ] || {
    echo "Ghidra export failed or produced an empty artifact: $backend_generated/$artifact" >&2
    exit 5
  }
done

i686-w64-mingw32-objdump -p "$engine" > "$generated/engine-pe.txt"
i686-w64-mingw32-objdump -p "$game_dll" > "$generated/game-pe.txt"
i686-w64-mingw32-objdump -p "$tq" > "$generated/tq-pe.txt"
i686-w64-mingw32-objdump -p "$d3d11" > "$backend_generated/pe.txt"

echo "Shadow audit generated in $generated"
