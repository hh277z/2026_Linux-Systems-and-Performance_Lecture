#!/usr/bin/env bash
# l1_point.sh: 장치 설정 하나를 재고 logs/l1/results.csv에 한 줄을 더한다 (Level 1).
# usage: sudo bash tools/l1_point.sh <name> [key=value ...]
#   잰다: 4 KB random read QD 1과 QD 32 (각 3초), 1 MB sequential write QD 4 (64 MB)
#   busy 비율은 fio가 실제로 돈 시간을 분모로 쓴다
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
name=${1:?usage: l1_point.sh <name> [key=value ...]}
shift
mkdir -p logs/l1
out=logs/l1/results.csv
[ -f "$out" ] || echo \
    "name,keys,qd1_iops,qd1_fio_p50_us,qd1_dev_p50_us,"\
    "qd32_iops,qd32_die_busy_pct,qd32_bus_busy_pct,"\
    "write_mbps,write_die_busy_pct" \
    > "$out"
export EMMC_CTL=/tmp/emmcsim.ctl
bash tools/emmc.sh up "$@" > "logs/l1/$name-up.txt" || exit 1
./bin/emmcctl conf > "logs/l1/$name-conf.txt"
run() {   # run <tag> <fio args>
    ./bin/emmcctl reset > /dev/null
    fio --name="$1" --filename=/dev/nbd0 --direct=1 --ioengine=libaio --size=512M "${@:2}" \
        --output-format=json --output="logs/l1/$name-$1.json" > /dev/null 2>&1
    ./bin/emmcctl stat raw > "logs/l1/$name-$1.raw"
}
run qd1 --rw=randread --bs=4k --iodepth=1 --runtime=3 --time_based
run qd32 --rw=randread --bs=4k --iodepth=32 --runtime=3 --time_based
run write --rw=write --bs=1M --iodepth=4 --size=64M
bash tools/emmc.sh down > /dev/null
python3 - "$name" "$*" "logs/l1/$name" >> "$out" <<'PY'
import json, sys
name, keys, base = sys.argv[1:4]
def fio(tag):
    t = open(f'{base}-{tag}.json').read()
    j = json.loads(t[t.index('{'):])['jobs'][0]
    d = j['read'] if j['read']['io_bytes'] else j['write']
    return d, j['job_runtime'] * 1e6          # runtime in ns
def raw(tag):
    return {
        k: float(v)
        for k, v in (
            l.strip().split('=', 1)
            for l in open(f'{base}-{tag}.raw')
            if '=' in l
        )
    }
q1, _ = fio('qd1')
r1 = raw('qd1')
q32, t32 = fio('qd32')
r32 = raw('qd32')
w, tw = fio('write')
rw = raw('write')
busy = lambda r, d: (
    r[f'die{d}_read_ns'] + r[f'die{d}_prog_ns']
    + r[f'die{d}_erase_ns'] + r[f'die{d}_redo_ns']
)
p50 = q1['clat_ns']['percentile']['50.000000'] / 1e3
print(
    f"{name},{keys.replace(',', ' ')},{q1['iops']:.0f},{p50:.0f},"
    f"{r1['read_lat_p50_us']:.0f},{q32['iops']:.0f},"
    f"{100 * busy(r32, 0) / t32:.1f},"
    f"{100 * r32['bus_busy_ns'] / t32:.1f},"
    f"{w['bw_bytes'] / 1e6:.1f},{100 * busy(rw, 0) / tw:.1f}"
)
PY
tail -n 1 "$out"
