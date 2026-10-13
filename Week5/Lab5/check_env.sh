#!/usr/bin/env bash
# Week 5 실습 환경 점검 (실습 0).
set -u
ok()   { printf 'OK    %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; }
for t in gcc make perf bpftrace chrt; do command -v "$t" >/dev/null 2>&1 && ok "$t found" || warn "$t missing"; done
k=$(uname -r); case "$k" in 6.6*) ok "kernel $k (EEVDF)" ;; *) warn "kernel $k (expected 6.6.x; 6.12+ has the fair server and per-task slice)" ;; esac
n=$(nproc); [ "$n" -eq 2 ] && ok "nproc=2" || warn "nproc=$n: 실습은 vCPU 2개 기준이다. %UserProfile%\.wslconfig의 [wsl2]에 processors=2를 넣고 wsl --shutdown (lab.sh는 CPU 수에 맞춰 부하를 늘리므로 그대로 진행해도 된다)"
[ -d /sys/kernel/tracing/events/sched ] || sudo mount -t tracefs tracefs /sys/kernel/tracing 2>/dev/null
for tp in sched:sched_switch sched:sched_wakeup sched:sched_pi_setprio syscalls:sys_enter_futex; do
    sudo bpftrace -l "tracepoint:$tp" 2>/dev/null | grep -q "$tp" && ok "tracepoint $tp" || warn "tracepoint $tp not found"
done
sudo bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); } interval:ms:200 { exit(); }' >/dev/null 2>&1 \
    && ok "bpftrace runs a tracepoint program with args" || warn "bpftrace cannot compile tracepoint args: run bash tools/install_bpftrace.sh again"
v=$(bpftrace --version 2>/dev/null | awk '{print $2}' | tr -d v)
[ -n "$v" ] && [ "$(printf '%s\n0.20\n' "$v" | sort -V | head -n 1)" = "0.20" ] && ok "bpftrace $v (0.20 or later)" || warn "bpftrace $v is older than 0.20: run bash tools/install_bpftrace.sh again"
bs=$(sudo cat /sys/kernel/debug/sched/base_slice_ns 2>/dev/null) || { sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; bs=$(sudo cat /sys/kernel/debug/sched/base_slice_ns 2>/dev/null); }
[ -n "$bs" ] && ok "debugfs base_slice_ns = $bs" || warn "base_slice_ns not readable (실습 3의 slice 단계 생략)"
echo "rt throttling: sched_rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us) / sched_rt_period_us=$(cat /proc/sys/kernel/sched_rt_period_us), rr_timeslice_ms=$(cat /proc/sys/kernel/sched_rr_timeslice_ms)"
sudo chrt -f 10 true 2>/dev/null && ok "SCHED_FIFO allowed" || warn "SCHED_FIFO refused"
sudo chrt -d --sched-runtime 2000000 --sched-deadline 10000000 --sched-period 10000000 0 true 2>/dev/null \
    && ok "SCHED_DEADLINE allowed" || warn "SCHED_DEADLINE refused (shell이 특정 CPU에 pin되어 있으면 거부된다)"
tmp=/tmp/check_env_sched.data; sudo perf sched record -o "$tmp" -- true >/dev/null 2>&1 && ok "perf sched record works" || warn "perf sched record failed"; sudo rm -f "$tmp" "$tmp.old"
