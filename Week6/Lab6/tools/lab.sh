#!/usr/bin/env bash
# Week 6 실습 실행 스크립트
# 실행하는 명령어를 화면에 표시하고, 결과를 logs/에 저장한다.
#
# 사용법: bash tools/lab.sh <step>
#   1        실습 1-1: counter 비교 (plain, atomic, mutex, local), thread 2개
#   1tsan    실습 1-2: TSan 빌드를 활용해서 plain, atomic 검사
#   2a       실습 2-1: lock 비교 (spin, mcs, mutex), thread 1개
#   2b       실습 2-2: lock 비교 (spin, mcs, mutex, adaptive), thread 2개
#   2c       실습 2-3: lock 비교 (spin, mcs, mutex), thread (총 CPU 수 + 1)개
#   2d       실습 2-4: 읽기 중심 workload에서 rwlock, rcu, nogp 비교
#   3ifwait  실습 3-1: if 기반 condition variable 대기
#   3iolock  실습 3-2: lock을 유지한 상태의 log I/O와 수정된 버전 비교
#   3hang    실습 3-3: unlock 누락으로 발생한 hang 분석 (gdb, wchan)
#   3futex   실습 3-4: futex 호출 횟수 및 소요 시간 분석

set -u
cd "$(dirname "$0")/.." || exit 1
step=${1:-}

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        exec sudo bash "$0" "$step"
    fi
}

# 빌드는 일반 사용자 권한으로 수행
[ -n "${SUDO_UID:-}" ] || make -s || exit 1

mkdir -p logs
OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}"
trap 'chown -R "$OWNER" logs 2>/dev/null' EXIT

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
show() { echo "\$ $*"; }

step1() {
    for m in plain atomic mutex local; do
        say "Counter: $m (thread 2개, 각 5,000,000회)"
        show "./bin/counter -m $m -t 2 -n 5000000"
        ./bin/counter -m "$m" -t 2 -n 5000000 \
            | tee "logs/lab1-$m.txt" | tail -n 2
    done
}

step1tsan() {
    local failed=0 status log
    for m in plain atomic; do
        log="logs/lab1-tsan-$m.txt"
        say "TSan: $m (thread 2개, 각 100,000회)"
        show "./bin/counter_tsan -m $m -n 100000"

        if ./bin/counter_tsan -m "$m" -n 100000 > "$log" 2>&1; then
            status=0
        else
            status=$?
        fi

        # 초기화 오류와 signal 종료를 race report보다 먼저 구분한다.
        if grep -Eq 'FATAL:|CHECK failed|Sanitizer:DEADLYSIGNAL' "$log" || (( status >= 128 )); then
            echo "TSan 실행 실패 (종료 코드 $status): $log"
            head -n 12 "$log"
            failed=1
        elif grep -q 'WARNING: ThreadSanitizer' "$log"; then
            echo "TSan 경고 발견 (종료 코드 $status): $log"
            grep -m1 -A 9 'WARNING: ThreadSanitizer' "$log"
            grep -m1 '^SUMMARY' "$log" || true
            if ! grep -q 'ns_per_increment=' "$log"; then
                echo "경고 후 프로그램이 완료되기 전에 종료됨"
            fi
        elif (( status == 0 )) && grep -q 'ns_per_increment=' "$log"; then
            echo "TSan 정상 실행: warning 없음"
        else
            echo "TSan 실행 실패 또는 결과 미완료 (종료 코드 $status): $log"
            head -n 12 "$log"
            failed=1
        fi

        grep 'ns_per_increment=' "$log" || true
    done
    return "$failed"
}

# 동일한 thread 수에서 lock 방식 비교
lockbench_all() {
    local t=$1
    shift

    for m in "$@"; do
        say "Lockbench: $m (thread $t개)"
        show "./bin/lockbench -m $m -t $t"
        ./bin/lockbench -m "$m" -t "$t" \
            | tee "logs/lab2-t$t-$m.txt" | tail -n +2
    done
}

# 지정한 lock의 thread별 futex syscall 횟수 측정
futex_count() {
    local mode=$1 t=$2
    local log="logs/lab2-t$t-$mode-futex.txt"
    local strace_log="logs/lab2-t$t-$mode-strace.txt"

    rm -f "$strace_log"

    say "Futex 호출 횟수: $mode (thread $t개)"

    if perf trace -s -- true > /dev/null 2>&1; then
        show "sudo perf trace -s ./bin/lockbench -m $mode -t $t"

        perf trace -s ./bin/lockbench -m "$mode" -t "$t" \
            > "$log" 2>&1

        awk '
            /^ [^ ].*\([0-9]+\), [0-9]+ events/ {
                name = $1 " " $2
                sub(/,$/, "", name)
                order[++n] = name
                c[name] = 0
            }
            /^   futex / {
                c[name] = $2
            }
            END {
                for (i = 1; i <= n; i++)
                    printf "  %-20s futex %s회\n", order[i], c[order[i]]
            }
        ' "$log"
    else
        say "perf trace 사용 불가: strace로 측정"
        show "strace -f -e trace=futex -o $strace_log ./bin/lockbench -m $mode -t $t"

        strace -f -e trace=futex \
            -o "$strace_log" \
            ./bin/lockbench -m "$mode" -t "$t" \
            > "$log" 2>&1

        awk '
            { n[$1] += 0 }
            $2 ~ /^futex\(/ { n[$1]++ }
            END {
                for (p in n)
                    print "  tid " p "  futex " n[p] "회"
            }
        ' "$strace_log" | sort -k2 -n
    fi
}

step2a() {
    need_root
    lockbench_all 1 spin mcs mutex
    futex_count mutex 1
}

step2b() {
    need_root
    lockbench_all 2 spin mcs mutex adaptive
    futex_count mutex 2
    futex_count adaptive 2
}

step2c() {
    local t=$(( $(nproc) + 1 ))
    lockbench_all "$t" spin mcs mutex
}

step2d() {
    for r in 1 2; do
        for m in rwlock rcu; do
            say "cfgread: $m (reader $r개, 2초)"
            show "./bin/cfgread -m $m -r $r"

            ./bin/cfgread -m "$m" -r "$r" \
                | tee "logs/lab2-cfg-$m-r$r.txt" | tail -n +2
        done
    done

    say "cfgread: nogp (reader 2개, grace period 없이 free)"
    show "./bin/cfgread -m nogp -r 2"

    ./bin/cfgread -m nogp -r 2 \
        | tee logs/lab2-cfg-nogp-r2.txt | tail -n +2
}

step3ifwait() {
    say "Condition Variable: ifwait (item 200개)"
    show "./bin/cvbug -m ifwait"

    ./bin/cvbug -m ifwait 2>&1 \
        | tee logs/lab3-ifwait.txt | grep -v '^pid='
}

step3iolock() {
    for m in iolock fixed; do
        say "Condition Variable: $m (item 200개, log당 5ms)"
        show "./bin/cvbug -m $m"

        ./bin/cvbug -m "$m" 2>&1 \
            | tee "logs/lab3-$m.txt" | grep -vE '^pid=|rejected'
    done
}

step3hang() {
    need_root

    say "A. Unlock 누락으로 인한 hang"
    show "./bin/cvbug -m nounlock &"

    ./bin/cvbug -m nounlock \
        > logs/lab3-nounlock.txt 2>&1 < /dev/null &
    p=$!

    sleep 2
    cat logs/lab3-nounlock.txt

    say "B. GDB: thread별 backtrace"
    show "sudo gdb -p $p -batch -ex 'thread apply all bt'"

    gdb -p "$p" -batch \
        -ex 'set debuginfod enabled off' \
        -ex 'thread apply all bt' 2>/dev/null \
        | grep -E '^Thread|^#' \
        | tee logs/lab3-gdb.txt

    say "C. Thread별 wchan 및 상태"
    show "for t in /proc/$p/task/*; do echo \"\$t \$(cat \$t/comm) \$(cat \$t/wchan) \$(awk '{print \$3}' \$t/stat)\"; done"

    for t in /proc/"$p"/task/*; do
        echo "$t $(cat "$t/comm") $(cat "$t/wchan") $(awk '{print $3}' "$t/stat")"
    done | tee logs/lab3-wchan.txt

    say "D. Mutex owner TID"
    show "sudo gdb -p $p -batch -ex 'p q.lock.__data.__owner'"

    gdb -p "$p" -batch \
        -ex 'set debuginfod enabled off' \
        -ex 'p q.lock.__data.__owner' 2>/dev/null \
        | grep '^\$' \
        | tee -a logs/lab3-gdb.txt

    echo "남은 thread TID: $(ls /proc/"$p"/task | tr '\n' ' ')"

    show "kill $p"
    kill "$p"
}

step3futex() {
    need_root

    say "Futex 분석: fixed (item 200개, log당 2ms)"
    show "sudo bpftrace tools/futex.bt -c './bin/cvbug -m fixed -n 200 -s 2'"

    bpftrace tools/futex.bt \
        -c './bin/cvbug -m fixed -n 200 -s 2' \
        > logs/lab3-futex.txt 2>&1

    if ! grep -q '^@futex\[' logs/lab3-futex.txt; then
        echo "bpftrace 측정 실패: logs/lab3-futex.txt"
        grep -i 'error' logs/lab3-futex.txt
        return 1
    fi

    grep -E '^(taken|elapsed)' logs/lab3-futex.txt

    echo "-- Futex 호출 횟수 [thread, pthread 함수, op]"
    grep '^@futex\[' logs/lab3-futex.txt \
        | sort -t: -k2 -n -r

    echo "-- Futex 누적 시간 (us) [pthread 함수]"
    grep '^@futex_time_us' logs/lab3-futex.txt
}

case "$step" in
    1)       step1 ;;
    1tsan)   step1tsan ;;
    2a)      step2a ;;
    2b)      step2b ;;
    2c)      step2c ;;
    2d)      step2d ;;
    3ifwait) step3ifwait ;;
    3iolock) step3iolock ;;
    3hang)   step3hang ;;
    3futex)  step3futex ;;
    *)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac

