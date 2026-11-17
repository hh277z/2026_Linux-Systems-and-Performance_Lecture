#!/usr/bin/env python3
"""Bounded cgroup v2 runner. No system-wide sysctl, swap or killer changes."""

import argparse
import ctypes
import json
import os
import pwd
import signal
import subprocess
import sys
import time
import uuid
import tempfile
from pathlib import Path


def identity(pid):
    return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])


def write(p, s):
    p.write_text(str(s))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--parent", type=Path, default=Path("/sys/fs/cgroup"))
    ap.add_argument("--max-mib", type=int, default=128)
    ap.add_argument("--high-mib", type=int)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--group-kill", action="store_true")
    ap.add_argument("--uid", type=int, default=int(os.environ.get("SUDO_UID", "1000")))
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--role", choices=["candidate", "observe"], default="candidate")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.command[1:] if a.command[:1] == ["--"] else a.command
    if not cmd:
        ap.error("command is required after --")
    if not 16 <= a.max_mib <= 1024 or not 0 < a.seconds <= 600:
        ap.error("max 16..1024MiB; duration 0..600s")
    if a.high_mib is not None and not 1 <= a.high_mib < a.max_mib:
        ap.error("high must be below max")
    if os.geteuid() != 0:
        ap.error("requires root with a delegated memory-controller parent")
    if a.uid == 0:
        ap.error("use a non-root --uid for workload")
    account = pwd.getpwuid(a.uid)
    parent = a.parent.resolve()
    mount = Path("/sys/fs/cgroup")
    if not parent.is_relative_to(mount) or not (parent / "cgroup.controllers").exists():
        ap.error("parent must be a cgroup v2 subtree")
    if "memory" not in (parent / "cgroup.subtree_control").read_text().split():
        ap.error(
            "memory is not delegated to children; prepare the course subtree first"
        )

    a.out.mkdir(parents=True, exist_ok=False)
    cg = parent / ("lab10-" + uuid.uuid4().hex[:12])
    cg.mkdir()
    child = None
    collector = None
    kernel = None
    kernel_out = (a.out / "kernel.log").open("x")
    kernel_err = (a.out / "kernel-error.log").open("x")
    regfd, regname = tempfile.mkstemp(
        prefix="lab10-registry-", suffix=".tsv", dir="/tmp"
    )
    os.close(regfd)
    live_registry = Path(regname)
    (a.out / "registry-path.txt").write_text(regname)
    manifest = {
        "command": cmd,
        "cgroup": str(cg),
        "uid": a.uid,
        "max_bytes": a.max_mib * 1048576,
        "wall_start": time.time(),
        "monotonic_start": time.monotonic(),
        "seconds": a.seconds,
    }

    try:
        kernel = subprocess.Popen(
            ["dmesg", "--follow-new", "--time-format", "raw"],
            stdout=kernel_out,
            stderr=kernel_err,
        )
        write(cg / "memory.max", manifest["max_bytes"])
        write(cg / "memory.swap.max", 0)
        write(cg / "memory.oom.group", int(a.group_kill))
        if a.high_mib:
            write(cg / "memory.high", a.high_mib * 1048576)

        def prepare():
            # Child placement precedes exec, allocation and dropping privileges.
            write(cg / "cgroup.procs", os.getpid())
            libc = ctypes.CDLL(None)
            libc.prctl(1, signal.SIGKILL, 0, 0, 0)
            if os.getppid() == 1:
                os._exit(125)
            os.setgroups([])
            os.setgid(account.pw_gid)
            os.setuid(a.uid)

        with (a.out / "workload.log").open("x") as log:
            child = subprocess.Popen(
                cmd, stdout=log, stderr=subprocess.STDOUT, preexec_fn=prepare
            )

            try:
                start = identity(child.pid)
            except FileNotFoundError:
                start = None

            write(a.out / "registry.tsv", f"{child.pid} {start or 0} {a.role} {cg}\n")
            write(live_registry, f"{child.pid} {start or 0} {a.role} {cg}\n")
            manifest.update(pid=child.pid, start_ticks=start)
            # Capture local counters/PSI even when the student collector is not implemented.
            mon = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(__file__).with_name("observe.py")),
                    str(a.out / "support.csv"),
                    "--cgroup",
                    str(cg),
                    "--pid",
                    str(child.pid),
                    "--seconds",
                    str(a.seconds + 2),
                ]
            )
            collector = mon
            try:
                manifest["returncode"] = child.wait(timeout=a.seconds)
                manifest["stop_reason"] = "exit"
            except subprocess.TimeoutExpired:
                manifest["stop_reason"] = "watchdog"
                write(cg / "cgroup.kill", 1)
                manifest["returncode"] = child.wait(timeout=5)

            time.sleep(0.2)
    finally:
        if (cg / "cgroup.kill").exists():
            write(cg / "cgroup.kill", 1)
        if child and child.poll() is None:
            child.wait(timeout=5)
        if collector:
            collector.terminate()
            try:
                collector.wait(timeout=5)
            except subprocess.TimeoutExpired:
                collector.kill()
                collector.wait()

        if kernel:
            kernel.terminate()
            manifest["kernel_logger_returncode"] = kernel.wait(timeout=5)

        kernel_out.close()
        kernel_err.close()
        manifest["events_final"] = (
            (cg / "memory.events").read_text()
            if (cg / "memory.events").exists()
            else "NA"
        )
        manifest["monotonic_end"] = time.monotonic()
        manifest["wall_end"] = time.time()
        (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2))
        live_registry.unlink(missing_ok=True)
        for _ in range(50):
            try:
                cg.rmdir()
                break
            except OSError:
                time.sleep(0.05)
        else:
            print(f"cleanup pending: {cg}", file=sys.stderr)

    print(json.dumps(manifest))
    return 0 if manifest.get("returncode") == 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as e:
        print(f"labrun: {e}", file=sys.stderr)
        sys.exit(1)
