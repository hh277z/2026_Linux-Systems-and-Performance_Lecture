#!/usr/bin/env bash
# cpu-clock sampling profile: thread별 report와 flame graph. usage: bash tools/perf_profile.sh <tag> -- <command...>
set -eu
tag=${1:?usage: perf_profile.sh <tag> -- <command...>}
shift
[ "${1:-}" = "--" ] && shift
mkdir -p logs
data="logs/perf-prof-$tag.data"
sudo perf record -F 999 -e cpu-clock -g -o "$data" -- "$@"             # KEY software timer samples with call chains (no PMU needed)
sudo perf report -i "$data" --stdio --no-children --sort comm,dso,sym 2>/dev/null | grep -v '^#' | grep -v '^$' | head -n 40 > "logs/perf-prof-$tag-report.txt"
sudo perf script -i "$data" > "logs/perf-prof-$tag.stacks"
sudo chown "$(id -u):$(id -g)" "$data" "logs/perf-prof-$tag-report.txt" "logs/perf-prof-$tag.stacks"
perl tools/stackcollapse-perf.pl "logs/perf-prof-$tag.stacks" > "logs/perf-prof-$tag.folded"
perl tools/flamegraph.pl --title "on-CPU $tag" "logs/perf-prof-$tag.folded" > "logs/perf-prof-$tag.svg"   # KEY on-CPU flame graph
cat "logs/perf-prof-$tag-report.txt"
echo "Saved: logs/perf-prof-$tag-report.txt, logs/perf-prof-$tag.svg"
