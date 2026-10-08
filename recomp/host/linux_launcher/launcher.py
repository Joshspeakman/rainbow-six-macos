#!/usr/bin/env python3
"""DS2's Mac-style launch window, with artwork read from the player's own archive."""
import argparse
import io
import json
import os
from pathlib import Path
import sys
from settings import (DISTANCES, read_settings, save_settings, environment, resolution_choices,
                      resolution_choice, frame_rate_choices, save_cdkey)
from tank import read_art
from profiles import select_profile, command as profile_command


def main():
    parser = argparse.ArgumentParser()
    for name in ('exe', 'game', 'data', 'binary'): parser.add_argument('--' + name, required=True)
    parser.add_argument('--profiles', type=Path)
    args = parser.parse_args()
    profiles = json.loads(args.profiles.read_text()) if args.profiles else {'Game folder': args.game}
    saved = read_settings(args.data)
    def selected_profile(label):
        return select_profile(profiles, label, binary=args.binary, exe=args.exe, game=args.game, data=args.data)
    active = selected_profile(saved.get('gameProfile'))
    command = profile_command(active)
    shot = os.environ.get('DS_LAUNCHER_SHOT')
    if os.environ.get('DS_TEST') and not shot: os.execve(command[0], command, os.environ)
    if os.environ.get('DS_NO_LAUNCHER') and not shot: os.execve(command[0], command, environment(saved))

    from PIL import Image
    from PySide6.QtCore import Qt, QRectF, QUrl
    from PySide6.QtGui import QColor, QFont, QIcon, QImage, QPixmap, QPainter, QDesktopServices
    from PySide6.QtWidgets import (QApplication, QWidget, QLabel, QComboBox, QPushButton,
                                  QInputDialog, QLineEdit, QMessageBox)
    app = QApplication(sys.argv[:1])
    app.setApplicationName('dungeon-siege-2-linux')
    app.setApplicationDisplayName('Dungeon Siege II')
    app.setDesktopFileName('dungeon-siege-2-linux')
    app.setWindowIcon(QIcon(str(Path(args.binary).resolve().parent / 'dungeon-siege-2.ico')))
    screen = app.primaryScreen()
    geo = screen.geometry()
    width, height = round(geo.width()*screen.devicePixelRatio()), round(geo.height()*screen.devicePixelRatio())
    images = {}
    archives = sorted((Path(active['game']) / 'Resources').glob('*'), key=lambda p: p.name.lower())
    archive = next((p for p in archives if p.name.lower() == 'objects.ds2res'), None)
    if archive:
        for name, raw in read_art(archive).items():
            try:
                with Image.open(io.BytesIO(raw)) as dds:
                    rgba = dds.convert('RGBA')
                    if not name.startswith('back') and rgba.getbbox(): rgba = rgba.crop(rgba.getbbox())
                    image = QImage(rgba.tobytes(), rgba.width, rgba.height, rgba.width*4, QImage.Format.Format_RGBA8888).copy()
                    images[name] = QPixmap.fromImage(image)
            except (OSError, ValueError): pass
    background = QPixmap(1024,512); background.fill(QColor('#210502'))
    painter = QPainter(background)
    for i in range(8):
        tile = images.get(f'back{i+1:02}.dds')
        if tile: painter.drawPixmap(i%4*256,i//4*256,256,256,tile)
    painter.end()

    class Launcher(QWidget):
        def __init__(self):
            super().__init__()
            self.setWindowTitle('Dungeon Siege II — Linux')
            self.scale = min(1., (screen.availableGeometry().height()-80)/720)
            self.setFixedSize(round(780*self.scale),round(720*self.scale))
            self.accepted = False
            self.controls = {}
            self.setStyleSheet('QLabel { color: #dbc58c; background: transparent; } '
                              'QComboBox, QPushButton { color: #f0e8d6; background: #241812; border: 1px solid #9a744a; padding: 6px; } '
                              'QComboBox QAbstractItemView { color: #f0e8d6; background: #241812; selection-background-color: #714423; } '
                              'QPushButton:hover { background: #59391f; }')
            self.setFont(QFont('DejaVu Serif',11))
            self.row('resolution','RESOLUTION',resolution_choices(width,height,saved.get('resolution')),f'{width}x{height}',0)
            self.row('viewDistance','VIEW DISTANCE',DISTANCES,'100',1)
            self.row('frameRate','FRAME RATE',frame_rate_choices(screen.refreshRate(),saved.get('frameRate')),'auto',2)
            self.row('windowMode','DISPLAY',[('fullscreen','Fullscreen'),('windowed','Windowed')],'fullscreen',3)
            self.label('ONLINE',70,472,280,40)
            key = QPushButton('Change CD key' if (Path(args.data)/'cdkey-hash.txt').exists() else 'Enter your CD key',self)
            self.place(key,376,472,334,40); key.clicked.connect(self.key_dialog)
            if len(profiles)>1:
                self.label('GAME FILES',70,530,288,38)
                combo=QComboBox(self); self.place(combo,376,530,334,38)
                for label in profiles: combo.addItem(label,label)
                combo.setCurrentIndex(max(0,combo.findData(saved.get('gameProfile','GOG'))))
                self.controls['gameProfile']=combo
            hint = self.label('Linux preview · Fixed-function graphics',54,574,672,24)
            hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
            hint = self.label('F12 releases the mouse · F11 toggles fullscreen',54,595,672,20)
            hint.setAlignment(Qt.AlignmentFlag.AlignCenter); hint.setFont(QFont('DejaVu Serif',9))
            repo = QPushButton('PROJECT',self); self.place(repo,54,642,140,50)
            repo.clicked.connect(lambda: QDesktopServices.openUrl(QUrl('https://github.com/Joshspeakman/dungeon-siege-2-linux')))
            lab = QPushButton('TEST LAB',self); self.place(lab,208,642,160,50); lab.clicked.connect(self.open_lab)
            self.lab = None
            quit = QPushButton('QUIT',self); self.place(quit,382,642,140,50); quit.clicked.connect(self.close)
            play = QPushButton('PLAY',self); self.place(play,536,642,190,50); play.clicked.connect(self.start)
            play.setDefault(True)
            self.move(screen.availableGeometry().center()-self.rect().center())
        def place(self,widget,x,y,w,h): widget.setGeometry(*(round(v*self.scale) for v in (x,y,w,h)))
        def label(self,text,x,y,w,h):
            label=QLabel(text,self); self.place(label,x,y,w,h); return label
        def row(self,key,title,choices,default,index):
            y=216+index*64
            self.label(title,70,y,288,42)
            combo=QComboBox(self); self.place(combo,376,y,334,42)
            for choice in choices:
                combo.addItem(choice[1],choice[0])
                if len(choice)>2: combo.setItemData(combo.count()-1,choice[2],Qt.ItemDataRole.ToolTipRole)
            selected=combo.findData(saved.get(key,default)); combo.setCurrentIndex(max(0,selected))
            combo.setAccessibleName(title); self.controls[key]=combo
            combo.activated.connect(lambda index,k=key: self.custom(k))
        def custom(self,key):
            combo=self.controls[key]
            if combo.currentData()!='custom': return
            if key=='resolution':
                text,ok=QInputDialog.getText(self,'Custom resolution','Width x height (pixels):',text=f'{width}x{height}')
                if ok:
                    try: value,label,_=resolution_choice(text.strip().lower().replace('×','x').replace(' ',''))
                    except ValueError as e: QMessageBox.warning(self,'Resolution',str(e)); ok=False
            else:
                fps,ok=QInputDialog.getInt(self,'Custom frame rate','Maximum FPS:',round(screen.refreshRate()) or 60,1,10000)
                value,label=str(fps),f'{fps} FPS'
            if ok:
                found=combo.findData(value)
                if found<0: combo.insertItem(combo.count()-1,label,value); found=combo.findData(value)
                combo.setCurrentIndex(found)
            else: combo.setCurrentIndex(0)
        def key_dialog(self):
            key,ok=QInputDialog.getText(self,'Your Dungeon Siege II CD key',
                'Enter your own 25-character retail key for online play.\nOnly its hash is saved. Each online player needs a distinct key.',QLineEdit.EchoMode.Password)
            if ok:
                try: save_cdkey(args.data,key)
                except (OSError,ValueError) as e: QMessageBox.warning(self,'CD key',str(e)); return
                QMessageBox.information(self,'CD key','The key hash has been saved.')
        def open_lab(self):
            if self.lab and self.lab.isVisible():
                self.lab.raise_(); self.lab.activateWindow(); return
            try:
                from test_lab import TestLab
                selected = self.controls.get('gameProfile')
                profile = selected_profile(selected.currentData() if selected else None)
                settings = {key:widget.currentData() for key,widget in self.controls.items()}
                self.lab = TestLab(profile['binary'], profile['exe'], profile['game'], profile['data'], self, settings=settings, edition=profile['edition'])
                self.lab.show()
            except (OSError, RuntimeError, ValueError) as e:
                QMessageBox.warning(self,'Test Lab',str(e))
        def start(self):
            if self.lab and self.lab.isVisible(): self.lab.close()
            selections={key:widget.currentData() for key,widget in self.controls.items()}
            try:
                selected_command = profile_command(selected_profile(selections.get('gameProfile')))
                self.launch_env=environment(selections); save_settings(args.data,selections)
            except (OSError,ValueError) as e: QMessageBox.warning(self,'Settings',str(e)); return
            command[:] = selected_command
            self.accepted=True; self.close()
        def keyPressEvent(self,event):
            if event.key() in (Qt.Key.Key_Return,Qt.Key.Key_Enter): self.start()
            elif event.key()==Qt.Key.Key_Escape: self.close()
            else: super().keyPressEvent(event)
        def paintEvent(self,event):
            p=QPainter(self); p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
            p.scale(self.scale,self.scale)
            p.drawPixmap(QRectF(0,0,780,720),background,QRectF(background.rect()))
            p.fillRect(QRectF(0,0,780,720),QColor(0,0,0,90))
            logo=images.get('logo.dds')
            if logo:
                size=logo.size().scaled(620,165,Qt.AspectRatioMode.KeepAspectRatio)
                p.drawPixmap(QRectF((780-size.width())/2,16,size.width(),size.height()),logo,QRectF(logo.rect()))
            else:
                p.setFont(QFont('DejaVu Serif',30)); p.setPen(QColor('#dbc58c'))
                p.drawText(QRectF(0,30,780,130),Qt.AlignmentFlag.AlignCenter,'DUNGEON SIEGE II')
            p.setPen(QColor('#dbc58c')); p.setFont(QFont('DejaVu Serif',11))
            p.drawText(QRectF(0,174,780,24),Qt.AlignmentFlag.AlignCenter,'NATIVE EDITION FOR LINUX')
            p.fillRect(QRectF(54,208,672,316),QColor(0,0,0,190))
            p.setPen(QColor('#9a744a')); p.drawRect(QRectF(54,208,672,316))
    window=Launcher()
    if shot:
        window.ensurePolished(); window.grab().save(shot); return 0
    window.show(); app.exec()
    if window.accepted: os.execve(command[0],command,window.launch_env)
    return 0

if __name__=='__main__': sys.exit(main())
