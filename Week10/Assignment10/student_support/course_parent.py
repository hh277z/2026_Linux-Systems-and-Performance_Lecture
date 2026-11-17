#!/usr/bin/env python3
"""Create or remove ONE empty course cgroup parent; no global controller changes."""

import argparse
import os
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("action", choices=["create", "remove"])
p.add_argument("name")
a = p.parse_args()
if (
    os.geteuid() != 0
    or not a.name.startswith("lab10-course-")
    or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in a.name)
):
    p.error("root and a lowercase lab10-course-NAME are required")

cg = Path("/sys/fs/cgroup") / a.name
if a.action == "create":
    if (
        "memory"
        not in Path("/sys/fs/cgroup/cgroup.subtree_control").read_text().split()
    ):
        p.error("memory must already be enabled by the course environment")

    cg.mkdir()
    try:
        (cg / "cgroup.subtree_control").write_text("+memory")
    except BaseException:
        cg.rmdir()
        raise

    print(cg)
else:
    cg.rmdir()  # Refuses nonempty groups; never recurses or kills outside labrun.
    print("removed", cg)
