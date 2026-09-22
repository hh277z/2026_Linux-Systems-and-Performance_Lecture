"""Behavior tests on real Linux processes, no root required."""
import os
from pathlib import Path
import selectors
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
os.chdir(ROOT)
INT_BIT = 1 << (signal.SIGINT - 1)
TERM_BIT = 1 << (signal.SIGTERM - 1)
children = []

def start(name, *args):
    p = subprocess.Popen([str(ROOT / 'bin' / name), *args], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    children.append(p)
    return p

def until(p, token):
    data = b''
    deadline = time.monotonic() + 5
    with selectors.DefaultSelector() as sel:
        sel.register(p.stdout, selectors.EVENT_READ)
        while token.encode() not in data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not sel.select(remaining):
                raise AssertionError(f'Timeout waiting for {token}: {data!r}')
            chunk = os.read(p.stdout.fileno(), 8192)
            if not chunk:
                raise AssertionError(f'Unexpected EOF waiting for {token}: {data!r}')
            data += chunk
    return data.decode()

def masks(p):
    rows = dict(x.split(':', 1) for x in Path(f'/proc/{p.pid}/status').read_text().splitlines())
    return {k: int(rows[k].strip(), 16) for k in ['SigPnd','ShdPnd','SigBlk','SigIgn','SigCgt']}

def enter(p):
    p.stdin.write(b'\n')

try:
    p = start('signal_steps')
    until(p, 'Do not send yet.')
    s = masks(p)
    assert all(not (s[k] & INT_BIT) for k in s)
    enter(p); until(p, 'then Enter to snapshot pending state.')
    s = masks(p); assert s['SigBlk'] & INT_BIT and s['SigCgt'] & INT_BIT
    p.send_signal(signal.SIGINT)
    s = masks(p); assert s['ShdPnd'] & INT_BIT and not s['SigPnd'] & INT_BIT
    enter(p); until(p, 'then Enter to snapshot again.')
    p.send_signal(signal.SIGINT); p.send_signal(signal.SIGINT)
    assert masks(p)['ShdPnd'] & INT_BIT
    enter(p); until(p, 'Enter to restore the previous mask.')
    enter(p); output = until(p, 'Enter to finish.')
    assert output.count('HANDLER SIGINT') == 1 and 'handled=1' in output
    s = masks(p)
    assert not (s['ShdPnd'] & INT_BIT or s['SigBlk'] & INT_BIT) and s['SigCgt'] & INT_BIT
    enter(p); assert p.wait(timeout=5) == 0
    print('PASS SIGINT: default -> caught+blocked -> process pending -> coalesced -> delivered')
    for mode in ['default','ignore','block','catch']:
        p = start('term_demo', mode); until(p, f'SIGTERM={signal.SIGTERM}')
        p.send_signal(signal.SIGTERM)
        if mode == 'default':
            assert p.wait(timeout=5) == -signal.SIGTERM
            print('PASS default: terminated by SIGTERM'); continue
        if mode == 'catch': until(p, 'returning without exit')
        s = masks(p)
        assert p.poll() is None
        assert bool(s['SigIgn'] & TERM_BIT) == (mode == 'ignore')
        assert bool(s['SigBlk'] & TERM_BIT) == (mode == 'block')
        assert bool(s['SigCgt'] & TERM_BIT) == (mode == 'catch')
        assert bool(s['ShdPnd'] & TERM_BIT) == (mode == 'block')
        p.kill(); assert p.wait(timeout=5) == -signal.SIGKILL
        print(f'PASS {mode}: exact TERM masks + alive -> killed by SIGKILL')
    p = start('term_demo', 'bad-mode'); assert p.wait(timeout=5) == 2
    print('PASS invalid mode rejected')
finally:
    for p in children:
        if p.poll() is None: p.kill()
        p.wait(timeout=5)
