#!/usr/bin/env bash
# Week 6 실습 환경 점검.
set -u
ok()   { printf 'OK    %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; }

for t in gcc make gdb bpftrace; do
    command -v "$t" >/dev/null 2>&1 && ok "$t found" || warn "$t missing (sudo apt install $t)"
done
n=$(nproc)
[ "$n" -ge 2 ] && ok "nproc=$n" || warn "nproc=$n: 실습 1, 2는 CPU가 2개 이상 있어야 thread가 동시에 실행된다"

# ThreadSanitizer: 작은 프로그램을 빌드해서 실제로 실행해 본다
tmp=$(mktemp -d)
cat > "$tmp/t.c" <<'EOF'
#include <pthread.h>
static void *f(void *a) { return a; }
int main(void) { pthread_t t; pthread_create(&t, 0, f, 0); pthread_join(t, 0); return 0; }
EOF
if gcc -fsanitize=thread -pthread -o "$tmp/t" "$tmp/t.c" 2>"$tmp/err"; then
    if "$tmp/t" >"$tmp/out" 2>&1; then
        ok "ThreadSanitizer builds and runs"
    else
        warn "ThreadSanitizer program failed: $(head -n 1 "$tmp/out")"
        grep -q 'unexpected memory mapping' "$tmp/out" && warn "  -> sudo sysctl vm.mmap_rnd_bits=28 후 다시 실행한다"
    fi
else
    warn "gcc -fsanitize=thread failed: $(head -n 1 "$tmp/err") (sudo apt install libtsan0)"
fi
rm -rf "$tmp"

# bpftrace (Week 5 실습 1에서 설치), libc uprobe, futex tracepoint
v=$(bpftrace --version 2>/dev/null | awk '{print $2}' | tr -d v)
[ -n "$v" ] && [ "$(printf '%s\n0.20\n' "$v" | sort -V | head -n 1)" = "0.20" ] && ok "bpftrace $v" \
    || warn "bpftrace $v: 0.20 이상이 필요하다 (Week 5의 tools/install_bpftrace.sh)"
[ -d /sys/kernel/tracing/events/syscalls ] || sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null
sudo bpftrace -l 'tracepoint:syscalls:sys_enter_futex' 2>/dev/null | grep -q futex \
    && ok "tracepoint syscalls:sys_enter_futex" || warn "tracepoint syscalls:sys_enter_futex not found"
sudo bpftrace -l 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:pthread_mutex_lock' 2>/dev/null | grep -q pthread_mutex_lock \
    && ok "uprobe on libc pthread_mutex_lock" || warn "libc uprobe not found (/lib/x86_64-linux-gnu/libc.so.6)"

# 실습 2의 futex 호출 수: perf trace, 안 되면 strace
if sudo perf trace -s -- true >/dev/null 2>&1; then
    ok "perf trace works"
elif command -v strace >/dev/null 2>&1; then
    warn "perf trace failed: 실습 2는 strace로 대신 센다 (lab.sh가 자동으로 바꾼다)"
else
    warn "perf trace failed and strace missing: sudo apt install strace"
fi

# gdb로 실행 중인 다른 process를 보려면 ptrace 권한이 필요하다
ps=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)
ok "ptrace_scope=$ps (1 이상이면 gdb -p는 sudo로 실행한다. lab.sh 3hang은 root로 실행된다)"
