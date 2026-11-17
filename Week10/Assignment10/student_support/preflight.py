#!/usr/bin/env python3
import json
import os
import platform
import shutil
from pathlib import Path


def read(p):
    try:
        return Path(p).read_text().strip()
    except OSError as e:
        return {"unsupported": str(e)}


print(
    json.dumps(
        {
            "kernel": platform.release(),
            "machine": platform.machine(),
            "page_size": os.sysconf("SC_PAGESIZE"),
            "cpu_count": os.cpu_count(),
            "gcc": shutil.which("gcc"),
            "make": shutil.which("make"),
            "self_cgroup": read("/proc/self/cgroup"),
            "controllers": read("/sys/fs/cgroup/cgroup.controllers"),
            "delegated": read("/sys/fs/cgroup/cgroup.subtree_control"),
            "memory_psi": read("/proc/pressure/memory"),
            "meminfo": read("/proc/meminfo"),
            "swaps": read("/proc/swaps"),
            "config": {
                n: read("/proc/sys/vm/" + n)
                for n in [
                    "overcommit_memory",
                    "overcommit_ratio",
                    "overcommit_kbytes",
                    "panic_on_oom",
                    "oom_kill_allocating_task",
                    "swappiness",
                    "compaction_proactiveness",
                ]
            },
        },
        indent=2,
    )
)
