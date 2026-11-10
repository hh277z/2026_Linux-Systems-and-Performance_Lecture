"""Bounded implementation trials. Global reclaim experiments are not substituted."""

import argparse
import json
import os
from pathlib import Path
import select
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

from collect import snapshot, counters, psi
from report import calculate, terminal

BASE = Path(__file__).resolve().parents[1]

PRESETS = {'normal': (160, 0, False), 'cpu': (160, 2, False),
           'memory': (64, 0, False), 'mixed': (64, 1, False),
           'file-refault': (64, 0, False), 'high': (64, 0, False),
           'max': (192, 0, True), 'swap-in': (192, 0, False),
           'background': (256, 0, False), 'global-direct': (256, 0, False)}


def check():
    data = {'kernel': os.uname().release,
            'psi': Path('/proc/pressure/memory').read_text(),
            'controllers': Path('/sys/fs/cgroup/cgroup.subtree_control').read_text(),
            'filesystem': subprocess.check_output(['stat', '-fc', '%T', '/var/tmp'], text=True).strip(),
            'swap': Path('/proc/swaps').read_text()}

    if 'memory' not in data['controllers'].split():
        raise RuntimeError('Enable memory controller before running the lab.')

    if data['filesystem'] not in ['ext2/ext3', 'ext4', 'xfs', 'btrfs']:
        raise RuntimeError('/var/tmp must be on a Linux disk filesystem.')

    return data


def print_live_sample(previous, current, start_time, group):
    """터미널 한 행에 같은 구간의 시스템·그룹 PSI와 전역 스캔 속도를 표시한다."""

    def some_percent(path):
        first = previous['files'][path]
        last = current['files'][path]

        stall_us = psi(last['raw'])['some'] - psi(first['raw'])['some']
        elapsed = last['t'] - first['t']

        return stall_us / elapsed / 10000

    before = counters(previous['files']['/proc/vmstat']['raw'])
    after = counters(current['files']['/proc/vmstat']['raw'])
    interval = current['t'] - previous['t']
    rates = []

    for name in ('pgscan_kswapd', 'pgscan_direct'):
        if name in before and name in after:
            rates.append(f'{(after[name] - before[name]) / interval:,.0f}')
        else:
            rates.append('unsupported')

    system_memory = some_percent('/proc/pressure/memory')
    group_memory = some_percent(str(group/'memory.pressure'))
    group_io = some_percent(str(group/'io.pressure'))

    print(f"{current['t'] - start_time:8.1f} | {system_memory:11.2f} | "
          f"{group_memory:10.2f} | {group_io:9.2f} | {rates[0]:20} | {rates[1]:13}",
          flush=True)


def run(args):
    environment = check()

    if os.geteuid() != 0:
        raise RuntimeError('Run using sudo; workloads drop privileges.')

    high, cpu_count, expected_oom = PRESETS[args.scenario]

    run_id = time.strftime('%Y%m%d-%H%M%S')+'-'+args.scenario+'-'+uuid.uuid4().hex[:6]
    temporary_log = tempfile.TemporaryDirectory(prefix='week9-log-', dir='/var/tmp') if args.log_dir is None else None
    out = Path(temporary_log.name) if temporary_log else Path(args.log_dir)/run_id
    out.mkdir(parents=True, exist_ok=True)

    cg = Path('/sys/fs/cgroup')/('week9-v2-'+uuid.uuid4().hex)
    processes, handles, samples = [], [], []

    manifest = dict(scenario=args.scenario, environment=environment, scope='cgroup',
                    adaptation='memory/high use constrained file cache, not global anon competition',
                    seconds=args.seconds, expected_group_oom=expected_oom)

    try:
        cg.mkdir()

        swap_case = args.scenario == 'swap-in'

        if swap_case:
            high = args.swap_mib+64

        background = args.scenario in ['background', 'global-direct']
        memory_info = {l.split()[0].rstrip(':'): int(l.split()[1]) for l in Path('/proc/meminfo').read_text().splitlines()}
        file_mib = min(12288, memory_info['MemTotal']//1024*5//4) if background else 128

        if background and file_mib*1024 <= memory_info['MemTotal']:
            raise RuntimeError('RAM exceeds the 12 GiB file budget; background case needs a separately sized environment')

        if shutil.disk_usage('/var/tmp').free < (512 if background else file_mib+512)*1024**2:
            raise RuntimeError('Insufficient disk for experiment file')

        manifest['file_mib'] = file_mib
        manifest['scope'] = 'global clean file-cache pressure' if background else 'cgroup'

        if background:
            manifest['adaptation'] = 'Sparse zero-file cache exceeds Linux RAM; not a storage throughput benchmark'

        for name, value in [('memory.high', high*1024**2),
                            ('memory.max', (args.swap_mib+128 if swap_case else 192)*1024**2),
                            ('memory.swap.max', (args.swap_mib if swap_case else 0)*1024**2), ('pids.max', 16)]:
            (cg/name).write_text('max' if background and name in ['memory.high', 'memory.max'] else str(value))

        print('GROUP='+str(cg), flush=True)
        print('SCOPE='+manifest['scope']+'; system and group counters are separately labelled.', flush=True)

        # KEY: 경로 복사와 B 터미널의 CG 설정을 마친 후 실험을 시작한다.
        if not args.auto:
            try:
                input('\nPAUSED: set CG in terminal B, then press Enter here to prepare and start. ')
            except EOFError as exc:
                raise RuntimeError('Interactive input required; use --auto for unattended runs.') from exc

        def enter_workload_group():
            # KEY: 자식만 그룹에 넣는다. 부모 관측기는 그룹 밖에 남는다.
            (cg/'cgroup.procs').write_text(str(os.getpid()))
            os.sched_setaffinity(0, {min(os.sched_getaffinity(0))})

            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)

        # Run nobody-readable executable copies from a temporary Linux directory.
        with tempfile.TemporaryDirectory(prefix='week9-v2-', dir='/var/tmp') as temp:
            folder = Path(temp)
            folder.chmod(0o755)

            for name in ['file_reader', 'cpu_busy', 'anon_work', 'psi_watch']:
                target = folder/name
                target.write_bytes((BASE/'bin'/name).read_bytes())
                target.chmod(0o755)

            data = folder/'data.bin'
            prepare_start = time.monotonic()

            with data.open('wb') as stream:
                if background:
                    print(f'PREPARE: sparse zero-file, logical {file_mib} MiB; no payload writes.', flush=True)
                    stream.truncate(file_mib*1024**2)
                else:
                    block = bytes([37])*1024**2
                    print(f'PREPARE: writing {file_mib} MiB on Linux disk.', flush=True)

                    for _ in range(file_mib):
                        stream.write(block)

                stream.flush()
                os.fsync(stream.fileno())

            manifest['file_allocated_bytes'] = data.stat().st_blocks*512
            data.chmod(0o666)

            # File created outside group: worker first drops this file's cache,
            # then warms it inside the group before READY is accepted below.
            with data.open('rb') as stream:
                os.posix_fadvise(stream.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)

            # 1. 실험 조건에 맞는 작업을 선택한다.
            duration = str(args.seconds)

            if swap_case:
                workload = [str(folder/'anon_work'), str(args.swap_mib), duration, 'touch']
            elif expected_oom:
                workload = [str(folder/'anon_work'), '256', duration, 'hold']
            else:
                workload = [str(folder/'file_reader'), str(data), duration]

            commands = [workload]

            for _ in range(cpu_count):
                commands.append([str(folder/'cpu_busy'), duration])

            if background:
                rate = args.reader_mibps

                if rate is None:
                    rate = 50 if args.scenario == 'background' else 0

                commands[0].extend([str(rate), 'random'])
                manifest['readahead_hint'] = 'POSIX_FADV_RANDOM'
                manifest['reader_mibps'] = rate

            if expected_oom:
                # OOM occurs during first touch, before READY: capture it from launch.
                oom_start = snapshot(cg)

            for i, command in enumerate(commands):
                err = (out/f'worker-{i}.stderr').open('w')
                handles.append(err)

                proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=err, text=True, preexec_fn=enter_workload_group)
                processes.append(proc)

                if not select.select([proc.stdout], [], [], 90 if background else 30)[0]:
                    raise TimeoutError('READY timeout')

                line = proc.stdout.readline().strip()

                if line != 'READY' and not expected_oom:
                    raise RuntimeError('Worker failed before READY')

            if swap_case:
                # Explicit preparation, not a claim about global watermarks.
                reclaim = str(args.swap_mib*3//4)+'M swappiness=200'
                (cg/'memory.reclaim').write_text(reclaim)

                manifest['swap_preparation'] = {
                    'method': 'memory.reclaim '+reclaim,
                    'swap_current': int((cg/'memory.swap.current').read_text())}

                print('PREPARED swap: '+str(manifest['swap_preparation']['swap_current'])+' bytes', flush=True)

                if manifest['swap_preparation']['swap_current'] == 0:
                    raise RuntimeError('No actual swapped pages; swap-in case unsupported')

            if background:
                # Let warm-up reclaim settle before collecting the measured interval.
                print('BASELINE: wait 2 s after cache warm-up.', flush=True)
                time.sleep(2)

            manifest['prepare_seconds'] = time.monotonic()-prepare_start
            manifest['file_allocated_bytes_after_warmup'] = data.stat().st_blocks*512

            print(f'PREPARE complete: {manifest["prepare_seconds"]:.2f} s; file allocated {manifest["file_allocated_bytes_after_warmup"]} bytes', flush=True)
            # KEY: max의 첫 쓰기는 READY 전에 일어나므로 실행 전 표본을 사용한다.
            start = oom_start if expected_oom else snapshot(cg)
            (out/'start.json').write_text(json.dumps(start, indent=2))

            print('START: raw system PSI\n'+start['files']['/proc/pressure/memory']['raw'], flush=True)
            print('Raw group path: '+str(cg), flush=True)
            print('Raw group PSI\n'+start['files'][str(cg/'memory.pressure')]['raw'], flush=True)

            if expected_oom:
                processes[0].wait(timeout=10)
            else:
                watch_log = (out/'trigger.txt').open('w')
                handles.append(watch_log)
                # KEY: trigger 이벤트 감시와 아래 PSI 표본 수집은 독립적으로 동작한다.
                watcher = subprocess.Popen([str(folder/'psi_watch'), 'trigger',
                    str(cg/'memory.pressure'), str(args.seconds)], stdout=watch_log,
                    stderr=watch_log)
                processes.append(watcher)

                for proc in processes[:-1]:
                    proc.stdin.write('GO\n')
                    proc.stdin.flush()

                deadline = time.monotonic()+args.seconds+15
                previous = start
                print(' elapsed | SYSTEM mem% | GROUP mem% | GROUP io% | SYSTEM kswapd scan/s | direct scan/s', flush=True)

                while any(p.poll() is None for p in processes):
                    current = snapshot(cg)
                    samples.append(current)
                    dt = current['t']-previous['t']

                    if dt >= .8:
                        print_live_sample(previous, current, start['t'], cg)
                        previous = current

                    if time.monotonic() > deadline:
                        raise TimeoutError('Workload deadline')

                    time.sleep(.5)

            end = snapshot(cg)
            (out/'end.json').write_text(json.dumps(end, indent=2))

            for i, proc in enumerate(processes):
                if proc.stdout:
                    (out/f'worker-{i}.stdout').write_text(proc.stdout.read())

            manifest['returncodes'] = [p.returncode for p in processes]
            manifest['calculation'] = calculate(start, end)

            terminal(start, end)

            for i in range(len(commands)):
                print('WORKLOAD '+str(i)+': '+(out/f'worker-{i}.stdout').read_text().strip(), flush=True)

            events_text = (out/'trigger.txt').read_text() if (out/'trigger.txt').exists() else ''
            print('PSI trigger notifications: '+str(events_text.count('TRIGGER')), flush=True)

            if background:
                a = counters(start['files']['/proc/vmstat']['raw'])
                b = counters(end['files']['/proc/vmstat']['raw'])
                manifest['background_observed'] = b.get('pgscan_kswapd', 0) > a.get('pgscan_kswapd', 0)
                print('Background counter increased: '+str(manifest['background_observed']), flush=True)

            if expected_oom:
                manifest['oom_events'] = (cg/'memory.events').read_text()

                if 'oom_kill 0\n' in manifest['oom_events']:
                    raise RuntimeError('Expected group OOM did not occur')
            elif any(p.returncode != 0 for p in processes):
                raise RuntimeError('Unexpected workload failure')
    except BaseException as exc:
        manifest['error'] = repr(exc)
        raise
    finally:
        for proc in processes:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

        for handle in handles:
            handle.close()

        if cg.exists():
            for _ in range(30):
                try:
                    cg.rmdir()
                    break
                except OSError:
                    time.sleep(.1)

        manifest['cleanup_ok'] = not cg.exists()
        (out/'samples.json').write_text(json.dumps(samples, indent=2))
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2))

        print('CLEANUP: '+('OK' if manifest['cleanup_ok'] else 'FAILED'), flush=True)

        if temporary_log:
            temporary_log.cleanup()
        else:
            print('Optional diagnostic log: '+str(out), flush=True)

        if not manifest['cleanup_ok']:
            raise RuntimeError('Experiment group cleanup failed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('scenario', choices=['check', *PRESETS])
    parser.add_argument('--seconds', type=int, choices=range(2, 21), default=8)
    parser.add_argument('--log-dir', help='Optional diagnostic log folder; students need no log files')
    parser.add_argument('--swap-mib', type=int, choices=[128, 256, 512], default=256)
    parser.add_argument('--reader-mibps', type=int, choices=[0, *range(10, 1001)], default=None,
                        help='0 = unlimited; default background 50, global-direct unlimited')

    parser.add_argument('--auto', action='store_true',
                        help='Skip the Enter pause for automated validation')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))

    if args.scenario == 'check':
        for key, value in check().items():
            print(f'{key}:\n{value}')
    else:
        run(args)
