#!/usr/bin/env bash
# Week 5 실습 단계를 터미널 하나로 실행한다.
# 부하(B)는 background로 띄우고, 측정할 프로그램(A)과 관측 명령(C)을 알맞은 시점에 실행한 뒤 결과를 모아 보여 준다.
# 실행하는 명령은 모두 "$ ..."로 화면에 찍고, 원본 출력은 logs/에 남긴다.
#
# usage: bash tools/lab.sh <step>
#   0        실습 0 4단계  busy thread + sched_switch one-liner
#   1a       실습 1 (a)    일반 busy 2개 + DEADLINE thread (job 1.5ms, runtime 2ms / period 10ms) + chrt -p
#   1b       실습 1 (b)    일반 busy 2개 + DEADLINE thread 2개 (job이 runtime보다 큼) + perf sched timehist
#   1c       실습 1 (c)    admission control (runtime 7ms / period 10ms를 세 thread가 요청)
#   2fifo    실습 2 1단계  CPU 0에 FIFO 50 두 개 + 일반 하나
#   2rr      실습 2 2단계  CPU 0에 Round Robin 50 두 개 + 일반 하나 + perf sched timehist + dmesg
#   2pi      실습 2 3~4단계 pi_demo (none, inherit) + piwatch.bt
#   3 <tag> <indexer spec>   실습 3 한 조건 (예: bash tools/lab.sh 3 nice10 busy,cpu=0,nice=10)
#   3slice   실습 3 3단계  base slice를 0.3ms로 줄여 한 번 실행하고 원래 값으로 되돌린다
set -u
# script 전체를 root로 한 번만 실행한다. sudo를 background로 띄우면 sudo가 터미널을 raw mode로 바꿔
# 출력이 계단처럼 밀리고, 미리 입력해 둔 명령을 가로챈다. 그래서 sudo는 맨 처음 한 번만 부른다.
if [ "$(id -u)" -ne 0 ]; then
    echo "sudo 암호를 한 번 묻는다. 이 script는 root로 실행된다."
    exec sudo bash "$0" "$@"
fi
sudo() { "$@"; }                                   # 이미 root이므로 script 안의 sudo는 그대로 실행한다
OWNER="${SUDO_UID:-0}:${SUDO_GID:-0}"
cd "$(dirname "$0")/.." || exit 1
mkdir -p logs
trap 'chown -R "$OWNER" logs bin 2>/dev/null' EXIT    # logs와 bin을 원래 사용자 소유로 되돌린다
[ -x bin/mtwork ] && [ -x bin/pi_demo ] || make -s || exit 1
NCPU=$(nproc)
[ "$NCPU" -gt 32 ] && NCPU=32

pids=()
cleanup() { for p in "${pids[@]}"; do sudo kill "$p" 2>/dev/null; done; }
trap 'echo; echo "중단: background 프로그램을 정리한다"; cleanup; exit 130' INT TERM

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
show() { echo "\$ $*"; }
run_bg() {          # run_bg <log> <command...> : background로 실행하고 출력은 log에 남긴다
    local log=$1; shift
    show "$* > $log &"
    "$@" > "$log" 2>&1 < /dev/null &               # 터미널 입력을 읽지 않게 stdin을 끊는다
    pids+=($!)
}
wait_tids() {       # 프로그램이 thread를 다 만들고 TIDS 줄을 출력할 때까지 기다린다
    for _ in $(seq 1 50); do grep -q '^TIDS=' "$1" 2>/dev/null && return 0; sleep 0.1; done
    echo "TIDS 줄이 나오지 않았다. $1 을 확인한다"; return 1
}
table() { sed -n '/^idx /,$p' "$1"; grep -E 'setup failed|refused|Operation not permitted' "$1" | grep -v '^[0-9]' || true; }
need_sudo() { :; }
busy_n() { local s=""; for _ in $(seq 1 "$NCPU"); do s="$s busy"; done; echo $s; }
cpu_note() {
    [ "$NCPU" -eq 2 ] && return 0
    echo "참고: vCPU가 ${NCPU}개다. 실습 문서는 vCPU 2개 기준이며, 이 script는 부하를 CPU 수에 맞춰 늘린다."
}

step0() {
    say "B: CPU를 계속 쓰는 thread 하나를 background로 띄운다 (6초)"
    run_bg logs/lab0-busy.txt ./bin/mtwork -d 6 -n idx busy
    sleep 0.5
    say "A: 3초 동안 CPU에 올라간 횟수를 task별로 센다"
    show "sudo bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); } interval:s:3 { exit(); }'"
    sudo bpftrace -e 'tracepoint:sched:sched_switch { @[args->next_comm] = count(); } interval:s:3 { exit(); }' \
        > logs/lab0-oneliner.txt 2>&1
    wait
    say "결과: 횟수가 많은 task 10개 (전체는 logs/lab0-oneliner.txt)"
    grep '^@' logs/lab0-oneliner.txt | tail -n 10
    say "idx-0의 횟수"
    grep '^@\[idx-0\]' logs/lab0-oneliner.txt || echo "(목록에 없음: 3초 동안 거의 한 번도 새로 올라가지 않았다)"
}

step1a() {
    cpu_note
    say "B: 일반 busy thread ${NCPU}개로 모든 CPU를 채운다 (9초)"
    run_bg logs/lab1a-bg.txt ./bin/mtwork -d 9 -n bg $(busy_n)
    sleep 0.5
    say "A: DEADLINE thread (runtime 2ms / period 10ms, job 1.5ms, 8초)"
    run_bg logs/lab1a-dl.txt sudo ./bin/mtwork -d 8 -n dl periodic:10000:1500,sched=dl:2000/10000
    wait_tids logs/lab1a-dl.txt || { cleanup; exit 1; }
    tid=$(sed -n 's/^TIDS=//p' logs/lab1a-dl.txt)
    say "C: dl-0(TID $tid)의 scheduling 설정을 확인한다"
    show "chrt -p $tid"; chrt -p "$tid"
    say "A와 B가 끝나기를 기다린다 (약 9초)"
    wait
    say "A 결과 (dl-0의 iters, resp_p99u, miss)"; table logs/lab1a-dl.txt
    say "B 결과 (bg thread들의 cpu_ms)"; table logs/lab1a-bg.txt
}

step1b() {
    cpu_note
    say "B: 일반 busy thread ${NCPU}개로 모든 CPU를 채운다 (9초)"
    run_bg logs/lab1b-bg.txt ./bin/mtwork -d 9 -n bg $(busy_n)
    sleep 0.5
    say "A: DEADLINE thread 두 개 (dl-0: job 3ms / runtime 2ms, dl-1: job 2.5ms / runtime 3ms, 8초)"
    run_bg logs/lab1b-dl.txt sudo ./bin/mtwork -d 8 -n dl periodic:10000:3000,sched=dl:2000/10000 periodic:10000:2500,sched=dl:3000/10000
    wait_tids logs/lab1b-dl.txt || { cleanup; exit 1; }
    sleep 1
    say "C: 2초 동안 perf sched로 기록하고 dl-0, dl-1의 줄만 본다"
    show "bash tools/perf_sched.sh dl-b 2 '^dl-' 12"
    bash tools/perf_sched.sh dl-b 2 '^dl-' 12
    say "A와 B가 끝나기를 기다린다"
    wait
    say "A 결과"; table logs/lab1b-dl.txt
    say "B 결과"; table logs/lab1b-bg.txt
}

step1c() {
    rt=$(cat /proc/sys/kernel/sched_rt_runtime_us); per=$(cat /proc/sys/kernel/sched_rt_period_us)
    k=$(awk -v n="$NCPU" -v r="$rt" -v p="$per" 'BEGIN { print int(n * r / p / 0.7) + 1 }')
    [ "$k" -gt 32 ] && k=32
    say "DEADLINE 사용률 상한 = CPU 수 × rt_runtime / rt_period"
    show "cat /proc/sys/kernel/sched_rt_runtime_us /proc/sys/kernel/sched_rt_period_us; nproc"
    echo "rt_runtime_us=$rt rt_period_us=$per nproc=$NCPU"
    awk -v n="$NCPU" -v r="$rt" -v p="$per" -v k="$k" 'BEGIN { printf "상한 = %d × %d / %d = %.2f,  runtime 7ms / period 10ms(0.7)를 %d개 요청하면 합 = %.1f\n", n, r, p, n * r / p, k, 0.7 * k }'
    specs=""; for _ in $(seq 1 "$k"); do specs="$specs periodic:10000:5000,sched=dl:7000/10000"; done
    say "thread ${k}개가 각각 runtime 7ms / period 10ms를 요청한다"
    show "sudo ./bin/mtwork -d 2 -n adm periodic:10000:5000,sched=dl:7000/10000 (× $k)"
    ./bin/mtwork -d 2 -n adm $specs > logs/lab1c.txt 2>&1 < /dev/null
    grep -vE '^(pid|TIDS)=' logs/lab1c.txt
}

step2fifo() {
    say "CPU 0에 FIFO 50 두 개와 일반 thread 하나 (4초). CPU 0의 셸이 잠시 느려질 수 있다"
    show "sudo ./bin/mtwork -d 4 -n fifo busy,cpu=0,sched=fifo:50 busy,cpu=0,sched=fifo:50 busy,cpu=0"
    sudo ./bin/mtwork -d 4 -n fifo busy,cpu=0,sched=fifo:50 busy,cpu=0,sched=fifo:50 busy,cpu=0 > logs/lab2-fifo.txt 2>&1
    table logs/lab2-fifo.txt
}

step2rr() {
    say "A: CPU 0에 Round Robin 50 두 개와 일반 thread 하나 (6초)"
    run_bg logs/lab2-rr.txt sudo ./bin/mtwork -d 6 -n rr busy,cpu=0,sched=rr:50 busy,cpu=0,sched=rr:50 busy,cpu=0
    wait_tids logs/lab2-rr.txt || { cleanup; exit 1; }
    sleep 1
    say "B: 3초 동안 perf sched로 기록하고 rr-0, rr-1, rr-2의 줄만 본다"
    show "bash tools/perf_sched.sh rr 3 '^rr-' 25"
    bash tools/perf_sched.sh rr 3 '^rr-' 25
    wait
    say "A 결과"; table logs/lab2-rr.txt
    say "kernel log의 RT throttling 메시지 (부팅 후 한 번만 찍힌다)"
    show "sudo dmesg | grep -i 'rt throttling'"
    sudo dmesg | grep -i 'rt throttling' || echo "(없음: 부팅 후 이미 한 번 찍혀 dmesg에서 지워졌거나, throttling이 일어나지 않았다)"
}

step2pi() {
    for p in none inherit; do
        say "pi_demo -p $p (5라운드)"
        show "sudo ./bin/pi_demo -p $p"
        sudo ./bin/pi_demo -p $p | tee logs/lab2-pi-$p.txt
    done
    for p in inherit none; do
        say "piwatch.bt로 kernel 기록 확인 (pi_demo -p $p -r 3)"
        show "sudo bpftrace bt/piwatch.bt -c './bin/pi_demo -p $p -r 3'"
        sudo bpftrace bt/piwatch.bt -c "./bin/pi_demo -p $p -r 3" 2>&1 | tee logs/lab2-piwatch-$p.txt | grep -vE '^$|^Attaching'
    done
}

summary3() {   # 실습 3 기록지용 요약
    printf '\n%-8s %16s %11s %11s %28s %8s %14s\n' tag "renderer wait_ms" runq_sum_ms runq_max_us "@preempt[renderer,indexer]" "@wakeup" "indexer cpu_ms"
    for t in "$@"; do
        f=logs/eevdf-$t
        [ -f "$f-renderer.txt" ] || continue
        w=$(awk '$1 == "0" {print $6}' "$f-renderer.txt")
        c=$(awk '$1 == "0" {print $5}' "$f-indexer.txt")
        s=$(sed -n 's/^@runq_sum_ms\[renderer-0\]: //p' "$f-runq.txt")
        m=$(sed -n 's/^@runq_max_us\[renderer-0\]: //p' "$f-runq.txt")
        p=$(sed -n 's/^@preempt\[renderer-0, indexer-0\]: //p' "$f-preempt.txt")
        k=$(sed -n 's/^@wakeup\[renderer-0\]: //p' "$f-preempt.txt")
        printf '%-8s %16s %11s %11s %28s %8s %14s\n' "$t" "${w:--}" "${s:-0}" "${m:--}" "${p:-0}" "${k:-0}" "${c:--}"
    done
}

step3slice() {
    f=/sys/kernel/debug/sched/base_slice_ns
    if ! sudo test -w "$f"; then
        sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
    fi
    if ! orig=$(sudo cat "$f" 2>/dev/null) || ! echo "$orig" | sudo tee "$f" >/dev/null 2>&1; then
        echo "base_slice_ns를 바꿀 수 없어 slice 단계를 건너뛴다"; return 0
    fi
    trap 'echo "$orig" | sudo tee '"$f"' >/dev/null; echo; echo "중단: base_slice_ns를 $orig 로 되돌렸다"; cleanup; exit 130' INT TERM
    say "base slice를 $orig ns에서 300000 ns로 줄인다"
    show "sudo bash tools/slice.sh set 300000"; sudo bash tools/slice.sh set 300000
    bash tools/eevdf_run.sh slice busy,cpu=0
    show "sudo bash tools/slice.sh restore"; sudo bash tools/slice.sh restore      # KEY 시스템 전체 설정이므로 반드시 되돌린다
    trap 'echo; cleanup; exit 130' INT TERM
}

step=${1:-}
case "$step" in
    0) need_sudo; step0 ;;
    1a) need_sudo; step1a ;;
    1b) need_sudo; step1b ;;
    1c) need_sudo; step1c ;;
    2fifo) need_sudo; step2fifo ;;
    2rr) need_sudo; step2rr ;;
    2pi) need_sudo; step2pi ;;
    3) need_sudo; shift; [ $# -eq 2 ] || { echo "usage: bash tools/lab.sh 3 <tag> <indexer spec>"; exit 1; }
       bash tools/eevdf_run.sh "$1" "$2"; summary3 "$1" ;;
    3slice) need_sudo; step3slice; summary3 slice ;;
    *) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
