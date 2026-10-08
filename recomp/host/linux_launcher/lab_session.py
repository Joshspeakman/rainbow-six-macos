"""Save isolation and launch settings shared by the Test Lab panel and checks."""
from pathlib import Path
import shutil
import time
from settings import environment
from profiles import save_relative

SAVE_RELATIVE = Path('drive_c/Users/player/Documents/My Games/Dungeon Siege 2/Save/SinglePlayer')


def copy_saves(source, destination, edition='gog'):
    relative = save_relative(edition)
    src, dst = Path(source) / relative, Path(destination) / relative
    if dst.exists():
        backup = Path(destination) / 'save-backups' / str(time.time_ns())
        backup.parent.mkdir(parents=True, exist_ok=True)
        dst.rename(backup)
    if src.is_dir():
        shutil.copytree(src, dst, symlinks=False)
    else:
        dst.mkdir(parents=True, exist_ok=True)


def launch_environment(settings, data):
    env = environment(settings)
    # Visible window. Inherited scripts, exit timers and key overrides must not
    # affect this isolated run. Keep the explicitly chosen graphics settings.
    for key in list(env):
        if key.startswith(('DS_', 'DS2_', 'DSR_')) and key not in (
            'DS_RESOLUTION', 'DS_DRAW_DISTANCE', 'DSR_FPSCAP', 'DSR_VSYNC'):
            del env[key]
    env.update(DS_TEST_LAB='1', DS_WINDOW_MODE='windowed', DS_NO_PORTMAP='1',
               DS_FRAME_TRACE=str(Path(data) / f'frames-{time.time_ns()}.csv'))
    return env
