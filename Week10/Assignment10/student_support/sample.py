#!/usr/bin/env python3
"""Read-only Assignment 9 collector. Python standard library; no root required."""

import argparse
import csv
import json
import re
import sys
import time
from pathlib import Path


def read_stats(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        parts = line.replace(":", "").split()
        if len(parts) >= 2 and parts[1].isdigit():
            result[parts[0]] = int(parts[1])

    return result


def read_psi(resource):
    result = {}
    for line in Path("/proc/pressure/" + resource).read_text().splitlines():
        parts = line.split()
        for pair in parts[1:]:
            key, value = pair.split("=", 1)
            result[parts[0] + "_" + key] = value

    return result


def read_zones():
    zones, current = [], None
    for line in Path("/proc/zoneinfo").read_text().splitlines():
        match = re.match(r"Node\s+(\d+),\s+zone\s+(\S+)", line)
        if match:
            current = {"node": int(match[1]), "zone": match[2]}
            zones.append(current)
        elif current is not None:
            match = re.match(r"\s*(pages free|min|low|high)\s+(\d+)\s*$", line)
            if match:
                current[match[1].replace("pages ", "")] = int(match[2])

    if not zones or any(
        not all(k in z for k in ("free", "min", "low", "high")) for z in zones
    ):
        raise ValueError("Could not parse per-zone watermarks from /proc/zoneinfo")

    return zones


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output", help="new output CSV path; existing files are not overwritten"
    )
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--interval", type=float, default=1)
    args = parser.parse_args()
    if not (0 < args.seconds <= 86400 and 0 < args.interval <= args.seconds):
        parser.error("require 0 < interval <= seconds <= 86400")
    initial_vm = read_stats("/proc/vmstat")
    vm_names = sorted(
        k
        for k in initial_vm
        if k.startswith(("pgscan_", "pgsteal_", "allocstall", "workingset_refault"))
        or k in ("pswpin", "pswpout")
    )
    mem_names = [
        "MemTotal",
        "MemFree",
        "MemAvailable",
        "Cached",
        "SwapTotal",
        "SwapFree",
    ]
    psi_names = [
        "memory_some_total_us",
        "memory_full_total_us",
        "memory_some_avg10_pct",
        "memory_full_avg10_pct",
        "cpu_some_total_us",
    ]
    fields = (
        ["monotonic_s", "elapsed_s"]
        + [k + "_KiB" for k in mem_names]
        + psi_names
        + ["vm_" + k for k in vm_names]
        + ["zones_json"]
    )
    start = time.monotonic()
    deadline = start + args.seconds
    with open(args.output, "x", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        try:
            while True:
                now = time.monotonic()
                mem = read_stats("/proc/meminfo")
                vm = read_stats("/proc/vmstat")
                mp, cp = read_psi("memory"), read_psi("cpu")
                row = {"monotonic_s": f"{now:.6f}", "elapsed_s": f"{now-start:.6f}"}
                row.update({k + "_KiB": mem.get(k, "NA") for k in mem_names})
                row.update(
                    {
                        "memory_some_total_us": mp.get("some_total", "NA"),
                        "memory_full_total_us": mp.get("full_total", "NA"),
                        "memory_some_avg10_pct": mp.get("some_avg10", "NA"),
                        "memory_full_avg10_pct": mp.get("full_avg10", "NA"),
                        "cpu_some_total_us": cp.get("some_total", "NA"),
                        "zones_json": json.dumps(read_zones(), separators=(",", ":")),
                    }
                )
                row.update({"vm_" + k: vm.get(k, "NA") for k in vm_names})
                writer.writerow(row)
                handle.flush()
                if now >= deadline:
                    break
                time.sleep(
                    max(0, min(now + args.interval, deadline) - time.monotonic())
                )
        except KeyboardInterrupt:
            print("Sampling stopped; completed rows retained.", file=sys.stderr)

    print("Saved " + args.output, file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        print("sample.py: " + str(exc), file=sys.stderr)
        sys.exit(1)
