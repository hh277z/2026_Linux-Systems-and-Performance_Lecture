#!/usr/bin/env bash
# Level 1: close, timeout, and stress tests with the normal build and the TSan build.
# Results go to logs/l1-<binary>-<test>.txt
# usage: bash tools/l1_check.sh
set -u
cd "$(dirname "$0")/.." || exit 1
make -s bin/bqtest bin/bqtest_tsan || exit 1
mkdir -p logs
status=0

run() {   # run <log> <command...>: prints the verdict line, flags any ThreadSanitizer report
    local log=$1
    shift
    echo "\$ $*"
    "$@" > "$log" 2>&1
    local rc=$?
    echo "  $(grep -E '^(PASS|FAIL)' "$log" | tail -n 1)"
    if grep -q 'ThreadSanitizer' "$log"; then
        echo "  ThreadSanitizer reported a problem: see $log"
        rc=1
    fi
    [ "$rc" -eq 0 ] || status=1
}

for b in bqtest bqtest_tsan; do
    run "logs/l1-$b-close.txt"    "./bin/$b" close
    run "logs/l1-$b-timeout.txt"  "./bin/$b" timeout
    run "logs/l1-$b-stress.txt"   "./bin/$b" -p 4 -c 4 -n 20000 -q 4
    run "logs/l1-$b-stress-b.txt" "./bin/$b" -p 4 -c 4 -n 20000 -q 4 -b
done
echo
if [ "$status" -eq 0 ]; then echo "l1_check: all PASS"; else echo "l1_check: FAIL (see logs/l1-*.txt)"; fi
exit "$status"
