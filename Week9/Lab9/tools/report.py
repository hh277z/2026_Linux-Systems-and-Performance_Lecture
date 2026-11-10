import json
import sys
from pathlib import Path
from collect import counters, psi


def calculate(start, end):
    """동일 경로의 시작·끝 표본으로 PSI 비율과 카운터 증가율을 계산한다."""
    result = {}

    for path, first in start['files'].items():
        last = end['files'][path]
        elapsed = last['t'] - first['t']

        if 'pressure' in path:
            before = psi(first['raw'])
            after = psi(last['raw'])
            percentages = {}

            for level, initial_total in before.items():
                stall_us = after[level] - initial_total

                # KEY: μs를 초로 환산한 뒤 실제 측정 구간의 백분율을 구한다.
                percentages[level] = None
                if stall_us >= 0:
                    percentages[level] = stall_us / elapsed / 10000

            result[path] = {'elapsed': elapsed, 'pct': percentages}

        elif path.endswith(('vmstat', 'memory.stat', 'memory.events')):
            before = counters(first['raw'])
            after = counters(last['raw'])
            changes = {}
            prefixes = ('pgscan', 'pgsteal', 'allocstall', 'workingset_refault', 'pswp')

            for name, initial_value in before.items():
                relevant = name.startswith(prefixes) or path.endswith('memory.events')
                if not relevant or name not in after:
                    continue

                # KEY: delta는 구간 전체 증가량이고 per_second는 초당 증가율이다.
                delta = after[name] - initial_value
                changes[name] = {
                    'start': initial_value,
                    'end': after[name],
                    'delta': delta,
                    'per_second': delta / elapsed,
                }

            result[path] = {'elapsed': elapsed, 'counters': changes}

    return result


def terminal(start, end, title='Interval summary'):
    """Display evidence, not an automatic diagnosis."""
    print('\n'+title, flush=True)
    print('Scope / metric                         start          end        delta     result')
    values = calculate(start, end)
    for path, value in values.items():
        scope = 'SYSTEM' if path.startswith('/proc/') else 'GROUP'
        print(f'  {path}  [elapsed {value["elapsed"]:.3f} s]')
        if 'pct' in value:
            resource = 'cpu' if 'cpu' in path else 'io' if '/io' in path else 'memory'
            a = psi(start['files'][path]['raw'])
            b = psi(end['files'][path]['raw'])
            for key, pct in value['pct'].items():
                suffix = 'invalid' if pct is None else f'{pct:.2f}%'
                print(f'{scope+" "+resource+" "+key+" (us)":36} {a[key]:12} {b[key]:12} {b[key]-a[key]:12} {suffix:>10}')
        else:
            for key, c in value['counters'].items():
                if key in ['pgscan_kswapd', 'pgscan_direct', 'pgsteal_kswapd',
                           'pgsteal_direct', 'workingset_refault_file',
                           'workingset_refault_anon', 'pswpin', 'pswpout',
                           'high', 'max', 'oom', 'oom_kill'] or key.startswith('allocstall'):
                    print(f'{scope+" "+key:36} {c["start"]:12} {c["end"]:12} {c["delta"]:12} {c["per_second"]:9.1f}/s')
    for path, entry in start['files'].items():
        if path.endswith(('memory.current', 'memory.high', 'memory.max')):
            print(f'GROUP {Path(path).name}: {entry["raw"].strip()} -> {end["files"][path]["raw"].strip()} bytes (max = unlimited)')
    print('PSI totals: microseconds. Scan/steal/swap: pages. Events/allocstall: counts.\n'
          'Rates use per-file monotonic elapsed time. Missing fields are not zero.', flush=True)


if __name__ == '__main__':
    folder = Path(sys.argv[1])
    terminal(json.loads((folder/'start.json').read_text()),
             json.loads((folder/'end.json').read_text()))
