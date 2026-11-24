#!/usr/bin/env bash
# l2_run.sh: 새 장치에 ext4를 만들고 logger 한 번을 blkio.bt와 함께 잰다 (Level 2).
# usage: sudo bash tools/l2_run.sh <name> [emmcsim key=value ...] -- <logger args without -f>
#   예: sudo bash tools/l2_run.sh mine -- -m policy -n 20000 -r 4000 -s 200 -e 100 -t 50 -b 64
#   결과: logs/l2/<name>-*.txt, 그리고 line 하나당 host write, NAND program, padding, FLUSH
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
name=${1:?usage: l2_run.sh <name> [key=value ...] -- <logger args>}
shift
keys=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do keys+=("$1"); shift; done
[ "${1:-}" = "--" ] && shift
mkdir -p logs/l2
export EMMC_CTL=/tmp/emmcsim.ctl
bash tools/emmc.sh up "${keys[@]}" > /dev/null || exit 1
bash tools/emmc.sh mkfs > /dev/null || exit 1
./bin/emmcctl reset > /dev/null
echo "\$ bpftrace tools/blkio.bt -c \"bin/logger -f /mnt/emmc/$name.log $*\""
bpftrace tools/blkio.bt -c "./bin/logger -f /mnt/emmc/$name.log $*" > "logs/l2/$name-blkio.txt" 2>&1
./bin/emmcctl stat > "logs/l2/$name-stat.txt"
./bin/emmcctl stat raw > "logs/l2/$name-stat.raw"
grep -E '^mode=|^sync latency|^logger:' "logs/l2/$name-blkio.txt"
grep -E '^host|^nand|NAND bytes|flush  latency|write  latency' "logs/l2/$name-stat.txt"
lines=$(sed -n 's/^mode=.* lines=\([0-9]*\) .*/\1/p' "logs/l2/$name-blkio.txt")
r() { grep "^$1=" "logs/l2/$name-stat.raw" | cut -d= -f2; }
[ -n "$lines" ] && awk \
    -v hb="$(r bytes_written)" -v nb="$(r nand_bytes)" \
    -v pad="$(r pad_bytes)" -v fl="$(r host_flushes)" -v n="$lines" \
    'BEGIN {
        printf "per line: host write %.0f B, NAND program %.0f B " \
               "(padding %.0f B), FLUSH %.3f\n", hb/n, nb/n, pad/n, fl/n
    }' \
    | tee "logs/l2/$name-perline.txt"
grep -E '^@requests' "logs/l2/$name-blkio.txt"
bash tools/emmc.sh down > /dev/null
