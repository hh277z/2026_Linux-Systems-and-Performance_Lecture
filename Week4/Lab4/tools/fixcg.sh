#!/usr/bin/env bash
# cgroup fixes for the jank case study: lab/fg (renderer) and lab/bg (indexer).
# usage: sudo bash tools/fixcg.sh setup [bg_weight]     create lab/fg and lab/bg, cpu.weight fg=100 bg=<w> (default 20)
#        sudo bash tools/fixcg.sh max <quota_us> <period_us>   cap lab/bg, e.g. 30000 100000 = 30 % of one CPU
#        sudo bash tools/fixcg.sh cpuset <cpus>          confine lab/bg to <cpus>, e.g. 1
#        sudo bash tools/fixcg.sh show
#        sudo bash tools/fixcg.sh teardown
set -eu
root=${CG_ROOT:-/sys/fs/cgroup}
lab="$root/lab"
case "${1:-}" in
    setup)
        grep -qw cpu "$root/cgroup.controllers" || { echo "no cgroup v2 cpu controller (systemd=true in /etc/wsl.conf, then wsl --shutdown)"; exit 1; }
        echo "+cpu +cpuset" > "$root/cgroup.subtree_control" 2>/dev/null || true
        mkdir -p "$lab/fg" "$lab/bg"
        echo "+cpu +cpuset" > "$lab/cgroup.subtree_control"                    # KEY both controllers for the two children
        echo 100 > "$lab/fg/cpu.weight"
        echo "${2:-20}" > "$lab/bg/cpu.weight"                                 # KEY culprit's share among siblings
        echo max > "$lab/bg/cpu.max"
        echo "lab/fg cpu.weight=$(cat "$lab/fg/cpu.weight")  lab/bg cpu.weight=$(cat "$lab/bg/cpu.weight") cpuset.cpus='$(cat "$lab/bg/cpuset.cpus")' (empty = parent's)"
        ;;
    max)
        echo "${2:?quota_us} ${3:?period_us}" > "$lab/bg/cpu.max"             # KEY hard cap on the culprit: quota per period
        echo "lab/bg cpu.max=$(cat "$lab/bg/cpu.max")"
        ;;
    cpuset)
        echo "${2:?cpus}" > "$lab/bg/cpuset.cpus"                             # KEY move the culprit to other CPUs, overriding its affinity
        echo "lab/bg cpuset.cpus=$(cat "$lab/bg/cpuset.cpus") effective=$(cat "$lab/bg/cpuset.cpus.effective")"
        ;;
    show)
        for g in fg bg; do echo "lab/$g weight=$(cat "$lab/$g/cpu.weight") max='$(cat "$lab/$g/cpu.max")' cpuset='$(cat "$lab/$g/cpuset.cpus")' procs=$(tr '\n' ' ' < "$lab/$g/cgroup.procs")"; done
        ;;
    teardown)
        for g in fg bg; do for p in $(cat "$lab/$g/cgroup.procs" 2>/dev/null); do echo "$p" > "$root/cgroup.procs" 2>/dev/null || true; done; rmdir "$lab/$g" 2>/dev/null || true; done
        rmdir "$lab" 2>/dev/null && echo "removed $lab" || echo "$lab still busy"
        ;;
    *) echo "usage: fixcg.sh setup [bg_weight] | max <quota_us> <period_us> | cpuset <cpus> | show | teardown"; exit 1 ;;
esac
