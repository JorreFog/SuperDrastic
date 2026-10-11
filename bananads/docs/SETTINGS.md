# BananaDS settings

In `bananads.conf` (`key = value`, `#` for comments), found in `$BANANADS_DATA`. That is
`/storage/.config/bananads` when it exists, the current directory otherwise; `--config` names another file. Every
key can also come from the environment as `BANANADS_<KEY>` (dots become underscores, upper case:
`BANANADS_WIFI_DNS=altwfc`), which wins over the file, and from `--set key=value`, which wins over both.

| Key | Default | |
|---|---|---|
| `frontend` | `sdl` (or `headless` in a build without SDL) | `sdl`, `headless`; `kms` comes with milestone D |
| `jit` | `on` | melonDS's JIT; `off` for the interpreter (slow, for debugging) |
| `jit.timing` | `block` | BananaJIT's timing: `block` (melonDS's JIT's level: fast memory, cycles counted per block) or `exact` (the interpreter's, cycle for cycle; slower) |
| `core.slice` | `64` | the scheduler slice: the most system cycles (33.5 MHz) the ARM9 and then the ARM7 run before they are interleaved again (melonDS's `kMaxIterationCycles`, patches/melonds/0003). `64` is melonDS's. Longer is faster (on the RG DS, 256 gave +8.5% and 512 +10% in HeartGold) and an accuracy trade: coarser ARM9/ARM7 interleaving, each CPU seeing the other's writes up to a slice late, which matters for IPC handshakes and anything timing-sensitive between the CPUs; scheduled events (timers, DMA, video, IRQs from them) stay on time, except ones the ARM7 starts inside a slice, which run at its end. `auto`: adaptive, `64` near IPC and `core.slice_idle` away from it. Test many games before changing the default. An event due within 8 cycles of a slice's end (`kIterationCycleMargin`) ends it instead |
| `core.slice_idle` | `512` with `auto`, `core.slice` otherwise | the slice while neither CPU has touched the IPC registers (IPCSYNC, IPCFIFOCNT, IPCFIFOSEND, IPCFIFORECV) or WRAMCNT for `core.slice_hold` cycles; any value other than `core.slice` makes the slice adaptive. Handshakes through main RAM alone aren't seen (fast memory), and the first IPC access after a quiet spell still lands in a long slice |
| `core.slice_hold` | `4096` | how many system cycles (4096: about two scanlines) after an IPC access the short slice stays |
| `core.slice_stats` | `0` | every this many frames, log the IPC accesses per frame and the share of time near one (the short slice's share), for tuning `auto` per game |
| `core.fast_gxdma` | `off` | DMA words from main RAM to the geometry FIFO go straight to the 3D engine instead of through melonDS's bus and IO dispatch (patches/melonds/0004). Exact: the same results and timing; off until measured on the handheld |
| `core.fast_spu` | `off` | the mixer skips sound channels that are off (patches/melonds/0004). Exact; off until measured |
| `core.fast_sched` | `off` | the scheduler keeps the time of its next event instead of walking its event list twice every slice (patches/melonds/0006). Exact; off until measured |
| `render.2d` | `banana` | the 2D renderer: `banana` (the Banana 2D engine, gpu2d/: melonDS's picture, pixel for pixel, in about a third of the time) or `melonds` (melonDS's own) |
| `render.renderer` | `gengis` | the 3D renderer: `gengis` (Gengis Engine, SuperDrastic's rasterizer, on several threads; aarch64 builds) or `soft` (melonDS's software renderer) |
| `render.gengis_threads` | `3` | Gengis Engine's render threads (the emulator's thread also renders a bin when it has to wait for one) |
| `render.gengis_defer` | `off` | Gengis Engine's deferred shading of opaque polygons |
| `render.threaded` | `on` | melonDS's software 3D renderer on its own thread (`render.renderer=soft`) |
| `save.dir` | the ROM's folder | where `<game>.sav` lives |
| `save.per_game_folder` | `off` | `<save.dir>/<game>/<game>.sav` (ROCKNIXDS #42/#56) |
| `state.dir` | the save folder | savestates: `<game>.resume.mst` (the quit/resume state) and `<game>.<1-3>.mst`, each with a `.pic` |
| `resume.on_quit` | `on` | save the resume state when the game is quit |
| `resume.on_start` | `on` | load it when the game starts |
| `firmware.nickname` | `BananaDS` | the DS's user name (10 characters at most; games and WFC show it) |
| `firmware.language` | the firmware's | 0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish |
| `rtc.fixed` | none | the DS clock's start, `YYYY-MM-DD HH:MM:SS` (or `T` or `_` for the space), instead of the current time: two runs of a game that seeds its random numbers from the clock then go the same way (for tests) |
| `wifi.dns` | `kaeru` | `kaeru`, `altwfc`, `wiilink`, an IPv4 address, `system` (the handheld's own DNS) or `off` (no Wi-Fi at all) |
| `wifi.local` | `off` | `host`, or `join:<the host's address>`: local wireless with another handheld (builds with `BANANADS_LAN`) |
| `wifi.local_players` | `4` | how many can join a host |
| `mic.source` | `device` | `device` (ALSA or SDL capture) or `none` (only the menu's Blow) |
| `mic.device` | `default` | the ALSA capture device |
| `mic.threshold` | `0.15` | how far above the room's noise a sound must be to reach the game (full scale 1). ROCKNIXDS's high / medium / low are 0.03 / 0.15 / 0.3. `0` turns the gate off |
| `mic.gate` | `3` | the echo gate's factor over the learned speaker leak; `0` turns it off |
| `mic.coupling_max` | `4` | the most leak it will learn |
| `mic.hold_ms` | `120` | the shortest time the gate stays open |
| `audio.out_device` | `default` | the ALSA output device |
| `audio.latency_us` | `30000` | ALSA's buffer |
| `audio.realtime` | `on` | the sound thread at SCHED_FIFO 10, when allowed |
| `sdl.scale` | `2` | the window's size |
| `headless.frames` | `600` | how many frames `headless` runs |
| `headless.dump_dir`, `headless.dump_every` | none, `60` | write both screens as `frame<N>.ppm` every N frames |
| `headless.measure_from` | `0` | also report the fps from this frame on (a game's scene, after loading) |
| `headless.input` | none | scripted presses: `<frame>:<keys>[:<frames held>]`, comma-separated, keys joined by `+` (`a b select start right left up down r l x y`), held 6 frames by default: `600:start,900:a:10` |
| `log.file` | none | a copy of the log (stderr always gets it) |
| `log.level` | `1` | 0 debug, 1 info, 2 warnings, 3 errors |

## The window's keys (`frontend=sdl`)

Arrows: the D-pad. X / Z: A / B. S / A: X / Y. Q / W: L / R. Enter: Start. Right Shift or Backspace: Select. The
mouse on the bottom screen: the stylus. F1: blow into the mic. F5-F7: save to slot 1-3. F9-F11: load slot 1-3.
Esc: quit (with the resume state).
