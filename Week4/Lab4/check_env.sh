#!/usr/bin/env bash
# Week 4 실습 환경 점검.
set -u
ok()   { printf 'OK    %s\n' "$*"; }
warn() { printf 'WARN  %s\n' "$*"; }
for t in gcc make perf lscpu taskset ps perl; do command -v "$t" >/dev/null 2>&1 && ok "$t found" || warn "$t missing"; done
k=$(uname -r); case "$k" in 6.6*) ok "kernel $k" ;; *) warn "kernel $k (expected 6.6.x; wsl --update)" ;; esac
n=$(nproc); [ "$n" -ge 2 ] && ok "nproc=$n" || warn "nproc=$n (processors=2 in .wslconfig)"
[ "$(ps -p 1 -o comm=)" = systemd ] && ok "PID 1 is systemd" || warn "PID 1 is not systemd: [boot] systemd=true in /etc/wsl.conf, then wsl --shutdown"
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then grep -qw cpu /sys/fs/cgroup/cgroup.controllers && grep -qw cpuset /sys/fs/cgroup/cgroup.controllers && ok "cgroup v2 cpu+cpuset controllers" || warn "cgroup v2 without cpu or cpuset controller"; else warn "cgroup v2 not mounted (systemd=true needed for 실습 3)"; fi
echo "caches: $(lscpu | grep -iE 'L1d|L2|L3' | sed 's/ \+/ /g' | tr '\n' ';')"
[ -z "$(lscpu | grep -i 'L1d')" ] && warn "lscpu shows no cache sizes; use the ws_demo cliffs only"
tmp=$(mktemp); sudo perf stat -e task-clock,cpu-migrations -- true >/dev/null 2>"$tmp" && ok "perf stat works" || warn "perf stat failed (see Week 1 tools/perf.sh)"
sudo perf record -F 99 -e cpu-clock -g -o "$tmp" -- true >/dev/null 2>&1 && ok "perf record cpu-clock -g works" || warn "perf record failed"
sudo rm -f "$tmp" "$tmp.old"
tmp2=$(mktemp); sudo perf sched record -o "$tmp2" -- true >/dev/null 2>&1 && ok "perf sched record works" || warn "perf sched record failed"; sudo rm -f "$tmp2" "$tmp2.old"
