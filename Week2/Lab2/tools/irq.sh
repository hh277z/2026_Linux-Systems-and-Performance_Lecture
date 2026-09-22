#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p logs
out=$(mktemp -d logs/irq-XXXXXXXX)
cat /proc/interrupts > "$out/interrupts-before.txt"
echo 'Recording IRQ entry/exit for 3 seconds. Enter your Ubuntu sudo password if asked.'
if sudo bash tools/irq_record.sh > "$out/irq-trace.txt" 2> "$out/recorder.txt"; then
    cat /proc/interrupts > "$out/interrupts-after.txt"
    echo "Saved: $out"
    python3 tools/irq_summary.py "$out/irq-trace.txt" | tee "$out/summary.txt"
    echo 'Read recorder.txt for per-CPU overrun statistics.'
else
    cat /proc/interrupts > "$out/interrupts-after.txt"
    echo "UNOBSERVED: read $out/recorder.txt; use samples/irq-trace.txt if needed."
    cat "$out/recorder.txt" >&2
    exit 3
fi
