"""Synthetic archive/parser/protocol regressions; no game assets needed."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'host/linux_launcher'))
from lab_content import Tank, parse_gas, flatten
from lab_commands import Bridge, command
from lab_session import SAVE_RELATIVE, copy_saves, launch_environment


def archive(path, content, compressed=False, extra=b''):
    """One-file Tank, including its real variable-length metadata alignment."""
    data = bytearray(512)
    data[:8] = b'DSg2Tank'
    def u32(at, value): struct.pack_into('<I', data, at, value)
    u32(12, 64); u32(16, 128); u32(24, 512)
    u32(64, 1); u32(68, 8); u32(72, 0)
    struct.pack_into('<H', data, 88, 0)  # root directory name
    u32(128, 1); u32(132, 8)
    at = 136; name = b'example.gas'
    u32(at, 8); u32(at+4, len(content)+len(extra)); u32(at+8, 0)
    struct.pack_into('<H', data, at+24, int(compressed))
    struct.pack_into('<H', data, at+28, len(name)); data[at+30:at+30+len(name)] = name
    if compressed:
        raw = zlib.compress(content)
        table = (at+30+len(name)+4) & ~3
        u32(table, len(raw)+len(extra)); u32(table+4, 65536)
        for offset, value in zip((8,12,16,20),(len(content)+len(extra),len(raw),len(extra),0)):
            u32(table+offset, value)
        data.extend(raw+extra)
    else:
        data.extend(content)
    path.write_bytes(data)


class ContentTests(unittest.TestCase):
    def test_nested_strings_comments_and_typed_values(self):
        source = '''// [not:a,node]
        [t:template,n:fire] { specializes = base_magic;
           [magic] { f power = 2; [effects] { name = "flame; {} // not comment"; } }
           [common] { screen_name = "A \\\"quoted\\\" spell"; }
        } /* ignored [block] {} */'''
        nodes = parse_gas(source)
        self.assertEqual(len(nodes), 1)
        values = flatten(nodes[0])
        self.assertEqual(values['magic.effects.name'], 'flame; {} // not comment')
        self.assertEqual(values['magic.power'], '2')
        self.assertEqual(values['common.screen_name'], 'A "quoted" spell')

    def test_shipped_stray_braces_do_not_discard_following_templates(self):
        issues = []
        nodes = parse_gas('} [t:template,n:first] { x=1; } } [t:template,n:second] { x=2; }', issues)
        self.assertEqual([n['name'] for n in nodes], ['first', 'second'])
        self.assertTrue(issues)

    def test_plain_and_compressed_chunk_with_raw_tail(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'test.ds2res'
            for compressed, extra in ((False,b''),(True,b''),(True,b'RAW-END')):
                content = b'GAS contents\n'*200
                archive(path, content, compressed, extra)
                with Tank(path) as tank:
                    self.assertEqual(tank.get('example.gas'), content+extra)

    def test_truncated_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'test.ds2res'; path.write_bytes(b'DSg2Tank')
            with self.assertRaises(ValueError): Tank(path)


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bridge = Bridge(self.temp.name)

    def test_no_overwrite_or_duplicate_when_response_is_late(self):
        b = self.bridge; b.send('prepare')
        request = (b.directory/'request').read_text()
        with self.assertRaises(RuntimeError): b.send('cleanup')
        self.assertEqual(request, (b.directory/'request').read_text())
        (b.directory/'response').write_text(f'{b.sequence-1}\tok\told reply\n')
        self.assertIsNone(b.poll())
        (b.directory/'response').write_text(f'{b.sequence}\tok\tprepared\n')
        self.assertEqual(b.poll()['message'], 'prepared')
        self.assertIsNone(b.poll())

    def test_timeout_is_unconfirmed_and_not_retried(self):
        b = self.bridge; b.send('spawn', {'id':'test_enemy'})
        with patch('lab_commands.time.time', return_value=b.pending['time']+121):
            self.assertEqual(b.poll()['status'], 'unconfirmed')
        self.assertFalse((b.directory/'request').exists())
        self.assertIsNone(b.pending)

    def test_engine_error_is_not_a_pass(self):
        b = self.bridge; b.send('cast', {'id':'test_spell'})
        (b.directory/'response').write_text(f'{b.sequence}\tok\tERROR: bad target\n')
        self.assertEqual(b.poll()['status'], 'error')
        records = [json.loads(line) for line in (b.directory/'history.jsonl').read_text().splitlines()]
        self.assertEqual([x['status'] for x in records], ['submitted', 'error'])

    def test_all_mutations_guard_multiplayer_and_reject_script_injection(self):
        for action in ('prepare','invincible','refill','wound','corpse','cleanup'):
            self.assertIn('WorldState.CurrentState == WS_SP_INGAME', command(action))
        with self.assertRaises(ValueError): command('spawn', {'id':'bad");danger();'})
        with self.assertRaises(ValueError): command('equip', {'id':'helmet','slot':'bad();'})


class SessionTests(unittest.TestCase):
    def test_save_copy_refresh_never_changes_originals_or_copies_key(self):
        with tempfile.TemporaryDirectory() as temp:
            original, lab = Path(temp)/'normal', Path(temp)/'lab'
            save = original/SAVE_RELATIVE/'Hero/Hero.ds2party'; save.parent.mkdir(parents=True)
            save.write_bytes(b'original save')
            (original/'cdkey-hash.txt').write_text('synthetic key marker')
            copy_saves(original, lab)
            copied = lab/SAVE_RELATIVE/'Hero/Hero.ds2party'
            copied.write_bytes(b'lab changes')
            self.assertEqual(save.read_bytes(), b'original save')
            copy_saves(original, lab)
            self.assertEqual(copied.read_bytes(), b'original save')
            self.assertEqual(next((lab/'save-backups').glob('*/Hero/Hero.ds2party')).read_bytes(), b'lab changes')
            self.assertFalse((lab/'cdkey-hash.txt').exists())

    def test_lab_uses_chosen_graphics_but_removes_input_and_key_overrides(self):
        with patch.dict('os.environ', {'DS_TEST':'1','DS_SCRIPT':'inherited','DS_EXIT_AFTER':'1','DS2_CDKEY':'synthetic'}):
            env = launch_environment({'resolution':'3440x1440','frameRate':'244','windowMode':'fullscreen'}, '/tmp/lab')
        self.assertEqual(env['DS_RESOLUTION'], '3440x1440')
        self.assertEqual(env['DSR_FPSCAP'], '244')
        self.assertEqual(env['DS_WINDOW_MODE'], 'windowed')
        self.assertEqual(env['DS_TEST_LAB'], '1')
        for name in ('DS_TEST','DS_SCRIPT','DS_EXIT_AFTER','DS2_CDKEY'): self.assertNotIn(name, env)


if __name__ == '__main__': unittest.main()
