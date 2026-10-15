"""relay.c end to end, without the game: recomp/relay/r6relay.py on a free loopback port, tests/relay_test.c as the
hosting game and as joining games (separate processes: relay.c reads its settings once, from the environment).
  python tests/relay_e2e.py <relay_test binary>        (from recomp/)
Checks: a room code reaches the host's status file; two joiners at once each carry 100 kB both ways, intact and in
order, and reach the game from different loopback addresses (not on macOS: only 127.0.0.1 there); the host's status
counts them; a wrong code and another build are refused with the relay's message in the joiner's status file."""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BIN = sys.argv[1] if len(sys.argv) > 1 else str(HERE.parent / 'relay_test')
failed = 0


def check(ok, what):
    global failed
    print(('ok   ' if ok else 'FAIL ') + what)
    failed += not ok


def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close(); return p


def status(path, timeout=10, until=None):
    end = time.time() + timeout; st = {}
    while time.time() < end:
        try: st = dict(l.split('=', 1) for l in Path(path).read_text().splitlines() if '=' in l)
        except OSError: st = {}
        if until is None or until(st): return st
        time.sleep(0.05)
    return st


tmp = Path(tempfile.mkdtemp(prefix='r6relay-'))
rport, gport = free_port(), free_port()
relay = subprocess.Popen([sys.executable, str(HERE.parent / 'relay' / 'r6relay.py'), '--host', '127.0.0.1', '--port', str(rport)],
                         stderr=subprocess.PIPE, text=True)
procs = [relay]
try:
    while 'listening' not in relay.stderr.readline(): pass
    base = dict(os.environ, R6_RELAY=f'127.0.0.1:{rport}', R6_RELAY_BUILD='4175039d2a57c4a7', R6_RELAY_NAME='JOHN DOE')
    host = subprocess.Popen([BIN, 'host', str(gport)], stdin=subprocess.PIPE, stderr=subprocess.DEVNULL,   # a listen on another port is not relayed
                            env=dict(base, R6_ROOM='host', R6_RELAY_CODE='K7QM4X', R6_RELAY_GAME_PORT=str(gport), R6_RELAY_STATUS=str(tmp / 'host.txt')))
    procs.append(host)
    st = status(tmp / 'host.txt', until=lambda s: s.get('state') == 'hosting')
    room = st.get('room', '')
    check(st.get('state') == 'hosting' and room == 'K7QM4X', f'host: the room it asked for, {room!r}, in the status file')

    def join(code, build='4175039d2a57c4a7', size=100_000, name='j'):
        port = str(free_port())                                          # each joiner's own local listener
        p = subprocess.Popen([BIN, 'join', port, str(size)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
                             env=dict(base, R6_ROOM=code, R6_RELAY_BUILD=build, R6_RELAY_LOCAL_PORT=port,
                                      R6_RELAY_STATUS=str(tmp / f'{name}.txt')))
        procs.append(p); return p
    a, b = join(room, name='a'), join(room.lower(), name='b')
    ra, rb = a.communicate(timeout=30)[0].split(), b.communicate(timeout=30)[0].split()
    check(ra[:1] == ['ok'] and rb[:1] == ['ok'], f'two joiners at once: 100 kB each way intact ({ra}, {rb})')
    if sys.platform != 'darwin':
        check(len(ra) > 1 and len(rb) > 1 and ra[1] != rb[1] and '1' not in (ra[1], rb[1]),
              f'joiners reach the game from different loopback addresses (127.0.0.{ra[1:2]} / 127.0.0.{rb[1:2]})')
    st = status(tmp / 'host.txt', until=lambda s: s.get('players') == '0')
    check(st.get('players') == '0', 'host: players back to 0 after both left')

    w = join('BBBBBB', size=10, name='wrong'); w.communicate(timeout=30)
    st = status(tmp / 'wrong.txt', until=lambda s: s.get('state') == 'error')
    check(st.get('message') == 'no such room', f'wrong code refused: {st.get("message")!r}')
    v = join(room, build='0123456789abcdef', size=10, name='build'); v.communicate(timeout=30)
    st = status(tmp / 'build.txt', until=lambda s: s.get('state') == 'error')
    check('version' in st.get('message', ''), f'another build refused: {st.get("message")!r}')
finally:
    for p in procs[::-1]:
        if p.stdin: p.stdin.close()
        if p is not relay and p is not host: p.kill()
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill()
print('relay end to end: ' + ('all passed' if not failed else f'{failed} FAILED'))
sys.exit(1 if failed else 0)
