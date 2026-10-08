# Delta Force 1 & 2: port notes and Mac handoff

State as of 2026-10-08. Everything here was done and verified on Linux (CachyOS, SDL2/OpenGL host), comparing against
the original executables running under Proton. The Mac side has not been compiled yet; that is the next step.

## The executables

| | Delta Force | Delta Force 2 |
|---|---|---|
| Steam app / build | 32620 / 19311 | 32630 / 19312 |
| File, SHA-256 (as shipped) | `Df.exe` 307bd095… | `Df2.exe` 0765acdf… |
| Compiler | Watcom C/C++ 10.x (sections `AUTO`, `DGROUP`, `.bss`; register calling convention) | MSVC 5 (`.text`) + MASM (`_text`, `_TEXT`) |
| Wrapper | SteamStub 2.1 | SteamStub 2.1 |
| DLLs recompiled with it | `Netsock.dll` (LAN), `Cmprssh0.dll` (rebased 0x11000000) | same + `Hw_d3d.dll` (Direct3D driver, rebased 0x12000000) |
| Stubbed | DirectPlay probe, MIDI, joystick | + `Netcomm.dll` (NovaWorld voice), `iBrowse.dll` (NovaWorld web pages) |

`recomp/tools/steamstub21.py` undoes SteamStub 2.1 on a copy in the build folder (the header is found from the stub's
first instructions, XOR-chained; the payload holds the XTEA-encrypted SteamDRMP.dll, whose code says where the AES key,
IV and stolen bytes of `.text`/`AUTO` sit). Format as documented by the Steamless project; the code is our own.

## What was specific to these games

**Self-modifying renderers.** Both games' MMX span renderers (the voxel terrain) rewrite their own instructions every
frame:
- *Patched fields*: immediates and displacements assembled as `0x1234567` placeholders, written with absolute-address
  stores. `lift.py smc_scan` finds every such store into code and `emit_smc` reads the field from guest memory when the
  instruction runs (4-byte fields through a marker value; 1-byte fields as a dispatch over all 256 values).
  `work/<game>/smc.json` lists them.
- *Code slots*: multiply-by-constant sequences copied in from tables (8-byte `lea`/`shl`/`neg` entries, 64 of them,
  the index signed so half sit below the table's base) followed by a 3-byte shift copied from a second table.
  `find_slots` recognises the writers and `emit_slot` lifts each slot as a dispatch on its current bytes.
- `W32_SMCLOG=1` reports, at every presented frame, any code byte that changed at run time — compare with smc.json to
  find a store the static scan missed (most hits are data kept inside Watcom's code section).

**Watcom.** VirtualSize 0 and a raw-size-only `.bss` (pe.py, loader.c); the relocation table seeds the analysis (the
C runtime's init tables are 6-byte entries at 2 mod 4); `repnz movsd` (F2 A5) is a rep copy — Capstone drops that F2,
lift.py reads the prefix bytes itself; segment register moves are no-ops in the flat model; one `call` jumps over an
inline string ("MU Block") to push its address.

**DF2.** MSVC two-level jump tables whose bound is compared in a stack slot, found from the instructions the function
walk decoded (not a backward decode); interpreter handler tables inside `_TEXT` seed the analysis.

**CPU.** DF1 refuses to start without MMX: cpuid reports a Pentium MMX for both games (`rt_cpuid_mmx`, DF_EDITION).
MMX `packsswb/packuswb/packssdw` needed their 64-bit forms. `tests/fuzz_sse.py` passes every MMX form both games use.

**Graphics.** DirectDraw comes from the Diablo II port (`runtime/win32/ddraw_pal.c`: 8/16/32-bit surfaces, palettes,
DirectX 6 `IDirectDraw4` + `IDirect3D3`). Added here: `IDirect3D3::FindDevice`; **depth buffers the game fills with the
CPU** — DF2's hardware driver draws the voxel terrain in software, depth included, then its Direct3D polygons against
it — sent with the new `DSR_DEPTH_UPLOAD` command (GL: `glTexSubImage2D` into the depth-stencil texture; Metal: a float
texture written as depth by a full-screen pass, `fs_zupload`); windowed modes draw with `StretchDIBits` /
`SetDIBitsToDevice` (df_imports.c), which become the frame on screen.

**Input.** Both games measure mouse-look between WM_MOUSEMOVEs and re-centre with SetCursorPos, so SetCursorPos posts the
WM_MOUSEMOVE Windows would; mouse messages carry client coordinates; DF2 makes a new main window when it changes driver
and the host follows it (Linux host and Mac host).

**Audio.** One streaming DirectSound buffer that the game mixes into from a multimedia timer; works as is. DF2's MSACM
calls (IMA and Microsoft ADPCM) are decoded in df_imports.c.

## Verified on Linux (against Proton)

- DF1: Choose Soldier, main menu, settings, campaign map, briefing, first Peru mission (terrain, trees, sky, HUD,
  weapon), walking, mouse-look; 16M colours, 256 colours, windowed; menu music and effects.
- DF2: Choose a Soldier, main menu, settings, quick missions, briefing, "Six of One" (snow terrain, falling snow, HUD,
  weapon), mouse-look; Full Screen (Software), Full Screen (Hardware/Direct3D), Windowed.

Click scripts that reach a mission (test host, `DS_TEST=1`, game coordinates 640x480, frame numbers):
- DF1: `600:click:318,439;900:click:320,142;1200:click:320,439;1600:click:320,439` (accept soldier, single player,
  accept mission, start mission). Settings: `900:click:320,202` from the main menu; Colors arrows at (294,221)/(543,221),
  Windowed (543,293), accept (320,439).
- DF2: `300:click:317,439;600:click:320,89;900:click:329,439;1300:click:322,439`. Settings `600:click:320,209`; driver
  arrows (270,226)/(574,226): Software → Hardware → Windowed; accept (320,439).
- Settings persist in the data folder: start each comparison from a fresh one.

## Mac handoff: what to do first

1. `recomp/build_df.sh "<game folder>"` on the Mac: tools/build_runtime.sh's macOS branch now passes the edition flags
   (`-DDF_EDITION -DDF1_EDITION/-DDF2_EDITION`), the third_party include paths and MojoShader, then runs
   tools/build_app.sh (`app/DeltaForce`, `app/DeltaForce2`). None of this has been compiled on macOS: expect small fixes
   (runtime files written for Linux, e.g. vulkan.c, gdi32.c's font backend, platform.h).
2. `src/renderer/renderer.m` / `shaders.metal`: the `DSR_DEPTH_UPLOAD` path (`depth_upload`, `fs_zupload`) is new and
   unbuilt. Check DF2 Hardware against a Linux run of the same mission.
3. `recomp/host/main.m`: under DF_EDITION the launch window is skipped (the game's own Settings screen chooses
   renderer and resolution), DungeonSiege2.ini handling is off, input follows the newest main window, script clicks
   are translated to desktop coordinates. Mouse capture and scaling of the 640x480 frame to the window come from the
   shared host.
4. `./install.sh --game-dir "<folder>"` makes `~/Applications/Delta Force.app` / `Delta Force 2.app`.

## Known gaps

- Intro videos: DF2's `DF2INTRO.BIK` is skipped (no Bink decoder here; the game moves on). DF1 opens `INTRO.KDV`
  (NovaLogic's own format) at start-up; whether it plays as on Windows has not been checked.
- DF2 Windowed: mouse-look does not respond after switching to Windowed in the same session (it does when the game
  starts in Windowed). SetCursorPos is never called in that mode; not yet investigated.
- LAN multiplayer: Netsock.dll is recompiled and its imports are implemented, but no game has been hosted or joined.
- Direct3D mode was compared against one reference frame per mission start; effects such as explosions and smoke in
  hardware mode have not been looked at.
- Mouse sensitivity is 1:1 with host pixels; the game's own sensitivity slider applies on top.

## Tools and switches

| | |
|---|---|
| `recomp/build_df.sh <folder>` | unwrap, analyse, lift (exe + DLLs), compile, link. `DF_RELINK=1`: runtime and host only. `DS_TRACE_HOOKS=0xA,0xB`: hooks at those addresses |
| `W32_SNAP=<addr>[:n\|:0]` `W32_SNAP_FILE=<path>` | at a hook, write registers + every committed page (one call, or every call up to 64) |
| `tests/replay.py <libgame> <snapshot> <addr>` | that call through the original x86 (Unicorn) and the recompiled code; registers and memory compared |
| `W32_SMCLOG=1` | code bytes changed at run time |
| `W32_DDLOG=1` | DirectDraw/Direct3D calls and each distinct draw state |
| `DF_MOUSELOG=1` | SetCursorPos and posted mouse messages |
| `W32_TRACE=<file>` | every Windows call |
| `W32_AUDIOLOG=1`, `W32_AUDIO_DUMP=<wav>` | mixer peaks, the mixed output |
| `tests/fuzz_insn.py`, `tests/fuzz_sse.py` | instruction forms vs Unicorn (`FUZZ_EXE`, `FUZZ_ANALYSIS`) |
| gdb | lifted code's line numbers are guest addresses: `break x86:<decimal address>` |

Reference runs: the unwrapped exe (`recomp/work/<game>/Df*.exe`) in a folder of links to the Steam files, under
Proton in a headless gamescope (`gamescope --backend headless -W 1024 -H 768 -w 640 -h 480`), XTEST input from
python-xlib, screenshots with `gamescopectl screenshot`.
