#!/usr/bin/env python3
"""Supplied read-only event monitor; not the student's full memmon/memdiag."""

import argparse
import csv
import json
import signal
import time
from pathlib import Path

stop = False


def ending(*_):
    global stop
    stop = True


def text(p):
    try:
        return Path(p).read_text()
    except OSError as e:
        return {"error": str(e)}


def kv(p):
    s = text(p)
    if isinstance(s, dict):
        return s
    return {
        x.split()[0].rstrip(":"): " ".join(x.split()[1:])
        for x in s.splitlines()
        if len(x.split()) > 1
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("output", type=Path)
    p.add_argument("--cgroup", action="append", default=[])
    p.add_argument("--pid", action="append", default=[])
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--interval", type=float, default=0.5)
    a = p.parse_args()
    if not 0 < a.seconds <= 86400 or not 0.05 <= a.interval <= 60:
        p.error("invalid duration/interval")
    signal.signal(signal.SIGTERM, ending)
    signal.signal(signal.SIGINT, ending)
    start = time.monotonic()
    with a.output.open("x", newline="") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "monotonic_s",
                "wall_s",
                "cycle_end_s",
                "meminfo_json",
                "psi_memory",
                "vmstat_json",
                "cgroup_json",
                "process_json",
            ]
        )

        while not stop:
            now = time.monotonic()
            groups = {
                g: {
                    n: text(Path(g) / n)
                    for n in [
                        "memory.current",
                        "memory.high",
                        "memory.max",
                        "memory.swap.max",
                        "memory.events",
                        "memory.stat",
                        "memory.pressure",
                    ]
                }
                for g in a.cgroup
            }
            procs = {
                pid: {
                    "stat": text(f"/proc/{pid}/stat"),
                    "status": text(f"/proc/{pid}/status"),
                    "smaps_rollup": text(f"/proc/{pid}/smaps_rollup"),
                }
                for pid in a.pid
            }
            mem = kv("/proc/meminfo")
            ps = text("/proc/pressure/memory")
            vm = kv("/proc/vmstat")
            finished = time.monotonic()
            w.writerow(
                [
                    now,
                    time.time(),
                    finished,
                    json.dumps(mem),
                    json.dumps(ps),
                    json.dumps(vm),
                    json.dumps(groups),
                    json.dumps(procs),
                ]
            )
            f.flush()
            if now - start >= a.seconds:
                break
            time.sleep(a.interval)


if __name__ == "__main__":
    main()
