#!/usr/bin/env bash
# l1_check.sh: logs/l1/results.csv를 검사한다 (Level 1).
#   1. die/time-scale, plane/page, 공통 NAND/bus 필수 조합이 모두 있는가
#   2. 모든 행의 QD 32 IOPS가 die, NAND 채널, host bus 상한 안에 있는가
#   3. 무작위로 고른 두 행을 다시 재서 IOPS가 15% 안에서 같은가
# usage: sudo bash tools/l1_check.sh
set -u
cd "$(dirname "$0")/.." || exit 1
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행한다"; exit 1; }
[ "$#" -eq 0 ] || { echo "usage: sudo bash tools/l1_check.sh"; exit 2; }
source tools/params.sh > /dev/null
csv=logs/l1/results.csv
[ -f "$csv" ] || { echo "FAIL  $csv가 없다"; exit 1; }
fail=0
python3 - "$csv" "$L1_TIMES" "$L1_BUS" <<'PY' || fail=1
import csv, math, sys
with open(sys.argv[1], newline='') as f:
    rows = list(csv.DictReader(f))
required_columns = {'name', 'keys', 'qd32_iops'}
columns = set(rows[0].keys()) if rows else set()
if not required_columns <= columns:
    print('FAIL  results.csv에 필요한 열이 없음: ' + ', '.join(sorted(required_columns - columns)))
    sys.exit(1)
need_t, need_b = sys.argv[2].split(), sys.argv[3]
def kv(s):
    d = dict(x.split('=', 1) for x in (s or '').split() if '=' in x)
    return d
def number(s, default):
    try:
        return float(s)
    except (TypeError, ValueError):
        return float(default)
def same(k, expected):
    actual = k.get(expected.split('=', 1)[0])
    value = expected.split('=', 1)[1]
    return actual is not None and math.isclose(number(actual, float('nan')),
                                                number(value, float('nan')),
                                                rel_tol=0, abs_tol=1e-9)
def has(k, required):
    return all(same(k, item) for item in required)
def find_row(predicate):
    return any(predicate(kv(r.get('keys', ''))) for r in rows)
ok = True

# (a) Scale the device times so the die sweep is visible through the WSL/NBD path.
missing = []
for dies in (1, 2, 4, 6, 8):
    if not find_row(lambda k, d=dies: has(k, [f'dies={d}', 'time_scale=4'])):
        missing.append(f'dies={dies}, time_scale=4')
print(('OK    ' if not missing else 'FAIL  ') +
      'die sweep (dies=1,2,4,6,8; time_scale=4)' +
      (f"; 누락: {'; '.join(missing)}" if missing else ''))
ok &= not missing

# (b) Each plane/page pair is a controlled sweep from the default 2-die device.
missing = []
for planes in (1, 2):
    for page_kb in (4, 8, 16):
        if not find_row(lambda k, p=planes, s=page_kb:
                        has(k, [f'planes={p}', f'page_kb={s}']) and
                        number(k.get('dies'), 2) == 2 and
                        number(k.get('time_scale'), 1) == 1):
            missing.append(f'planes={planes}, page_kb={page_kb}')
print(('OK    ' if not missing else 'FAIL  ') +
      'plane/page sweep (2 dies, time_scale=1)' +
      (f"; 누락: {'; '.join(missing)}" if missing else ''))
ok &= not missing

# (c) Each die-count point must include the common NAND and bus settings.
configured = need_t + [need_b]
missing = []
for dies in (1, 2, 4):
    if not find_row(lambda k, d=dies:
                    has(k, [f'dies={d}', *configured]) and
                    number(k.get('time_scale'), 1) == 1):
        missing.append(f'dies={dies}')
print(('OK    ' if not missing else 'FAIL  ') +
      f"configured NAND/bus sweep ({' '.join(configured)}; dies=1,2,4)" +
      (f"; 누락: {'; '.join(missing)}" if missing else ''))
ok &= not missing

# The assignment requires a unique row name for every point.
names = [r.get('name', '') for r in rows if r.get('name', '')]
duplicates = sorted({name for name in names if names.count(name) > 1})
print(('OK    ' if not duplicates else 'FAIL  ') + '모든 측정 행의 이름이 고유함' +
      (f"; 중복: {', '.join(duplicates)}" if duplicates else ''))
ok &= not duplicates
bad_rows = [r.get('name') or f'row {i + 2}' for i, r in enumerate(rows)
            if not r.get('name') or not math.isfinite(number(r.get('qd32_iops'), float('nan')))
            or number(r.get('qd32_iops'), 0) <= 0]
print(('OK    ' if not bad_rows else 'FAIL  ') + '모든 행에 이름과 유효한 QD 32 IOPS가 있음' +
      (f"; 오류: {', '.join(bad_rows)}" if bad_rows else ''))
ok &= not bad_rows

for r in rows:
    k = kv(r['keys'])
    ts = number(k.get('time_scale'), 1)
    dies = number(k.get('dies'), 2)
    tr = number(k.get('t_r_us'), 120) * ts
    bus = number(k.get('host_mbps'), 400)
    nand = number(k.get('nand_mbps'), 1600)
    cmd = number(k.get('t_cmd_us'), 1) * ts
    host_us = cmd + 4096 / bus * ts
    nand_us = 4096 / nand * ts
    # Independent resource ceilings for one 4 KB read: die service, NAND channel,
    # and host command+data bus. The observed result may be lower due to contention.
    bound = min(dies / tr, 1 / host_us, 1 / nand_us) * 1e6
    if number(r.get('qd32_iops'), 0) > bound * 1.05:
        print(f"FAIL  {r['name']}: QD 32 IOPS {r['qd32_iops']}가 die/NAND/host 상한 {bound:.0f}을 넘는다")
        ok = False
if ok:
    print(f"OK    {len(rows)}개 행 모두 필수 조합과 상한을 만족")
sys.exit(0 if ok else 1)
PY
# 두 행을 다시 잰다
mapfile -t pick < <(tail -n +2 "$csv" | grep -v '^recheck-' | shuf -n 2)
for row in "${pick[@]}"; do
    name=$(echo "$row" | cut -d, -f1)
    keys=$(echo "$row" | cut -d, -f2)
    old=$(echo "$row" | cut -d, -f6)
    bash tools/l1_point.sh "recheck-$name" $keys > /dev/null
    new=$(tail -n 1 "$csv" | cut -d, -f6)
    sed -i '$d' "$csv"                                  # 다시 잰 행은 결과에서 뺀다
    if awk -v a="$old" -v b="$new" 'BEGIN { exit !(b >= a * 0.85 && b <= a * 1.15) }'; then
        echo "OK    다시 잰 $name: QD 32 IOPS $old -> $new"
    else
        echo "FAIL  다시 잰 $name: QD 32 IOPS $old -> $new (15% 밖)"; fail=1
    fi
done
[ $fail -eq 0 ] && echo "result: PASS" || echo "result: FAIL"
exit $fail
