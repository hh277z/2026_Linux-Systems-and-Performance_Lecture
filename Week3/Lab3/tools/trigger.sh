#!/usr/bin/env bash
set -euo pipefail

cd -- "$(dirname -- "$0")/.."

if [ "$#" -ne 1 ] || [[ "$1" != lt && "$1" != et-bad && "$1" != et-drain ]]; then
    echo 'Usage: bash tools/trigger.sh lt|et-bad|et-drain' >&2
    exit 2
fi

mkdir -p logs

# Optional shortcut AFTER practicing the commands in Lab3_instructions.docx.
# -tt: timestamps; -T: syscall duration; -s: string limit; -e: syscall filter.
strace -s 120 -tt -T -e trace=epoll_create1,epoll_ctl,epoll_wait,epoll_pwait,epoll_pwait2,read,write,ioctl \
    -o "logs/trigger-$1-strace.txt" -- ./bin/trigger_steps "$1" | tee "logs/trigger-$1.txt"
