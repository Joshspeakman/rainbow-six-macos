"""Edition routing, expansion content isolation and save regressions, without game files."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'tools'), str(ROOT/'host/linux_launcher')]
from steam_data_view import RESOURCES, BW_RESOURCES, make_view
from profiles import select_profile, command, save_relative
from lab_session import copy_saves


class Editions(unittest.TestCase):
    def test_profile_switches_code_with_assets(self):
        defaults = dict(binary='/base/native', exe='/base/game.exe', game='/base/assets', data='/saves')
        profiles = {'Steam base game': '/steam/base', 'Broken World': dict(
            binary='/bw/native', exe='/bw/prepared.exe', game='/bw/assets', edition='broken-world')}
        base = select_profile(profiles, 'Steam base game', **defaults)
        bw = select_profile(profiles, 'Broken World', **defaults)
        self.assertEqual(command(base), ['/base/native','--exe','/base/game.exe','--game','/steam/base','--data','/saves'])
        self.assertEqual(command(bw), ['/bw/native','--exe','/bw/prepared.exe','--game','/bw/assets','--data','/saves'])
        with self.assertRaises(ValueError):
            select_profile({'bad': {'edition':'broken-world','game':'/bw/assets'}},'bad',**defaults)

    def test_asset_views_keep_campaigns_separate(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); source=root/'source'
            for sub, names in [('Resources',RESOURCES+BW_RESOURCES+['unrequested-mod.ds2res']),
                               ('Maps',['World.ds2map','xDS2World.ds2map','xDS2XWorld.ds2map'])]:
                (source/sub).mkdir(parents=True)
                for name in names: (source/sub/name).write_bytes(b'fixture')
            (source/'system').mkdir()
            (source/'presaves').mkdir()
            (source/'presaves/hero.ds2party').write_bytes(b'premade hero')
            base=make_view(source,root/'base')
            bw=make_view(source,root/'bw',broken_world=True)
            self.assertEqual({p.name for p in (base/'Maps').iterdir()},{'World.ds2map'})
            self.assertEqual(len(list((bw/'Maps').iterdir())),3)
            self.assertEqual({p.name for p in (bw/'Resources').iterdir()},set(RESOURCES+BW_RESOURCES))
            self.assertTrue((bw/'Resources/xLogic.ds2res').is_symlink())
            self.assertEqual((bw/'presaves/hero.ds2party').read_bytes(), b'premade hero')
            self.assertFalse((base/'presaves').exists())
            (source/'Resources/xLogic.ds2res').unlink()
            with self.assertRaises(ValueError): make_view(source,root/'invalid',broken_world=True)
            self.assertFalse((root/'invalid').exists())

    def test_expansion_lab_copies_only_expansion_saves(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); source=root/'normal'; destination=root/'lab'
            for edition in ('gog','broken-world'):
                path=source/save_relative(edition); path.mkdir(parents=True)
                (path/'hero.save').write_text(edition)
            copy_saves(source,destination,'broken-world')
            copied=destination/save_relative('broken-world')/'hero.save'
            self.assertEqual(copied.read_text(),'broken-world')
            self.assertFalse((destination/save_relative('gog')).exists())
            copied.write_text('lab changes')
            self.assertEqual((source/save_relative('broken-world')/'hero.save').read_text(),'broken-world')


if __name__ == '__main__': unittest.main()
