#!/usr/bin/env bash
set -eu

cd -- "$(dirname -- "$0")/.."
mkdir -p logs

case "${1:-}" in
  task)
    strace -f -e trace=process \
        -o logs/task-strace.txt \
        -- ./bin/task_demo --auto
    ;;

  context)
    for mode in busy sleep; do
      bash tools/perf.sh stat -e task-clock,context-switches:uk \
        -o "logs/context-$mode-perf.txt" \
        -- ./bin/context_demo "$mode" 1000 10
      if grep -Eq '<not (supported|counted)>' "logs/context-$mode-perf.txt"; then
        echo 'An event was not measured. Read the perf log; do not record it as zero.' >&2
        exit 1
      fi
    done
    ;;

  fd)
    for mode in shared reopen pread; do
      strace -f -yy -e trace=process,openat,read,pread64,lseek,close \
        -o "logs/fd-$mode-strace.txt" \
        -- ./bin/fd_fork_demo "$mode" --auto
    done
    ;;

  syscall)
    for mode in byte batch; do
      sudo perf stat \
        -e task-clock,raw_syscalls:sys_enter -- \
        ./bin/syscall_demo "$mode" 1048576 1 \
        > /dev/null 2> "logs/syscall-$mode-perf.txt"
    done
    ;;

  *)
    echo 'Usage: bash tools/observe.sh task|context|fd|syscall' >&2
    exit 2
    ;;
esac
