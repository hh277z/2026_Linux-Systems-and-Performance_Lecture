#!/usr/bin/env bash
# Level 2: runs albumsvc in one mode for 10 s with tools/lockwait.bt (8 s) and 2 s of perf sched.
# Results go to logs/l2-<tag>-*.txt
# usage: bash tools/l2_run.sh <lock|snapshot|handoff> [albumsvc options...]    e.g. bash tools/l2_run.sh lock -D 5
#        TAG=burn bash tools/l2_run.sh lock -D 0 -c 1          log name (default: the mode)
#        ALBUMSVC=./bin/albumsvc_ref bash tools/l2_run.sh ...    use albumsvc built with lib/libbq_ref.a
set -eu
cd "$(dirname "$0")/.."
if [ "$(id -u)" -ne 0 ]; then
    make -s "${ALBUMSVC:-./bin/albumsvc}"            # build as the normal user, before becoming root
    # Re-run the whole script as root once. A backgrounded sudo puts the terminal in raw mode, which garbles output and swallows typed input.
    exec sudo env TAG="${TAG:-}" ALBUMSVC="${ALBUMSVC:-}" bash "$0" "$@"
fi
mode=${1:?usage: l2_run.sh <lock|snapshot|handoff> [albumsvc options]}
shift
tag=${TAG:-}
[ -n "$tag" ] || tag=$mode
bin=${ALBUMSVC:-}
[ -n "$bin" ] || bin=./bin/albumsvc
owner="${SUDO_UID:-0}:${SUDO_GID:-0}"
mkdir -p logs
log=logs/l2-$tag

echo "\$ $bin -m $mode -d 10 $* -o $log-frames.csv"
"$bin" -m "$mode" -d 10 -o "$log-frames.csv" "$@" > "$log-albumsvc.txt" 2>&1 < /dev/null &
p=$!
sleep 0.5
echo "\$ sudo bpftrace tools/lockwait.bt $p 8"
bpftrace tools/lockwait.bt "$p" 8 > "$log-lockwait.txt" 2>&1 < /dev/null &
sleep 4
echo "\$ bash tools/perf_sched.sh l2-$tag 2 '^(ui|saver)[[]'"
bash tools/perf_sched.sh "l2-$tag" 2 '^(ui|saver)[[]' 40 > "$log-timehist.txt" 2>&1
wait
chown "$owner" logs "$log"* logs/perf-sched-l2-"$tag"* 2>/dev/null || true

echo "== $tag   albumsvc -m $mode $*"
sed -n '/^frames=/,$p' "$log-albumsvc.txt"
echo "-- lockwait.bt: slow pthread_mutex_lock calls (waiter, owner, caller) and system calls made while holding a mutex"
grep -E '^@(slow_calls|syscalls_under_lock)' "$log-lockwait.txt" || echo "(none)"
echo "Saved: $log-{albumsvc,lockwait,timehist}.txt $log-frames.csv"
