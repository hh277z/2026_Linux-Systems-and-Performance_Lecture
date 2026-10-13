#!/usr/bin/env bash
# bpftrace 설치와 확인 (실습 0).
# 1. apt로 설치한다.
# 2. tracepoint 인자(args->...)를 쓰는 프로그램을 실제로 한 번 돌려 본다.
# 3. 실패하거나 버전이 0.20보다 낮으면 bpftrace 공식 release의 정적 실행 파일(v0.20.4)을 /usr/local/bin에 설치한다.
#    Ubuntu 22.04의 apt 패키지(0.14)는 kernel BTF를 읽지 못하면 tracepoint 인자의 타입(pid_t 등)을 해석하지 못한다.
#    WSL2에는 kernel header가 없어서 header로 대신할 수도 없다.
set -u
REL=v0.20.4
URL=https://github.com/bpftrace/bpftrace/releases/download/$REL/bpftrace
. /etc/os-release
echo "distro: $PRETTY_NAME  kernel: $(uname -r)"

sudo apt-get update -qq
sudo apt-get install -y -qq bpftrace curl >/dev/null                             # KEY 먼저 apt로 설치

if [ ! -d /sys/kernel/tracing/events/sched ]; then
    sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null                 # KEY tracefs가 없으면 tracepoint를 찾지 못한다
fi
[ -d /sys/kernel/tracing/events/sched ] && echo "OK    tracefs mounted" || echo "WARN  tracefs not mounted"

works() {   # tracepoint 인자를 읽는 짧은 프로그램이 실제로 컴파일되고 실행되는지 본다
    sudo bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); } interval:ms:300 { exit(); }' >/dev/null 2>&1
}
new_enough() {   # 0.20 이상인가
    v=$(bpftrace --version 2>/dev/null | awk '{print $2}' | tr -d v)
    [ -n "$v" ] && [ "$(printf '%s\n0.20\n' "$v" | sort -V | head -n 1)" = "0.20" ]
}

if works && new_enough; then
    echo "OK    bpftrace $(bpftrace --version | awk '{print $2}') ($(command -v bpftrace))"
    exit 0
fi

echo "INFO  apt의 bpftrace($(bpftrace --version 2>/dev/null | awk '{print $2}'))로는 실습 스크립트를 실행할 수 없어 $REL 정적 실행 파일을 설치한다"
tmp=$(mktemp)
if curl -fL --progress-bar -o "$tmp" "$URL"; then                                # KEY 공식 release의 정적 실행 파일 (약 160MB)
    sudo install -m 0755 "$tmp" /usr/local/bin/bpftrace                           # /usr/local/bin이 /usr/bin보다 먼저 찾아진다
    rm -f "$tmp"
    hash -r
else
    rm -f "$tmp"
    echo "WARN  다운로드 실패. 브라우저로 $URL 을 받아 sudo install -m 0755 <받은 파일> /usr/local/bin/bpftrace 로 설치한다"
    exit 1
fi
if works && new_enough; then
    echo "OK    bpftrace $(sudo bpftrace --version | awk '{print $2}') (/usr/local/bin/bpftrace)"
else
    echo "WARN  bpftrace가 여전히 동작하지 않는다. sudo bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); }' 의 오류를 조교에게 보여 준다"
    exit 1
fi
