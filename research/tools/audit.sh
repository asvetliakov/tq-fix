#!/usr/bin/env bash
# Shared preamble and driver for the reproducible headless Ghidra audits under
# research/.  It resolves the game installation, refuses binaries that are not
# the supported build, locates Ghidra and its JDK, runs the exporter over one
# binary with one seed file, and verifies that the export is non-empty.
#
# usage:
#   research/tools/audit.sh PROJECT BINARY SEEDS OUTDIR [PE_BINARY=NAME ...]
#
#   PROJECT     Ghidra project name inside $AUDIT_PROJECT_DIR.
#   BINARY      absolute path to the PE to import and analyze.
#   SEEDS       seed file passed to the exporter.
#   OUTDIR      directory that receives the generated evidence.
#   PE_BINARY=NAME
#               optional trailing pairs; each writes
#               `objdump -p PE_BINARY` to OUTDIR/NAME.  With none given the
#               audited BINARY is dumped to OUTDIR/pe.txt.
#
# Environment:
#   TQ_GAME_DIR             required; folder containing TQ.exe and Engine.dll.
#   AUDIT_PROJECT_DIR       Ghidra project directory (default build/audit/ghidra).
#   AUDIT_SCRIPT_PATH       Ghidra -scriptPath (default research/shadows/tools,
#                           where the shared exporter lives).
#   AUDIT_EXPORT_SCRIPT     exporter file name (default ExportShadowAudit.java).
#   AUDIT_ANALYSIS_TIMEOUT  -analysisTimeoutPerFile seconds (default 1800).
#   GHIDRA_HOME, JAVA_HOME  override tool discovery.
#
# Exit codes: 2 bad invocation or missing input, 3 unsupported binary,
# 4 missing Ghidra/JDK, 5 empty export.
set -euo pipefail

if [ "$#" -lt 4 ]; then
  echo 'usage: audit.sh PROJECT BINARY SEEDS OUTDIR [PE_BINARY=NAME ...]' >&2
  exit 2
fi
project="$1"
binary="$2"
seeds="$3"
outdir="$4"
shift 4

repo="$(cd "$(dirname "$0")/../.." && pwd)"
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

# Pinned in research/shadows/supported-build.md.
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

[ -f "$binary" ] || { echo "Missing audit target: $binary" >&2; exit 2; }
[ -f "$seeds" ] || { echo "Missing seed file: $seeds" >&2; exit 2; }

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

project_dir="${AUDIT_PROJECT_DIR:-$repo/build/audit/ghidra}"
script_path="${AUDIT_SCRIPT_PATH:-$repo/research/shadows/tools}"
export_script="${AUDIT_EXPORT_SCRIPT:-ExportShadowAudit.java}"
analysis_timeout="${AUDIT_ANALYSIS_TIMEOUT:-1800}"
mkdir -p "$project_dir" "$outdir"

if [ -f "$project_dir/$project.gpr" ]; then
  "$headless" "$project_dir" "$project" \
    -process "$(basename "$binary")" \
    -noanalysis \
    -scriptPath "$script_path" \
    -postScript "$export_script" "$outdir" "$seeds"
else
  "$headless" "$project_dir" "$project" \
    -import "$binary" \
    -analysisTimeoutPerFile "$analysis_timeout" \
    -scriptPath "$script_path" \
    -postScript "$export_script" "$outdir" "$seeds"
fi

for artifact in functions.csv calls.csv callgraph.dot disassembly.asm decompiled.c data-references.csv; do
  [ -s "$outdir/$artifact" ] || {
    echo "Ghidra export failed or produced an empty artifact: $outdir/$artifact" >&2
    exit 5
  }
done

if [ "$#" -eq 0 ]; then
  set -- "$binary=pe.txt"
fi
for pair in "$@"; do
  source_pe="${pair%%=*}"
  target_pe="${pair#*=}"
  i686-w64-mingw32-objdump -p "$source_pe" > "$outdir/$target_pe"
done

echo "Audit generated in $outdir"
