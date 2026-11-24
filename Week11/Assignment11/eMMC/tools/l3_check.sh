#!/usr/bin/env bash
# l3_check.sh: otawriter -m policy (src/apps/otapolicy.c)를 검사한다 (Level 3).
#   기준: suspension 없는 장치에서 O_DIRECT, 1 MB chunk, 제한 없는 OTA
#   통과: burst p90이 기준의 1/2 이하, OTA 시간이 기준의 2배 이하
#   장치 안 read p99는 비교만 출력한다 (burst의 첫 read는 정책과 상관없이 진행 중인 program을 기다린다)
# usage: sudo bash tools/l3_check.sh <policy args>
#   예: sudo bash tools/l3_check.sh -c 128 -q 20
#   nbd 경로가 느리면 장치 시간을 늘려서 검사한다: sudo TS=4 bash tools/l3_check.sh -c 128 -q 20
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
source tools/params.sh
make -s || exit 1
mkdir -p logs/l3
bash tools/l3_run.sh check-base "$L3_N" "$L3_PERIOD" -- -s "$L3_OTA" -d > logs/l3/check-base.txt 2>&1
bash tools/l3_run.sh check-policy "$L3_N" "$L3_PERIOD" -- \
    -s "$L3_OTA" -m policy "$@" > logs/l3/check-policy.txt 2>&1
ota()   { sed -n 's/^ota: [0-9]* MB in \([0-9.]*\) s.*/\1/p' "logs/l3/$1-ota.txt"; }
burst() { sed -n 's/.*burst p50 [0-9.]* ms  p90 \([0-9.]*\) ms.*/\1/p' "logs/l3/$1-ui.txt"; }
dev()   { grep '^read_lat_p99_us=' "logs/l3/$1-stat.raw" | cut -d= -f2; }
b_ota=$(ota check-base); p_ota=$(ota check-policy)
b_bu=$(burst check-base); p_bu=$(burst check-policy)
b_dev=$(dev check-base); p_dev=$(dev check-policy)
printf '%-10s %10s %14s %18s\n' run "OTA s" "burst p90 ms" "device read p99 us"
printf '%-10s %10s %14s %18s\n' base "${b_ota:-?}" "${b_bu:-?}" "${b_dev:-?}"
printf '%-10s %10s %14s %18s\n' policy "${p_ota:-?}" "${p_bu:-?}" "${p_dev:-?}"
fail=0
chk() {   # chk <desc> <awk condition>
    if awk -v bo="$b_ota" -v po="$p_ota" -v bb="$b_bu" -v pb="$p_bu" \
        -v bd="$b_dev" -v pd="$p_dev" "BEGIN { exit !($2) }"; then
        echo "OK    $1"
    else
        echo "FAIL  $1"; fail=1
    fi
}
[ -n "$p_ota" ] && [ -n "$b_ota" ] || { echo "FAIL  OTA가 끝나지 않았다 (logs/l3/check-*-ota.txt)"; exit 1; }
chk "burst p90이 기준의 1/2 이하" "pb * 2 <= bb"
chk "OTA 시간이 기준의 2배 이하" "po <= 2 * bo"
[ $fail -eq 0 ] && echo "result: PASS" || echo "result: FAIL"
exit $fail
