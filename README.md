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
