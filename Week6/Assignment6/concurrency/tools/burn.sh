#!/usr/bin/env bash
# One CPU-bound normal task pinned to a CPU, as competing load for the lock holder.
# usage: bash tools/burn.sh <cpu> [seconds]
cpu=${1:?usage: burn.sh <cpu> [seconds]}
secs=${2:-20}
timeout "$secs" taskset -c "$cpu" sh -c 'while :; do :; done' &
echo "burn: busy loop on CPU $cpu for $secs s (pid $!)"
wait
