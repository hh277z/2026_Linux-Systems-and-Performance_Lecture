#!/usr/bin/env bash
# Week 11 실습 환경 점검.
set -u
ok() {
    printf 'OK    %s\n' "$*"
}

warn() {
    printf 'WARN  %s\n' "$*"
}

# WSL 커널: 6.6부터 nbd 모듈이 함께 배포된다 (5.15 커널은 config에만 있고 파일이 없다)
k=$(uname -r)
kv=$(echo "$k" | grep -oE '^[0-9]+\.[0-9]+')
if [ "$(printf '%s\n6.6\n' "$kv" | sort -V | head -n 1)" = "6.6" ]; then
    ok "kernel $k"
else
    warn "kernel $k: 6.6 이상이 필요하다 (Windows PowerShell에서 wsl --update 후 wsl --shutdown)"
fi

if sudo modprobe nbd 2>/dev/null && [ -b /dev/nbd0 ]; then
    ok "nbd module, /dev/nbd0"
else
    warn "nbd module을 올릴 수 없다: wsl --update로 커널 6.6 이상을 받는다"
fi

for t in gcc make nbdkit nbd-client fio bpftrace python3 iostat mkfs.ext4; do
    if command -v "$t" >/dev/null 2>&1; then
        ok "$t found"
    else
        warn "$t missing (sudo apt install -y" \
            "nbdkit nbdkit-plugin-dev nbd-client fio bpftrace sysstat)"
    fi
done
if [ -f /usr/include/nbdkit-plugin.h ]; then
    ok "nbdkit-plugin-dev (nbdkit-plugin.h)"
else
    warn "nbdkit-plugin.h missing (sudo apt install nbdkit-plugin-dev)"
fi
v=$(nbdkit --version 2>/dev/null | awk '{print $2}')
if [ -n "$v" ] && [ "$(printf '%s\n1.24\n' "$v" | sort -V | head -n 1)" = "1.24" ]; then
    ok "nbdkit $v"
else
    warn "nbdkit $v: 1.24 이상이 필요하다"
fi

if fio --enghelp=libaio >/dev/null 2>&1; then
    ok "fio libaio engine"
else
    warn "fio에 libaio engine이 없다 (sudo apt install libaio1t64 또는 libaio1)"
fi

# block tracepoint (실습 2의 blkio.bt)
if [ ! -d /sys/kernel/tracing/events/block ]; then
    sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null
fi
if sudo bpftrace -l 'tracepoint:block:block_rq_issue' 2>/dev/null | grep -q block_rq_issue; then
    ok "tracepoint block:block_rq_issue"
else
    warn "tracepoint block:block_rq_issue not found" \
        "(sudo mount -t tracefs tracefs /sys/kernel/tracing)"
fi

# cgroup v2 memory/io controllers (실습 3b-3d의 OTA write limit)
if grep -qw io /sys/fs/cgroup/cgroup.controllers 2>/dev/null && \
   grep -qw memory /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then
    ok "cgroup v2 memory/io controllers"
else
    warn "실습 3b-3d에는 cgroup v2 memory/io controllers가 필요하다. /etc/wsl.conf에서 systemd=true를 확인한다"
fi

# emmcsim은 쓴 data를 RAM에 둔다 (기본 size=1G)
m=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo)
if [ "$m" -ge 2048 ]; then
    ok "MemAvailable ${m} MB"
else
    warn "MemAvailable ${m} MB: 2 GB 이상이 좋다 (.wslconfig의 memory를 늘린다)"
fi
n=$(nproc)
if [ "$n" -ge 2 ]; then
    ok "nproc=$n"
else
    warn "nproc=$n: vCPU 2개 이상이 필요하다"
fi

