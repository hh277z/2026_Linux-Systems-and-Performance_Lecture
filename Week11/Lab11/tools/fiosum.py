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
            job_name = job['jobname']
            io_count = f"{d.get('total_ios', 0):8d}"
            iops = f"{d['iops']:8.1f}"
            bandwidth = f"{d['bw_bytes'] / 1e6:7.1f}"
            p50 = f"{us('50.000000'):7.0f}"
            p99 = f"{us('99.000000'):7.0f}"
            p999 = f"{us('99.900000'):7.0f}"
            print(
                f"Simulator {job_name} {kind:5}  I/Os {io_count}  IOPS {iops}  {bandwidth} MB/s  "
                f"latency p50 {p50} us  p99 {p99} us  p99.9 {p999} us"
            )
