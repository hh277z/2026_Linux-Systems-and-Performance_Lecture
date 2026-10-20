#!/usr/bin/env bash
# Level 3: diagnoses one case. Runs it with lockcheck.so, then the ThreadSanitizer
# build of the same case (with TSan's own lock order check, which is on by default).
# Results go to logs/l3-<case>-{lockcheck,tsan}.txt (TAG=name changes <case> in the names).
# usage: bash tools/l3_run.sh <1|2|3|4> [devsvc options]
#        DEVSVC_RWLOCK=writer TAG=3-writer bash tools/l3_run.sh 3
set -u
cd "$(dirname "$0")/.." || exit 1
c=${1:?usage: l3_run.sh <1|2|3|4> [devsvc options]}
shift
DEVSVC=${DEVSVC:-./bin/devsvc}
DEVSVC_TSAN=${DEVSVC_TSAN:-./bin/devsvc_tsan}
make -s bin/devsvc bin/devsvc_tsan bin/lockcheck.so || exit 1
mkdir -p logs
log=logs/l3-${TAG:-$c}

echo "\$ LD_PRELOAD=./bin/lockcheck.so $DEVSVC -c $c $*"
timeout 60 env LD_PRELOAD=./bin/lockcheck.so "$DEVSVC" -c "$c" "$@" > "$log-lockcheck.txt" 2>&1
echo "\$ $DEVSVC_TSAN -c $c $*"
timeout 120 env TSAN_OPTIONS="second_deadlock_stack=1 ${TSAN_OPTIONS:-}" "$DEVSVC_TSAN" -c "$c" "$@" > "$log-tsan.txt" 2>&1

echo "== case $c: lockcheck"
grep -E '^(lockcheck: (W[1-4]|summary)|  |HANG|check failed|result:)' "$log-lockcheck.txt" \
    || echo "(no output: was lockcheck.so loaded?)"
echo "== case $c: ThreadSanitizer"
grep -E '^(WARNING|SUMMARY): ThreadSanitizer|^(HANG|check failed|result:)' "$log-tsan.txt" || echo "(no report)"
echo "Saved: $log-lockcheck.txt $log-tsan.txt"
