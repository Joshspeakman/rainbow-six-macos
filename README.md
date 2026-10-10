# Rainbow Six, native (and the Delta Force ports it was forked from)

Red Storm's **Tom Clancy's Rainbow Six** (1998, GOG 1.04), statically recompiled from your own copy into native code for
Linux and Apple Silicon Macs: no Wine, no CrossOver, no emulator. It is the native half of a
cross-OS co-op project: Windows players run a `RainbowSix.exe` with the co-op mod patched in, Linux and Mac players run
this recompilation of the same patched file, and they play together over the game's own LAN/TCP multiplayer.

Nothing from the game is in this repository: `recomp/build_r6.sh "<game folder>"` recompiles whatever `RainbowSix.exe`
is in the folder (stock, or patched with extra code sections) plus `SMACKW32.DLL`, into the git-ignored `recomp/work/`.

| Rainbow Six (GOG 1.04) | state (Linux, compared with the original under Wine) |
|---|---|
| Intro movie (Smacker), menus, training, campaign screens | working |
| Missions: Direct3D (DirectX 5 HAL) world, HUD, movement, mouse-look, firing | working |
| Sound effects; music (CD audio from `MUSIC/TrackNN.ogg`) | working |
| LAN/TCP co-op with the original running under Wine, either side hosting | working (M01, joined and played) |
| The co-op mod's patched exe (`.r6coop` section) | recompiles and runs |
| Voice chat (Voxware) | off, as under Wine ("no codec") |
| macOS app | built and runs; see the Mac validation and remaining coverage in the port notes |

```sh
recomp/build_r6.sh "/path/to/Rainbow Six"          # -> recomp/work/r6/app/RainbowSixLinux (about 3 minutes)
./install.sh --game-dir "/path/to/Rainbow Six"     # ~/.local/bin/rainbow-six, or Rainbow Six.app on a Mac
```

Port notes, test recipes and Mac validation: [docs/R6-PORT.md](docs/R6-PORT.md).

The Delta Force material below describes this fork's original handoff. Current Delta Force builds and validation
live in their own `delta-force-macos` repository.

---

# Delta Force and Delta Force 2, native

NovaLogic's **Delta Force** (1998) and **Delta Force 2** (1999), statically recompiled from your own Steam copies into
native code: no Wine, no Rosetta, no emulator. It uses the same recompiler as the Dungeon Siege, Fallout: New Vegas,
Diablo II and Sims ports. The target is Apple Silicon Macs (Metal, Core Audio); the Linux build (SDL, OpenGL) runs the same
recompiled code and is where the port has been developed and tested so far.

Nothing from the games is in this repository. The build verifies your `Df.exe` / `Df2.exe`, undoes the Steam wrapper
on a copy in the build folder, recompiles that copy, and reads the game's data from your folder.

## State

| | Delta Force (Steam 32620) | Delta Force 2 (Steam 32630) |
|---|---|---|
| Menus, soldier, campaign/quick missions, briefing | working (Linux) | working (Linux) |
| Missions: voxel terrain, models, HUD, movement, mouse-look | working (Linux) | working (Linux) |
| Renderers | **16 million colours** and **256 colours** (Settings → Colors), **windowed** | **Full Screen (Software)**, **Full Screen (Hardware)** (Direct3D, `Hw_d3d.dll` recompiled), **Windowed** |
| Sound and music | working (Linux) | working (Linux) |
| LAN multiplayer (`Netsock.dll`, recompiled) | builds, untested | builds, untested |
| NovaWorld voice/browser (`Netcomm.dll`, `iBrowse.dll`) | — | not available (stubbed) |
| macOS app | written, **not yet built** | written, **not yet built** |

"Working (Linux)" means checked against the original game running under Proton with the same inputs (screenshots
compared). The Mac side (Metal renderer, Core Audio, app host) is shared with the other ports and has the Delta Force
changes written in, but it has not been compiled or run on a Mac yet: see [docs/DF-PORT.md](docs/DF-PORT.md) for the
handoff, known gaps and how to test.

## Install

Copy the Steam game folder over from the PC as it is (`steamapps/common/Delta Force` or `.../Delta Force 2`), then:

```sh
git clone <this repository> && cd delta-force-macos
./install.sh --game-dir "/path/to/Delta Force"      # Delta Force.app (macOS) or ~/.local/bin/delta-force (Linux)
./install.sh --game-dir "/path/to/Delta Force 2"    # Delta Force 2.app or delta-force-2
```

Needs the Xcode command line tools (macOS) or clang plus the SDL2, GLEW, freetype, fontconfig, libsndfile and libpng
development packages (Linux), and python3. The build takes a minute or two. The game folder is only read; settings,
soldiers and saves go to the data folder (`~/Games/DeltaForceNative`, `~/Games/DeltaForce2Native`; `--data-dir` to
choose).

Choose the renderer in the game's own Settings screen, as on Windows.

## Building by hand

```sh
recomp/build_df.sh "/path/to/Delta Force 2"           # -> recomp/work/df2 (Linux: app/DeltaForce2Linux; macOS: libgame.dylib + app/DeltaForce2)
recomp/work/df2/app/DeltaForce2Linux --exe recomp/work/df2/Df2.exe --game "/path/to/Delta Force 2" --data /tmp/df2data
```

`DF_RELINK=1 recomp/build_df.sh ...` rebuilds only the runtime and host (after changes there).

## How it works

[docs/DF-PORT.md](docs/DF-PORT.md) covers what was specific to these games: the SteamStub 2.1 wrapper, the self-modifying
MMX span renderers (instruction fields patched at run time, and multiply-by-constant code copied in from tables), Watcom
C quirks, DirectDraw and Direct3D 6 on the shared renderer (including depth buffers the game writes itself), and the
testing tools. The recompiler itself is described in [recomp/README.md](recomp/README.md).
