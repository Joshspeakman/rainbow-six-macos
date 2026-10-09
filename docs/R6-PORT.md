# Rainbow Six: port notes and Mac handoff

State as of 2026-10-09. Done and verified on Linux (CachyOS, SDL2/OpenGL host) against the original GOG executable
running under Wine in a headless gamescope (the `~/Projects/r6-coop/tools` scripts). The Mac side has not been compiled.

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

## Mac handoff: what remains

1. `recomp/build_r6.sh "<game folder>"` on the Mac: `tools/build_runtime.sh`'s macOS branch passes
   `-DDF_EDITION -DR6_EDITION` and links `tools/build_app.sh` (`app/RainbowSix`); `./install.sh --game-dir` makes
   `~/Applications/Rainbow Six.app`. Nothing of this has been compiled on macOS (the Delta Force Mac path hasn't either):
   expect small fixes in Linux-first runtime files.
2. `src/renderer/renderer.m` / `shaders.metal`: the keyed-blit mode 3 (`fs_quad`) is new and unbuilt. The world draws
   use the existing fixed-function path with transforms (slots 1-3), which DF never exercised on Metal: check the first
   frame of training against Linux.
3. `cdaudio.c` opens its own `ds_audio_open` stream (Core Audio on the Mac): check it plays alongside DirectSound.
4. `net_ole.c`: the default-route address is found with a connected UDP socket's `getsockname` (works on macOS); macOS
   asks for local-network permission (Info.plist has `NSLocalNetworkUsageDescription`).

## Known gaps

- Only M01 co-op with 2 players and training were played; adversarial, more players and the campaign are untested.
- LAN discovery (the server list) between the native build and Wine on one machine is untested because the test ports
  differ per instance; manual join works. Between machines, use the standard ports.
- Voice chat is off (no Voxware codecs); the original under Wine has none either.
- The music level options (`ActionMusicVolume`) are not applied to the CD stream.
- The Linux host caps the game at 60 fps (`DSR_FPSCAP=<n>` overrides); its speed above 60 was never compared with the
  original.
- Crash/hang reports still say "Dungeon Siege Native" and name the image DungeonSiege.exe (inherited, cosmetic).
- `tools/analyze.py`/`lift.py` changes (zero-run cutoff, slack bytes, C++ continuations) apply to the Delta Force builds
  too and have not been re-checked there.
