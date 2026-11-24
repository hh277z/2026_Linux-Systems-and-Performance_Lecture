#!/usr/bin/env bash
# Week 11 실습 환경 점검.
set -u
ok()   { printf 'OK    %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; }

# WSL 커널: 6.6부터 nbd 모듈이 함께 배포된다 (5.15 커널은 config에만 있고 파일이 없다)
k=$(uname -r)
kv=$(echo "$k" | grep -oE '^[0-9]+\.[0-9]+')
[ "$(printf '%s\n6.6\n' "$kv" | sort -V | head -n 1)" = "6.6" ] && ok "kernel $k" \
    || warn "kernel $k: 6.6 이상이 필요하다 (Windows PowerShell에서 wsl --update 후 wsl --shutdown)"
if sudo modprobe nbd 2>/dev/null && [ -b /dev/nbd0 ]; then ok "nbd module, /dev/nbd0"
else warn "nbd module을 올릴 수 없다: wsl --update로 커널 6.6 이상을 받는다"; fi

for t in gcc make nbdkit nbd-client fio bpftrace python3 iostat mkfs.ext4; do
    command -v "$t" >/dev/null 2>&1 \
        && ok "$t found" \
        || warn "$t missing (sudo apt install -y" \
            "nbdkit nbdkit-plugin-dev nbd-client fio bpftrace sysstat)"
done
[ -f /usr/include/nbdkit-plugin.h ] \
    && ok "nbdkit-plugin-dev (nbdkit-plugin.h)" \
    || warn "nbdkit-plugin.h missing (sudo apt install" "nbdkit-plugin-dev)"
v=$(nbdkit --version 2>/dev/null | awk '{print $2}')
[ -n "$v" ] && \
    [ "$(printf '%s\n1.24\n' "$v" | sort -V | head -n 1)" = "1.24" ] \
    && ok "nbdkit $v" \
    || warn "nbdkit $v: 1.24 이상이 필요하다"
fio --enghelp=libaio >/dev/null 2>&1 \
    && ok "fio libaio engine" \
    || warn "fio에 libaio engine이 없다 (sudo apt install" \
        "libaio1t64 또는 libaio1)"

# block tracepoint (실습 2의 blkio.bt)
[ -d /sys/kernel/tracing/events/block ] || sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null
sudo bpftrace -l 'tracepoint:block:block_rq_issue' 2>/dev/null | grep -q block_rq_issue \
    && ok "tracepoint block:block_rq_issue" \
    || warn "tracepoint block:block_rq_issue not found" \
        "(sudo mount -t tracefs tracefs /sys/kernel/tracing)"

# cgroup v2 io controller (실습 3-5, Week 4와 같은 systemd=true 설정)
grep -qw io /sys/fs/cgroup/cgroup.controllers 2>/dev/null && ok "cgroup v2 io controller" \
    || warn "cgroup v2 io controller가 없다: /etc/wsl.conf에 [boot] systemd=true (Week 4 설정). 실습 3-5만 건너뛴다"

# emmcsim은 쓴 data를 RAM에 둔다 (기본 size=1G)
m=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
[ "$m" -ge 2048 ] \
    && ok "MemAvailable ${m} MB" \
    || warn "MemAvailable ${m} MB: 2 GB 이상이 좋다" \
        "(.wslconfig의 memory를 늘린다)"
n=$(nproc)
[ "$n" -ge 2 ] && ok "nproc=$n" || warn "nproc=$n: vCPU 2개 이상이 필요하다"
python3 -c 'import matplotlib' 2>/dev/null \
    && ok "python3 matplotlib (tools/plot.py)" \
    || warn "matplotlib 없음: 그림만 못 그린다" \
        "(sudo apt install python3-matplotlib)"
