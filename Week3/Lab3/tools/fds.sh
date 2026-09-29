#!/usr/bin/env bash
set -eu

if [ "$#" -ne 2 ] || [[ ! "$1" =~ ^[1-9][0-9]*$ ]] || [[ ! "$2" =~ ^[0-9]+$ ]]; then
    echo 'Usage: bash tools/fds.sh PID EPFD' >&2
    exit 2
fi

# Positional arguments replace the PID/EPFD variables typed in the exercise.
p=$1
ep=$2

ps -p "$p" -o pid,comm,nlwp
ls -l "/proc/$p/fd"
target=$(readlink "/proc/$p/fd/$ep")

if [ "$target" != 'anon_inode:[eventpoll]' ]; then
    echo 'EPFD does not refer to an epoll instance. Check the current PID and epfd.' >&2
    exit 1
fi

cat "/proc/$p/fdinfo/$ep"
echo 'tfd = registered target FD; fdinfo is the interest list, not the ready list.'

if command -v lsof >/dev/null; then
    echo 'Optional higher-level FD view:'
    lsof -p "$p"
fi
