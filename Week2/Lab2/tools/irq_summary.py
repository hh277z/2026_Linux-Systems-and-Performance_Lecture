"""Summarize IRQ actions; pair by CPU and nesting, not by PID or adjacency."""
import collections
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
entry = collections.Counter()
stacks = collections.defaultdict(list)
pairs = []
unmatched = 0
for line in text.splitlines():
    m = re.search(r'\[(\d+)\].*?\s(\d+\.\d+):\s+irq_handler_(entry|exit):\s+irq=(\d+)(.*)', line)
    if not m:
        continue
    cpu, timestamp, kind, irq, tail = m.groups()
    if kind == 'entry':
        name = tail.split('name=', 1)[-1].strip()
        entry[(irq, name)] += 1
        stacks[cpu].append((irq, float(timestamp), name, line))
    elif stacks[cpu] and stacks[cpu][-1][0] == irq:
        _, start, name, original = stacks[cpu].pop()
        pairs.append((cpu, irq, name, (float(timestamp)-start)*1e6, original, line))
    else:
        unmatched += 1
print(f'INPUT {path}')
print(f'ENTRY events={sum(entry.values())}; matched pairs={len(pairs)}')
print(f'Unpaired entry={sum(map(len,stacks.values()))}; unpaired exit={unmatched}')
for (irq, name), count in entry.most_common(8):
    print(f'irq={irq:>4} entries={count:>6} name={name}')
if pairs:
    cpu, irq, name, elapsed, start, end = pairs[0]
    print(f'FIRST PAIR cpu={cpu} irq={irq} name={name} elapsed_us={elapsed:.3f}')
    print(start)
    print(end)
else:
    print('No matched pair observed. This is NOT proof that the system had no interrupts.')
print('Check recorder.txt overruns before interpreting counts. Tracepoint interval is not full hardware latency.')
