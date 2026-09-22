#!/usr/bin/env bash
# Private ftrace instance: never clear or disable the global tracing buffer.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run through tools/irq.sh with sudo.' >&2; exit 2; }
base=/sys/kernel/tracing
if [[ ! -d $base/events/irq ]]; then
    base=/sys/kernel/debug/tracing
fi
[[ -d $base/instances ]] || { echo 'UNAVAILABLE: mounted tracefs with instances required.' >&2; exit 3; }
instance="$base/instances/week2-lab-$$"
mkdir -- "$instance"
cleanup() {
    printf '0\n' > "$instance/tracing_on" 2>/dev/null || true
    printf '0\n' > "$instance/events/enable" 2>/dev/null || true
    rmdir -- "$instance" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
printf '0\n' > "$instance/tracing_on"
printf '0\n' > "$instance/events/enable"
printf 'nop\n' > "$instance/current_tracer"
printf '256\n' > "$instance/buffer_size_kb"
for event in irq_handler_entry irq_handler_exit; do
    [[ -e $instance/events/irq/$event/enable ]] || { echo "UNAVAILABLE: $event" >&2; exit 3; }
    printf '1\n' > "$instance/events/irq/$event/enable"
done
printf '# LIVE CAPTURE UTC=%s\n' "$(date -u +%FT%TZ)"
printf '# kernel=%s\n' "$(uname -srmo)"
printf '# scope=all CPUs of this Linux kernel; duration=3s; private instance\n'
printf '1\n' > "$instance/tracing_on"
sleep 3
printf '0\n' > "$instance/tracing_on"
cat "$instance/trace"
for stats in "$instance"/per_cpu/cpu*/stats; do
    echo "STATS $stats" >&2
    cat "$stats" >&2
done
