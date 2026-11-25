#!/usr/bin/env python3
"""The launch window's art for Rainbow Six, made from the player's own copy (and GOG's store art, unless --no-downloads).

    r6_art.py <game folder> <art folder> [--no-downloads]

From the copy, with ffmpeg when it is installed: frames of the game's own Smacker movies (data/video: the intro, the
ending), blown up and sharpened for the window: intro-<n>.jpg, the backdrop's slides. The window itself reads the main
menu's pictures (data/bitmap/*.RSB) and keys the title out of mm_bg.RSB.
From GOG (Tom Clancy's Rainbow Six, product 1207658752): store-portrait.jpg (the box), store-hero.jpg (the store
page's background) and store-shot-<n>.jpg (its screenshots).
A file that is there already is kept. Nothing from the game is stored in the project: this runs on the player's Mac.
"""
import json, os, shutil, subprocess, sys, urllib.request

MOVIES = [('intro.smk', (0.12, 0.30, 0.48, 0.66, 0.84)), ('EndSplsh.smk', (0.25, 0.55))]
GOG = 'https://api.gog.com/v2/games/1207658752'


def find_ci(folder, *parts):
    for part in parts:
        if not os.path.isdir(folder): return None
        hit = next((n for n in os.listdir(folder) if n.lower() == part.lower()), None)
        if not hit: return None
        folder = os.path.join(folder, hit)
    return folder


def movies(game, art):
    ffmpeg = shutil.which('ffmpeg') or next((p for p in ('/opt/homebrew/bin/ffmpeg', '/usr/local/bin/ffmpeg') if os.path.exists(p)), None)
    if not ffmpeg: return
    n = 0
    for name, ats in MOVIES:
        movie = find_ci(game, 'data', 'video', name)
        if not movie: continue
        r = subprocess.run([ffmpeg.replace('ffmpeg', 'ffprobe'), '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', movie], capture_output=True, text=True)
        try: dur = float(r.stdout.strip())
        except ValueError: continue
        for at in ats:
            n += 1; out = os.path.join(art, 'intro-%d.jpg' % n)
            if os.path.exists(out): continue
            subprocess.run([ffmpeg, '-v', 'error', '-y', '-ss', '%.1f' % (dur * at), '-i', movie, '-frames:v', '1',
                            '-vf', 'crop=iw:ih*0.74:0:ih*0.13,scale=1280:-2:flags=lanczos,unsharp=5:5:0.7', '-q:v', '2', out + '.part.jpg'], capture_output=True)
            if os.path.exists(out + '.part.jpg') and os.path.getsize(out + '.part.jpg') > 15000: os.replace(out + '.part.jpg', out)
            elif os.path.exists(out + '.part.jpg'): os.remove(out + '.part.jpg')


def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'}), timeout=20) as r: return r.read()


def store(art):
    try: d = json.loads(fetch(GOG))
    except Exception: return
    links = d.get('_links', {})
    want = {'store-portrait.jpg': (links.get('boxArtImage') or {}).get('href'), 'store-hero.jpg': (links.get('backgroundImage') or {}).get('href')}
    for k, s in enumerate(d.get('_embedded', {}).get('screenshots', [])[:6]):
        href = ((s.get('_links') or {}).get('self') or {}).get('href', '')
        if href: want['store-shot-%d.jpg' % (k + 1)] = href.replace('{formatter}', '1600')
    for name, url in want.items():
        out = os.path.join(art, name)
        if not url or os.path.exists(out): continue
        try: data = fetch(url)
        except Exception: continue
        if len(data) > 15000 and data[:3] == b'\xff\xd8\xff':
            with open(out + '.part', 'wb') as f: f.write(data)
            os.replace(out + '.part', out)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 2: sys.exit(__doc__)
    os.makedirs(args[1], exist_ok=True)
    if args[0] != '-': movies(args[0], args[1])
    if '--no-downloads' not in sys.argv: store(args[1])


if __name__ == '__main__':
    main()
