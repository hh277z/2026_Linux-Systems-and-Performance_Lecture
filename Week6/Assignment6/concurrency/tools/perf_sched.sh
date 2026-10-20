#!/usr/bin/env bash
# Records scheduling events on all CPUs and shows the timehist lines of the threads you choose.
# usage: bash tools/perf_sched.sh <tag> <seconds> <task-regex> [lines]     e.g. bash tools/perf_sched.sh lock 2 '^ui[[]' 40
set -eu
[ "$(id -u)" -eq 0 ] && sudo() { "$@"; }      # already root (l2_run.sh): run perf directly
owner="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}"
tag=${1:?usage: perf_sched.sh <tag> <seconds> <task-regex> [lines]}
secs=${2:?seconds}
re=${3:?task regex}
lines=${4:-30}
mkdir -p logs
data="/tmp/perf-sched-$tag.data"            # perf data goes to /tmp
sudo perf sched record -a -o "$data" -- sleep "$secs" >/dev/null 2>&1
sudo perf sched timehist -i "$data" > "logs/perf-sched-$tag-timehist.txt" 2>/dev/null
sudo chown "$owner" "logs/perf-sched-$tag-timehist.txt"
echo "time            cpu     task                            wait(ms)  sch_delay  run(ms)"
awk -v re="$re" 'NR > 3 && $3 ~ re' "logs/perf-sched-$tag-timehist.txt" | sed -n "6,$((5 + lines))p"
echo "Saved: logs/perf-sched-$tag-timehist.txt"
