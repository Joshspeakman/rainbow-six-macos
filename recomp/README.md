# Recompiler: Rainbow Six as native code

Static recompilation of the user's own `RainbowSix.exe` (GOG 1.04, or that file with a mod patched in) and
`SMACKW32.DLL` into C, compiled for Apple Silicon or Linux x86-64. The build regenerates its output locally under
`recomp/work/` (git-ignored). Installing and playing: the [root README](../README.md). Game-specific implementation
and testing notes: [docs/R6-PORT.md](../docs/R6-PORT.md).

## Building by hand

From the repository root, in a clone whose path contains no spaces:

```sh
recomp/build_r6.sh "/path/to/Rainbow Six"             # -> recomp/work/r6/app/RainbowSix (Mac) or RainbowSixLinux
R6_RELINK=1 recomp/build_r6.sh "/path/to/Rainbow Six" # runtime or host changes only, after a full build
```

Build time and peak disk use have not been verified for a clean install. `R6_BUILD_JOBS=<n>` sets the parallel
compile jobs (default 16). The first build creates `recomp/.venv` and installs Capstone and Unicorn with pip.

`build_r6.sh` prints whether the executable matches the stock GOG 1.04 SHA-256 in `r6_stock.sha256`:

```
556ed69b7029d58852458afe1a11ce3aa95a3cf16d2bca43e5f322c6c8aec8a4
```

Other executables are accepted by default; that is not a compatibility guarantee. Set `R6_EXE_SHA=<sha256>` to
require one exact file. `R6_RELINK=1` also checks that the executable matches the previous lift's `lifted.sha256`.

| Step | Implementation |
|---|---|
| Analysis | `tools/analyze.py`: writes `analysis.json` and `analysis.log`; handles multiple executable sections, including code added by a mod. `R6_SEEDS=<file>` supplies extra hexadecimal function addresses. |
| Lifting | `tools/lift.py --game r6`: translates the executable, then `SMACKW32.DLL`; `tools/link_tables.py` combines their function and import tables. |
| Compilation | `tools/cc1.sh`: compiles the generated C. A normal build removes the previous object directory and recompiles it. |
| Runtime and host | `tools/build_runtime.sh`: selects `R6_EDITION` with the shared `DF_EDITION` profile. On macOS it calls `tools/build_app.sh`; on Linux it calls `tools/build_linux.sh`. |

## Runtime and launcher

- `runtime/rt.c`, `rt.h`, `rt_sse.h`: the CPU execution model.
- `runtime/win32/`: the Windows APIs, files and registry, DirectDraw/Direct3D, DirectSound and Winsock; Rainbow Six
  has its own imports in `r6_imports.c`. GOG's CD-audio replacement DLLs are not lifted: the runtime plays
  `MUSIC/TrackNN.ogg` itself. Voxware voice chat is unavailable.
- `host/main.m`, `host/r6_launcher.m`: the Mac host and launch window; `../src/renderer` draws with Metal.
  Without the separate mod's `r6coop-launcher.json`, the launch window has Play and Quit. With it, settings come
  from that specification and the Mac additions described in the root README.
- `host/linux.c`, `host/linux_launcher/r6_launcher.py`: the Linux host and optional PySide6 mod launcher.
- Settings are stored in `<data>/registry.txt`; game-folder writes, including `sherman.kmp`, go into `<data>/game/`.
  Crash and hang reports go into `<data>/CrashReports/`. The installed Mac launcher writes `<data>/RainbowSix.log`.

This repository inherited other ports' scripts and runtime modules. Dungeon Siege II's function counts, shaders,
Miles audio, DirectPlay 8 lobby and play-test results do not describe Rainbow Six. `build_df.sh` and
[docs/DF-PORT.md](../docs/DF-PORT.md) concern Delta Force and Delta Force 2.

## Execution model

- **Memory:** one 4 GB host region; guest addresses are offsets. The game's pointers and structures retain their
  32-bit layout.
- **Functions:** one C function per original function. Registers are C locals, written back to a per-thread context
  around calls. The guest stack preserves the original calling conventions and return addresses.
- **Flags:** flag-setting instructions record their operands; consumers calculate the flags they need.
- **x87:** eight doubles, TOP and control/status words, with rounding at float stores. `runtime/rt.c` uses
  CORE-MATH for tan, sin, cos and atan2; other transcendentals use the host math library. The tag word is not modelled.
- **Indirect calls and jumps:** an address-to-function table, translated jump tables, and import dispatch through
  `rt_import`. Unsupported instructions and unresolved targets can stop execution with a diagnostic.

## Verification tools

The inherited instruction harnesses (`tests/harness.py`, `tests/fuzz_insn.py`, `tests/fuzz_sse.py`) compare recompiled
instructions with Unicorn. `tests/replay.py` compares a recorded call with the original x86 execution.
`tests/metal_regressions.sh` and `tests/check_linux.sh` exercise runtime and renderer code without game files.
Their presence does not establish complete Rainbow Six coverage; recorded game testing and its limits are in the
root README and [port notes](../docs/R6-PORT.md).

For development, `DS_TEST=1` uses a background window without mouse capture or the launcher; `DS_RESOLUTION=WxH`,
`DS_SCRIPT` and `DS_CMDFILE` control scripted runs. `W32_TRACE=<file>`, `W32_SEHLOG=1`, `W32_REGLOG=1` and
`W32_AUDIOLOG=1` provide diagnostics. These are runtime options, not installer flags.

## A note from the author

These projects are roughly 90% built with AI, with human testing along the way. I started them because some of my
favorite games either ran badly on macOS or wouldn’t run at all, and Wine and CrossOver didn’t always get me where I
wanted.

I’m a 40-year-old dad with a full-time job. I don’t have the time or desire to write all of this from scratch, and AI
made it possible to get somewhere with it.

If that puts you off, no hard feelings. Use them, skip them, fork them. It’s all good. I would have happily used an
existing, human-written native port, but I couldn’t find one that did what I wanted. Mostly, I just want to play my
games.

The aim is straightforward native builds without unwanted extras or a stack of translation layers. Forks and
improvements are welcome.
