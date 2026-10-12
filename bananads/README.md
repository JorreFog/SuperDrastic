# BananaDS

**A standalone DS emulator for Linux handhelds, with Wi-Fi and a real microphone.** BananaDS takes everything
SuperDrastic and ROCKNIXDS learned about running DS games on the Anbernic RG DS and puts it around an emulator
core that is open source: [melonDS](https://melonds.kuribo64.net). DraStic is not involved.

> Status: **0.1.0, the first scaffold. Not tested on a handheld yet.** It builds for the PC and for aarch64,
> boots SuperDrastic's 3D stress-test ROM (on a PC and under qemu with the ARM64 JIT), and passes its host tests.
> Its handheld display path (`frontend=kms`, milestone D) runs on BananaLinux; it has been checked in QEMU there,
> not yet on the panels themselves.

## Why a new emulator, and why melonDS

SuperDrastic gets the most out of DraStic, but DraStic is closed source and comes as one fixed binary
(r2.5.2.2). Two things on the owner's list can't be done from outside it:

- **Wi-Fi.** DraStic doesn't emulate the DS's Wi-Fi. SuperDrastic's WFC work (SuperDrastic `1.6-prep`, parked)
  built an access point and a NAT around it. Then it found that DraStic's recompiled ARM7 writes Wi-Fi registers
  through `store_io_register_arm7_*` and never sends a packet. No game ever connected.
- **The microphone.** DraStic has no microphone input, only a "fake microphone" key that plays a WAV file. So the
  RG DS's mic could only press that key when it heard a blow (ROCKNIXDS #26). Speech, claps and the length of a
  blow never reached the game.

melonDS emulates both: the Wi-Fi hardware with an access point (WFC through libslirp, and local wireless
between consoles), and the mic as real PCM. It also has an ARM64 JIT, needs no BIOS or firmware files (FreeBIOS
and a generated firmware, as with DraStic), and is GPL-3.0, so BananaDS can be shipped, changed and fixed in the
open.

What we reverse-engineered from DraStic stays where it belongs: in SuperDrastic, for DraStic. Nothing from
DraStic's binary is copied into BananaDS. Its reverse-engineered internals (offsets, structures) can't be reused
either, because melonDS's internals are different. What carries over is SuperDrastic's **own code and
measurements**: the KMS display path, the frame pacing, the audio pump, the mic gate, the shaders, the in-game
menu, RetroAchievements, the CPU governor. [docs/PORTING.md](docs/PORTING.md) is the file-by-file map.

## What's in 0.1.0

| | |
|---|---|
| Core | melonDS at a pinned commit (`CMakeLists.txt`), JIT on (`jit=off` for the interpreter), software 3D at 1x |
| Microphone | Real PCM from the mic (ALSA or SDL), with SuperDrastic's gate in front of it: DC blocker, noise floor, echo gate (the RG DS's speakers sit next to its mic), hysteresis and minimum hold. A "blow" for the in-game menu (F1 in the window) works without a mic. `src/mic.cpp` |
| Wi-Fi (internet) | Nintendo WFC revivals through slirp: no root, no tun device. `wifi.dns` = `kaeru` (default), `altwfc`, `wiilink`, any address, `system` or `off` (the same presets as ROCKNIXDS's `nds.wfc_dns`). Each handheld gets its own MAC from `/etc/machine-id` (DraStic gives every device 00:01:02:03:04:05). `src/wifi.cpp` |
| Wi-Fi (local) | Two handhelds on one network play together through melonDS's LAN: `wifi.local=host` on one, `wifi.local=join:<address>` on the other (build with `-DBANANADS_LAN=ON`, needs libenet) |
| Saves | `<game>.sav` next to the ROM or in `save.dir`, per-game folders (`save.per_game_folder`, ROCKNIXDS #42/#56). Always written to a temporary file and renamed, a second after the game's last write |
| Savestates | Slots 1-3 (F5-F7 save, F9-F11 load in the window) and the resume state (slot 0) written on quit, loaded on start (`resume.on_quit`, `resume.on_start`). Each has a `.pic` with both screens for the menu's slot pictures |
| Frontends | `sdl`: a desktop window for development. `headless`: frames as fast as possible, a benchmark that dumps PPM pictures. `kms`: the handheld path: both screens straight onto the two panels through DRM/KMS (one atomic commit a frame, the display plane scaling, frames queued one refresh), the pad and the touchscreen from evdev, ALSA sound, SIGTERM saves and quits (`src/frontend_kms.cpp`). The default without a display server |
| Sound | SDL in the window. `src/alsa.cpp` is the handheld's: real-time output thread with 256-frame chunks, the PCMs opened one at a time (the PipeWire crash from #26). `frontend=kms` uses it |

## Building

```sh
cmake -S bananads -B build-bananads            # fetches melonDS at the pinned commit
cmake --build build-bananads -j
./build-bananads/bananads-tests                # host tests, no ROM needed
./build-bananads/bananads --frontend headless --set headless.frames=600 game.nds
```

Needs CMake 3.16+ and a C++17 compiler (libslirp comes with melonDS), and optionally SDL2 (`-DBANANADS_SDL=OFF` without),
ALSA (`-DBANANADS_ALSA=OFF`) and libenet (`-DBANANADS_LAN=ON`). For the handhelds,
`-DCMAKE_TOOLCHAIN_FILE=bananads/cmake/aarch64-linux-gnu.cmake`; `-DBANANADS_MELONDS_DIR=<checkout>` builds
against a local melonDS. Settings: [docs/SETTINGS.md](docs/SETTINGS.md).

## Where it's going

[docs/PLAN.md](docs/PLAN.md) has the milestones. The first one that needs a handheld (B) decides how fast this
can go: melonDS's 1x speed on the RG DS's Cortex-A55s, against DraStic at 2x. [TRACKING.md](TRACKING.md) lists
the work in progress in SuperDrastic and ROCKNIXDS, and what each piece means for BananaDS.

## License

BananaDS is GPL-3.0-or-later (`LICENSE`), because it is built on melonDS (GPL-3.0-or-later). The rest of this
repository (SuperDrastic) stays MIT; SuperDrastic code ported into `bananads/` comes under the GPL there, which
MIT allows. BananaDS contains no games, BIOS or firmware: use only your own
dumps, as ROCKNIXDS's LEGAL.md says.

Credits: the [melonDS team](https://github.com/melonDS-emu/melonDS) for the core, libslirp and ENet; and everyone
credited in SuperDrastic's README for the work this frontend comes from.
