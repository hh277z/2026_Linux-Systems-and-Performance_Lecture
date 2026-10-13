#!/usr/bin/env bash
# 실습 3 한 조건 실행: indexer(CPU 0) + renderer(CPU 0)를 같은 6초 동안 돌리며 runq.bt, preempt.bt를 함께 건다.
# usage: bash tools/eevdf_run.sh <tag> <indexer spec>
#   예: bash tools/eevdf_run.sh nice10 busy,cpu=0,nice=10
set -eu
# root로 한 번만 실행한다. sudo를 background로 띄우면 터미널이 raw mode로 바뀌어 출력이 계단처럼 밀린다.
if [ "$(id -u)" -ne 0 ]; then exec sudo bash "$0" "$@"; fi
tag=${1:?usage: eevdf_run.sh <tag> <indexer spec>}
ispec=${2:?indexer spec, e.g. busy,cpu=0}
owner="${SUDO_UID:-0}:${SUDO_GID:-0}"
secs=6
mkdir -p logs
log="logs/eevdf-$tag"
bpftrace bt/runq.bt $((secs - 1)) > "$log-runq.txt" 2>&1 < /dev/null &              # KEY 대기 시간 histogram
bpftrace bt/preempt.bt $((secs - 1)) > "$log-preempt.txt" 2>&1 < /dev/null &        # KEY 누가 누구를 preempt했나
sleep 1.5                                                                             # bpftrace가 probe를 붙일 시간
./bin/mtwork -d $secs -n indexer "$ispec" > "$log-indexer.txt" 2>&1 < /dev/null &
sleep 0.2
./bin/mtwork -d $secs -n renderer periodic:16667:4000,cpu=0 > "$log-renderer.txt" 2>&1 < /dev/null
wait
chown "$owner" logs "$log"-*.txt 2>/dev/null || true
echo "== $tag   indexer: $ispec   base_slice_ns: $(cat /sys/kernel/debug/sched/base_slice_ns 2>/dev/null || echo '?')"
awk '$1 == "idx" || $1 == "0"' "$log-renderer.txt" | sed 's/^/renderer  /'
awk '$1 == "0" {printf "indexer   cpu_ms %s  wait_ms %s  invol %s\n", $5, $6, $8}' "$log-indexer.txt"
grep -E '^@runq_(max|sum)' "$log-runq.txt" | grep renderer || true
sed -n '/^@runq_us\[renderer/,/^$/p' "$log-runq.txt"
grep '^@' "$log-preempt.txt" || true
echo "Saved: $log-{renderer,indexer,runq,preempt}.txt"
