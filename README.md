# SuperDrastic

**Full-speed DraStic on Linux handhelds.** SuperDrastic is a library that runs alongside the Linux build of
[DraStic](https://drastic-ds.com) and replaces how its frames, sound and input reach the device: each DS screen
goes straight to its own panel, frames are paced to the panels' refresh, and it adds GPU shaders, a steady audio
pump, touch, the microphone, RetroAchievements and battery-aware clocks. Drop it into any Linux firmware in place
of the DraStic command.

It started as `libdsflip`, the DraStic engine of [ROCKNIXDS](https://github.com/JorreFog/ROCKNIXDS) (ROCKNIX on
the Anbernic RG DS), where it is the default DraStic launcher. This repository is that engine on its
own, for any firmware.

> SuperDrastic is not affiliated with DraStic or its author, Exophase. It doesn't include DraStic and doesn't
> modify it: it is loaded next to the DraStic your firmware already ships.

## What it does

- **Straight to the panels.** DraStic draws into the panels' scanout buffers (KMS, no compositor, no GL, no copy);
  the display controller scales them. DraStic's 2x internal resolution holds 60 fps where the stock SDL path drops
  (3D stress test, heaviest level: 50.4 fps against 39.0; real games hold 60 in play).
- **Even frame pacing.** Frames are committed at an adaptive point in the refresh cycle, with a one-frame queue that
  turns DraStic's uneven frame times into even display.
- **Shaders at full speed**: `ds-crisp` (exact pixel-area scaling), `ds-grid` (the DS LCD's grid), both also with the
  DS screen's colours, `ds-grid-2x`, `ds-integer` (pixel-perfect 2x in a bezel, touch follows) and `ds-fsr` (AMD FSR
  1.0 upscaling). The GPU reads DraStic's frames where they are (dma-buf), about 0.6 ms per panel.
- **An audio pump** that calls DraStic's audio at an exact rate on a real-time timer, which removes the stutter
  SDL's audio caused, and an echo-gated **microphone**.
- **Touch** on the bottom panel, mapped to exactly where the DS screen is drawn.
- **RetroAchievements** (softcore) with pop-ups, badges and a progress indicator, including sets that read the DS's
  DTCM memory, for `.nds` ROMs and ROMs in a `.zip`.
- **Its own CPU governor**: the lowest clock at which the game drops no more frames than at full clock (HeartGold at
  2x: ~1450-1660 MHz instead of 1992), and the GPU's clock chosen per shader.
- **DraStic's menu** on the bottom panel while the game stays on top, with the time and the battery on the top screen.
- **Quitting** with Start + Select or Menu + Start held (with a save to resume from, where the frontend wants one).

The measurements behind these numbers are in ROCKNIXDS's
[README](https://github.com/JorreFog/ROCKNIXDS#dsflip-drastic-straight-to-the-panels) and
[optimization report](https://github.com/JorreFog/ROCKNIXDS/blob/main/docs/optimization-1.4.md).

## Where it runs

| | Status |
|---|---|
| **Anbernic RG DS** (two 640x480 panels), ROCKNIX | Tested daily; [ROCKNIXDS](https://github.com/JorreFog/ROCKNIXDS) installs it with a DSi-style frontend |
| Anbernic RG DS, other Linux firmwares | Should work (see *Requirements*); not tested yet, reports welcome |
| Anbernic RG DS Plus (two 1024x768 panels) | In progress (an untested alpha lives in ROCKNIXDS's `plus-alpha` branch) |
| Single-screen handhelds | Planned for the next release: both DS screens laid out on one display |
| Android | Not possible, see below |

**Requirements:** the Linux DraStic build (r2.5.2.2, aarch64, SDL2), glibc 2.38 or newer, KMS with atomic
modesetting and two connected panels, and the right to take the display while a game runs (root, or the seat's
session). Optional: libcurl (RetroAchievements), zlib (RetroAchievements for zipped ROMs), EGL + GLES2 with dma-buf
import (shaders), ALSA (audio pump).

**Why not Android:** SuperDrastic works by loading into the *Linux* DraStic and driving the panels directly. Android's
DraStic is a different, paid app, which can't be extended from outside without root-level patching, and Android apps
can't drive the display themselves.

## Install

1. Download `superdrastic-<version>-aarch64.tar.gz` from the [releases](https://github.com/JorreFog/SuperDrastic/releases)
   and unpack it anywhere on the device, e.g. `/storage/superdrastic` or `/opt/superdrastic`.
2. Wherever your firmware starts DraStic, start `superdrastic-run` instead, with the same arguments after the path of
   DraStic itself:

   ```sh
   /storage/superdrastic/superdrastic-run /path/to/drastic /path/to/game.nds
   ```

3. If your frontend runs under a compositor that doesn't use seatd or logind, tell it how to stop and start it
   (`superdrastic.conf`, see `superdrastic.conf.example`):

   ```sh
   DSFLIP_HANDOVER=cmd
   DSFLIP_STOP_CMD="systemctl stop my-frontend"
   DSFLIP_START_CMD="systemctl start my-frontend"
   ```

`superdrastic-update` updates the folder to the newest release and keeps your settings. On ROCKNIX, ROCKNIXDS's
installer does all of this (and more).

**Firmware developers:** [docs/INTEGRATION.md](docs/INTEGRATION.md) is the full contract: how the display is handed
over, the verdict file, the log, every setting, exit hotkeys and fallbacks.

## Building

```sh
sh build.sh <aarch64-sysroot>          # build/libsuperdrastic.so; add "shtest" for the shader benchmark
```

Any machine with clang and lld can build it; the sysroot recipe is at the top of `build.sh`, and
[CI](.github/workflows/build.yml) builds and checks every push and packages releases. `stressrom/` builds the 3D
stress-test ROM used for the benchmarks, `tools/` has the test and measurement tools (the shader benchmark, touch and
pad injection, the display-path probe).

## Credits

- [DraStic](https://drastic-ds.com) by **Exophase**: the emulator itself. SuperDrastic only changes how its frames,
  sound and input reach the device.
- [GammaOS Nano](https://github.com/TheGammaSqueeze/GammaOSNext)'s **DraStic Nano** by **TheGammaSqueeze**: the core
  idea. It showed that DraStic's 2x mode runs at full speed on this hardware once GL and the compositor are out of the
  way.
- [DSperate](https://github.com/beebono/DSperate) by **beebono**: the RG DS measurements of SDL2's display-path cost,
  and its KMS/dma-buf presentation as a reference on this device.
- [ROCKNIX](https://github.com/ROCKNIX/distribution): the `drastic-sa` package, launch scripts and `libdrastouch`,
  whose touch handling showed how DraStic expects stylus input.
- [RetroAchievements](https://retroachievements.org) and [rcheevos](https://github.com/RetroAchievements/rcheevos).

## License

[MIT](LICENSE). The parts that come from others keep their own terms: [THIRD_PARTY.md](THIRD_PARTY.md).
