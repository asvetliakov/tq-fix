#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="${1:?usage: gen-winmm-proxy.sh OUTDIR [manifest]}"
MANIFEST="${2:-$ROOT/src/winmm_exports.txt}"

[ -f "$MANIFEST" ] || { echo "missing winmm export manifest: $MANIFEST" >&2; exit 1; }

records="$(awk '
  /^[[:space:]]*(#|$)/ { next }
  NF != 2 || ($1 != "required" && $1 != "optional") ||
      $2 !~ /^[A-Za-z_][A-Za-z0-9_]*$/ {
    print FILENAME ":" FNR ": invalid winmm export record" > "/dev/stderr"
    bad = 1
    next
  }
  { print $1, $2 }
  END { exit bad }
' "$MANIFEST" | sort -k2,2)"
[ -n "$records" ] || { echo "no winmm exports found in $MANIFEST" >&2; exit 1; }

names="$(awk '{print $2}' <<<"$records")"
unique="$(sort -u <<<"$names")"
if [ "$(wc -l <<<"$names")" -ne "$(wc -l <<<"$unique")" ]; then
  echo "duplicate winmm export in $MANIFEST" >&2
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
  while read -r kind name; do
    printf '_tq_wt_%s:\t.long\t_tq_winmm_unresolved\t/* %s */\n' "$index" "$name"
    index=$((index + 1))
  done <<<"$records"
  printf '\n\t.text\n'
  index=0
  while read -r kind name; do
    printf '\t.globl\t_%s\n' "$name"
    printf '\t.def\t_%s;\t.scl\t2;\t.type\t32;\t.endef\n' "$name"
    printf '_%s:\n\tjmp\t*_tq_wt_%s\n' "$name" "$index"
    index=$((index + 1))
  done <<<"$records"
} > "$OUTDIR/winmm_stubs.S"

while read -r kind name; do
  if [ "$kind" = required ]; then required=true; else required=false; fi
  printf 'TQ_WINMM_NAME("%s", %s)\n' "$name" "$required"
done <<<"$records" > "$OUTDIR/winmm_names.inc"

echo "generated $(wc -l <<<"$names" | tr -d ' ') winmm exports ($(awk '$1 == "required" {n++} END {print n + 0}' <<<"$records") required)"
