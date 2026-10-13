#!/usr/bin/env bash
# Starts N CPU-bound normal tasks (background load for Level 1). usage: bash tools/burn.sh [N, default = number of CPUs] [seconds]
n=${1:-$(nproc)}; secs=${2:-30}
for i in $(seq 1 "$n"); do timeout "$secs" sh -c 'while :; do :; done' & done
echo "burn: $n busy loops for $secs s (pids: $(jobs -p | tr '\n' ' '))"
wait
