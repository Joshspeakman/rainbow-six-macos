#!/usr/bin/env python3
"""Fallout: New Vegas launch window (resolution, display mode, frame rate, antialiasing), as the original game's
launcher does before the game starts; artwork read from the player's own archives. FNV_NO_LAUNCHER=1 starts the game
straight away with the saved choices."""
import argparse
import io
import os
from pathlib import Path
import sys
from fnv_settings import (ANTI_ALIASING, WINDOW_MODES, environment, frame_rate_choices, read_bsa_files,
                          read_settings, resolution_choice, resolution_choices, save_settings)

TITLE = 'textures\\interface\\main\\main_title.dds'
DESK = 'textures\\interface\\loading\\loading_desktop01.dds'

def main():
    parser = argparse.ArgumentParser()
    for name in ('exe', 'game', 'data', 'binary'): parser.add_argument('--' + name, required=True)
    args, rest = parser.parse_known_args()
    command = [args.binary, '--exe', args.exe, '--game', args.game, '--data', args.data] + rest
    saved = read_settings(args.data)
    shot = os.environ.get('DS_LAUNCHER_SHOT')
    if os.environ.get('DS_TEST') and not shot: os.execve(command[0], command, os.environ)

    from PySide6.QtCore import Qt, QRectF
    from PySide6.QtGui import QColor, QFont, QIcon, QImage, QPixmap, QPainter
    from PySide6.QtWidgets import QApplication, QWidget, QLabel, QComboBox, QPushButton, QInputDialog, QMessageBox
    app = QApplication(sys.argv[:1])
    app.setApplicationName('fallout-nv-linux'); app.setApplicationDisplayName('Fallout: New Vegas')
    app.setDesktopFileName('fallout-nv-linux')
    icon = Path(args.game) / 'FalloutNV.ico'
    if icon.exists(): app.setWindowIcon(QIcon(str(icon)))
    screen = app.primaryScreen(); geo = screen.geometry()
    width, height = round(geo.width() * screen.devicePixelRatio()), round(geo.height() * screen.devicePixelRatio())
    refresh = screen.refreshRate()
    if os.environ.get('FNV_NO_LAUNCHER') and not shot:
        os.execve(command[0], command, environment(saved, os.environ, refresh))

    art = {}
    try:
        from PIL import Image
        files = read_bsa_files(Path(args.game) / 'Data' / 'Fallout - Textures2.bsa', {TITLE, DESK})
        for name, raw in files.items():
            with Image.open(io.BytesIO(raw)) as dds:
                rgba = dds.convert('RGBA')
                if name == TITLE and rgba.getbbox(): rgba = rgba.crop(rgba.getbbox())
                art[name] = QPixmap.fromImage(QImage(rgba.tobytes(), rgba.width, rgba.height, rgba.width * 4,
                                                     QImage.Format.Format_RGBA8888).copy())
    except (OSError, ValueError, ImportError): pass

    W, H = 820, 640
    class Launcher(QWidget):
        def __init__(self):
            super().__init__()
            self.setWindowTitle('Fallout: New Vegas — Linux')
            self.scale = min(1., (screen.availableGeometry().height() - 80) / H)
            self.setFixedSize(round(W * self.scale), round(H * self.scale))
            self.accepted = False; self.controls = {}; self.launch_env = None
            self.setStyleSheet('QLabel { color: #ffb642; background: transparent; } '
                               'QComboBox, QPushButton { color: #ffcf7a; background: #1a1206; border: 1px solid #b07a22; padding: 6px; } '
                               'QComboBox QAbstractItemView { color: #ffcf7a; background: #1a1206; selection-background-color: #6b4810; } '
                               'QPushButton:hover, QComboBox:hover { background: #3d2a0c; } QPushButton:default { border: 2px solid #ffb642; }')
            self.setFont(QFont('DejaVu Sans Condensed', 11))
            self.row('resolution', 'RESOLUTION', resolution_choices(width, height, saved.get('resolution')), f'{width}x{height}', 0)
            self.row('windowMode', 'DISPLAY', WINDOW_MODES, 'fullscreen', 1)
            self.row('frameRate', 'FRAME RATE', frame_rate_choices(refresh, saved.get('frameRate')), '60', 2)
            self.row('antiAliasing', 'ANTIALIASING', ANTI_ALIASING, 'msaa4', 3)
            hint = self.label('Native Linux build · F11 window / full screen · F12 releases the mouse', 60, 500, 700, 24)
            hint.setAlignment(Qt.AlignmentFlag.AlignCenter); hint.setFont(QFont('DejaVu Sans Condensed', 9))
            quit = QPushButton('QUIT', self); self.place(quit, 60, 560, 200, 50); quit.clicked.connect(self.close)
            play = QPushButton('PLAY', self); self.place(play, 560, 560, 200, 50); play.clicked.connect(self.start)
            play.setDefault(True); play.setFocus()
            self.move(screen.availableGeometry().center() - self.rect().center())
        def place(self, widget, x, y, w, h): widget.setGeometry(*(round(v * self.scale) for v in (x, y, w, h)))
        def label(self, text, x, y, w, h):
            label = QLabel(text, self); self.place(label, x, y, w, h); return label
        def row(self, key, title, choices, default, index):
            y = 236 + index * 60
            self.label(title, 90, y, 260, 42)
            combo = QComboBox(self); self.place(combo, 370, y, 360, 42)
            for choice in choices:
                combo.addItem(choice[1], choice[0])
                if len(choice) > 2: combo.setItemData(combo.count() - 1, choice[2], Qt.ItemDataRole.ToolTipRole)
            combo.setCurrentIndex(max(0, combo.findData(saved.get(key, default))))
            if combo.currentData() != saved.get(key, default): combo.setCurrentIndex(max(0, combo.findData(default)))
            combo.setAccessibleName(title); self.controls[key] = combo
            combo.activated.connect(lambda index, k=key: self.custom(k))
        def custom(self, key):
            combo = self.controls[key]
            if combo.currentData() != 'custom': return
            if key == 'resolution':
                text, ok = QInputDialog.getText(self, 'Custom resolution', 'Width x height (pixels), any aspect ratio:', text=f'{width}x{height}')
                if ok:
                    try: value, label, _ = resolution_choice(text.strip().lower().replace('×', 'x').replace(' ', ''))
                    except ValueError as e: QMessageBox.warning(self, 'Resolution', str(e)); ok = False
            else:
                fps, ok = QInputDialog.getInt(self, 'Custom frame rate', 'Maximum FPS:', round(refresh) or 60, 1, 1000)
                value, label = str(fps), f'{fps} FPS'
            if ok:
                found = combo.findData(value)
                if found < 0: combo.insertItem(combo.count() - 1, label, value); found = combo.findData(value)
                combo.setCurrentIndex(found)
            else: combo.setCurrentIndex(0)
        def start(self):
            selections = {key: widget.currentData() for key, widget in self.controls.items()}
            try: self.launch_env = environment(selections, os.environ, refresh); save_settings(args.data, selections)
            except (OSError, ValueError) as e: QMessageBox.warning(self, 'Settings', str(e)); return
            self.accepted = True; self.close()
        def keyPressEvent(self, event):
            if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter): self.start()
            elif event.key() == Qt.Key.Key_Escape: self.close()
            else: super().keyPressEvent(event)
        def paintEvent(self, event):
            p = QPainter(self); p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform); p.scale(self.scale, self.scale)
            desk = art.get(DESK)
            if desk:
                src = QRectF(desk.rect()); aspect = W / H
                if src.width() / src.height() > aspect: src.setWidth(src.height() * aspect)
                else: src.setHeight(src.width() / aspect)
                p.drawPixmap(QRectF(0, 0, W, H), desk, src)
            else: p.fillRect(QRectF(0, 0, W, H), QColor('#2a1c0c'))
            p.fillRect(QRectF(0, 0, W, H), QColor(0, 0, 0, 110))
            title = art.get(TITLE)
            if title:
                size = title.size().scaled(560, 150, Qt.AspectRatioMode.KeepAspectRatio)
                p.drawPixmap(QRectF((W - size.width()) / 2, 24, size.width(), size.height()), title, QRectF(title.rect()))
            else:
                p.setFont(QFont('DejaVu Sans Condensed', 32, QFont.Weight.Bold)); p.setPen(QColor('#ffb642'))
                p.drawText(QRectF(0, 40, W, 120), Qt.AlignmentFlag.AlignCenter, 'FALLOUT: NEW VEGAS')
            p.fillRect(QRectF(60, 216, 700, 260), QColor(0, 0, 0, 200))
            p.setPen(QColor('#b07a22')); p.drawRect(QRectF(60, 216, 700, 260))
    window = Launcher()
    if shot: window.ensurePolished(); window.grab().save(shot); return 0
    window.show(); app.exec()
    if window.accepted: os.execve(command[0], command, window.launch_env)
    return 0

if __name__ == '__main__': sys.exit(main())
