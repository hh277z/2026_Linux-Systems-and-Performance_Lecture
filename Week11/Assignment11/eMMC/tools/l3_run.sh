#!/usr/bin/env bash
# l3_run.sh: app 실행 burst를 켜 둔 채 OTA를 쓴다 (Level 3).
# usage: sudo bash tools/l3_run.sh <name> <burst reads> <period ms>
#        [emmcsim key=value ...] -- <otawriter args without -f>
#   예: sudo bash tools/l3_run.sh base 200 500 -- -s 256 -d
#       sudo bash tools/l3_run.sh mine 200 500 -- -s 256 -m policy -c 128 -q 20
#       sudo bash tools/l3_run.sh ips 200 500 suspend=ips -- -s 256 -d
#   결과: OTA 시간, burst 시간(p50, p90, max), 장치 안 read latency, die별 suspend와 redo
#   TS=<배수>를 주면 모든 장치 시간을 늘린다 (nbd 경로가 장치보다 느린 노트북)
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
name=${1:?usage}; n=${2:?usage}; period=${3:?usage}
shift 3
keys=()
[ -n "${TS:-}" ] && keys+=("time_scale=$TS")      # 느린 노트북: TS=4 bash tools/l3_run.sh ...
while [ $# -gt 0 ] && [ "$1" != "--" ]; do keys+=("$1"); shift; done
[ "${1:-}" = "--" ] && shift
mkdir -p logs/l3
export EMMC_CTL=/tmp/emmcsim.ctl
MNT=/mnt/emmc
bash tools/emmc.sh up "${keys[@]}" > /dev/null || exit 1
bash tools/emmc.sh mkfs > /dev/null || exit 1
dd if=/dev/urandom of=$MNT/ui.bin bs=1M count=64 conv=fsync status=none
sync; echo 3 > /proc/sys/vm/drop_caches
./bin/emmcctl reset > /dev/null
echo "\$ bin/otawriter -f $MNT/ota.bin $* &   bin/uiburst -f $MNT/ui.bin -n $n -p $period"
rm -f /tmp/ota.pid
( echo $BASHPID > /tmp/ota.pid
  exec ./bin/otawriter -f $MNT/ota.bin "$@" ) \
    > "logs/l3/$name-ota.txt" 2>&1 &
./bin/uiburst -f $MNT/ui.bin -n "$n" -p "$period" \
    -w /tmp/ota.pid -o "logs/l3/$name-bursts.csv" \
    > "logs/l3/$name-ui.txt"
wait
./bin/emmcctl stat > "logs/l3/$name-stat.txt"
./bin/emmcctl stat raw > "logs/l3/$name-stat.raw"
./bin/emmcctl trace > "logs/l3/$name-trace.csv"
cat "logs/l3/$name-ota.txt" "logs/l3/$name-ui.txt"
grep -E '^die|read   latency' "logs/l3/$name-stat.txt"
bash tools/emmc.sh down > /dev/null
