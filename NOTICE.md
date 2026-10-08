# Notices and credits

**The Sims Legacy Collection** © Electronic Arts. This project contains no part of the game. The build recompiles the
executable of a copy the user owns (bought on Steam), after verifying its checksum; it undoes the Steam wrapper on a
copy in the local build folder so the code can be read, and the app reads the game's data from the user's copy.

This repository started from the Dungeon Siege II / Fallout: New Vegas recompiler (Dungeon Siege and Fallout are ©
their respective rights holders; no part of those games is included either).

Included:
- **Vulkan-Headers** (Apache-2.0 / MIT, The Khronos Group): `recomp/third_party/vulkan` (headers and the `vk.xml`
  registry the Vulkan thunks are generated from), vulkan-sdk-1.4.341.0. https://github.com/KhronosGroup/Vulkan-Headers

Used at build time (installed into `recomp/.venv` with pip, not redistributed here):
- **Capstone** (BSD) — disassembly for the recompiler. https://www.capstone-engine.org/
- **Unicorn** (GPL-2.0) — x86 emulation, used only by the test suite to compare the recompiled code with the original.
  https://www.unicorn-engine.org/

Specifications and prior work this project learned from:
- Microsoft's published Open Specifications for DirectPlay 8 ([MC-DPL8CS], [MC-DPL8R], [MC-DPLHP]), from which the
  multiplayer's network protocol is implemented; `recomp/tests/dp8test.c` uses their example packets.
- **SiegeFX** (GPL-3.0) and **OpenSiege** (GPL-3.0), open-source reimplementations of the engine; no code was taken
  from them.
- `src/dsr/dsr_snapshot.bin` records the device capabilities reported by Wine's DirectDraw implementation (Wine,
  LGPL), so that the game sees a consistent Direct3D 7 device.
- The launch window uses Copperplate, a font that ships with macOS (not distributed here), and draws its artwork from
  the player's own game files at run time.
