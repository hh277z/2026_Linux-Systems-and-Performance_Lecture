#!/usr/bin/env bash
# 모든 CPU의 scheduling event를 기록하고 timehist에서 원하는 thread만 보여 준다 (실습 1, 2).
# usage: bash tools/perf_sched.sh <tag> <seconds> <comm-regex> [lines]
#   예: bash tools/perf_sched.sh dl-b 3 '^dl-' 40
set -eu
[ "$(id -u)" -eq 0 ] && sudo() { "$@"; }      # lab.sh처럼 이미 root로 실행 중이면 sudo를 거치지 않는다
owner="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}"
tag=${1:?usage: perf_sched.sh <tag> <seconds> <comm-regex> [lines]}
secs=${2:?seconds}
re=${3:?comm regex}
lines=${4:-30}
mkdir -p logs
data="/tmp/perf-sched-$tag.data"            # perf data는 ext4(/tmp)에 쓴다. /mnt/c 위에서는 mmap이 실패한다
sudo perf sched record -a -o "$data" -- sleep "$secs" >/dev/null 2>&1          # KEY 모든 CPU의 switch / wakeup
sudo perf sched timehist -i "$data" > "logs/perf-sched-$tag-timehist.txt" 2>/dev/null
sudo chown "$owner" "logs/perf-sched-$tag-timehist.txt"
echo "time            cpu     task                            wait(ms)  sch_delay  run(ms)"
awk -v re="$re" 'NR > 3 && $3 ~ re' "logs/perf-sched-$tag-timehist.txt" | sed -n "6,$((5 + lines))p"   # 시작 직후 몇 줄은 건너뛴다
echo "Saved: logs/perf-sched-$tag-timehist.txt"
