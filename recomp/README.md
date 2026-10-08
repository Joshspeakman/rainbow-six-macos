# Recompiler: Dungeon Siege II as native code

Static recompilation of the user's own GOG 2.3 `DungeonSiege2.exe` (and the Miles and Bink DLLs in its folder) into
C, compiled for Apple Silicon or Linux x86-64. Linux build status and instructions are in the
[root README](../README.md); the tables and shader notes below describe the upstream Mac implementation. Nothing derived from the game is committed: `build.sh` regenerates everything locally
from the user's files. Installing and playing: [README](../README.md); plan and open work: [docs/PLAN.md](../docs/PLAN.md).

```sh
recomp/build.sh "/path/to/Dungeon Siege 2/DungeonSiege2.exe"   # -> recomp/work/full/libgame.dylib (about 10 minutes)
recomp/tools/build_app.sh recomp/work/full
recomp/run.sh "/path/to/Dungeon Siege 2"
```

## Status

| Step | State |
|---|---|
| Analysis (`tools/analyze.py`) | 85,833 functions, 97.1% of `.text`; 479 jump tables resolved, 73 bounded heuristically |
| Lifter (`tools/lift.py`), runtime core (`runtime/`) | the whole executable and its DLLs compile and link (89,293 functions) |
| Win32 layer (`runtime/win32`): loader, memory, heaps, threads, SEH, files, registry, USER32 (with timers), GDI, DbgHelp, WSOCK32 (`winsock.c`) | working |
| Direct3D 9 (`d3d9.c`, `d3d9_fmt.c`) on the Metal renderer (`../src/renderer`), D3DX textures (`d3dx9.c`) | working: fixed function, and the game's HLSL effects compiled to Metal (`hlsl.c`, `d3dx9_fx.c`) |
| Miles 6.6g (`mss32.dll` and its providers) and Bink, recompiled from the game folder; DirectSound over Core Audio | working |
| App bundle, installer (`../install.sh`), launch window (`host/launcher.m`) | working |
| Multiplayer (DirectPlay 8, GameSpy lobby on OpenSpy) | switched back on: the lobby works with an original CD key; hosting and joining untested (`../docs/MULTIPLAYER.md`) |

The recompiled game runs without Wine or Rosetta: intro, menus, new hero, the campaign, saving and loading.

Test mode: `DS_TEST=1` (background window, no mouse capture, no launch window), `W32_AUDIO_MUTE=1`,
`DS_SCRIPT="@20:click:402,264;@30:shot:/tmp/a.png"` (scripted input in the game's display-mode coordinates; also
`move`, `rclick`, `press`, `key`, `type`), `DS_RESOLUTION=WxH`. Logs: `D3D9_LOG=1|2` (Direct3D calls), `D3D9_FPS=1`,
`W32_TRACE=<file>` (every Windows call), `W32_SEHLOG=1` (exceptions and the SEH chain), `W32_VMLOG=1`,
`W32_REGLOG=1` (registry keys not found), `W32_AUDIOLOG=1`. Crash and hang reports go to `<data>/CrashReports`.

Shader effects (`runtime/win32/hlsl.c`, `d3dx9_fx.c`; the game's Options → Video → Shaders, saved as `shaders = ...` in
`Save/prefs.gas`): `DS2_SHADERS=0` turns them off (the Radeon 8500 identity, fixed function only). Development:
`FX_PARAMLOG=1` (each parameter's first and first non-zero value), `FX_FOGLOG=1` (fog parameter changes),
`FX_DUMP=n` (every parameter at the first n passes of each effect), `D3D9_TEXLOG=n` (texture binding during effect
passes), `HLSL_MAGENTA=1` (effect pixels drawn magenta), `DSR_FXPATCH="old=>new||old=>new"` (text replaced in every
effect program's Metal before compiling), `DSR_FXTEXLOG=n` (fog, alpha and textures at effect draws), `DSR_RTLOG=n`
(render target changes and clears), `DSR_SHOWSURF=<surface id>` (that surface drawn in the screen's corner),
`DSR_GLOWLOG=1` (draws into and out of effect render targets, which are shown in the screen's corner),
`D3D9_FXSTATELOG=1` (states the game changes between an effect's Begin and End that the effect doesn't own).
`D3D9_STATELOG=1` (each render and stage state value the game uses, once). `tests/hlsl_check.m` compiles every pass of
`.fx` files with Metal (`HLSL_DUMP=<technique>` prints its source); `tests/fx_render_test.m <folder with the .fx files>`
renders the game's pixel shaders with known inputs and checks their results (glow taps and weights, shadow blur, heat
shimmer, Dot3, ice, specular, water, sampler slots, XRGB alpha).

Native hot loops (`runtime/win32/native.c`, entered through `tools/lift.py` HOOKS and OVERRIDES): the vertex lighting
of directional lights and the terrain's 3x3 vertex transform, bit-identical to the original. `DS2_NATIVE=0` runs the
recompiled original instead, `DS2_NATIVE_CHECK=1` runs both and reports any difference, `DS2_NATIVE_PROF=1` prints
the lighting loop's time. `DSR_CAPLOG=1` prints the game's work per frame that the automatic frame cadence (120/60) is
chosen from.

## Execution model

- **Memory:** one 4 GB host region; guest address = offset. The image sits at its preferred base `0x400000`, so all
  pointers the game stores (vtables, saves, its own structures) keep their 32-bit values.
- **Functions:** one C function per original function. Registers are C locals, written back to a per-thread context
  around calls. The guest stack is real (`call` pushes the original return address; arguments are read where the
  original reads them), so calling conventions, `this` pointers and stack-walking code behave as before.
- **Flags:** lazy — flag-setting instructions record their operands; consumers (`jcc`, `setcc`, `adc`, `sahf` …)
  compute only the flags they need.
- **x87:** eight doubles + TOP + control/status words, the precision model this game was measured to run with under
  Rosetta: double arithmetic, float rounding at 32-bit stores, `fist(p)` honouring the rounding
  mode. x87 register forms are decoded from the opcode bytes (Intel semantics), not from disassembler text.
- **Indirect calls/jumps:** an address → function table; jump tables become `switch` statements; imports are dispatched
  through `rt_import` (IAT slots hold thunk addresses, so a call through a register still lands there).
- Junk decodes (data read as code) and padding become `rt_unhandled()` traps.

## Verification and known gaps

The instruction-level harness (`tests/harness.py`, `tests/fuzz_insn.py`, `tests/test_slice.py`) comes from the
Dungeon Siege 1 port, where every instruction form the game used was checked bit-for-bit against Unicorn; it has not
been re-run on DS2's instruction mix yet. DS2 has been checked by playing: menus, hero creation, movies, the opening
of the campaign, save/load, and a 6-minute random-input soak.

- **SEH:** access violations and RaiseException go through the game's handlers. First touches of reserved archive
  pages pass over C++ `catch(...)` frames (the game reads archives inside them); C++ catch continuations are not
  implemented, and the game has not needed them so far.
- **Indirect calls** to addresses outside the function table stop the game with the address logged (add it as a
  seed in `work/observed_targets.json` and rebuild).
- **x87 tag word** is not modelled; transcendentals use the host math library.
