#!/usr/bin/env python3
"""fiosum.py: one line per fio job from fio's JSON output.

usage: python3 tools/fiosum.py FILE.json [FILE.json ...]
"""
import json
import sys


def load(path):
    text = open(path).read()
    return json.loads(text[text.index('{'):])          # fio may print a line before the JSON


for path in sys.argv[1:]:
    for job in load(path)['jobs']:
        for kind in ('read', 'write'):
            d = job[kind]
            if not d['io_bytes']:
                continue
            p = d['clat_ns'].get('percentile', {})
            us = lambda q: p.get(q, 0) / 1000
            print(f"{job['jobname']:>6} {kind:5}  IOPS {d['iops']:8.0f}  {d['bw_bytes'] / 1e6:7.1f} MB/s"
                  f"  latency p50 {us('50.000000'):7.0f} us  p99 {us('99.000000'):7.0f} us"
                  f"  p99.9 {us('99.900000'):7.0f} us")
