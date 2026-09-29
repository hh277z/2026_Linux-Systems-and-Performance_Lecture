#!/usr/bin/env bash
set -u

cd -- "$(dirname -- "$0")"
failed=0

for tool in bash gcc make python3 strace perf ps; do
    if command -v "$tool" >/dev/null; then
        echo "OK $tool"
    else
        echo "MISSING $tool"
        failed=1
    fi
done

uname -srmo

if [ "$(uname -s)" != Linux ]; then
    echo 'FAIL: use WSL Ubuntu Bash'
    failed=1
fi

if command -v perf >/dev/null && perf stat -e task-clock -- true >/dev/null 2>&1; then
    echo 'OK perf task-clock access'
else
    echo 'FAIL perf task-clock access'
    failed=1
fi

if command -v lsof >/dev/null; then
    echo 'OPTIONAL lsof available'
else
    echo 'OPTIONAL lsof unavailable; use /proc commands'
fi

if [ ! -r /proc/self/status ]; then
    echo 'FAIL /proc'
    failed=1
fi

mkdir -p logs

if command -v strace >/dev/null && strace -o logs/strace-check.txt -- true; then
    echo 'OK strace execution'
else
    echo 'FAIL strace execution'
    failed=1
fi

if command -v python3 >/dev/null; then
    python3 - <<'PY' || failed=1
import select
import socket

poller = select.epoll()
poller.close()

a, b = socket.socketpair(socket.AF_UNIX)
a.close()
b.close()
print('OK epoll and Unix domain sockets')
PY
fi

echo 'No sudo, kernel tracing, or additional Python packages required.'
exit "$failed"
