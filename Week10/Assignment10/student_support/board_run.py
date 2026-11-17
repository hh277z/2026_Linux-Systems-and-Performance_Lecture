#!/usr/bin/env python3
"""Board comparison harness. Lab cgroup only. Pilot values, not validated board profiles."""

import argparse
import os
import pwd
import signal
import subprocess
import time
import json
import tempfile
from pathlib import Path
from labrun import identity, write


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--parent", type=Path, required=True)
    p.add_argument("--earlyoom", type=Path, required=True)
    p.add_argument("--memhog", type=Path, required=True)
    p.add_argument("--foreground", type=Path, required=True)
    p.add_argument("--uid", type=int, default=1000)
    p.add_argument("--config", type=int, choices=[1, 2, 3, 4], required=True)
    p.add_argument("--seconds", type=int, default=90)
    p.add_argument("--scale-mib", type=int, default=64)
    p.add_argument("--some-us", type=int, default=150000)
    p.add_argument("--full-us", type=int, default=500000)
    p.add_argument("--grow-rate", type=float, default=1)
    p.add_argument("--burst", action="store_true")
    a = p.parse_args()
    if os.geteuid() != 0 or a.uid == 0:
        p.error("root launcher and non-root workload uid required")
    if (
        not 16 <= a.scale_mib <= 512
        or not 5 <= a.seconds <= 600
        or not 1 <= a.some_us <= 1000000
    ):
        p.error("invalid bounds")

    if not 0 <= a.full_us <= 1000000 or not 0.1 <= a.grow_rate <= 1000:
        p.error("invalid full threshold/growth rate")
    parent = a.parent.resolve()
    if (
        not str(parent).startswith("/sys/fs/cgroup/")
        or "memory" not in (parent / "cgroup.subtree_control").read_text().split()
    ):
        p.error("use an instructor-delegated parent with memory enabled")

    a.out.mkdir(parents=True, exist_ok=False)
    cg = parent / ("lab10-board-" + str(os.getpid()))
    cg.mkdir()
    write(cg / "memory.max", a.scale_mib * 2 * 1048576)
    write(cg / "memory.swap.max", 0)
    # No swap configuration or platform manager is changed.
    procs = []
    handles = []
    daemon = None
    mon = None
    kernel = None
    regfd, regname = tempfile.mkstemp(
        prefix="lab10-registry-", suffix=".tsv", dir="/tmp"
    )
    os.close(regfd)
    registry = Path(regname)
    account = pwd.getpwuid(a.uid)

    def prep(adj):
        write(cg / "cgroup.procs", os.getpid())
        write(Path("/proc/self/oom_score_adj"), adj)
        os.setgroups([])
        os.setgid(account.pw_gid)
        os.setuid(a.uid)

    def launch(name, cmd, adj, candidate=True):
        f = (a.out / (name + ".log")).open("x")
        handles.append(f)
        child = subprocess.Popen(
            cmd, stdout=f, stderr=subprocess.STDOUT, preexec_fn=lambda: prep(adj)
        )
        procs.append(child)
        row = f"{child.pid} {identity(child.pid)} {'candidate' if candidate else 'observe'} {cg}\n"
        with registry.open("a") as r:
            r.write(row)
        os.chmod(registry, 0o600)
        return child

    m = lambda ratio: str(max(1, int(a.scale_mib * ratio))) + "MiB"
    manifest = {
        "config": a.config,
        "pilot": True,
        "scale_mib": a.scale_mib,
        "cgroup": str(cg),
        "monotonic_start": time.monotonic(),
        "wall_start": time.time(),
        "platform_manager": "unchanged; operator records status",
        "note": "cgroup hard cap is a safety boundary; system PSI/meminfo may not trigger before local OOM",
    }

    try:
        k = (a.out / "kernel.log").open("x")
        e = (a.out / "kernel-error.log").open("x")
        handles.extend([k, e])
        kernel = subprocess.Popen(
            ["dmesg", "--follow-new", "--time-format", "raw"], stdout=k, stderr=e
        )
        # Atomic rewrite for every registry addition precedes daemon startup.
        fg = launch(
            "foreground", [str(a.foreground.resolve()), m(0.30), str(a.seconds)], 0
        )
        launch(
            "background",
            [str(a.memhog.resolve()), "anon", m(0.15), "--hold", str(a.seconds)],
            700,
        )

        for i in range(2):
            launch(
                f"cached{i}",
                [str(a.memhog.resolve()), "anon", m(0.10), "--hold", str(a.seconds)],
                900,
            )

        f = (a.out / "probe.log").open("x")
        handles.append(f)
        probe = subprocess.Popen(
            [str(a.memhog.resolve()), "probe", "--seconds", str(a.seconds)], stdout=f
        )
        procs.append(probe)
        mon = subprocess.Popen(
            [
                "python3",
                str(Path(__file__).with_name("observe.py")),
                str(a.out / "support.csv"),
                "--cgroup",
                str(cg),
                "--seconds",
                str(a.seconds),
            ]
        )
        # Register grower before starting daemon; initial delay is in the child.
        delay = min(30, max(1, a.seconds // 3))
        cmd = [
            "python3",
            "-c",
            "import os,sys,time;time.sleep(float(sys.argv[1]));os.execv(sys.argv[2],sys.argv[2:])",
            str(delay),
            str(a.memhog.resolve()),
            "anon",
            m(0.4 if a.burst else 1.0),
            "--rate",
            str(a.grow_rate),
            "--hold",
            "1" if a.burst else str(a.seconds),
        ]
        grower = launch("grower", cmd, 0)
        if a.config != 1:
            f = (a.out / "earlyoom.log").open("x")
            handles.append(f)
            cmd = [
                str(a.earlyoom.resolve()),
                "--lab",
                "--registry",
                str(registry.resolve()),
                "--seconds",
                str(a.seconds),
            ]

            if a.config >= 3:
                cmd += ["--psi", "some", str(a.some_us), "1000000"]
            if a.config >= 3 and a.full_us:
                cmd += ["--psi", "full", str(a.full_us), "1000000"]
            if a.config == 4:
                cmd += ["--levels", "900,700,200,0"]
            manifest["daemon_command"] = cmd
            daemon = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT)

        deadline = time.monotonic() + a.seconds
        while time.monotonic() < deadline:
            if grower.poll() is not None:
                manifest["stop_reason"] = "grower_exit"
                break

            time.sleep(0.1)
        else:
            manifest["stop_reason"] = "watchdog"

        manifest["foreground_alive"] = fg.poll() is None
    finally:
        if daemon:
            daemon.terminate()
            daemon.wait(timeout=5)

        write(cg / "cgroup.kill", 1)
        for child in procs:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)

        if mon:
            mon.terminate()
            mon.wait(timeout=5)

        if kernel:
            kernel.terminate()
            kernel.wait(timeout=5)

        manifest["events_final"] = (cg / "memory.events").read_text()
        manifest["wall_end"] = time.time()
        manifest["returncodes"] = [x.returncode for x in procs]
        (a.out / "registry.tsv").write_text(registry.read_text())
        registry.unlink(missing_ok=True)
        (a.out / "manifest.json").write_text(json.dumps(manifest, indent=2))
        for f in handles:
            f.close()
        for _ in range(50):
            try:
                cg.rmdir()
                break
            except OSError:
                time.sleep(0.05)


if __name__ == "__main__":
    main()
