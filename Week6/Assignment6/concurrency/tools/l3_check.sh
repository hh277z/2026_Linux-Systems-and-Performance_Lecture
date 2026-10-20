#!/usr/bin/env bash
# Level 3: checks one case against the five pass criteria.
#   lockcheck  LD_PRELOAD=./bin/lockcheck.so ./bin/devsvc -c N -d 3
#              result: PASS, and lockcheck summary W1=0 W2=0 W3=0 W4=0
#   TSan       ./bin/devsvc_tsan -c N -d 3 with TSAN_OPTIONS=detect_deadlocks=0
#              (data races only: lock order is lockcheck's job), result: PASS and no report
#   stress     ./bin/devsvc -c N -d 2 with -t 2, 4, 4, 4, 8: every run result: PASS
#   parallel   parallel= of the three -t 4 runs: every one at least 3%
#   speed      the three -t 4 runs alternate with bin/devsvc_ref (the reference fixes):
#              median ops_per_sec at least 0.70 x the reference median
# Logs go to logs/l3check-<case>-*.txt (TAG=name changes <case> in the names).
# usage: bash tools/l3_check.sh <1|2|3|4>
#        TAG=w2 bash tools/l3_check.sh 2
set -u
cd "$(dirname "$0")/.." || exit 1
unset DEVSVC_RWLOCK                     # always the default rwlock kind here
c=${1:?usage: l3_check.sh <1|2|3|4>}
case "$c" in 1|2|3|4) ;; *) echo "usage: l3_check.sh <1|2|3|4>"; exit 2 ;; esac
DEVSVC=${DEVSVC:-./bin/devsvc}
DEVSVC_TSAN=${DEVSVC_TSAN:-./bin/devsvc_tsan}
DEVSVC_REF=${DEVSVC_REF:-./bin/devsvc_ref}
case "$DEVSVC" in ./bin/devsvc) make -s bin/devsvc bin/devsvc_tsan bin/devsvc_ref bin/lockcheck.so || exit 1 ;; esac
mkdir -p logs
log=logs/l3check-${TAG:-$c}
pass=1

verdict() {   # name, ok(0/1), detail
    printf '%-10s %s  %s\n' "$1" "$([ "$2" = 1 ] && echo PASS || echo FAIL)" "$3"
    [ "$2" = 1 ] || pass=0
}
result_of() { grep -o '^result: [A-Z]*' "$1" | tail -1 | cut -d' ' -f2; }
field() { grep -o "$2=[0-9]*" "$1" | head -1 | cut -d= -f2; }

# 1. lockcheck
timeout 60 env LD_PRELOAD=./bin/lockcheck.so "$DEVSVC" -c "$c" -d 3 > "$log-lockcheck.txt" 2>&1
rc=$?
sum=$(grep -o 'W1=[0-9]* W2=[0-9]* W3=[0-9]* W4=[0-9]*' "$log-lockcheck.txt" | tail -1)
r=$(result_of "$log-lockcheck.txt")
ok=0; [ "$sum" = "W1=0 W2=0 W3=0 W4=0" ] && [ "$r" = PASS ] && ok=1
verdict lockcheck $ok "${sum:-no summary (exit status $rc)}, result: ${r:-none}"

# 2. TSan, data races only
timeout 120 env TSAN_OPTIONS="detect_deadlocks=0 ${TSAN_OPTIONS:-}" "$DEVSVC_TSAN" -c "$c" -d 3 > "$log-tsan.txt" 2>&1
n=$(grep -c '^WARNING: ThreadSanitizer' "$log-tsan.txt")
r=$(result_of "$log-tsan.txt")
ok=0; [ "$n" = 0 ] && [ "$r" = PASS ] && ok=1
verdict TSan $ok "$n report(s), result: ${r:-none}"

# 3-5. stress runs; the -t 4 runs alternate with the reference
runs=0; good=0; par=""; parok=1; mine=(); ref=()
one() {   # tag, binary, threads
    timeout 60 "$2" -c "$c" -t "$3" -d 2 > "$log-$1.txt" 2>&1
}
for i in 1 2 3 4 5; do
    case $i in 1) t=2 ;; 5) t=8 ;; *) t=4 ;; esac
    if [ "$t" = 4 ]; then
        one ref$i "$DEVSVC_REF" 4
        ref+=("$(field "$log-ref$i.txt" ops_per_sec)")
    fi
    one run$i "$DEVSVC" "$t"
    r=$(result_of "$log-run$i.txt")
    runs=$((runs + 1)); [ "$r" = PASS ] && good=$((good + 1))
    if [ "$t" = 4 ]; then
        p=$(field "$log-run$i.txt" parallel)
        par="$par ${p:-?}%"
        [ -n "$p" ] && [ "$p" -ge 3 ] || parok=0
        mine+=("$(field "$log-run$i.txt" ops_per_sec)")
    fi
done
verdict stress $([ $good = $runs ] && echo 1 || echo 0) "$good of $runs runs PASS (-t 2 4 4 4 8)"
verdict parallel $parok "${par# } (at least 3%)"

median() { printf '%s\n' "$@" | grep -v '^$' | sort -n | awk '{a[NR]=$1} END {print (NR ? a[int((NR+1)/2)] : 0)}'; }
m=$(median "${mine[@]}"); mr=$(median "${ref[@]}")
ratio=$(awk -v a="$m" -v b="$mr" 'BEGIN {printf "%.2f", (b > 0 ? a / b : 0)}')
ok=$(awk -v a="$m" -v b="$mr" 'BEGIN {print (b > 0 && a >= 0.70 * b) ? 1 : 0}')
verdict speed "$ok" "$m ops/s, reference $mr ($ratio, at least 0.70)"

echo "case $c: $([ $pass = 1 ] && echo PASS || echo FAIL)   (logs: $log-*.txt)"
[ $pass = 1 ]
