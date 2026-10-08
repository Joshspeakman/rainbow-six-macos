"""Display choices for the Fallout: New Vegas launcher, and the environment the native build reads them from."""
import math
import struct
import zlib
from settings import parse_resolution, resolution_choice, resolution_choices, read_settings, save_settings  # noqa: F401

def frame_rate_choices(refresh, saved=None):
    """Any rate works: above 60 the host gives Havok a matching step (Fallout.ini fMaxTime = 1 / rate)."""
    refresh = round(refresh) if math.isfinite(refresh) and refresh > 0 else 60
    rates = {30, 60, 75, 90, 100, 120, 144, 165, 180, 200, 240, 360, refresh}
    if isinstance(saved, str) and saved.isdecimal() and 1 <= int(saved) <= 1000:
        rates.add(int(saved))
    return [('display', f'Display ({refresh} Hz, VSync)', 'Follows the display refresh with VSync')] + [
        (str(n), f'{n} FPS' + (' (original)' if n == 60 else ''),
         'The rate the game was made for' if n == 60 else f'Up to {n} FPS, VSync off; physics stepped to match')
        for n in sorted(rates)] + [
        ('custom', 'Custom…', 'Choose your own frame rate limit'),
        ('unlimited', 'Unlimited', 'No cap or VSync; physics stepped for the display refresh')]

ANTI_ALIASING = [('off', 'Off', 'No antialiasing'),
                 ('fxaa', 'FXAA', 'Fast post-process edge smoothing; almost free'),
                 ('msaa2', 'MSAA 2×', 'Multisampling, 2 samples'),
                 ('msaa4', 'MSAA 4×', 'Multisampling, 4 samples (recommended)'),
                 ('msaa8', 'MSAA 8×', 'Multisampling, 8 samples'),
                 ('msaa4-fxaa', 'MSAA 4× + FXAA', 'Multisampling with FXAA on top for alpha-tested foliage'),
                 ('msaa8-fxaa', 'MSAA 8× + FXAA', 'The smoothest edges')]
WINDOW_MODES = [('fullscreen', 'Fullscreen', 'Borderless full screen; the render size is scaled to fit, aspect kept'),
                ('windowed', 'Windowed', 'A resizable window')]

def environment(saved, base, refresh):
    """saved launcher choices -> environment variables for FalloutNVLinux"""
    env = dict(base)
    if saved.get('resolution'):
        parse_resolution(saved['resolution'])
        env['DS_RESOLUTION'] = saved['resolution']
    env['DS_WINDOW_MODE'] = 'windowed' if saved.get('windowMode') == 'windowed' else 'fullscreen'
    fps = saved.get('frameRate', '60')
    refresh = round(refresh) if math.isfinite(refresh) and refresh > 0 else 60
    if fps == 'display': env['DSR_FPSCAP'], env['DSR_VSYNC'] = str(refresh), '1'
    elif fps == 'unlimited': env['DSR_FPSCAP'], env['DSR_VSYNC'] = '0', '0'
    elif str(fps).isdecimal() and 1 <= int(fps) <= 1000: env['DSR_FPSCAP'], env['DSR_VSYNC'] = str(int(fps)), '0'
    else: raise ValueError('Frame rate must be between 1 and 1000 FPS, Display or Unlimited.')
    aa = saved.get('antiAliasing', 'off')
    if aa not in {a[0] for a in ANTI_ALIASING}: raise ValueError('Unknown antialiasing choice')
    env.pop('DSR_MSAA', None); env.pop('DSR_FXAA', None)
    if aa.startswith('msaa'): env['DSR_MSAA'] = aa[4]
    if 'fxaa' in aa: env['DSR_FXAA'] = '1'
    return env

def read_bsa_files(path, wanted):
    """{name: bytes} for the files (lower-case archive paths) in a Fallout NV (version 104) BSA"""
    out = {}
    with open(path, 'rb') as f:
        magic, ver, off, aflags, nfold, nfile, foldlen, filelen, fflags = struct.unpack('<4sIIIIIIII', f.read(36))
        if magic != b'BSA\0' or ver != 104: return out
        f.seek(off); folders = [struct.unpack('<QII', f.read(16)) for _ in range(nfold)]
        entries = []
        for _, count, _ in folders:
            n = f.read(1)[0]; folder = f.read(n).rstrip(b'\0').decode('latin1')
            for _ in range(count):
                _, size, o = struct.unpack('<QII', f.read(16)); entries.append((folder, size, o))
        names = f.read(filelen).split(b'\0')
        for (folder, size, o), name in zip(entries, names):
            full = (folder + '\\' + name.decode('latin1')).lower()
            if full not in wanted: continue
            comp = bool(aflags & 4) != bool(size & 0x40000000); size &= 0x3fffffff
            f.seek(o); b = f.read(size)
            if aflags & 0x100: b = b[1 + b[0]:]
            out[full] = zlib.decompress(b[4:]) if comp else b
    return out
