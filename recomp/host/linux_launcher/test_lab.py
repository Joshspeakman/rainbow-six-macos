"""Local content/visual testing panel, intentionally separate from normal saves."""
from concurrent.futures import ThreadPoolExecutor
import fcntl
import json
from pathlib import Path
import subprocess
import time

from PySide6.QtCore import Qt, QTimer, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
    QComboBox, QLineEdit, QListWidget, QListWidgetItem, QTextEdit, QSpinBox,
    QCheckBox, QMessageBox, QFileDialog, QSplitter, QInputDialog)

from lab_commands import Bridge
from lab_content import build_catalog
from lab_session import copy_saves, launch_environment
from profiles import save_relative
from settings import read_settings


class TestLab(QWidget):
    def __init__(self, binary, exe, game, data, parent=None, settings=None, edition='gog'):
        super().__init__(parent, Qt.WindowType.Window)
        title = 'Dungeon Siege II: Broken World' if edition == 'broken-world' else 'Dungeon Siege II'
        self.setWindowTitle(title + ' — Test Lab')
        self.resize(1040, 800)
        self.setStyleSheet('QWidget { background: #241d19; color: #eee4d3; } '
                          'QLineEdit, QTextEdit, QListWidget, QComboBox, QSpinBox { background: #151311; border: 1px solid #715d44; padding: 5px; } '
                          'QPushButton { background: #453426; border: 1px solid #97794b; padding: 7px; } '
                          'QPushButton:hover { background: #63492f; } QPushButton:disabled { color: #84776a; border-color: #4a4035; } '
                          'QListWidget::item:selected { background: #735333; }')
        self.binary, self.exe, self.game = map(str, (binary, exe, game))
        self.normal_data = Path(data)
        self.settings = dict(settings if settings is not None else read_settings(data))
        self.edition = edition
        self.save_relative = save_relative(edition)
        self.data = self.normal_data / ('test-lab-broken-world' if edition == 'broken-world' else 'test-lab')
        self.data.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.lock = (self.data / 'panel.lock').open('w')
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            raise RuntimeError('The Test Lab panel is already open.')
        self.bridge = Bridge(self.data)
        self.catalog = None
        self.ready = False
        self.last = None
        self.next_status = 0
        self.sweep = []
        self.next_sweep = 0
        self.executor = ThreadPoolExecutor(max_workers=1)
        self.future = self.executor.submit(build_catalog, self.game)
        self.controls = []
        layout = QVBoxLayout(self)
        intro = QLabel('Load a single-player save, then prepare your hero. F12 releases the game mouse.\n'
                       'Test saves are separate. Starting the lab copies your single-player saves the first time.')
        intro.setWordWrap(True); layout.addWidget(intro)
        row = QHBoxLayout(); layout.addLayout(row)
        self.start_button = self.button(row, 'Start test game', self.start_game)
        self.copy_button = self.button(row, 'Refresh save copies', self.refresh_saves)
        self.button(row, 'Open test folder', lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.data))))
        self.button(row, 'Export report…', self.export_report)
        self.status = QLabel('Reading installed content…'); layout.addWidget(self.status)
        row = QHBoxLayout(); layout.addLayout(row)
        self.action_button(row, 'Prepare hero / unlock skills', lambda: self.submit('prepare', skills=self.catalog['skills']))
        self.action_button(row, 'Invincible', lambda: self.submit('invincible', enabled=True))
        self.action_button(row, 'Vulnerable', lambda: self.submit('invincible', enabled=False))
        self.action_button(row, 'Refill life / mana / power', lambda: self.submit('refill'))
        self.action_button(row, 'Wound hero', lambda: self.submit('wound'))
        self.action_button(row, 'Clean up test objects', lambda: self.submit('cleanup'))
        row = QHBoxLayout(); layout.addLayout(row)
        self.group = QComboBox()
        for key, label in [('locations','Locations'), ('gear','Gear'), ('enemies','Enemies'),
                           ('spells','Spells / power templates'), ('powers','Active powers'),
                           ('objects','Other objects'), ('effects','Effect definitions'), ('shaders','Shader definitions')]:
            self.group.addItem(label, key)
        row.addWidget(self.group)
        self.search = QLineEdit(); self.search.setPlaceholderText('Search name, template, damage type, target or effect…')
        row.addWidget(self.search, 1)
        self.bases = QCheckBox('Include bases / developer content'); row.addWidget(self.bases)
        splitter = QSplitter(); layout.addWidget(splitter, 1)
        self.items = QListWidget(); splitter.addWidget(self.items)
        self.details = QTextEdit(); self.details.setReadOnly(True); splitter.addWidget(self.details)
        splitter.setSizes([500, 480])
        self.count_label = QLabel(); layout.addWidget(self.count_label)
        row = QHBoxLayout(); layout.addLayout(row)
        self.primary = self.action_button(row, 'Travel', self.run_selected)
        self.equip = self.action_button(row, 'Give and equip', lambda: self.run_selected('equip'))
        self.count_caption = QLabel('Count:'); row.addWidget(self.count_caption)
        self.count = QSpinBox(); self.count.setRange(1, 16); row.addWidget(self.count)
        self.immortal = QCheckBox('Invincible spawn'); self.immortal.setChecked(True); row.addWidget(self.immortal)
        self.friendly = QCheckBox('Friendly spawn'); row.addWidget(self.friendly)
        self.target_caption = QLabel('Cast at:'); row.addWidget(self.target_caption)
        self.target = QComboBox()
        for label, key in [('Automatic','auto'), ('Self','self'), ('Test target','target'), ('Ground near target','ground')]:
            self.target.addItem(label, key)
        row.addWidget(self.target)
        self.action_button(row, 'Target party member…', self.party_target)
        self.action_button(row, 'Kill test target', lambda: self.submit('corpse'))
        row = QHBoxLayout(); layout.addLayout(row)
        self.cast_path_label = QLabel('Spell path:'); row.addWidget(self.cast_path_label)
        self.cast_path = QComboBox()
        self.cast_path.addItem('Trigger effect', True); self.cast_path.addItem('Animated cast job', False)
        row.addWidget(self.cast_path)
        self.sweep_button = self.action_button(row, 'Cast filtered spells in sequence', self.start_sweep)
        self.button(row, 'Stop sequence', self.stop_sweep)
        row.addWidget(QLabel('Seconds between casts:'))
        self.interval = QSpinBox(); self.interval.setRange(3, 60); self.interval.setValue(6); row.addWidget(self.interval)
        row.addStretch()
        self.log = QTextEdit(); self.log.setReadOnly(True); self.log.setMaximumHeight(145); layout.addWidget(self.log)
        row = QHBoxLayout(); layout.addLayout(row)
        self.note = QLineEdit(); self.note.setPlaceholderText('Visual result / artifact / missing effect…'); row.addWidget(self.note, 1)
        self.button(row, 'Mark last test: looks correct', lambda: self.mark('visual-pass'))
        self.button(row, 'Mark last test: problem', lambda: self.mark('visual-fail'))
        hint = QLabel('A queued action is not a rendering pass. Check it in-game before marking the result.\n'
                      'Linux currently uses fixed-function graphics; listed .fx definitions are an inventory, not proof of shader execution.')
        hint.setWordWrap(True); layout.addWidget(hint)
        self.search.textChanged.connect(self.populate)
        self.group.currentIndexChanged.connect(self.populate)
        self.bases.toggled.connect(self.populate)
        self.items.currentItemChanged.connect(self.describe)
        self.timer = QTimer(self); self.timer.timeout.connect(self.tick); self.timer.start(200)
        self.populate()

    def button(self, row, text, callback):
        button = QPushButton(text); button.clicked.connect(callback); row.addWidget(button); return button

    def action_button(self, row, text, callback):
        button = self.button(row, text, callback); self.controls.append(button); return button

    def running(self):
        try:
            pid = int((self.data / 'game.pid').read_text())
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            return str(self.data).encode() in argv and self.binary.encode() in argv
        except (OSError, ValueError):
            return False

    def start_game(self):
        if self.running(): return
        try:
            if not (self.data / self.save_relative).exists(): copy_saves(self.normal_data, self.data, self.edition)
            for name in ('request', 'response'):
                (self.bridge.directory / name).unlink(missing_ok=True)
            self.bridge.pending = None
            env = launch_environment(self.settings, self.data)
            self.bridge.record({'action':'session', 'time':time.time(), 'game':self.game,
                                'backend':'linux-fixed-function', 'settings':self.settings,
                                'frame_trace':env['DS_FRAME_TRACE']})
            args = [self.binary, '--exe', self.exe, '--game', self.game, '--data', str(self.data)]
            with (self.data / 'game.log').open('w') as log:
                process = subprocess.Popen(args, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            (self.data / 'game.pid').write_text(str(process.pid))
            self.log.append('Started isolated test game. Load a save through Single Player.')
        except (OSError, ValueError) as e:
            QMessageBox.warning(self, 'Start test game', str(e))

    def refresh_saves(self):
        if self.running(): return
        try:
            copy_saves(self.normal_data, self.data, self.edition)
            self.log.append('Fresh single-player copies created. Previous test saves moved to save-backups.')
        except OSError as e:
            QMessageBox.warning(self, 'Copy saves', str(e))

    def populate(self, *unused):
        self.items.clear()
        group = self.group.currentData()
        actions = {'locations':'Travel', 'gear':'Give item', 'enemies':'Spawn enemy',
                   'spells':'Cast spell / template', 'powers':'Trigger active power', 'objects':'Spawn object'}
        self.primary.setText(actions.get(group, 'Definitions only'))
        self.primary.setVisible(group in actions)
        self.equip.setVisible(group == 'gear')
        self.sweep_button.setVisible(group == 'spells')
        self.cast_path_label.setVisible(group == 'spells'); self.cast_path.setVisible(group == 'spells')
        for widget in (self.count_caption, self.count, self.immortal, self.friendly):
            widget.setVisible(group in ('enemies', 'objects'))
        for widget in (self.target_caption, self.target):
            widget.setVisible(group in ('spells', 'powers'))
        if not self.catalog: return
        tokens = self.search.text().lower().split()
        entries = sorted(self.catalog[group], key=lambda x: (x['label'].lower(), x['id']))
        for entry in entries:
            developer = any(part in entry['source'] for part in ('/dev/', '/test/'))
            if not self.bases.isChecked() and (entry.get('base') or developer): continue
            searchable = json.dumps(entry, ensure_ascii=False).lower()
            if not all(word in searchable for word in tokens): continue
            text = entry['label'] + (f"  [{entry['id']}]" if entry['label'] != entry['id'] else '')
            item = QListWidgetItem(text); item.setData(Qt.ItemDataRole.UserRole, entry); self.items.addItem(item)
        self.count_label.setText(f'{self.items.count()} shown / {len(entries)} definitions in selected game files')
        if self.items.count(): self.items.setCurrentRow(0)

    def selected(self):
        item = self.items.currentItem()
        return item.data(Qt.ItemDataRole.UserRole) if item else None

    def describe(self, *unused):
        entry = self.selected()
        self.details.setPlainText('\n\n'.join(f'{k}: {", ".join(v) if isinstance(v, list) else v}'
                                             for k, v in (entry or {}).items() if v))

    def submit(self, action, entry=None, **options):
        if not self.running() or self.bridge.pending: return
        try:
            self.bridge.send(action, entry, **options)
        except (OSError, ValueError, RuntimeError) as e:
            self.log.append(str(e)); self.stop_sweep()

    def run_selected(self, override=None):
        entry = self.selected()
        if not entry: return
        action = override if isinstance(override, str) else {'locations':'travel','gear':'give','enemies':'spawn',
                  'spells':'cast','powers':'power','objects':'spawn'}.get(self.group.currentData())
        if action: self.submit(action, entry, count=self.count.value(), immortal=self.immortal.isChecked(),
                               friendly=self.friendly.isChecked(), target=self.target.currentData(), direct=self.cast_path.currentData())

    def party_target(self):
        member, ok = QInputDialog.getInt(self, 'Party target',
            'Party member number (1 is usually the hero).\nUse a companion for resurrection tests.', 2, 1, 6)
        if ok:
            self.target.setCurrentIndex(self.target.findData('target'))
            self.submit('party_target', member=member)

    def start_sweep(self):
        if self.group.currentData() != 'spells': return
        self.sweep = [self.items.item(i).data(Qt.ItemDataRole.UserRole) for i in range(self.items.count())]
        # Include-bases is useful for investigation but must not flood a sweep
        # with abstract templates that the retail engine cannot instantiate.
        self.sweep = [e for e in self.sweep if not e.get('base')]
        self.next_sweep = time.monotonic()
        self.log.append(f'Sequence: {len(self.sweep)} spell templates. Pauses on a rejected request; Stop leaves current effects running.')

    def stop_sweep(self):
        if self.sweep: self.log.append(f'Sequence stopped with {len(self.sweep)} casts remaining.')
        self.sweep = []

    def tick(self):
        if self.future and self.future.done():
            try:
                self.catalog = self.future.result()
                (self.data / 'catalog.json').write_text(json.dumps(self.catalog, indent=2))
                self.log.append('Content catalog ready. ' + '; '.join(self.catalog['errors'][:5]))
                self.populate()
            except Exception as e:
                self.log.append('Content scan failed: ' + str(e))
            self.future = None
        alive = self.running()
        self.start_button.setEnabled(not alive)
        self.copy_button.setEnabled(not alive)
        try:
            result = self.bridge.poll()
            if result:
                if result['action'] == 'status':
                    self.ready = result['status'] == 'ok' and result['message'].startswith('Ready')
                    self.status.setText(result['message'])
                else:
                    self.last = result
                    self.log.append(f"{result['action']} {result['content']}: {result['message']}")
                    self.next_sweep = time.monotonic() + self.interval.value()
                    if result['status'] != 'ok': self.stop_sweep()
            if not alive:
                self.ready = False
                self.status.setText('Test game stopped. Start it, then load a single-player save.')
                self.stop_sweep()
            now = time.monotonic()
            if alive and not self.bridge.pending:
                if now >= self.next_status:
                    self.bridge.send('status'); self.next_status = now + 3
                elif self.sweep and self.ready and now >= self.next_sweep:
                    self.submit('cast', self.sweep.pop(0), target=self.target.currentData(), direct=self.cast_path.currentData())
            for control in self.controls:
                control.setEnabled(bool(alive and self.ready and self.catalog and not self.bridge.pending and not self.sweep))
        except (OSError, ValueError) as e:
            self.status.setText(str(e)); self.stop_sweep()

    def mark(self, status):
        if not self.last:
            self.log.append('Run a test first.'); return
        record = dict(self.last, status=status, note=self.note.text(), reviewed=time.time())
        try:
            self.bridge.record(record); self.log.append(f"{status}: {record['content']} {record['note']}")
            self.note.clear()
        except OSError as e:
            self.log.append(str(e))

    def export_report(self):
        name, _ = QFileDialog.getSaveFileName(self, 'Export test report', str(self.data / 'test-report.json'), 'JSON (*.json)')
        if not name: return
        try:
            history = self.bridge.directory / 'history.jsonl'
            records = [json.loads(line) for line in history.read_text().splitlines()] if history.exists() else []
            report = {'backend':'linux-fixed-function', 'shader_execution_verified':False,
                      'catalog':self.catalog, 'history':records,
                      'note':'Submitted/ok records describe command handling. Only visual-pass/visual-fail are manual rendering reviews.'}
            Path(name).write_text(json.dumps(report, indent=2)); self.log.append('Exported ' + name)
        except (OSError, ValueError) as e:
            QMessageBox.warning(self, 'Export', str(e))

    def closeEvent(self, event):
        self.timer.stop(); self.stop_sweep()
        self.executor.shutdown(wait=False)
        self.lock.close()
        super().closeEvent(event)
