#!/usr/bin/env python3
"""A relay co-op session on one machine, for testing: r6relay on loopback, a hosting game, and, once the host's game is
listening (its room code is known), a joining game. Each game has its own data folder (registry.txt set for relay play,
windowed, no intro) and its own log with W32_NETLOG=1. Nothing listens beyond 127.0.0.1, so no firewall prompt.

  r6_relay_local.py --binary <RainbowSixWindows.exe> --exe <RainbowSix.exe> --game <game folder> --out <folder>
                    [--joiners 1] [--relay-port 47100]

In the host's window: Multiplayer, host a co-op game. The joiner starts by itself; in its window: Multiplayer, join by
address (already filled in: 127.0.0.1 and the joiner's local port). When every game has closed, the logs are summed up:
which sockets, binds, listens and UDP sends each game made (relay mode should need one TCP connection per joiner)."""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'host' / 'linux_launcher'))
from r6_launcher import Registry, REG_SZ   # noqa: E402  (the launcher's own registry.txt writer)

RELAY_SETTINGS = {                          # what the launcher forces in relay mode, plus test conveniences
    'MultiplayerBehindFirewall': 'TRUE', 'R6CoopOnline': 'FALSE', 'R6CoopRouter': 'FALSE', 'MultiplayerOnLan': 'FALSE',
    'Movie On': 'FALSE',                    # (windowed: DS_WINDOW_MODE below)
}


def prepare(data, name, extra):
    reg = Registry(data / 'registry.txt')
    for k, v in {**RELAY_SETTINGS, 'MultiplayerPlayerName': name, **extra}.items():
        reg.set(k, REG_SZ, v.encode('latin-1') + b'\0')
    reg.save()


def status(path):
    try: return dict(l.split('=', 1) for l in path.read_text().splitlines() if '=' in l)
    except OSError: return {}


def summary(log):
    lines = log.read_text(errors='replace').splitlines() if log.exists() else []
    pick = lambda pat: [l for l in lines if re.match(pat, l)]
    out = [f'  {len(pick(r"net: socket tcp"))} TCP and {len(pick(r"net: socket udp"))} UDP sockets']
    for kind in ('bind', 'bind failed', 'listen', 'connect'):
        seen = sorted({re.sub(r' [0-9a-f]+ ', ' ', l.split(': ', 1)[1].replace(kind + ' ', '', 1)) for l in pick(rf'net: {kind} ')})
        if seen: out.append(f'  {kind}: ' + ', '.join(seen[:12]) + (' ...' if len(seen) > 12 else ''))
    to = sorted({l.split()[3] for l in pick(r'net: sendto ') if len(l.split()) > 3})
    if to: out.append('  UDP sendto: ' + ', '.join(to[:12]))
    out += ['  ' + l for l in lines if l.startswith('relay: ') and ('error' in l or 'message' in l)][:5]
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser()
    for a in ('binary', 'exe', 'game', 'out'): ap.add_argument('--' + a, required=True)
    ap.add_argument('--joiners', type=int, default=1)
    ap.add_argument('--relay-port', type=int, default=47100)
    ap.add_argument('--drive', action='store_true', help='test mode: hidden windows, no sound, and each game reads '
                    'commands appended to <out>/<game>/cmd.txt (DS_CMDFILE: click:x,y, key:vk, type:text, shot:png)')
    a = ap.parse_args()
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    build = hashlib.sha256(Path(a.exe).read_bytes()).hexdigest()[:16]
    relay_log = (out / 'relay.log').open('w')
    relay = subprocess.Popen([sys.executable, str(HERE.parent / 'relay' / 'r6relay.py'), '--host', '127.0.0.1', '--port', str(a.relay_port)],
                             stdout=relay_log, stderr=relay_log)
    base = dict(os.environ, W32_NETLOG='1', DS_WINDOW_MODE='windowed', R6_RELAY=f'127.0.0.1:{a.relay_port}', R6_RELAY_BUILD=build)
    games = []

    def start(name, env, extra):
        d = out / name; data = d / 'data'; data.mkdir(parents=True, exist_ok=True)
        prepare(data, name.upper(), extra)
        (d / 'status.txt').unlink(missing_ok=True)
        if a.drive: (d / 'cmd.txt').write_text(''); env = dict(env, DS_TEST='1', W32_AUDIO_MUTE='1', DS_CMDFILE=str(d / 'cmd.txt'))
        log = (d / 'game.log').open('w')
        p = subprocess.Popen([a.binary, '--exe', a.exe, '--game', a.game, '--data', str(data)], stdout=log, stderr=log,
                             env=dict(base, R6_RELAY_STATUS=str(d / 'status.txt'), **env))
        games.append((name, p, d)); print(f'{name}: started (log {d / "game.log"})'); return d

    try:
        h = start('host', {'R6_ROOM': 'host', 'R6_RELAY_NAME': 'HOST'}, {'MultiplayerServerName': 'RELAY TEST'})
        print('host: in its window, Multiplayer -> host a co-op game; waiting for the room code ...')
        room = ''
        while not room:
            if games[0][1].poll() is not None: print('host: the game closed before hosting'); return
            st = status(h / 'status.txt')
            if st.get('state') == 'error': print('host: relay error:', st.get('message')); return
            room = st.get('room', '') if st.get('state') == 'hosting' else ''
            time.sleep(0.5)
        print(f'host: room {room}')
        for k in range(a.joiners):
            port = str(2356 + k)
            start(f'join{k + 1}', {'R6_ROOM': room, 'R6_RELAY_LOCAL_PORT': port},
                  {'MultiplayerManualHostAddress': '127.0.0.1', 'MultiplayerManualJoinPort': port})
        print('joiners: in each window, Multiplayer -> join by address (127.0.0.1 is filled in)')
        last = {}
        while any(p.poll() is None for _, p, _ in games):
            for name, _, d in games:
                st = status(d / 'status.txt'); line = ' '.join(f'{k}={v}' for k, v in st.items())
                if line != last.get(name): print(f'{name}: {line}'); last[name] = line
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        for _, p, _ in games:
            if p.poll() is None: p.terminate()
        relay.terminate()
        for name, _, d in games: print(f'== {name}\n' + summary(d / 'game.log'))


if __name__ == '__main__':
    main()
