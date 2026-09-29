#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")/.."
mkdir -p logs

# Repeat the same program with one mode argument; tee saves AND displays stdout.
for mode in block busy epoll; do
    ./bin/wait_modes "$mode" | tee "logs/wait-$mode.txt"
done
