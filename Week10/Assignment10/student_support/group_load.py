#!/usr/bin/env python3
"""Children inherit the launcher's cgroup before allocating."""

import argparse
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument("memhog")
p.add_argument("--size", default="16MiB")
p.add_argument("--count", type=int, default=3)
p.add_argument("--hold", default="10")
a = p.parse_args()
if not 1 <= a.count <= 8:
    p.error("count 1..8")
children = []
try:
    children = [
        subprocess.Popen([a.memhog, "anon", a.size, "--hold", a.hold])
        for _ in range(a.count)
    ]
    codes = [x.wait() for x in children]
    print(codes)
    sys.exit(int(any(codes)))
finally:
    for x in children:
        if x.poll() is None:
            x.terminate()

    for x in children:
        x.wait()
