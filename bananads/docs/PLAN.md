# BananaDS: the plan

Each milestone ends with a check. **Device** means it needs an RG DS or RG DS Plus on the desk (ssh as `root`,
the same as ROCKNIXDS's `docs/handoff-local.md`). Everything else can be done and checked on a PC.

| | Milestone | Status |
|---|---|---|
| A | Core and platform layer: melonDS pinned, Platform.h, saves, savestates, the mic gate, WFC DNS, local wireless, SDL and headless frontends, host tests, an aarch64 build | **done in 0.1.0** (PC + qemu; not on a device) |
| B | **Device:** the speed of melonDS on the handhelds | next |
| C | **Device:** sound and the mic through ALSA, in a game | code written (`alsa.cpp`), needs D to run |
| D | The KMS frontend: SuperDrastic's display path, evdev touch and pad | to do |
| E | SuperDrastic's features: in-game menu, shaders, toasts, RetroAchievements, CPU governor, volume card, quit hotkeys | to do |
| F | Hi-res 3D (2x, 3x) | decided after B |
| G | **Device:** Wi-Fi with real games, online and between two handhelds | to do |
| H | ROCKNIXDS: BananaDS as a core next to DraStic in the menu | to do |

## B. How fast is it? (the first device check)

**The kit for this is ready:** `docs/DEVICE-TEST.md`. `device-test.sh` runs everything below, plus a
correctness pass on real hardware, and writes the report. The commands below are what it automates.


DraStic at 2x is the bar: 60 fps in real games on the RG DS. melonDS renders 3D at 1x in software and runs the
CPUs through its JIT.

```sh
# on the handheld, with an aarch64 build copied to /storage/bananads
cd /storage/bananads
for l in 1 5 10; do ./bananads --frontend headless --set headless.frames=1200 --set wifi.dns=off \
    --set save.dir=/tmp dsstress-L$l.nds 2>&1 | grep headless; done
./bananads --frontend headless --set headless.frames=3600 --set save.dir=/tmp "/storage/roms/nds/<a game>.nds"
```

`dsstress-L*.nds` come from `stressrom/build.py`. `headless` runs flat out, so "% of a DS" is the headroom: above
100% the game can hold 60 fps with that much CPU to spare. Record it at 1104 MHz and at full clock, with
`render.threaded=1` and `0`, `jit=on` and `off`, for the stress ROM and three real games (a 2D one, Pokémon
HeartGold, a heavy 3D one), on both handhelds. Results go into `docs/results-B.md`.

- **Above 100% at 1x in real games:** go on with D and E. F decides whether hi-res is possible too.
- **Below:** profile first (`perf` is on ROCKNIX). The likely costs are the software renderer's 3D (threaded
  helps), the 2D compositor and the JIT's memory path (`FastMemory`).

## C. Sound and the mic

On the device, once D can show a game: the sound has no crackle over 10 minutes (`audio.latency_us`, 30 ms like
SuperDrastic's `DSFLIP_ALSA_LATENCY`). Then a game that uses the mic:
- **Nintendogs:** a name spoken back.
- **Zelda: Phantom Hourglass:** blowing out the candles.
- **WarioWare: Touched!:** the microgames you blow into.

The game's own sound must not set off the mic at any volume. `log.level=0` logs the gate's level, floor and bar.

## D. The KMS frontend

Port `dsflip.c`'s display path (docs/PORTING.md) as `src/frontend_kms.cpp`, keeping the same settings where they
mean the same thing (`display.top` = `DSFLIP_TOP`, `display.queue`, the latch). It also needs evdev for the pad
and touch: the touch mapping is absolute now, with no relative-mouse workaround. The panels are 640x480 (RG DS)
or 1024x768 (RG DS Plus); the DS screen is scaled by the plane (integer or exact, as SuperDrastic does), or by a
shader in E. Handover from sway/ES works the way SuperDrastic's does (`DSFLIP_HANDOVER`, docs/INTEGRATION.md).

## E. SuperDrastic's features

The order: quit hotkeys and resume (players need them first), then the in-game menu (with the Microphone page,
the Wi-Fi page: DNS preset, host/join, and the three savestate slots with pictures), toasts and volume, shaders,
RetroAchievements, then the governor last (it needs the rest to measure).

## F. Hi-res 3D

melonDS has two hi-res renderers, OpenGL (desktop GL 3.2) and compute shaders (GL 4.3). The handhelds have
Mali GPUs with GLES 3.2, so neither runs as it is. The options, to choose with B's numbers:
1. Port melonDS's OpenGL renderer to GLES 3.x (its shaders are small; the hard part is the 2D compositor on the
   GPU). The CPU is left for the JIT.
2. Gengis Engine as a melonDS `Renderer3D` (docs/PORTING.md): NEON on the CPU, already measured faster than
   DraStic's renderer. It needs an adapter, its own walker (PR #8's `walk.c`, `hr.c`) and a hi-res compositor.
3. 1x only, with ds-fsr or ds-crisp scaling it up (E).

## G. Wi-Fi

- **Online:** with `wifi.dns=kaeru` (or `altwfc`, `wiilink`), Mario Kart DS gets to its Nintendo WFC menu and finds
  a race. Pokémon Platinum/HGSS reach the GTS. The setup happens in each game's Nintendo WFC settings, and needs
  a connection made once. `log.level=0` shows every DNS query going to the WFC server.
- **Local:** two handhelds on one network, `wifi.local=host` and `wifi.local=join:<address>`. Mario Kart DS's
  multi-card play, a Pokémon trade, PictoChat.
- Worth checking: whether slirp's polling costs much CPU on the A55 (it runs on the emulator's thread). If it
  does, SuperDrastic's `wfcnet.c` is the lighter fallback.

## H. ROCKNIXDS

A `bananads` core next to `drastic-sa` in ES, chosen per game. The ES feature options map onto BananaDS settings
(mic threshold, WFC DNS, save folders). `session.sh`'s launch and watchdog are reused, with the same logs
(`/storage/.config/bananads/bananads.log` beside `dsflip.log`). Perf logs keep the ROCKNIXDS #57 format, with the
emulator recorded as BananaDS. The ROCKNIXDS Store (#55) is a way to ship it before it is in the image.
