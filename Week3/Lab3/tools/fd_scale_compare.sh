#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")/.."
mkdir -p logs

# Keep K=3 and rounds fixed while N grows tenfold.
for count in 1000 10000; do
    for mode in select poll epoll; do
        echo
        echo "--- $mode: N=$count, K=3, rounds=200 ---"
        ./bin/fd_scale "$mode" "$count" 200 | tee "logs/fd-scale-$mode-$count.txt"
    done
done
