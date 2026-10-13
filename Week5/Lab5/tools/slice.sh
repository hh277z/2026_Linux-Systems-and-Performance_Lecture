#!/usr/bin/env bash
# fair class의 base slice 확인과 변경 (실습 3). 6.6의 debugfs 값은 모든 fair task에 공통이다.
# usage: sudo bash tools/slice.sh show | set <ns> | restore
set -eu
f=/sys/kernel/debug/sched/base_slice_ns
[ -r "$f" ] || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
mkdir -p logs
case "${1:-show}" in
    show)
        echo "base_slice_ns : $(cat $f)"
        hz=$( (zcat /proc/config.gz 2>/dev/null || cat "/boot/config-$(uname -r)" 2>/dev/null) | awk -F= '/^CONFIG_HZ=/ {print $2}')
        echo "CONFIG_HZ     : ${hz:-unknown}   (tick = $( [ -n "$hz" ] && echo "$((1000 / hz)) ms" || echo "?"))"
        ;;
    set)
        [ -f logs/base_slice_ns.orig ] || cat $f > logs/base_slice_ns.orig
        echo "${2:?ns}" > $f                                                        # KEY how soon: 한 번 CPU를 잡으면 얼마나 오래
        echo "base_slice_ns now: $(cat $f)  (원래 값: $(cat logs/base_slice_ns.orig))"
        ;;
    restore)
        [ -f logs/base_slice_ns.orig ] && cat logs/base_slice_ns.orig > $f && rm -f logs/base_slice_ns.orig
        echo "base_slice_ns restored: $(cat $f)"
        ;;
    *)
        echo "usage: slice.sh show | set <ns> | restore" >&2
        exit 1
        ;;
esac
