#!/usr/bin/env python3
"""Compare two log aggregations in one epoll server; optionally profile that C server."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DATASET = ROOT / 'service_log_dataset'


def expected_totals():
    """Derive the reply signature from the supplied service-level answer table."""
    totals = dict(requests=0, errors=0, duration_us=0, signature=0)

    with (DATASET / 'expected_stats.csv').open(newline='', encoding='utf-8') as file:
        rows = csv.DictReader(file)
        count = 0

        for expected_id, row in enumerate(rows):
            service = int(row['service_id'])
            if service != expected_id:
                raise RuntimeError('Expected statistics have missing or unordered services')

            values = {name: int(row[name]) for name in (
                'request_count', 'error_count', 'total_duration_us',
                'min_duration_us', 'max_duration_us',
            )}
            totals['requests'] += values['request_count']
            totals['errors'] += values['error_count']
            totals['duration_us'] += values['total_duration_us']
            totals['signature'] += (service + 1) * sum(values.values())
            count += 1

    if count != 1024:
        raise RuntimeError('Expected statistics must contain 1024 services')

    return totals


def parse_stats(reply):
    match = re.fullmatch(
        r'STATS requests=(\d+) errors=(\d+) duration_us=(\d+) signature=(\d+)', reply
    )
    if not match:
        raise RuntimeError(f'Invalid statistics reply: {reply}')

    return dict(zip(('requests', 'errors', 'duration_us', 'signature'),
                    (int(value) for value in match.groups())))


def show_inspection_commands(pid, epfd):
    """Always show a banner; add ANSI emphasis only on a terminal."""
    commands = (
        f'ps -p {pid} -o pid,comm,nlwp',
        f'ls -l /proc/{pid}/fd',
        f'cat /proc/{pid}/fdinfo/{epfd}',
    )
    highlight = sys.stdout.isatty() and 'NO_COLOR' not in os.environ
    start, end = ('\033[1;93m', '\033[0m') if highlight else ('', '')

    print('\n' + '=' * 64)
    print('B terminal: run the following commands in terminal B')
    print('-' * 64)

    for command in commands:  # KEY_B_COMMANDS
        print(f'{start}{command}{end}')

    print(f'Optional (if installed): lsof -p {pid}')
    print('=' * 64)
    print('Return to terminal A after inspecting the results.\n', flush=True)


def receive_line(sock):
    data = bytearray()

    while not data.endswith(b'\n'):
        part = sock.recv(1)
        if not part:
            raise RuntimeError('Server closed before reply')

        data.extend(part)
        if len(data) > 512:
            raise RuntimeError('Reply too long')

    return data.decode().strip()


def run(mode, inspect=False, profile=False):
    logs = ROOT / 'logs'
    logs.mkdir(exist_ok=True)
    expected = expected_totals()
    csv_path = DATASET / 'requests.csv'

    result = {}
    with tempfile.TemporaryDirectory(prefix='week3-ipc-', dir='/tmp') as folder:
        path = str(Path(folder) / 'service.sock')
        server = [str(ROOT / 'bin/ipc_server'), mode, path, str(csv_path)]
        command = server
        profile_path = logs / f'ipc-{mode}.perf.data'
        capture_path = Path(folder) / f'ipc-{mode}.perf.data'

        if profile:
            command = ['perf', 'record', '--quiet', '-e', 'cpu-clock:u', '-F', '99',
                       '--call-graph', 'fp', '-o', str(capture_path), '--', *server]

        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, start_new_session=True)
        ready = ''
        server_pid = None

        try:
            if not select.select([proc.stdout], [], [], 15)[0]:
                raise RuntimeError('Server startup timeout')

            ready = proc.stdout.readline()
            match = re.search(r'READY pid=(\d+) epfd=(\d+) listener=(\d+)', ready)
            if not match:
                raise RuntimeError('Server did not start: ' + ready)

            server_pid, epfd, listener = map(int, match.groups())
            print(ready.strip(), flush=True)

            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as a, \
                 socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as b:
                for connection in (a, b):
                    connection.settimeout(45)
                    connection.connect(path)
                    connection.sendall(b'PING\n')
                    if receive_line(connection) != 'PONG':
                        raise RuntimeError('Warmup failed')

                status = Path(f'/proc/{server_pid}/status').read_text()
                threads = int(re.search(r'^Threads:\s+(\d+)', status, re.M).group(1))
                fdinfo = Path(f'/proc/{server_pid}/fdinfo/{epfd}').read_text()
                watched = len(re.findall(r'^tfd:', fdinfo, re.M))
                print(f'OBSERVE server_threads={threads} watched_fds={watched} (listener + A + B)')
                print(fdinfo.strip())
                (logs / f'ipc-{mode}-fdinfo.txt').write_text(fdinfo)

                if inspect:
                    show_inspection_commands(server_pid, epfd)
                    input('Enter after FD inspection: ')

                a.sendall(b'STATS\n')  # KEY_CLIENT_STATS
                if receive_line(a) != 'START':
                    raise RuntimeError('Missing START')

                # START is flushed just before the server begins aggregation.
                start = time.monotonic()
                b.sendall(b'PING\n')  # KEY_CLIENT_PING
                reply_b = receive_line(b)
                delay = (time.monotonic() - start) * 1000
                statistics = parse_stats(receive_line(a))
                if reply_b != 'PONG' or statistics != expected:
                    raise RuntimeError(
                        f'Bad replies: B={reply_b}, stats={statistics}, expected={expected}'
                    )

                result = dict(mode=mode, server_threads=threads, watched_fds=watched,
                              b_reply=reply_b, stats=statistics, b_delay_ms=round(delay, 1))
                print(f'B reply={reply_b} delay_ms={delay:.1f}; A statistics match answer table')
                b.sendall(b'QUIT\n')
                if receive_line(b) != 'BYE':
                    raise RuntimeError('Server did not acknowledge cleanup')
        finally:
            if proc.poll() is None:
                if server_pid is not None and profile:
                    os.kill(server_pid, signal.SIGTERM)
                else:
                    proc.terminate()

            try:
                tail = proc.communicate(timeout=15)[0]
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                tail = proc.communicate()[0]
                raise RuntimeError('Server required forced cleanup')
            finally:
                if proc.stdout:
                    proc.stdout.close()

            (logs / f'ipc-{mode}-server.txt').write_text(ready + tail)
            match = re.search(r'AGGREGATE_END fd=\d+ compute_ms=([0-9.]+)', tail)
            if match:
                result['compute_ms'] = float(match.group(1))

        if proc.returncode != 0:
            raise RuntimeError(f'Server/profiler exit code {proc.returncode}')
        if Path(path).exists():
            raise RuntimeError('Server did not remove its socket')

        result['cleaned'] = True
        if profile:
            if not capture_path.exists() or capture_path.stat().st_size == 0:
                raise RuntimeError('perf did not create a profile')

            shutil.copy2(capture_path, profile_path)

            report = subprocess.run(
                ['perf', 'report', '--stdio', '--no-children', '--sort', 'symbol',
                 '-i', str(profile_path)], text=True, capture_output=True, timeout=30,
            )
            if report.returncode != 0:
                raise RuntimeError('perf report failed: ' + report.stderr)
            if mode == 'rescan' and 'aggregate_rescan' not in report.stdout:
                raise RuntimeError('perf report did not identify aggregate_rescan: ' + report.stderr)

            (logs / f'ipc-{mode}-perf-report.txt').write_text(report.stdout)
            result['profile'] = str(profile_path)
            print('Inspect from the Week3 folder in WSL: '
                  f'perf report --stdio --no-children --sort symbol -i logs/{profile_path.name}')

    (logs / f'ipc-{mode}.json').write_text(json.dumps(result, indent=2) + '\n')
    print('RESULT ' + json.dumps(result))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['rescan', 'onepass'])
    parser.add_argument('--inspect', action='store_true', help='Pause for /proc inspection')
    parser.add_argument('--profile', action='store_true', help='Record the C server with perf')
    args = parser.parse_args()

    try:
        run(args.mode, args.inspect, args.profile)
    except (OSError, RuntimeError, EOFError, KeyboardInterrupt) as exc:
        raise SystemExit(f'ERROR: {exc}')
