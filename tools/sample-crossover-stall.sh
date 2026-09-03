#!/bin/zsh
set -euo pipefail

duration="${1:-120}"
interval_ms="${2:-10}"
case "$duration" in
  (''|*[!0-9]*|0) print -u2 "duration must be a positive integer"; exit 2 ;;
esac
case "$interval_ms" in
  (''|*[!0-9]*|0) print -u2 "interval must be a positive integer"; exit 2 ;;
esac

game_dir="${TQ_GAME:-${HOME}/Library/Application Support/CrossOver/Bottles/Titan Quest/drive_c/GOG Games/Titan Quest - Anniversary Edition}"
openers="$(lsof -Fpc -- "$game_dir/winmm.dll" 2>/dev/null || true)"
game_pid="$(print -r -- "$openers" | awk '
  /^p/ { pid = substr($0, 2) }
  /^cTQ\.exe$/ { print pid; exit }
')"
server_pid="$(print -r -- "$openers" | awk '
  /^p/ { pid = substr($0, 2) }
  /^cwineserve/ { print pid; exit }
')"

if [[ -z "$server_pid" ]]; then
  print -u2 "the wineserver holding the installed winmm.dll is not running"
  exit 1
fi
if [[ -z "$game_pid" ]]; then
  print -u2 "TQ.exe is not running; start it from the CrossOver UI and stop at the menu"
  exit 1
fi

stamp="$(date +%Y%m%d-%H%M%S)"
output="cache/samples/run44-$stamp"
mkdir -p "$output"
print "sampling TQ.exe pid $game_pid and its wineserver pid $server_pid"
print "duration ${duration}s, interval ${interval_ms}ms, output $output"

/usr/bin/sample "$game_pid" "$duration" "$interval_ms" -mayDie \
  -file "$output/tq.txt" &
game_sampler=$!
/usr/bin/sample "$server_pid" "$duration" "$interval_ms" -mayDie \
  -file "$output/wineserver.txt" &
server_sampler=$!

sample_status=0
wait "$game_sampler" || sample_status=$?
wait "$server_sampler" || sample_status=$?
print "sampling complete: $output"
exit "$sample_status"
