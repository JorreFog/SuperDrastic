# The core: melonDS, pinned

`CMakeLists.txt` pins melonDS to one commit (`BANANADS_MELONDS_COMMIT`). BananaDS doesn't patch melonDS: it uses
melonDS's public pieces, `src/` as the `core` library and `src/net` for slirp and LAN. Everything BananaDS needs
from outside goes through `Platform.h`, which `src/platform.cpp` implements. Keeping melonDS unpatched is what
makes moving to a newer commit cheap.

| Pinned | Date | Why |
|---|---|---|
| `906e9ebb27da8c6a715cd7abab4abfe8a8d29427` | 2026-08-24 | the newest master when BananaDS began (2026-10-11) |

## Moving to a newer commit

1. Read melonDS's commits since the pin for changes to `Platform.h`, `NDS.h`, `Args.h`, `GPU.h`, `SPU.h`,
   `net/` and `SPI_Firmware.h`, the interfaces BananaDS uses.
2. Change `BANANADS_MELONDS_COMMIT`, build for the PC and for aarch64, and run `bananads-tests`.
3. Run the stress ROM headless on both (README) and compare its pictures with the last pin's: the same scene
   at the same frame should come out the same.
4. Add a row to the table above.

## What BananaDS leaves out of melonDS

The Qt frontend, the OpenGL and compute renderers (desktop GL; the handhelds have GLES, see PLAN.md F), the GDB
stub, netplay's UI, pcap (slirp needs no root), DSi mode (DS games only, as with DraStic; melonDS's DSi needs
real DSi BIOS and NAND dumps).
