#!/usr/bin/env bash
set -eu

# Ubuntu's /usr/bin/perf wrapper may demand an unavailable WSL kernel package.
if [ -n "${PERF_BIN:-}" ]; then
    exec "$PERF_BIN" "$@"
fi
if command -v perf >/dev/null 2>&1 && perf --version >/dev/null 2>&1; then
    exec perf "$@"
fi
while IFS= read -r candidate; do
    if [ -x "$candidate" ] && "$candidate" --version >/dev/null 2>&1; then
        exec "$candidate" "$@"
    fi
done < <(find /usr/lib/linux-tools -mindepth 2 -maxdepth 2 -name perf -type f 2>/dev/null | sort -V -r)
echo 'No usable perf. Install linux-tools-common linux-tools-generic, or set PERF_BIN to a validated perf binary.' >&2
exit 127
