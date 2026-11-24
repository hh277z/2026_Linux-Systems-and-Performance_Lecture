#!/usr/bin/env python3
"""plot.py: pictures from Week 11 logs (needs python3-matplotlib).

  python3 tools/plot.py timeline logs/lab3-b-trace.csv out.png [start_ms end_ms]
      die별로 read, program, erase가 언제 일어났는지 (emmcctl trace 출력)
  python3 tools/plot.py cdf out.png logs/lab3-b-ui.json logs/lab3-c-ui.json ...
      fio JSON 여러 개의 completion latency 분포
"""
import csv
import json
import sys

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

COLORS = {'read': '#d62728', 'program': '#1f77b4', 'erase': '#ff7f0e'}


def timeline(path, out, t0=None, t1=None):
    rows = [r for r in csv.DictReader(open(path)) if r['kind'] in COLORS]
    if not rows:
        sys.exit('no die operations in ' + path)
    s = [float(r['start_us']) / 1e3 for r in rows]
    t0 = min(s) if t0 is None else t0
    t1 = t0 + 20 if t1 is None else t1                     # 20 ms가 기본 창
    dies = sorted({int(r['die']) for r in rows})
    fig, ax = plt.subplots(figsize=(11, 0.6 * len(dies) + 1.5))
    for r in rows:
        a, b = float(r['start_us']) / 1e3, float(r['end_us']) / 1e3
        if b < t0 or a > t1:
            continue
        y = dies.index(int(r['die']))
        h = 0.35 if r['kind'] == 'read' else 0.7               # read는 얇게, 위에 그린다
        ax.barh(y, b - a, left=a, height=h, color=COLORS[r['kind']], edgecolor='none',
                zorder=3 if r['kind'] == 'read' else 2)
    ax.set_yticks(range(len(dies)), [f'die {d}' for d in dies])
    ax.set_xlim(t0, t1)
    ax.set_xlabel('time (ms, emmcsim trace)')
    for k, c in COLORS.items():
        ax.barh([], [], color=c, label=k)
    ax.legend(loc='upper right', ncol=3, fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)


def cdf(out, paths):
    fig, ax = plt.subplots(figsize=(7, 4))
    for path in paths:
        text = open(path).read()
        job = json.loads(text[text.index('{'):])['jobs'][0]
        d = job['read'] if job['read']['io_bytes'] else job['write']
        pts = sorted((float(k), v / 1e3) for k, v in d['clat_ns']['percentile'].items())
        ax.step([v for _, v in pts], [p for p, _ in pts], where='post', label=path.split('/')[-1])
    ax.set_xscale('log')
    ax.set_xlabel('completion latency (us, fio)')
    ax.set_ylabel('percentile')
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=150)


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'timeline':
        extra = [float(x) for x in sys.argv[4:6]]
        timeline(sys.argv[2], sys.argv[3], *extra)
    elif len(sys.argv) >= 4 and sys.argv[1] == 'cdf':
        cdf(sys.argv[2], sys.argv[3:])
    else:
        sys.exit(__doc__)
