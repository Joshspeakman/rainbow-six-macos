# The Sims Legacy Collection: port notes

Started 2026-10-07 from `fallout-nv-linux` (the Dungeon Siege II recompiler with SSE lifting, the SDL Linux host and
the Metal Mac host). Development is on Linux; the Mac build comes from the same tree.

## The game

| | |
|---|---|
| Steam app | 3314060, build 17905164, `Sims.exe` SHA-256 `b7c8765e…42e3` (`build_sims.sh` checks it) |
| Executable | MSVC 2022, linked 2025-03-28, 32-bit, base `0x400000`, image end `0xb02000`, static TLS, SEH4 |
| Wrapper | SteamStub 3.1 (`.bind`): `.text` AES-256-CBC. `tools/steamstub.py` unwraps a copy (SHA-256 of the result `526add76…083b`, `editions.py` `sims`) |
| Code | 40,022 functions, 96.1% of `.text`; SSE2 throughout, SSE4.1/AVX only behind CPU checks (the runtime reports an SSE2 CPU) |
| DLLs | `gimex.dll` (EA image codecs) and `ijl10.dll` (Intel JPEG, links the system MSVCRT), both loaded by name; both want `0x60000000`, so ijl10 is lifted at `0x61000000` |
| Graphics | Vulkan 1.0 + swapchain, 113 commands resolved through `vkGetInstanceProcAddr`; the game draws its sprites on the CPU, uploads them (`vkCmdCopyBufferToImage`) and composites them (about 100 draws a frame) |
| Other | DirectSound (`dsound.dll` by name), crashpad (switched off), WinINet telemetry (offline), Steamworks flat API + interfaces |

Data: the first run copies `TemplateUserData` to `Saved Games\Electronic Arts\The Sims 25\UserData` (and
`UserData2`–`8`) with std::filesystem.

## Where things are

- `recomp/build_sims.sh`: verify, unwrap, analyse, lift (exe and DLLs), compile, link. `SIMS_RELINK=1` relinks only.
- `recomp/tools/sims_iterate.sh`: relink and one test-mode run (`DATA=`, `T=`, `NOLINK=1`, `SIMS_GAME=`), log in
  `recomp/work/run.log`. Use a fresh `DATA` folder after a run that crashed during the first-run copy.
- `recomp/tools/steamstub.py`: SteamStub 3.1 x86 unwrapping (header at entry − 0xF0, XOR-chained; key, IV and
  "stolen" first ciphertext block in it). `cryptography` if installed, else `openssl enc`.
- `recomp/tools/lift.py --game sims`: no DS2 hooks; the override at `0x467020` (crash reporting); SEH4 support
  (`_except_handler4` at `0x7af450`, `__SEH_prolog4` `0x7a94e0`, `_GS` `0x944940`).
- `recomp/runtime/edition.h` `SIMS_EDITION`; `runtime/win32/sims_imports.c` (Vista synchronisation, wide APIs,
  filesystem, monitors, menus, WinINet, timers, APCs, ...), `steam_api.c`, `msvcrt.c`.
- Vulkan: `tools/gen_vulkan.py` → `runtime/win32/vulkan_gen.{c,h}` (regenerate after editing the generator or
  `vulkan_commands.txt`: `python3 recomp/tools/gen_vulkan.py recomp/third_party/vulkan/registry/vk.xml
  recomp/runtime/win32/vulkan_commands.txt recomp/runtime/win32/vulkan_gen.c`); `runtime/win32/vulkan.c`;
  `runtime/win32/vulkan_rt.h`; headers and registry in `third_party/vulkan` (vulkan-sdk-1.4.341.0).
- `recomp/host/linux.c` under `SIMS_EDITION`: Vulkan window, `w32_host_vk_*` hooks (loader, surface extensions,
  surface, presented), absolute cursor, an event loop instead of the DSR command ring.

## Design notes

**Vulkan across the 32/64-bit boundary.** Guest structures are MSVC x86: pointers, `size_t` and dispatchable
handles 4 bytes; `VkDeviceSize` and non-dispatchable handles 8 bytes, 8-aligned (`/Zp8`); stdcall arguments are
dwords (a 64-bit argument is two, unpadded). Structures without pointers, `size_t` or dispatchable handles have the
same layout on both sides and are used in place in guest memory (41 of 104); the rest are converted into a per-call
arena. Dispatchable handles are 16-byte guest allocations mapped to host pointers. Strings, scalar arrays and blittable
arrays are passed as pointers into guest memory. Unknown `pNext` structures are logged once and left out.

**Mapped memory.** Each allocation is mapped whole the first time and stays mapped. With `VK_EXT_map_memory_placed`
(NVIDIA on Linux has it) the driver maps it at a guest address reserved with `vm_alloc`; unmapping keeps the range
reserved (`VK_MEMORY_UNMAP_RESERVE_BIT_EXT`) and gives it back to the guest. Without it (`W32_VK_SHADOW=1` forces
this; expected on MoltenVK) the guest gets a copy: written to the device memory before every `vkQueueSubmit` and at
flushes; read back at invalidations, and for host-cached memory after successful fence/queue/device waits. The game
never reads GPU results back in what's been seen so far (no `vkCmdCopyImageToBuffer`).

**Instance and device.** `vkCreateInstance` swaps `VK_KHR_win32_surface` for the host's surface extensions (SDL on
Linux), adds portability enumeration where present (MoltenVK), raises the API version to 1.1 and drops unavailable
extensions and layers. `vkCreateDevice` drops unavailable extensions and features (logged) and adds
`VK_KHR_map_memory2` + `VK_EXT_map_memory_placed` when available, and `VK_KHR_portability_subset` on MoltenVK.

## Verified (2026-10-07, Linux, RTX 3080 Ti, NVIDIA 615)

- Start-up through Steam, CPU detection, locale, window, Vulkan device ("mapped memory placed in guest memory"),
  swapchain 1024×768 B8G8R8A8_UNORM FIFO.
- First run: the template user data copies completely; the neighbourhood screen draws with all its houses.
- The family button opens Select a Family (portraits drawn) with its "Create a Family?" dialog.
- Clicking a lot in the neighbourhood opens it: an empty lot (build grid, the live/build/buy panel, "Paused", the cab
  company notice) and an occupied one (house with pool, fences, flower beds; the Magic Town notice).
- Runs reach the test timeout (60–100 s) without game faults (the host's teardown on the kill is item 5 below).

## Not working / next steps

1. **Live mode.** Dismiss the notices and let a family play: Sims (3D characters) walking, needs, objects in use,
   time passing; then build and buy mode. One oddity: clicks on the marked Newbie house's roof (700,338 in the
   1024×768 test window) do nothing while other lots open; the hover tooltip never appears in shots either (the
   system cursor isn't in the swapchain). The real-window mouse path (absolute SDL coordinates) is written but has
   only been exercised through the test script.
2. **Sound.** DirectSound comes from the shared runtime; tests run with `W32_AUDIO_MUTE`. Check with `W32_AUDIOLOG=1`
   that music and effects reach the mixer.
3. **The Mac host** (`recomp/host/main.m`, `build_runtime.sh` on Darwin): a Vulkan path like Linux's: MoltenVK
   (bundle `libMoltenVK.dylib` in the app, or fetch it at install; it exports `vkGetInstanceProcAddr`), a
   `CAMetalLayer` view and `VK_EXT_metal_surface` for `w32_host_vk_surface`, `w32_host_vk_extensions` returning
   `VK_KHR_surface` + `VK_EXT_metal_surface`. Check whether MoltenVK offers `VK_EXT_map_memory_placed`; if not, the
   shadow path is used. `-I third_party/vulkan/include` for the runtime, `-lz` for screenshots. 64-bit atomics on
   arm64 need 8-byte alignment (`lock cmpxchg8b`). Diff `host/main.m` and `src/renderer/renderer.m` against
   `fallout-nv-macos` first: this tree's Mac files may be behind.
4. **Installer and launcher.** `install.sh` and the Linux launcher are still Dungeon Siege II's / Fallout NV's: find
   the Steam folder (libraryfolders.vdf), run `build_sims.sh`, install the app and a data folder.
5. **Exit.** Killing the host while the game runs segfaults in teardown (the Vulkan device is still live).
6. **Remaining imports** (`python3 recomp/tools/import_gap.py recomp/work/sims/Sims.exe`): 58, mostly unused by
   what's been run (threadpool timers/waits, named pipes, MSVFW32 video codecs, OLEAUT32 variants, font
   enumeration). Implement as they're reached; each stops with its name.
7. **Lifter gaps** (stop with their address if ever reached): SSSE3/SSE4.1 (`pshufb`, `pmulld`, `pblendvb`, ...)
   and AVX in CPU-dispatched paths, `enter` with nesting, segment registers in junk decodes.

## Ground rules

Own copy only: the build reads the player's Steam files and unwraps the Steam DRM locally, in the build folder; Josh
confirmed (2026-10-07) that the guides' "no DRM removal" means no building around copies people don't own. Nothing
derived from the game is committed (`recomp/work/` is ignored; stage explicit paths). No Wine or emulation at run time.
