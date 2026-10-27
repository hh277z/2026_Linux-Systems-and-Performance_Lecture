#!/usr/bin/env bash
# Run in terminal B while lab7_memory is waiting in terminal A.
set -euo pipefail
if [[ $# -ne 3 || ! $1 =~ ^[1-9][0-9]*$ || ! $2 =~ ^[1-5]$ ]]; then
    echo 'Usage: bash observe.sh PID STAGE RUN_DIRECTORY' >&2
    exit 1
fi
pid=$1
stage=$2
run_dir=$3
if [[ ! -d $run_dir ]]; then
    echo "Run directory does not exist: $run_dir" >&2
    exit 1
fi
stage_dir="$run_dir/stage$stage"
# Refuse to overwrite the evidence from an earlier observation.
mkdir -- "$stage_dir"
for item in maps status smaps_rollup; do
    cat "/proc/$pid/$item" > "$stage_dir/$item"
done
printf 'PID=%s STAGE=%s\n' "$pid" "$stage"
grep '^VmSize:' "$stage_dir/status"
grep '^Rss:' "$stage_dir/smaps_rollup"
printf 'Saved maps/status/smaps_rollup in %s\n' "$stage_dir"
