#!/usr/bin/env bash
set -euo pipefail

BOTTLE="${TQ_BOTTLE:-$HOME/Library/Application Support/CrossOver/Bottles/Titan Quest}"
OUTDIR="${1:?usage: gen-winmm-proxy.sh OUTDIR [real-winmm.dll]}"
SOURCE="${2:-$BOTTLE/drive_c/windows/syswow64/winmm.dll}"

[ -f "$SOURCE" ] || { echo "missing 32-bit winmm.dll: $SOURCE" >&2; exit 1; }
i686-w64-mingw32-objdump -f "$SOURCE" | grep -q pei-i386 || {
  echo "not a 32-bit x86 DLL: $SOURCE" >&2
  exit 1
}

names="$(i686-w64-mingw32-objdump -p "$SOURCE" \
  | awk '/\[Ordinal\/Name Pointer\] Table/,0' \
  | awk '/^\t\[/ {print $NF}' | sort)"
[ -n "$names" ] || { echo "no winmm exports found" >&2; exit 1; }
if grep -q '@' <<<"$names"; then
  echo "decorated winmm exports are unsupported" >&2
  exit 1
fi

mkdir -p "$OUTDIR"
{
  echo "LIBRARY winmm"
  echo "EXPORTS"
  sed 's/^/  /' <<<"$names"
} > "$OUTDIR/winmm.def"

{
  printf '\t.data\n\t.p2align 2\n\t.globl\t_tq_winmm_targets\n_tq_winmm_targets:\n'
  index=0
  while IFS= read -r name; do
    printf '_tq_wt_%s:\t.long\t_tq_winmm_unresolved\t/* %s */\n' "$index" "$name"
    index=$((index + 1))
  done <<<"$names"
  printf '\n\t.text\n'
  index=0
  while IFS= read -r name; do
    printf '\t.globl\t_%s\n' "$name"
    printf '\t.def\t_%s;\t.scl\t2;\t.type\t32;\t.endef\n' "$name"
    printf '_%s:\n\tjmp\t*_tq_wt_%s\n' "$name" "$index"
    index=$((index + 1))
  done <<<"$names"
} > "$OUTDIR/winmm_stubs.S"

sed 's/^\(.*\)$/TQ_WINMM_NAME("\1")/' <<<"$names" > "$OUTDIR/winmm_names.inc"
echo "generated $(( $(wc -l <<<"$names") )) winmm exports"
