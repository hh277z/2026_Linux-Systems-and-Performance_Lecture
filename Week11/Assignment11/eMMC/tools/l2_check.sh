#!/usr/bin/env bash
# l2_check.sh: logger -m policy (src/apps/logpolicy.c)를 검사한다 (Level 2).
#   1. budget  line 하나당 NAND program byte가 line마다 fsync하는 logger의 1/10 이하인가
#   2. error   error line이 lp_append가 돌아올 때 durable인가 (logger가 직접 확인한다)
#   3. powercut info line만 초당 200개 쓰다가 무작위 시점에 세 번 전원을 끊는다. acked line이 모두 남고,
#              잃은 line이 RAM에 둘 수 있는 양(max_delay와 buf_kb 중 작은 쪽의 두 배 + 50) 이하인가.
#              이 속도에서는 buffer가 차기 전에 max_delay가 먼저 온다
# usage: sudo bash tools/l2_check.sh
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
[ "$#" -eq 0 ] || { echo "usage: sudo bash tools/l2_check.sh"; exit 2; }
source tools/params.sh
make -s || exit 1
RATE=4000; PC_RATE=200; BUF=64
ARGS="-m policy -r $RATE -s $L2_LINE -e $L2_ERR -t $L2_DELAY -b $BUF"
PC_ARGS="-m policy -r $PC_RATE -s $L2_LINE -e 0 -t $L2_DELAY -b $BUF"
mkdir -p logs/l2
export EMMC_CTL=/tmp/emmcsim.ctl
fail=0
per_line()
{
    awk -F'[ ,]+' \
        '/^per line/ { for (i = 1; i <= NF; i++) if ($i == "program") print $(i + 1) }' \
        "$1"
}

echo "== 1. budget: line마다 fsync (기준) / policy"
bash tools/l2_run.sh check-line -- -m line -n 1000 -s "$L2_LINE" > logs/l2/check-line.txt 2>&1
bash tools/l2_run.sh check-policy -- $ARGS -n 20000 > logs/l2/check-policy.txt 2>&1
base=$(per_line logs/l2/check-line-perline.txt); mine=$(per_line logs/l2/check-policy-perline.txt)
echo "  NAND program per line: line mode ${base:-?} B, policy ${mine:-?} B"
if [ -n "$base" ] && [ -n "$mine" ] && \
    awk -v a="$base" -v b="$mine" 'BEGIN { exit !(b * 10 <= a) }'; then
    echo "OK    policy가 1/10 이하"
else
    echo "FAIL  policy의 line당 NAND program이 line mode의 1/10보다 크다"; fail=1
fi
echo "== 2. error line"
if grep -q 'is not durable after lp_append' logs/l2/check-policy-blkio.txt || \
    ! grep -q '^mode=policy' logs/l2/check-policy-blkio.txt; then
    echo "FAIL  logger가 끝까지 돌지 못했다:"\
        "$(grep -m1 -E '^logger:|lp_' logs/l2/check-policy-blkio.txt)"
    fail=1
else
    echo "OK    모든 error line이 lp_append가 돌아올 때 durable"
fi

echo "== 3. powercut x 3"
buf_lines=$(( BUF * 1024 / L2_LINE )); time_lines=$(( PC_RATE * L2_DELAY / 1000 ))
bound=$(( 2 * (buf_lines < time_lines ? buf_lines : time_lines) + 50 ))
for k in 1 2 3; do
    bash tools/emmc.sh up > /dev/null && bash tools/emmc.sh mkfs > /dev/null || exit 1
    ack=logs/l2/check-pc$k-ack.txt
    ./bin/logger -f /mnt/emmc/pc.log $PC_ARGS -n 100000000 \
        -a "$ack" > "logs/l2/check-pc$k-logger.txt" 2>&1 &
    pid=$!
    sleep "$(awk -v s=$RANDOM 'BEGIN { printf "%.2f", 1.0 + (s % 2000) / 1000 }')"
    bash tools/emmc.sh powercut > /dev/null
    wait "$pid" 2>/dev/null
    bash tools/emmc.sh restore > /dev/null || { echo "FAIL  restore"; fail=1; continue; }
    ./bin/logger -c /mnt/emmc/pc.log -a "$ack" > "logs/l2/check-pc$k.txt"
    lost=$(sed -n 's/^lines lost *//p' "logs/l2/check-pc$k.txt")
    if grep -q 'result: PASS' "logs/l2/check-pc$k.txt" && [ "${lost:-999999}" -le "$bound" ]; then
        echo "OK    powercut $k: acked line 모두 남음, 잃은 line $lost (한도 $bound)"
    else
        echo "FAIL  powercut $k: $(grep '^result' "logs/l2/check-pc$k.txt"),"\
            "잃은 line ${lost:-?} (한도 $bound)"
        fail=1
    fi
    bash tools/emmc.sh down > /dev/null
done
[ $fail -eq 0 ] && echo "result: PASS" || echo "result: FAIL"
exit $fail
