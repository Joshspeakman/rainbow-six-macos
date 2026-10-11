# Rainbow Six, native for Apple Silicon Macs and Linux

Red Storm's **Tom Clancy's Rainbow Six** (1998), statically recompiled from **your own GOG copy (version 1.04)** into
native code: arm64 with Metal and Core Audio on a Mac, x86-64 with SDL2 and OpenGL on Linux. It doesn't use Wine,
CrossOver, Rosetta or an emulator.

**This repository contains nothing from the game.** `./install.sh` reads the `RainbowSix.exe` and `SMACKW32.DLL` in
your game folder, translates their machine code into C, and compiles that together with this project's runtime, which
replaces the Windows, DirectX and audio functions the game calls. The build happens on your computer, in the
git-ignored `recomp/work/` folder. The app reads the game's data from your folder and never changes it.

- [Status](#status)
- [Stock game or co-op mod](#stock-game-or-co-op-mod)
- [Requirements](#requirements-mac)
- [Getting the game files onto a Mac](#getting-the-game-files-onto-a-mac)
- [Install](#install)
- [Playing](#playing)
- [Updating and uninstalling](#updating)
- [Known issues](#known-issues)
- [Troubleshooting](#troubleshooting)
- [Linux](#linux)
- [How it works](#how-it-works)
- [Documentation](#documentation)
- [Legal](#legal)

## Status

This is beta software. The game plays from the intro through the menus into training and missions. The game's own
multiplayer is **in alpha testing**. Some areas have had little or no testing (see [Known issues](#known-issues)).

| Area | State |
|---|---|
| Intro movie (Smacker), menus, training, campaign screens | working |
| Missions: the Direct3D (DirectX 5) world, HUD, movement, mouse look, firing, night vision | working |
| Sound effects; music (the CD tracks, played from `MUSIC/TrackNN.ogg`) | working; Mac sound not compared with the original yet |
| The game's own LAN / TCP-IP multiplayer | **In alpha testing**; one co-op mission tested with 2 players (see [Known issues](#known-issues)) |
| Frame rate | follows your display (for example 120 fps on a ProMotion MacBook); the game's speed doesn't change with it |
| Voice chat (Voxware) | off: its codecs aren't available, and the original has none under Wine either |

The Linux build was compared with the original game running under Wine, screenshot by screenshot. The Mac build was
compared with the same reference captures in training and on the first mission.

## Stock game or co-op mod

The project builds from whichever `RainbowSix.exe` is in your game folder.

**The stock GOG 1.04 executable** is all you need for:

- single player: training and the campaign (not yet played through from start to finish);
- the game's own multiplayer over LAN or TCP/IP, which is **in alpha testing**. The Linux port played the first
  co-op mission with the unmodified stock GOG 1.04 game running under Wine, with either side hosting.
  Compatibility with the game running on a native Windows PC is unverified.

The build recognises the stock file by its SHA-256 (`recomp/r6_stock.sha256`, `556ed69b7029d58852458afe1a11ce3aa95a3cf16d2bca43e5f322c6c8aec8a4`) and prints
`RainbowSix.exe: stock GOG 1.04`. Any other `RainbowSix.exe` is still built, with the note `not the stock GOG 1.04
file: patched?`. To make the build refuse anything but one exact file, set `R6_EXE_SHA=<sha256>`.

**The co-op mod ("Co-op Edition")** is a separate project for cross-platform co-op: a mod patched into
`RainbowSix.exe` that adds its own co-op features on top of the game's multiplayer. Windows players run the patched
file as it is; Mac and Linux players build it with this port. The mod is **not published**, so this isn't available
yet. The port includes support for its launcher specification; compatibility with a future mod release has not been verified.

With the mod's `RainbowSix.exe` and its `r6coop-launcher.json` in the game folder, you also get:

- the mod's launch window ("Co-op Edition"): control layouts (Classic, Modern, Keyboard Only or Custom), a key guide
  that rebinds every action, and the mod's display settings: frame rate, field of view, HUD size, mission resolution,
  full screen or window;
- missions at any resolution, including your display's own size. The stock game stops at 1024x768.

Without `r6coop-launcher.json` the launch window has only **Play** and **Quit**, and the settings are in the game's
own Options screen.

## Requirements (Mac)

- An **Apple Silicon** Mac. Intel Macs are untested.
- **macOS 13 (Ventura) or later**.
- The **Xcode command line tools**: `xcode-select --install`. They provide `clang`, `git` and `python3`.
- An **internet connection on the first build**. The build creates a Python environment in `recomp/.venv` and installs
  Capstone (the disassembler the recompiler uses) and Unicorn into it with pip.
- **Your own copy of Rainbow Six from GOG**, version 1.04 (the version GOG sells).
- Build time and peak disk use have not been verified for a clean install. Allow room for your game files,
  generated source and object files, and the app; time varies with the Mac. The first build
  analyses, translates and compiles the whole game. It runs 16 compile jobs at a time; `R6_BUILD_JOBS=<n>` changes that.

## Getting the game files onto a Mac

GOG sells Rainbow Six for Windows only. There are two ways to get its files onto a Mac:

**From GOG's offline installer.** Download the Windows offline installer (`setup_tom_clancys_rainbow_six_….exe`) from
your GOG library's web page, then extract it with [innoextract](https://constexpr.org/innoextract/):

```sh
brew install innoextract
innoextract -d "Rainbow Six" /path/to/setup_tom_clancys_rainbow_six_*.exe
```

Depending on the installer, the game files end up in this folder or in a subfolder such as `app/`. Use the folder that
contains `RainbowSix.exe`. GOG's installers need a recent innoextract (1.9 or later); Homebrew's is recent enough.

**From a Windows PC.** Copy the whole installed game folder over as it is.

The folder must contain:

| File or folder | What it is |
|---|---|
| `RainbowSix.exe` | the game: recompiled |
| `SMACKW32.DLL` | the Smacker video player: recompiled |
| `data/` | the game's data, read from your folder while you play |
| `MUSIC/` | the soundtrack as `TrackNN.ogg`; GOG's replacement for the CD audio |

Keep the rest of the folder as it is. GOG's `winmm.dll` and its Ogg/Vorbis DLLs, and `VCT32150.dll` (voice chat), are
not used; the port plays the music itself.

## Install

Keep the clone in a path without spaces: the current compile scripts split source paths on whitespace.

```sh
git clone https://github.com/Joshspeakman/rainbow-six-macos.git
cd rainbow-six-macos
./install.sh --game-dir "/path/to/Rainbow Six"
```

`install.sh` recompiles the game and installs **Rainbow Six.app** in `~/Applications`. Open it from there, from
Launchpad, or from Spotlight. You don't need to run `recomp/build_r6.sh` yourself: `install.sh` runs it.

| Option | Default | |
|---|---|---|
| `--game-dir DIR` | (required) | the folder with `RainbowSix.exe`. It is only read. |
| `--app-dir DIR` | `~/Applications` | where `Rainbow Six.app` goes |
| `--data-dir DIR` | `~/Games/RainbowSixNative` | where settings, saves and logs go |

The app remembers the game folder and data folder you installed it with, so keep the game folder where it was, or run
`install.sh` again after moving it.

The built app contains a copy of your `RainbowSix.exe` and code translated from it. It is for your own use: don't
share it.

### Where things go

| | |
|---|---|
| `~/Applications/Rainbow Six.app` | the app |
| `~/Games/RainbowSixNative/registry.txt` | the game's options (on Windows these live in the registry) |
| `~/Games/RainbowSixNative/game/` | everything the game writes: saved campaigns, plans, the key map (`sherman.kmp`), its own `Sherman.log` |
| `~/Games/RainbowSixNative/RainbowSix.log` | the port's log (moved to `RainbowSix.old.log` at the next launch after it exceeds 5 MB) |
| `~/Games/RainbowSixNative/CrashReports/` | crash and freeze reports |
| `recomp/work/r6/` | the build folder, in your clone |

The game folder is never written to. The game sees a copy-on-write overlay: files it creates or changes go to
`~/Games/RainbowSixNative/game/`.

## Playing

### The launch window

Opening the app shows a launch window with the game's main-menu art. **Play** starts the game and **Quit** closes the
window.

- **Stock game:** only Play and Quit. The game's own Options screen has its settings, as on Windows.
- **With the co-op mod:** the mod's rows, read from its `r6coop-launcher.json`. On a Mac the window adds:
  - **Mission Resolution** also lists this display's own size, and on a MacBook with a notch, the 16:10 area below
    the notch;
  - **Screen**: Full Screen, or Window: a resizable window covering four fifths of the screen;
  - **Frame Rate**: match the display, a fixed cap, or unlimited (also turns off vsync);
  - **Mouse Input** (after Mouse Speed): **Raw**, the default, uses the mouse's own movement, in even steps, whatever
    your Mac's tracking speed. **macOS Pointer** uses the pointer's movement, with your Mac's tracking speed. A
    trackpad always uses the pointer's movement.

The settings are written when you press Play: options to `registry.txt` and the key map to `game/sherman.kmp`.

### Display, mouse and frame rate

- The picture keeps its shape in full screen and in a window: the 640x480 menus have bars at the sides, and missions
  fill the screen at a matching resolution.
- The frame rate follows your display's refresh rate (120 fps on a ProMotion display). The game moves by real elapsed time, so it runs
  at the same speed at any frame rate.
- **Raw mouse input** is on for a mouse with the stock game too (it has no Mouse Input row; see
  [Troubleshooting](#troubleshooting) to switch it). A trackpad uses macOS's pointer movement.
- **Small mouse movements are not lost.** Above 40 fps the original game's rounding of mouse look swallowed small
  movements to the left and up. The port rounds both ways evenly, with the stock and the mod executable alike.

### Controls

The game's own controls apply, set in its Options screen (or in the mod's launch window and key guide).

| | |
|---|---|
| **Command-Q** | quit |
| **Command** (in a window) | gives the pointer back, to use the title bar or another app |
| **Click** (in a window) | gives the pointer to the game. The click itself isn't passed to the game. |

### Multiplayer

**In alpha testing.** See [Known issues](#known-issues) for the limits of testing so far.

Use the game's **Multiplayer** screen, as on Windows, with TCP/IP. The host listens on the game's TCP port **2346**.
The first time, macOS asks whether Rainbow Six may find devices on your local network: allow it, or other players
can't find or join you. Joining by IP address works. Between machines on one network, the server list should show
the game too; that is less tested.

The host advertises the address of the network interface your Mac uses for its default route. If that's the wrong one
(for example a VPN), start the app from Terminal with `R6_HOST_IP=<your address>` (see
[Troubleshooting](#troubleshooting)).

## Updating

The app has no built-in updater. To update, pull the latest code and install again:

```sh
cd rainbow-six-macos
git pull
./install.sh --game-dir "/path/to/Rainbow Six"
```

Repeat any `--app-dir` and `--data-dir` options from the original install. This rebuilds the game and replaces
the app. Your settings and saves in the selected data folder stay.

The `main` branch is the stable one. `nightly` gets new work first: `git checkout nightly` to try it.

### Uninstalling

```sh
rm -rf ~/Applications/"Rainbow Six.app"
rm -rf ~/Games/RainbowSixNative          # your settings and saves too
rm -rf rainbow-six-macos                 # the clone and its build folder
```

Your game folder isn't changed by the port; delete it separately if you want.

## Known issues

- **Blank frames at doorways.** When the camera crosses a doorway, the original game draws one frame with only the
  room ahead, and the 3D view goes blank for that frame. This happens most when spectating. The original does the
  same under Wine. The port hides these frames by showing the previous picture for at most 45 ms. It catches most of
  them, not all; a frame or two per walk through a building can still get through. `R6_HOLD_BLANK=0` turns the hold
  off.
- **Multiplayer is in alpha testing.** Co-op has been played on the first mission (Operation Steel Wind) with two players,
  plus training. Adversarial games, more than two players and the full campaign in co-op are untested. So is internet
  play between different networks.
- **Server list.** The server list between this port and the original game on the same machine is untested; joining
  by address works.
- **Voice chat** is off.
- **Music volume.** The game's music volume options don't apply to the music.
- **Sound on the Mac** hasn't been compared with the original game yet.
- **Crash and freeze reports** still name "Dungeon Siege Native", inherited from the project this port started from.
  They are still the right reports.
- **Intel Macs** are untested.

## Troubleshooting

**Running the app from Terminal.** Some options are environment variables. To set one, start the app's launcher from
Terminal:

```sh
R6_HOLD_BLANK=0 ~/Applications/"Rainbow Six.app"/Contents/MacOS/launcher
```

| Variable | Effect |
|---|---|
| `DS_MOUSE=macos` | use the macOS pointer's movement for a mouse too (the Mouse Input row, for the stock game) |
| `R6_HOLD_BLANK=0` | show the game's blank doorway frames as the original does |
| `R6_HOST_IP=a.b.c.d` | the address to host multiplayer games on |
| `DSR_FPSCAP=<fps>` | a frame-rate cap; `DSR_FPSCAP=0 DSR_VSYNC=0` for no cap |
| `DS_WINDOW_MODE=windowed` | play in a window (the mod's launch window has a row for it) |
| `DS_NO_LAUNCHER=1` | skip the launch window |
| `R6_MOUSE_ORIGINAL=1` | the original game's mouse-look rounding |
| `DF_GAME_DIR`, `DF_DATA_DIR` | a different game or data folder than the ones you installed with |

**The build says `no RainbowSix.exe` or `missing …/SMACKW32.DLL`.** `--game-dir` must point at the folder that contains
those files. After innoextract they can be in a subfolder (`app/`).

**The first build fails while installing Python packages.** It needs an internet connection to install Capstone into
`recomp/.venv`. If an earlier attempt was interrupted, delete `recomp/.venv` and run `install.sh` again.

**Building runs out of memory or makes the Mac unresponsive.** Use fewer compile jobs:
`R6_BUILD_JOBS=4 ./install.sh --game-dir …`.

**Something went wrong in the game.** Look at `~/Games/RainbowSixNative/RainbowSix.log`, the game's own
`~/Games/RainbowSixNative/game/Sherman.log`, and `~/Games/RainbowSixNative/CrashReports/`. Attach them when you report
a problem.

**Start with fresh settings.** Move `~/Games/RainbowSixNative/registry.txt` away; the game writes its defaults on the
next start. Your saves in `game/` stay.

## Linux

`install.sh` works on Linux too. It was developed and tested there on x86-64 (CachyOS).

Requirements: `clang`, `python3` with `venv` and `pip`, `pkg-config`, and the development packages for **SDL2,
GLEW, freetype, fontconfig, libsndfile and libpng**. For example:

```sh
# Debian / Ubuntu
sudo apt install clang python3-venv pkg-config libsdl2-dev libglew-dev libfreetype-dev libfontconfig-dev libsndfile1-dev libpng-dev
# Arch
sudo pacman -S clang python pkgconf sdl2 glew freetype2 fontconfig libsndfile libpng
```

Then the same command as on a Mac: `./install.sh --game-dir "/path/to/Rainbow Six"`. It installs:

| | |
|---|---|
| `~/.local/bin/rainbow-six` | the command that starts the game |
| `~/.local/share/rainbow-six/` | the program |
| `~/.local/share/applications/rainbow-six.desktop` | the app menu entry, with the game's icon if the folder has GOG's `goggame-*.ico` |
| `~/Games/RainbowSixNative` | settings, saves and logs, as on the Mac |

The launch window on Linux comes with the co-op mod only: it needs the mod's `r6coop-launcher.json` in the game folder
and PySide6 (`pip install PySide6`, or your distribution's package). Otherwise `rainbow-six` starts the game directly.
The Mac's Mouse Input row doesn't exist on Linux. To update, run `git pull` and `install.sh` again. To uninstall,
delete the four paths above.

## How it works

This is a *static recompilation*: `recomp/tools/analyze.py` finds every function in the 32-bit x86 executable,
`recomp/tools/lift.py` translates each one into C, and clang compiles the result for the machine you're on. The C keeps
the original's memory layout (the game's pointers and structures keep their 32-bit values) and its x87 floating point
behaviour. The game's calls into Windows (files, the registry, windows and input, Winsock, DirectDraw and Direct3D 2
from DirectX 5, DirectSound, CD audio) go to a runtime in `recomp/runtime/win32` that implements them on macOS or
Linux. The graphics go to a Metal renderer on the Mac and OpenGL on Linux.

The recompiler and runtime are shared with the author's earlier ports. This repository started as the Delta Force
port (`delta-force-macos`) and still contains its build files (`recomp/build_df.sh`, `docs/DF-PORT.md`). The parts
specific to Rainbow Six are in [docs/R6-PORT.md](docs/R6-PORT.md): Direct3D 2, colour-keyed blits for the cursor,
C++ exceptions as control flow, door portals, the CD-audio music, the network address, and how a patched executable
with extra code sections is recompiled.

## Documentation

- [docs/R6-PORT.md](docs/R6-PORT.md): port notes, building by hand, test and debugging options, what was verified
- [recomp/README.md](recomp/README.md): the recompiler: execution model, flags, x87, indirect calls, test harnesses
- [docs/DF-PORT.md](docs/DF-PORT.md): the Delta Force port this repository started from

### Building by hand

```sh
recomp/build_r6.sh "/path/to/Rainbow Six"                 # -> recomp/work/r6/app/RainbowSix (Mac) or RainbowSixLinux
R6_RELINK=1 recomp/build_r6.sh "/path/to/Rainbow Six"     # after runtime or host changes only
recomp/work/r6/app/RainbowSix --exe recomp/work/r6/RainbowSix.exe --game "/path/to/Rainbow Six" --data /tmp/r6data
```

## Legal

*Tom Clancy's Rainbow Six* is © Red Storm Entertainment, now part of Ubisoft. Rainbow Six, Red Storm and Ubisoft are
trademarks of Ubisoft Entertainment. GOG is a trademark of CD PROJEKT. This project is not affiliated with or endorsed
by Ubisoft, Red Storm or GOG.

This repository contains no part of the game. You need your own legally obtained copy, and what the build produces
from it is for your own use only. Third-party code and credits are listed in [NOTICE.md](NOTICE.md).

The code in this repository is released under the [MIT License](LICENSE).
