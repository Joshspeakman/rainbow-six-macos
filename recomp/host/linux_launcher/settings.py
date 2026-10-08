"""Display choices and settings for the native DS2 launcher."""
import hashlib
import json
import math
import os
import re
from pathlib import Path

DISTANCES = [('100', 'Original'), ('125', 'Far · 125%'), ('150', 'Farther · 150%'),
             ('200', 'Very far · 200%'), ('250', 'Horizon · 250%'), ('300', 'Maximum · 300%')]

def parse_resolution(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9]+x[0-9]+', value):
        raise ValueError('Use width x height, for example 3440x1440.')
    w, h = map(int, value.split('x'))
    if not (640 <= w <= 16384 and 480 <= h <= 16384):
        raise ValueError('Width must be 640–16384 and height 480–16384 pixels.')
    return w, h

def resolution_choice(value, note='Custom render size; aspect ratio preserved'):
    w, h = parse_resolution(value)
    return value, f'{w} × {h}', note

def resolution_choices(width, height, saved=None):
    ultrawide = {(2560,1080), (3440,1440), (3840,1600), (5120,1440), (5120,2160)}
    sizes = [(7680,4320),(7680,2160),(5120,2880),(5120,2160),(5120,1440),
             (3840,2160),(3840,1600),(3840,1080),(3440,1440),(2560,1600),(2560,1440),(2560,1080),
             (1920,1200),(1920,1080),(1680,1050),(1600,900),(1440,900),(1366,768),
             (1280,800),(1280,720),(1024,768),(800,600)]
    # These are render sizes, so ultrawide presets remain selectable even when
    # the launcher is currently on a smaller display. Presentation keeps aspect.
    values = list(dict.fromkeys(f'{w}x{h}' for w, h in [(width, height)] + sizes
                               if (w <= width and h <= height) or (w, h) in ultrawide))
    if saved not in values:
        try: parse_resolution(saved)
        except ValueError: pass
        else: values.append(saved)
    choices = []
    for i, value in enumerate(values):
        w, h = parse_resolution(value)
        note = ('This display (recommended)' if i == 0 else
                'Rendered at this size, scaled to fit your display' if w > width or h > height else
                'Aspect ratio preserved; borders if needed')
        choices.append(resolution_choice(value, note))
    return choices + [('custom', 'Custom…', 'Enter a render resolution, including ultrawide sizes')]

def frame_rate_choices(refresh, saved=None):
    refresh = round(refresh) if math.isfinite(refresh) and refresh > 0 else 60
    rates = {30, 60, 90, 120, 144, 165, 180, 200, 240, 244, 280, 300, 360, 480, 540, 600, 720, 1000, refresh}
    if isinstance(saved, str) and saved.isdecimal() and 1 <= int(saved) <= 10000:
        rates.add(int(saved))
    return [('auto', f'Display ({refresh} Hz)', 'Follows display refresh with VSync; no 120 FPS ceiling')] + [
        (str(n), f'{n} FPS', f'Up to {n} FPS; VSync off (may tear)') for n in sorted(rates)] + [
        ('custom', 'Custom…', 'Choose your own FPS limit'),
        ('unlimited', 'Unlimited', 'No frame cap or VSync; as fast as the machine can render')]

def read_settings(data):
    try:
        value = json.loads((Path(data) / 'launcher.json').read_text())
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError):
        return {}

def save_settings(data, value):
    path = Path(data) / 'launcher.json'
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix('.json.tmp')
    tmp.write_text(json.dumps(value, indent=2) + '\n')
    tmp.replace(path)


def environment(saved):
    env = dict(os.environ)
    if saved.get('resolution'):
        parse_resolution(saved['resolution'])
        env['DS_RESOLUTION'] = saved['resolution']
    distance = saved.get('viewDistance', '100')
    if distance not in {x[0] for x in DISTANCES}: raise ValueError('Unknown view distance')
    env['DS_DRAW_DISTANCE'] = distance
    fps = saved.get('frameRate', 'auto')
    if fps not in ('auto', 'unlimited') and not (str(fps).isdecimal() and 1 <= int(fps) <= 10000):
        raise ValueError('FPS limit must be between 1 and 10000, or Unlimited.')
    env['DSR_FPSCAP'] = '0' if fps == 'unlimited' else str(fps)
    env['DSR_VSYNC'] = '1' if fps == 'auto' else '0'
    env['DS_WINDOW_MODE'] = 'windowed' if saved.get('windowMode') == 'windowed' else 'fullscreen'
    return env

def cdkey_hash(key):
    # Same retail product-key derivation as the Mac launcher; keep only its double SHA-1 hash.
    alphabet = 'BCDFGHJKMPQRTVWXY2346789'
    key = key.upper().replace('-', '').replace(' ', '')
    if len(key) != 25 or any(c not in alphabet for c in key):
        raise ValueError('Enter the 25 letters and digits from your own retail product key.')
    value = 0
    for c in key: value = value * 24 + alphabet.index(c)
    return hashlib.sha1(hashlib.sha1(value.to_bytes(15, 'little')).digest()).digest()[:8].hex()

def save_cdkey(data, key):
    value = cdkey_hash(key)
    path = Path(data) / 'cdkey-hash.txt'
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path.with_suffix('.tmp'), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as f:
        os.fchmod(f.fileno(), 0o600)
        f.write(value + '\n')
    path.with_suffix('.tmp').replace(path)
