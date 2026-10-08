#!/usr/bin/env python3
"""Derive Linux launcher/window icons locally from the player's game executable."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from PIL import Image


def prepare_icon(exe, app):
    app = Path(app)
    app.mkdir(parents=True, exist_ok=True)
    # Keep all original ICO sizes for Qt; desktop shells and SDL use the PNG.
    with tempfile.TemporaryDirectory(dir=app) as temp:
        ico = Path(temp) / 'dungeon-siege-2.ico'
        png = Path(temp) / 'dungeon-siege-2.png'
        subprocess.run([sys.executable, str(Path(__file__).with_name('extract_icon.py')),
                        str(exe), str(ico)], check=True)
        raw = ico.read_bytes()
        frames = [struct.unpack_from('<BBBBHHII', raw, 6 + 16 * i)
                  for i in range(struct.unpack_from('<H', raw, 4)[0])]
        best = max(frames, key=lambda f: ((f[0] or 256) * (f[1] or 256), f[5]))
        with Image.open(ico) as source:
            # Some Pillow versions select the 16-color frame by default, even
            # when the same size is available in full color.
            source.ico.getimage((best[0] or 256, best[1] or 256), best[5]).convert('RGBA').save(png)
        ico.replace(app / ico.name)
        png.replace(app / png.name)
    return app / 'dungeon-siege-2.png'


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: prepare_linux_icon.py <game.exe> <app directory>')
    prepare_icon(Path(sys.argv[1]), Path(sys.argv[2]))
