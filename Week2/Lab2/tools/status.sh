#!/usr/bin/env bash
set -eu
if [[ $# != 2 || ! $1 =~ ^[1-9][0-9]*$ ]]; then
    echo 'Usage: bash tools/status.sh POSITIVE_PID INT|TERM' >&2; exit 2
fi
case "$2" in INT) n=$(kill -l INT);; TERM) n=$(kill -l TERM);; *) exit 2;; esac
python3 - "$1" "$n" <<'PY'
import sys
from pathlib import Path
pid, n = sys.argv[1], int(sys.argv[2]); bit = 1 << (n-1)
try:
    rows = dict(line.split(':',1) for line in Path(f'/proc/{pid}/status').read_text().splitlines())
except FileNotFoundError:
    sys.exit('Process is no longer present; check PID / terminal A.')
print(f'PID={pid} name={rows["Name"].strip()} signal={n} bit=0x{bit:016x}')
for field in ['SigPnd','ShdPnd','SigBlk','SigIgn','SigCgt']:
    value = rows[field].strip()
    print(f'{field}: {value}  selected_bit={int(bool(int(value,16)&bit))}')
PY
