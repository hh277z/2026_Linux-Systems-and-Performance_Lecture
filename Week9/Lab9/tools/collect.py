"""Raw observations retain scope and monotonic read times."""
import time
from pathlib import Path


def snapshot(group=None):
    paths = ['/proc/pressure/cpu', '/proc/pressure/memory', '/proc/pressure/io',
             '/proc/vmstat', '/proc/meminfo']
    if group:
        paths += [str(group / name) for name in
                  ['memory.pressure', 'cpu.pressure', 'io.pressure', 'memory.stat',
                   'memory.events', 'memory.current', 'memory.high', 'memory.max']]
    # KEY: 파일마다 읽은 시각을 남겨 같은 파일의 두 표본을 비교한다.
    result = {'t': time.monotonic(), 'files': {}}
    for path in paths:
        result['files'][path] = {'t': time.monotonic(), 'raw': Path(path).read_text()}

    return result


def counters(text):
    values = {}
    for line in text.splitlines():
        name, value = line.split()[:2]
        values[name] = int(value)

    return values


def psi(text):
    # KEY: some과 full은 따로 누적된다. 각 행에서 total(μs)을 읽는다.
    totals = {}
    for line in text.splitlines():
        level = line.split()[0]
        totals[level] = int(line.split('total=')[1])

    return totals
