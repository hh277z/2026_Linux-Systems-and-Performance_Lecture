#!/usr/bin/env bash
set -u
cd -- "$(dirname -- "$0")"
failed=0
for tool in bash gcc make strace python3 ps grep sudo; do
    if command -v "$tool" >/dev/null; then echo "OK $tool"; else echo "MISSING $tool"; failed=1; fi
done
uname -srmo
test -r /proc/self/status || failed=1
mkdir -p logs
if command -v strace >/dev/null && strace -o logs/strace-check.txt -- true; then
    echo 'OK strace execution'
else
    echo 'FAIL strace execution'; failed=1
fi
echo 'IRQ tracefs access is checked separately by bash tools/irq.sh (sudo).'
exit "$failed"
