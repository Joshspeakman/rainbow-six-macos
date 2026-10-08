#!/usr/bin/env python3
"""fnv_ref.py <outdir> <seconds> <script> : the original FalloutNV.exe under Proton (DXVK) as a rendering/behaviour
reference for the native port. Same script steps as the native host's DS_SCRIPT, timed in seconds from launch:
  @T:key:KEYSYM   @T:type:text   @T:shot:name   @T:move:dx,dy (uinput mouse, relative)   @T:click
Keys go through XTEST to the focused game window (mutter drops XTEST pointer motion, hence uinput for the mouse).
Screenshots are window captures (import). FNV_REF_PREFIX is the Proton prefix (~/Games/fnv-ref-prefix).
Inside gamescope (GAMESCOPE_WAYLAND_DISPLAY set: run as  gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 --
tests/fnv_ref.py ...) the game always has focus, input is XTEST on gamescope's X display (mouse too) and screenshots
come from gamescopectl, so it works with the desktop locked."""
import os, re, sys, time, subprocess
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest
from evdev import UInput, ecodes as e

OUT, SECS, SCRIPT = sys.argv[1], float(sys.argv[2]), sys.argv[3]
os.makedirs(OUT, exist_ok=True)
GAME = os.environ.get('GAME', os.path.expanduser('~/Games/FalloutNV-GOG/app'))
PFX = os.environ.get('FNV_REF_PREFIX', os.path.expanduser('~/Games/fnv-ref-prefix'))
PROTON = os.path.expanduser('~/.local/share/Steam/compatibilitytools.d/UMU-Proton-10.0-4/proton')
env = dict(os.environ, STEAM_COMPAT_DATA_PATH=PFX, STEAM_COMPAT_CLIENT_INSTALL_PATH=os.path.expanduser('~/.local/share/Steam'))
log = open(os.path.join(OUT, 'proton.log'), 'w')
proc = subprocess.Popen(['setsid', PROTON, 'run', os.path.join(GAME, 'FalloutNV.exe')], cwd=GAME, env=env, stdout=log, stderr=log, stdin=subprocess.DEVNULL)
d = display.Display(); root = d.screen().root
GS = bool(os.environ.get('GAMESCOPE_WAYLAND_DISPLAY'))
mouse = None if GS else UInput({e.EV_REL: [e.REL_X, e.REL_Y], e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT]}, name='fnv-ref-mouse')

def find():
    """the game's client area: the 1280x720 window inside the frame titled "Fallout: New Vegas" (else the frame)"""
    frame = None; wins = []
    def walk(w):
        nonlocal frame
        try:
            if w.get_attributes().map_state != X.IsViewable: return
            name = w.get_wm_name() or ''; g = w.get_geometry(); pos = w.translate_coords(root, 0, 0)
            if re.search(r'fallout', name, re.I) and not frame: frame = (w, -pos.x, -pos.y, g.width, g.height)
            wins.append((w, -pos.x, -pos.y, g.width, g.height))
            for c in w.query_tree().children: walk(c)
        except Exception: pass
    walk(root)
    if not frame: return None
    fw, fx, fy, fwid, fh = frame
    inner = [t for t in wins if t[0] != fw and fx <= t[1] and fy <= t[2] and t[1] + t[3] <= fx + fwid and t[2] + t[4] <= fy + fh and t[3] * t[4] > 640 * 360]
    return (max(inner, key=lambda t: t[3] * t[4])[0] if inner else fw, fw)

def activate(w):
    ev = protocol.event.ClientMessage(window=w, client_type=d.intern_atom('_NET_ACTIVE_WINDOW'), data=(32, [2, X.CurrentTime, 0, 0, 0]))
    root.send_event(ev, event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask); w.map(); d.sync()

def key(sym, hold=0.08):
    kc = d.keysym_to_keycode(XK.string_to_keysym(sym))
    xtest.fake_input(d, X.KeyPress, kc); d.sync(); time.sleep(hold)
    xtest.fake_input(d, X.KeyRelease, kc); d.sync()

SHIFTED = {'_': 'minus', ':': 'semicolon'}
def type_text(t):
    for ch in t:
        sym = {' ': 'space', '.': 'period', '-': 'minus', '`': 'grave', '/': 'slash'}.get(ch, ch)
        if ch.isupper() or ch in SHIFTED:
            sh = d.keysym_to_keycode(XK.string_to_keysym('Shift_L')); xtest.fake_input(d, X.KeyPress, sh); d.sync()
            key(SHIFTED.get(ch, ch.lower())); xtest.fake_input(d, X.KeyRelease, sh); d.sync()
        else: key(sym)
        time.sleep(0.05)

steps = []
for tok in SCRIPT.split(';'):
    m = re.match(r'@([\d.]+):(\w+)(?::(.*))?$', tok.strip())
    if m: steps.append((float(m.group(1)), m.group(2), m.group(3) or ''))
steps.sort(key=lambda s: s[0])
t0 = time.time(); win = None
for at, kind, arg in steps:
    while time.time() - t0 < at: time.sleep(0.05)
    if not GS:
        win = win or find()
        if not win: print('no window yet at', at); continue
        if kind != 'shot': activate(win[1]); time.sleep(0.05)
    if kind == 'key': key(arg)
    elif kind == 'type': type_text(arg)
    elif kind == 'move' and GS:
        dx, dy = (int(v) for v in arg.split(','))
        n = max(abs(dx), abs(dy)) // 10 + 1
        for k in range(n): xtest.fake_input(d, X.MotionNotify, detail=1, x=dx // n, y=dy // n); d.sync(); time.sleep(0.01)
    elif kind == 'click' and GS:
        xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.12); xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
    elif kind == 'shot' and GS:
        subprocess.run(['gamescopectl', 'screenshot', os.path.join(OUT, arg + '.png')], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif kind == 'move':
        dx, dy = (int(v) for v in arg.split(','))
        n = max(abs(dx), abs(dy)) // 10 + 1
        for k in range(n):
            mouse.write(e.EV_REL, e.REL_X, dx // n); mouse.write(e.EV_REL, e.REL_Y, dy // n); mouse.syn(); time.sleep(0.01)
    elif kind == 'click':
        mouse.write(e.EV_KEY, e.BTN_LEFT, 1); mouse.syn(); time.sleep(0.12); mouse.write(e.EV_KEY, e.BTN_LEFT, 0); mouse.syn()
    elif kind == 'shot':
        subprocess.run(['import', '-window', hex(win[0].id), os.path.join(OUT, arg + '.png')], stderr=subprocess.DEVNULL)
    print('%6.1f %s %s' % (time.time() - t0, kind, arg), flush=True)
while time.time() - t0 < SECS: time.sleep(0.5)
subprocess.run(['pkill', '-f', 'FalloutNV.exe'])
time.sleep(3); proc.kill()
