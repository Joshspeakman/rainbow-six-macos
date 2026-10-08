# Notices and credits

**Delta Force** and **Delta Force 2** © NovaLogic (now THQ Nordic). This project contains no part of either game. The
build recompiles the executable of a copy the user owns (bought on Steam), after verifying its checksum; it undoes the
Steam wrapper on a copy in the local build folder so the code can be read, and the app reads the game's data from the
user's copy. The recompiled program is for that user only.

This repository started from the Dungeon Siege II / Fallout: New Vegas / The Sims recompiler, and its DirectDraw and
Direct3D 6 layer (`recomp/runtime/win32/ddraw_pal.c`) comes from the Diablo II port. No part of those games is included
either.

Included:
- **Vulkan-Headers** (Apache-2.0 / MIT, The Khronos Group): `recomp/third_party/vulkan`, inherited from the Sims port.
- **MojoShader** (zlib), **minimp3** (CC0): `recomp/third_party`, inherited from the Fallout: New Vegas port.

Used at build time (installed into `recomp/.venv` with pip, not redistributed here):
- **Capstone** (BSD) — disassembly for the recompiler. https://www.capstone-engine.org/
- **Unicorn** (GPL-2.0) — x86 emulation, used only by the tests to compare the recompiled code with the original.
  https://www.unicorn-engine.org/

Prior work this project learned from:
- **Steamless** by atom0s (https://github.com/atom0s/Steamless) documents the SteamStub 2.1 format;
  `recomp/tools/steamstub21.py` is an independent implementation from that description, not a copy of its code.
- `src/dsr/dsr_snapshot.bin` records device capabilities reported by Wine's DirectDraw implementation (Wine, LGPL).
