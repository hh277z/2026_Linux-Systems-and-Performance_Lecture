#!/usr/bin/env python3
"""Build a local earlyoom copy; never installs or starts a service."""

import argparse
import shutil
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--policy", type=Path, default=Path("starter/lmk_policy.c"))
p.add_argument("--output", type=Path, default=Path("build/earlyoom"))
p.add_argument("--cc", default="cc")
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
dest = a.output.resolve()
if dest.exists():
    p.error("choose a fresh output directory")
if dest == Path("/") or not dest.is_relative_to(Path.cwd().resolve()):
    p.error("output must be under current working directory")
shutil.copytree(root / "vendor/earlyoom", dest)
source = (dest / "main.c").read_text()
assert source.count("int main(int argc, char* argv[])") == 1
(dest / "main.c").write_text(
    source.replace(
        "int main(int argc, char* argv[])",
        "int earlyoom_upstream_main(int argc, char* argv[])",
    )
)
shutil.copyfile(root / "student_support/lab_earlyoom.c", dest / "lab_earlyoom.c")
shutil.copyfile(root / "student_support/lmk_policy.h", dest / "lmk_policy.h")
policy = a.policy.resolve().read_text()
# The policy module sees the stable support header, independent of its source directory.
policy = policy.replace(
    '#include "../student_support/lmk_policy.h"', '#include "lmk_policy.h"'
).replace(
    '#include "../../student/student_support/lmk_policy.h"', '#include "lmk_policy.h"'
)
(dest / "lmk_policy.c").write_text(policy)
subprocess.run(
    ["make", "earlyoom", f"CC={a.cc}", "VERSION=v1.9.0-lab10", "LDFLAGS=-lm"],
    cwd=dest,
    check=True,
)
print(dest / "earlyoom")
