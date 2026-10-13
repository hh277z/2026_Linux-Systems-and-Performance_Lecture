#!/usr/bin/env bash
# Runs one Level 2 condition: svc (with options) + reqtrace.bt + svcload for 10 s. Results go to logs/l2-<tag>-*.txt
# usage: bash tools/l2_run.sh <tag> [svc options...]      e.g. bash tools/l2_run.sh inhprio -i -q prio
#        HOG=40 bash tools/l2_run.sh hog40 -i -q prio      (changes the hog priority, 0 = no hog)
set -eu
# Re-run the whole script as root once. A backgrounded sudo puts the terminal in raw mode, which garbles output and swallows typed input.
if [ "$(id -u)" -ne 0 ]; then exec sudo env HOG="${HOG:-20}" bash "$0" "$@"; fi
tag=${1:?usage: l2_run.sh <tag> [svc options]}
shift
owner="${SUDO_UID:-0}:${SUDO_GID:-0}"
mkdir -p logs
log=logs/l2-$tag
./bin/svc "$@" -d 16 > "$log-svc.txt" 2>&1 < /dev/null &
sleep 0.5
bpftrace bt/reqtrace.bt 13 > "$log-bt.txt" 2>&1 < /dev/null &
sleep 2                                                   # until the uprobes are attached
./bin/svcload -d 10 -H "${HOG:-20}" -o "$log.csv" > "$log-load.txt" 2>&1 < /dev/null
wait
chown "$owner" logs "$log"* 2>/dev/null || true
echo "== $tag   svc $*   hog prio ${HOG:-20}"
grep -E '^cl-' "$log-load.txt"
tail -n 1 "$log-svc.txt"
echo "Saved: $log-{load,svc,bt}.txt $log.csv"
