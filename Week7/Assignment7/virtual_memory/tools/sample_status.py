#!/usr/bin/env python3
"""Sample status only; this helper does not implement vasdump or parse smaps."""
import argparse
import csv
from pathlib import Path
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('pid',type=int)
p.add_argument('output',type=Path)
p.add_argument('--samples',type=int,default=12)
p.add_argument('--interval',type=float,default=1)
p.add_argument('--run-id',default='run1')
p.add_argument('--phase',default='record-marker-from-program-log')
a=p.parse_args()
if a.pid<=0 or a.samples<1 or a.interval<=0:p.error('positive PID, samples and interval required')
start=time.monotonic()
with a.output.open('x',newline='') as f:
    w=csv.writer(f);w.writerow(['run_id','sample','elapsed_s','phase','VmSize_kB','VmRSS_kB'])
    for i in range(a.samples):
        try:
            lines=Path(f'/proc/{a.pid}/status').read_text().splitlines()
            values={s.split(':',1)[0]:s.split(':',1)[1].strip().split()[0] for s in lines if ':' in s and s.split(':',1)[1].strip()}
            if 'VmRSS' not in values:raise ProcessLookupError('no memory fields (possibly exited)')
        except (OSError,ProcessLookupError) as e:
            p.exit(1,f'target unavailable; CSV contains partial evidence: {e}\n')
        w.writerow([a.run_id,i,f'{time.monotonic()-start:.6f}',a.phase,values['VmSize'],values['VmRSS']]);f.flush()
        if i+1<a.samples:time.sleep(max(0,start+(i+1)*a.interval-time.monotonic()))
