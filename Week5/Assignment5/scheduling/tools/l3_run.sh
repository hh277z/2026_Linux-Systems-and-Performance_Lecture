#!/usr/bin/env bash
# Level 3: starts a target program and attaches bt/whydelay.bt. Results go to logs/l3-<tag>-*.txt
# usage: bash tools/l3_run.sh case <1..4>        runs a mystery case for 10 s, whydelay on ui-render for 8 s
#        bash tools/l3_run.sh fix1               runs mystery case 1, switches thumbgen to SCHED_IDLE after 1 s, then measures
#        bash tools/l3_run.sh svc <tag> [svc options]   runs svc + svcload, whydelay on svc-worker for 12 s
set -eu
# Re-run the whole script as root once. A backgrounded sudo puts the terminal in raw mode, which garbles output and swallows typed input.
if [ "$(id -u)" -ne 0 ]; then exec sudo bash "$0" "$@"; fi
owner="${SUDO_UID:-0}:${SUDO_GID:-0}"
mkdir -p logs
mode=${1:?usage: l3_run.sh case <N> | fix1 | svc <tag> [svc options]}
shift

show_bt() {   # shows the main maps only, and @blame_us lines of 1 ms or more
    grep -E '^(whydelay|@oncpu_us|@wait_us|@wait_max_us|@victim_prio|@blame_us)' "$1" | awk '!/^@blame_us/ || $NF + 0 >= 1000' || true
}

case "$mode" in
case)
    c=${1:?case number 1..4}
    log=logs/l3-case$c
    ./scenarios/mystery "$c" 10 > "$log.txt" 2>&1 < /dev/null &
    p=$!
    sleep 0.5
    echo "\$ sudo bpftrace bt/whydelay.bt $p ui-render 8"
    bpftrace bt/whydelay.bt "$p" ui-render 8 > "$log-bt.txt" 2>&1 < /dev/null
    wait
    echo "== case $c"; tail -n 1 "$log.txt"; show_bt "$log-bt.txt"
    ;;
fix1)
    log=logs/l3-fix1
    ./scenarios/mystery 1 20 > "$log.txt" 2>&1 < /dev/null &
    p=$!
    sleep 1
    for t in $(ps -L -o tid=,comm= -p "$p" | awk '$2 == "thumbgen" {print $1}'); do
        echo "\$ sudo chrt -i -p 0 $t"; chrt -i -p 0 "$t"
    done
    echo "\$ sudo bpftrace bt/whydelay.bt $p ui-render 8"
    bpftrace bt/whydelay.bt "$p" ui-render 8 > "$log-bt.txt" 2>&1 < /dev/null
    wait
    echo "== case 1 after SCHED_IDLE"; tail -n 1 "$log.txt"; show_bt "$log-bt.txt"
    ;;
svc)
    tag=${1:?svc tag}; shift
    log=logs/l3-svc-$tag
    ./bin/svc "$@" -d 16 > "$log-svc.txt" 2>&1 < /dev/null &
    p=$!
    sleep 0.5
    ( sleep 2; ./bin/svcload -d 10 > "$log-load.txt" 2>&1 < /dev/null ) &
    echo "\$ sudo bpftrace bt/whydelay.bt $p svc-worker 12"
    bpftrace bt/whydelay.bt "$p" svc-worker 12 > "$log-bt.txt" 2>&1 < /dev/null
    wait
    echo "== svc $*"; grep -E '^cl-' "$log-load.txt"; show_bt "$log-bt.txt"
    ;;
*)
    sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
chown "$owner" logs logs/l3-* 2>/dev/null || true
