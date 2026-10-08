#!/usr/bin/env python3
"""Create a read-only asset view for the selected native engine.

The default view contains base-game assets for the GOG-derived engine. The
Broken World view requires a separately recompiled expansion engine. Existing
game files are never changed or copied over.
"""
import argparse
from pathlib import Path

RESOURCES = ['Logic.ds2res', 'Objects.ds2res', 'Terrain.ds2res', 'Sound1.ds2res',
             'Sound2.ds2res', 'Voices.ds2res', 'Movies1.ds2res', 'Movies2.ds2res']

BW_RESOURCES = ['xLogic.ds2res', 'xObjects.ds2res', 'xTerrain.ds2res',
                'xSound.ds2res', 'xVoices.ds2res', 'xMovies1.ds2res']

def make_view(source, target, *, broken_world=False):
    source, target = Path(source).resolve(), Path(target).absolute()
    if target.exists(): raise ValueError(f'Use a new directory for the data view: {target}')
    def find(folder, name):
        return next((p for p in folder.iterdir() if p.name.casefold()==name.casefold()), None)
    entries = []
    resources = RESOURCES + (BW_RESOURCES if broken_world else [])
    maps = ['World.ds2map'] + (['xDS2World.ds2map', 'xDS2XWorld.ds2map'] if broken_world else [])
    for sub, names in [('Resources', resources), ('Maps', maps)]:
        folder = find(source, sub)
        if not folder: raise ValueError(f'Missing {sub} in {source}')
        for name in names:
            file = find(folder, name)
            if not file or not file.is_file(): raise ValueError(f'Missing {sub}/{name} in {source}')
            entries.append((Path(sub)/name, file))
    system = find(source, 'system')
    if not system or not system.is_dir(): raise ValueError('Missing system directory')
    entries.append((Path('system'), system))
    detail = find(source, 'system_detail.gas')
    if detail: entries.append((Path('system_detail.gas'), detail))
    if broken_world:
        presaves = find(source, 'presaves')
        if presaves and presaves.is_dir(): entries.append((Path('presaves'), presaves))
    target.mkdir(parents=True)
    for rel, file in entries:
        dest = target/rel; dest.parent.mkdir(parents=True, exist_ok=True); dest.symlink_to(file)
    return target

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('steam_folder',type=Path); parser.add_argument('output',type=Path)
    parser.add_argument('--broken-world',action='store_true',help='include expansion assets for a separate BW native build')
    args=parser.parse_args()
    try: print(make_view(args.steam_folder,args.output,broken_world=args.broken_world))
    except (OSError,ValueError) as error: parser.exit(1,f'{error}\n')

if __name__=='__main__': main()
