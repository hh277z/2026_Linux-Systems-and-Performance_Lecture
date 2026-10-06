#!/usr/bin/env bash
# perf sched record 후 latency / timehist / map 을 logs/에 저장한다.
# usage: bash tools/perf_sched.sh <tag> -- <command...>
set -eu
tag=${1:?usage: perf_sched.sh <tag> -- <command...>}
shift
[ "${1:-}" = "--" ] && shift
mkdir -p logs
data="logs/perf-sched-$tag.data"
sudo perf sched record -o "$data" -- "$@"                              # KEY sched tracepoints for the whole run
sudo perf sched latency -i "$data" -s max > "logs/perf-sched-$tag-latency.txt"
sudo perf sched timehist -i "$data" > "logs/perf-sched-$tag-timehist.txt"
sudo perf sched map -i "$data" > "logs/perf-sched-$tag-map.txt"        # KEY per-CPU placement over time
sudo chown "$(id -u):$(id -g)" "$data" logs/perf-sched-"$tag"-*.txt
echo "== latency (mtwork threads) =="
grep -E 'mtwork' "logs/perf-sched-$tag-latency.txt" || true
echo "Saved: logs/perf-sched-$tag-{latency,timehist,map}.txt"
