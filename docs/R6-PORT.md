# Rainbow Six: port notes and Mac handoff

State as of 2026-10-09. Done and verified on Linux (CachyOS, SDL2/OpenGL host) against the original GOG executable
running under Wine in a headless gamescope (the `~/Projects/r6-coop/tools` scripts). The Mac host is now built and runs;
the historical handoff below is retained with its current status, and the 2026-10-10 review follows it.

## Building and running

```sh
recomp/build_r6.sh "<game folder>" [outdir]     # analyse, lift, compile, link: ~3 minutes, 16 compile jobs (R6_BUILD_JOBS)
R6_RELINK=1 recomp/build_r6.sh "<game folder>"  # runtime/host changes only (refused if the exe is not the lifted one)
recomp/work/r6/app/RainbowSixLinux --exe recomp/work/r6/RainbowSix.exe --game "<game folder>" --data <data folder>
```

The game folder is read only; everything the game writes (Sherman.log, saves, plans, temp files) goes to
`<data>/game` (copy-on-write overlay) and the registry to `<data>/registry.txt`. The folder needs `RainbowSix.exe`,
`SMACKW32.DLL`, `data/` and `MUSIC/`; GOG's `winmm.dll`, the libvorbis/libogg DLLs and `VCT32150.dll` are not used.

### Any RainbowSix.exe (the co-op mod)

`build_r6.sh` does not pin the executable. It prints whether the file is the stock GOG 1.04 one
(`recomp/r6_stock.sha256`); `R6_EXE_SHA=<sha256>` makes it refuse anything else. A patched executable with an extra
executable section (the mod's `.r6coop`, any name works) is handled like this:

- `tools/pe.py code_range` covers every executable section when there is more than one (the gaps, including
  `.rdata`/`.data`/`.rsrc`, read as `int3` and are never decoded); `analyze.py is_code` only accepts addresses inside an
  executable section.
- The first byte of every extra section seeds a function. Direct `call`s into the section (the mod's call-site hooks)
  are function seeds like any call; direct `jmp`s into it and back are followed as part of the hooked function; pointer
  tables inside it are scanned. Code reached only through pointers built at run time: list it in `R6_SEEDS=<file>` (hex
  addresses, one per line, `#` comments).
- 16 zero bytes end a decode in a multi-section executable (the mod's section is code followed by 2.9 MB of zeroed data;
  decoding that as `add [eax], al` produced 25 M instructions and a 30 GB compile).

Verified with `~/Projects/r6-coop/out/RainbowSix.exe` (2026-10-09 12:22): it recompiles in the usual time, the hook
targets (0x953000, 0x9534e0, 0x954790, 0x9547e0, 0x954840) are functions, it plays training, and the mod writes
`r6coop.log` ("r6coop 0.1 loaded") into the data folder's overlay.

## What was specific to this game

**Direct3D is DirectX 5.** The game asks IDirectDraw for `IDirect3D2` and draws with `IDirect3DDevice2`
(`ddraw_pal.c`: the DirectX 6 device behind DirectX 5 vtables, `EnumTextureFormats` with `DDSURFACEDESC`, materials for
the viewport background, lights accepted). The world is `D3DVT_LVERTEX` (pre-lit, untransformed): `SetTransform` feeds
the renderer's world/view/projection, and each vertex is repacked without its reserved dword. HUD and menus are
`D3DVT_TLVERTEX` and DirectDraw blits. There is one device (the HAL); the game's `SoftwareOnly` option still goes
through it. Flip swaps the two buffers as DirectDraw does (`R6_FLIP_NOSWAP=1` turns that off).

**Colour keys and the cursor.** The menus draw the cursor with colour-keyed `BltFast` onto the GPU back buffer and save
the background under it with `Blt` from the back buffer. Keyed copies to a render target are done on the GPU (quad
mode 3 discards keyed texels: `renderer.c`/`quad.frag.glsl`, mirrored in `renderer.m`/`shaders.metal`); `Blt` from a
render target first reads back what the GPU drew; textures and copy sources always have an alpha channel because the
key is set after the surface exists.

**C++ exceptions are control flow.** The parser and the renderer throw and catch during normal play (e.g. "HUMANPLAN:
Error reading plan" when a training course has no plan). `lift.py` gives every function with a Visual C++ EH frame
(`push -1; push <mov eax, FuncInfo; jmp __CxxFrameHandler>`) landing pads at its catch continuations, found from the
FuncInfo try-block map: the `mov eax, imm32; ret` exits of each catch funclet (walked across function boundaries, as
the analysis sometimes splits a funclet). `_JumpToContinuation`'s `jmp eax` then longjmps to the pad like an
`__except` block (the existing `rt_seh_push` mechanism). 155 functions have pads.

**GOG's patch lives after .text's VirtualSize.** `0x434bfd` jumps to `0x840f20`, code GOG added in the slack between
VirtualSize (0x43ff12) and the raw size (zeroing a 0x1e0-byte buffer). `pe.py` extends executable sections to the
page-rounded size when the file has the bytes; the loader maps every section that far, as Windows does.

**Registry.** All options live in `HKLM\Software\Red Storm Entertainment\Tom Clancy's Rainbow Six`. The installer's
path values are written at start-up (pointing at the virtual game folder), the game adds its defaults on the first run,
and the tree is saved to `<data>/registry.txt` after every change. The game stores strings without their terminator
("TRUE" as 4 bytes); reads terminate them as Windows does (before that, `VideoResolution` read as 640x480000 and the
mission renderer found no display mode). Opening the key read-only and writing to it is allowed (Windows 9x semantics).

**Voice chat.** `VCT32150.dll` (Voxware) loads its codecs as further DLLs (`MVoice.vxp`). It is not recompiled;
`r6_imports.c` answers "engine present, no codec", which is what the original reports under Wine too.

**Music.** CD audio through MCI (`cdaudio.c`): open "cdaudio", TMSF, number of tracks, play track N to N+1 with notify,
stop, close; `MUSIC/TrackNN.ogg` decoded with stb_vorbis (`third_party/stb_vorbis`, public domain/MIT) on its own output
stream; `MM_MCINOTIFY` posted when a track ends. `W32_CD_LOG=1` logs.

**Network.** The host binds its TCP listener (`MultiplayerGameJoinPort`, 2346) and its UDP announcer to the first
address of `gethostbyname(gethostname())`: that is now the default route's interface (not a VPN or docker bridge, never
the router's public address); `R6_HOST_IP=<a.b.c.d>` overrides. Accepted sockets keep the listening socket's
non-blocking mode (Winsock behaviour; a blocking accepted socket stalled the native host's loop on every exchange).

## Display (with the co-op mod's display options)

- `EnumDisplayModes` (R6_EDITION) lists 640x480 to 3840x2160 in every common shape plus the desktop's own size; the
  mod lifts the game's 1024x768 limit and its 16:9 cap on the 3D window, so missions run at any of them, letterboxed
  in the host window.
- The game's windowed mode presents with a Blt to the primary surface, which the host never shows: `registry.c`
  answers `FullScreen` = TRUE, and the launcher's Screen row picks the host window instead (`DS_WINDOW_MODE`).
- macOS (`host/r6_launcher.m`, `host/main.m`): the host passes this display's pixel size, and on a notched MacBook the
  16:10 area below the notch, as `DS_DISPLAY_MODES` (added to the mode list and to Mission Resolution); Screen = Window
  opens a titled, resizable window (four fifths of the screen; the first click takes the pointer, Command or leaving
  the app gives it back); `DSR_FPSCAP` is always exported (the display's refresh, as on Linux, unless the launcher or the player set one), since the
  mod leaves pacing to the host only when it is there. The picture keeps its shape in a window and full screen
  (the view places its layer proportionally: 640x480 menus are pillarboxed; measured 2971 px wide on a 3456x2234 screen).
- Viewports: a device keeps 64 (`dv_AddViewport`); with 8 the mission's 3D viewport was dropped, its per-frame Clear
  skipped and stale depth hid geometry (the M01 fire escape). Found and fixed on the Mac (9130ccc).
- Door and window portals: the game draws what lies beyond an opening through a small viewport whose D3DVIEWPORT2 clip
  volume is that opening's part of the whole view. The projection reaches the renderer composed with that mapping
  (`xf_emit`); ignoring it squeezed the whole view into every doorway (cd0d41e, from the Mac). Check native frames
  against Wine frames of the same room, not just "the room is there".

## Verified on Linux

- Main menu identical to the original's (intro movie skipped with `Movie On`/`PlayIntroMovie` FALSE); single player,
  training, multiplayer screens; text fields and lists.
- Training "Shooting Range I": the 3D view matches the original's first frame; walking, mouse-look, firing (ammo
  count), HUD, map.
- Sound effects (DirectSound) and menu music (CD track 11).
- Co-op M01 "Operation Steel Wind", 2 players: native client joining the original hosting under Wine, and native host
  with the original joining. Both reach the mission; each sees the other's team, and movement on one side shows on the
  other. Recipes: `~/Games/r6-native/mp_test.sh` and `mp_test_rev.sh` (the Wine side on port 2446, the native side on
  2546, so they don't collide with each other or with other instances on 2346).

## Testing

- `DS_TEST=1` (hidden window, no mouse capture), `W32_AUDIO_MUTE=1`, `DS_EXIT_AFTER=<s>`.
- `DS_CMDFILE=<file>`: append `move:x,y`, `click:x,y`, `hold:vk,frames` (38 = Up), `rel:dx,dy`, `key:vk`, `type:text`,
  `shot:<png>` lines while the game runs (640x480 game coordinates). Main menu: Single Player (192,220), Multiplayer
  (192,256), Training (192,292); Training's Load Tutorial (90,214); its start dialog's Okay (422,294).
- `W32_REGISTRY="key|name|value;..."` (test mode) seeds string options before the game reads them; for dwords edit
  `<data>/registry.txt` (`~/Games/r6-native/setreg.py`). Use a fresh data folder per comparison; settings persist.
- Logs: `W32_DDLOG=1` (DirectDraw/Direct3D calls, draw states, transforms, viewports), `W32_TRACE=<file>`
  (`W32_TRACE_ONLY=wsock32`), `W32_SEHLOG=1`, `W32_CD_LOG=1`, `W32_AUDIOLOG=1`. `<data>/game/Sherman.log` is the game's own.
- gdb: lifted code's line numbers are guest addresses (`break x86:<decimal>`); `Ctx *` is `$rdi` at a function's first
  instruction (`break *f_<addr>`); registers are spilled to it around calls.

## Original Mac handoff and current status

1. `recomp/build_r6.sh "<game folder>"` on the Mac: `tools/build_runtime.sh`'s macOS branch passes
   `-DDF_EDITION -DR6_EDITION` and links `tools/build_app.sh` (`app/RainbowSix`); `./install.sh --game-dir` makes
   `~/Applications/Rainbow Six.app`. Completed on Apple Silicon, including the co-op executable and its launch window.
2. `src/renderer/renderer.m` / `shaders.metal`: keyed-blit mode 3 (`fs_quad`), fixed-function transforms and training
   render on Metal. The viewport and portal fixes described above are retained.
3. `cdaudio.c` opens its own `ds_audio_open` stream (Core Audio on the Mac). The visual audit mutes audio; it does not
   establish music/effects mixing parity.
4. `net_ole.c`: the default-route address is found with a connected UDP socket's `getsockname` (works on macOS); macOS
   asks for local-network permission (Info.plist has `NSLocalNetworkUsageDescription`).

## Known gaps

- Only M01 co-op with 2 players and training were played; adversarial, more players and the campaign are untested.
- LAN discovery (the server list) between the native build and Wine on one machine is untested because the test ports
  differ per instance; manual join works. Between machines, use the standard ports.
- Voice chat is off (no Voxware codecs); the original under Wine has none either.
- The music level options (`ActionMusicVolume`) are not applied to the CD stream.
- (resolved) Frame rate: the game steps by real elapsed milliseconds, so the host no longer caps it at 60; the same AI
  walk took the same time at 60 and 287 fps. Auto follows the display; the launcher passes the mod's R6CoopFrameRate
  as `DSR_FPSCAP` (Unlimited = `DSR_FPSCAP=0 DSR_VSYNC=0`).
- Crash/hang reports still say "Dungeon Siege Native" and name the image DungeonSiege.exe (inherited, cosmetic).
- (re-checked 2026-10-10, Mac) The `tools/analyze.py`/`lift.py`/`pe.py` changes (extra code sections, zero-run
  cutoff, slack bytes, C++ continuations, smc branch lifted as written) against the Delta Force builds: both games and
  their DLLs analysed and lifted with the tools before them (47f0a3d) and after, the lifted C compared function by
  function. DF1 (Watcom, one code section): analysis identical (5263 functions); the only change is two "smc branch"
  instructions, both data (a string at 0x491f14, a MASM variable block at 0x499e7f), now lifted as written instead of
  `rt_unhandled`. DF2 has three code sections (`.text`, `_text`, `_TEXT`), so the zero-run cutoff and the slack apply:
  2663 -> 2643 functions, coverage 94.6% -> 83.6% of the (larger) span; every instruction lost is zero-filled data,
  alignment padding before it, a string, or two unreferenced debug routines at 0x4ab641/0x4ab6ab that only junk
  fall-through reached (misaligned); no shared function changed except two data runs that now end in `rt_unhandled`;
  2 smc fields fewer, both in data. The DLLs (Netsock, Cmprssh0, Hw_d3d) each gain one unreferenced junk function at
  the end of `.text`'s VirtualSize (a gap seed in the slack). No C++ continuation was found in either game. Both games
  were then played into a mission in test mode: DF1 Peru (16M colours), DF2 "Six of One" in Software and Hardware.

## Mac visual and performance review (2026-10-10)

The original-game reference is stock GOG 1.04 under Wine 11.19 on CachyOS with an RTX 3080 Ti, not native Windows.
The stock executable SHA256 is `556ed69b7029d58852458afe1a11ce3aa95a3cf16d2bca43e5f322c6c8aec8a4`; the local stock
copy and all 92 listed data assets match. The installed co-op executable is a different, patched file. Reference
coverage is 640x480 Shooting Range I and the roof-open Single Room I training partitions, including night vision.
The reference host selected 640x480 in the UI after an inherited 1920x1080 setting failed; the actual capture dimensions and
UI establish the resolution. Force Software is off, but the exact fallback driver is not established. There is
no water or enclosed campaign-interior reference in this set.

The white vertex fallback now supplies all 16 bytes copied by Metal. Uploads grow the buffer when necessary, using
64-bit extents, and both draw paths reserve their complete uploads before binding encoder state. This preserves
the earlier fixed-function triangle-fan ordering fix. The generic programmable path also needed protection from
wrapping its constants after pipeline binding; Rainbow Six itself uses fixed-function rendering. Default buffer
capacity remains 256 MB; `DSR_RINGMB=4` enables development stress. `recomp/tests/metal_regressions.sh` runs actual
Metal checks under ASAN/UBSAN for source bounds, oversized uploads, GPU copy, wrap and draw pixels. Before/after
checks reproduce the old overread and out-of-range allocation; isolating the old programmable path with a safe
initializer also reproduces its lost pipeline, rejected before an invalid GPU draw reaches the driver.

The frame limiter now sleeps briefly before a short final spin instead of spinning for up to the last 2 ms.
`recomp/tests/frame_wait_test.c` separates wait CPU cost from the renderer/game workload. No simulation, math,
projection, portal or shader behavior changes are part of this update.

On this M1 Pro, the isolated wait benchmark reports 60 Hz CPU cost of 1.280 to 0.198 ms/frame and 120 Hz cost
of 1.045 to 0.113 ms/frame. This is not a full-game benchmark. Both old and revised co-op builds hold about
120 submitted FPS in the static 640x480 range view, GPU about 0.6 ms. Single 15-second whole-process CPU
observations are 23.79% and 23.46% of one core: essentially unchanged, so no live FPS or CPU improvement is
claimed. This older host reports submitted FPS, not separate on-screen presentation timestamps.

The `DS_TEST` relative-mouse action now distributes integer pixel totals over its 60 frames. Repeated floating
division could retain a whole pixel in its fractional accumulator (a request of 65 delivered only 64), shifting
comparison cameras. This changes scripted tests only. The reference host's recorded range setup also includes an upward
mouse adjustment; unadjusted starting views look down. Captures require camera and message-timing qualification,
not blanket pixel-identical claims.

The separate stock build reproduces the range's sky, lamps, wooded perimeter and gravel, plus the room's lit/dark
wall faces, floor, doors and green night vision. Both stock and co-op training runs exit normally. The stock
4 MB run covers over 230,000 draws with zero reported drops; checked quiet intervals remain about 120 submitted
FPS, with GPU roughly 0.6–1.3 ms across these views. A close-wall night-vision capture taken about half a second
after toggling is almost black; later captures at the same position become green as the transition settles.
The transition capture is retained as timing evidence, not reported as a missing shader. Audio is muted, and
campaign, co-op networking and water are not covered by this visual run.

The signed installed co-op update also runs the range at 3456x2160 with an initial 4 MB buffer, exercising growth
for the larger resources. Corrected active-scene checks show about 60 submitted FPS at a 60 cap, roughly
3.4–3.5 ms GPU, movement, firing (30 to 27 rounds), settled night vision and normal UI exit. Early ~24 FPS
samples were a paused start dialog targeted with unscaled test coordinates and are excluded from gameplay
performance. High-resolution pause-menu buttons use scaled coordinates; the main menu returns to 640x480.
