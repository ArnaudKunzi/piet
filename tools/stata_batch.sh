#!/bin/sh
# stata_batch.sh — run a do-file through local StataBE in batch, judged by a LOG SENTINEL.
#
# Why: on this Mac the GUI-based StataBE binary runs `-b do f.do` to completion, writes f.log in
# full, and then frequently does NOT exit (memory: stata-batch-noexit, first seen 2026-07-10). And
# macOS ships no `timeout`. So: start it in the background, poll the log for the sentinel the
# do-file prints on its last line, reap the process, and return 0 only if the sentinel is there.
#
#   tools/stata_batch.sh <file.do> [sentinel] [max_seconds]
#     sentinel     default "RUN COMPLETE" — the do-file must `display "RUN COMPLETE"` last
#     max_seconds  default 600
# Runs in the do-file's own directory (Stata writes <name>.log beside it). Exit 0 = sentinel seen ·
# 1 = Stata exited without it or the log shows r(NNN) before it · 2 = usage/timeout.
STATA=${STATA_BIN:-/Applications/StataNow/StataBE.app/Contents/MacOS/StataBE}
[ $# -ge 1 ] || { echo "usage: stata_batch.sh <file.do> [sentinel] [max_seconds]" >&2; exit 2; }
dofile=$1; sentinel=${2:-RUN COMPLETE}; max=${3:-600}
[ -x "$STATA" ] || { echo "stata_batch: no StataBE at $STATA" >&2; exit 2; }
dir=$(cd "$(dirname "$dofile")" && pwd); base=$(basename "$dofile" .do); log="$dir/$base.log"
rm -f "$log"
( cd "$dir" && "$STATA" -b do "$base.do" >/dev/null 2>&1 ) &
pid=$!
t=0
while [ $t -lt "$max" ]; do
  if [ -f "$log" ] && grep -q -- "$sentinel" "$log"; then
    sleep 1; { kill $pid; pkill -9 -f "StataBE -b do $base.do"; wait $pid; } >/dev/null 2>&1
    echo "stata_batch: sentinel '$sentinel' after ${t}s — $log"; exit 0
  fi
  if ! kill -0 $pid 2>/dev/null; then
    if [ -f "$log" ] && grep -q -- "$sentinel" "$log"; then echo "stata_batch: sentinel '$sentinel' (exited) — $log"; exit 0; fi
    echo "stata_batch: StataBE exited WITHOUT the sentinel — $log" >&2
    [ -f "$log" ] && grep -n '^r([0-9]*);' "$log" | head -3 >&2
    exit 1
  fi
  sleep 1; t=$((t+1))
done
{ kill $pid; pkill -9 -f "StataBE -b do $base.do"; wait $pid; } >/dev/null 2>&1
echo "stata_batch: TIMEOUT after ${max}s without '$sentinel' — $log" >&2; exit 2
