# BananaDS settings

In `bananads.conf` (`key = value`, `#` for comments), found in `$BANANADS_DATA`. That is
`/storage/.config/bananads` when it exists, the current directory otherwise; `--config` names another file. Every
key can also come from the environment as `BANANADS_<KEY>` (dots become underscores, upper case:
`BANANADS_WIFI_DNS=altwfc`), which wins over the file, and from `--set key=value`, which wins over both.

| Key | Default | |
|---|---|---|
| `frontend` | `kms` without a display server (no `WAYLAND_DISPLAY` or `DISPLAY`), else `sdl` (`headless` in a build with neither) | `kms` (the handheld's panels), `sdl` (a desktop window), `headless` |
| `jit` | `on` | melonDS's JIT; `off` for the interpreter (slow, for debugging) |
| `jit.timing` | `block` | BananaJIT's timing: `block` (melonDS's JIT's level: fast memory, cycles counted per block) or `exact` (the interpreter's, cycle for cycle; slower) |
| `jit.codegen` | `new` | melonDS's JIT's code generation (patches/melonds/0007): `new` (fewer host instructions per block and per exit: cheaper exit stubs, linkable exits both ways of a block-ending branch, self loops checked inline, host flags kept in NZCV, a per-site cache for returns) or `old` (the code before 0007, byte for byte). Exact either way. Sets `MELONDS_JIT_OLDCODEGEN`; each part is also off alone with its `MELONDS_JIT_NO<name>=1` (docs/PERF.md lists them). `MELONDS_JIT_NOFASTLDM=1` keeps block loads (LDM, POP) off the fastmem path (0008). `MELONDS_JIT_LOOPREGS=1` turns on loops that keep their guest registers in host registers (off: HeartGold hangs with it, docs/PERF.md) |
| `jit.iofast` | `on` | direct routes for the JIT's slow memory paths: main RAM block transfers, VRAM, the GX FIFO and command ports, IE/IF/IME/DISPSTAT/VCOUNT/KEYINPUT reads (patches/melonds/0005). Exact. Sets `MELONDS_IOFAST`; `MELONDS_IOFAST_ROUTES` picks single routes, `MELONDS_IOSTATS=n` prints the slow accesses by address from frame n |
| `jit.vramfast` | `on` | VRAM with one bank at a 16 KB page written and read straight to the bank (with the dirty bit and the capture check), OAM and palette straight to the GPU (patches/melonds/0010). Exact. Bit 1 of `MELONDS_MEMFAST` |
| `jit.iodirect` | `on` | the divider, square root, IPCSYNC/IPCFIFOCNT, the IPC FIFOs and IME/IE/IF read and written straight to their state, other IO registers without the ARM9Read*/ARM7Read* dispatch, one function per hot register for the JIT's static-address calls (patches/melonds/0010). Exact. Bit 2 of `MELONDS_MEMFAST` |
| `core.slice` | `64` (`auto` at `render.scale=2`) | the scheduler slice: the most system cycles (33.5 MHz) the ARM9 and then the ARM7 run before they are interleaved again (melonDS's `kMaxIterationCycles`, patches/melonds/0003). `64` is melonDS's. Longer is faster (on the RG DS, 256 gave +8.5% and 512 +10% in HeartGold) and an accuracy trade: coarser ARM9/ARM7 interleaving, each CPU seeing the other's writes up to a slice late, which matters for IPC handshakes and anything timing-sensitive between the CPUs; scheduled events (timers, DMA, video, IRQs from them) stay on time, except ones the ARM7 starts inside a slice, which run at its end. `auto`: adaptive, `64` near IPC and `core.slice_idle` away from it. Test many games before changing the default. An event due within 8 cycles of a slice's end (`kIterationCycleMargin`) ends it instead |
| `core.slice_idle` | `512` with `auto`, `core.slice` otherwise | the slice while neither CPU has touched the IPC registers (IPCSYNC, IPCFIFOCNT, IPCFIFOSEND, IPCFIFORECV) or WRAMCNT for `core.slice_hold` cycles; any value other than `core.slice` makes the slice adaptive. Handshakes through main RAM alone aren't seen (fast memory), and the first IPC access after a quiet spell still lands in a long slice |
| `core.slice_hold` | `4096` | how many system cycles (4096: about two scanlines) after an IPC access the short slice stays |
| `core.slice_stats` | `0` | every this many frames, log the IPC accesses per frame and the share of time near one (the short slice's share), for tuning `auto` per game |
| `core.fast_gxdma` | `off` | DMA words from main RAM to the geometry FIFO go straight to the 3D engine instead of through melonDS's bus and IO dispatch (patches/melonds/0004). Exact: the same results and timing; off until measured on the handheld |
| `core.fast_spu` | `off` | the mixer skips sound channels that are off (patches/melonds/0004). Exact; off until measured |
| `core.fast_mix` | `on` | the SPU runs only the sound channels due to take a sample (all of them after a sound register write) and keeps the others' output, sums the mixers with NEON, and sound DMA and capture read and write main RAM without the bus dispatch (patches/melonds/0013). Exact: the same audio byte for byte (`headless.audio_dump`) and the same RAM |
| `core.fast_dma` | `on` | 32-bit DMA from main RAM to the GX FIFO or between main RAM addresses without the bus dispatch, and the ARM9's main RAM burst timing looked up once per run (patches/melonds/0013). Exact: the same timing and results |
| `core.dma_block` | `on` | 32-bit DMA from main RAM into a VRAM page with one bank and no display capture (e.g. a game's HBLANK DMA of a line) copied in blocks, each unit timed from the same burst table (patches/melonds/0018). Exact: the same frames and audio |
| `core.fast_sched` | `on` | the scheduler keeps the time of its next event instead of walking its event list twice every slice (patches/melonds/0006, 0009). Exact; on since `core.fast_loop` needs it |
| `core.fast_timers` | `on` | a CPU's timers run only when one of them overflows, not every slice (patches/melonds/0009). Exact |
| `core.fast_loop` | `on` | the scheduler's slice loop for the JIT with the common cases inline: block lookup and dispatch, a halted CPU, an idle 3D engine, a slice with no event due (patches/melonds/0009). Exact; used with `core.fast_sched` |
| `render.geom_batch` | `on` | with `render.geom_fast`, the geometry engine does the GX FIFO's DMA and IRQ checks once per run (after its commands) instead of after every refill of its 4-entry pipe, while no CPU write is stalled (patches/melonds/0050). Exact (nothing a command does reads IF or the DMAs; with the stall queue empty the FIFO's level only falls, so the last check decides). `MELONDS_GEOMBATCH=0` turns it off |
| `render.gx_dmafast` | `on` | a DMA from main RAM into the GX FIFO port (the GX FIFO DMA, mode 7, as Mario Kart DS sends its display lists; `core.dma_block`'s block path) unpacks its words into the FIFO with the command state, the FIFO's level and its write position in registers, and sets GXSTAT's bits once per block (patches/melonds/0060). Exact (same entries, order, FIFO and pipe contents; an entry into an empty or full FIFO takes the old path). `MELONDS_GXDMAFAST=0` turns it off. |
| `render.geom_fifo` | `on` | with `render.geom_batch` and no CPU write stalled, the geometry engine's command loop reads its 4-entry pipe and the GX FIFO as one queue, the pipe's refills kept as counts, and puts back both rings' state after the loop (patches/melonds/0061). Exact (nothing writes to either while the commands run). `MELONDS_GEOMFIFO=0` turns it off. |
| `render.geom_fast` | `on` | the 3D geometry engine's fast paths (patches/melonds/0011): the GX command ports and the GX FIFO port reached first, with the FIFO write inline; `GPU3D::Run` with each command's FIFO read, DMA and IRQ checks and cycle accounting inline; NEON for the matrix commands, the clip matrix and the vertex transform (s32 x s32 -> s64, then >> 12, as the C code); clipping only against the planes a polygon's vertices are outside of; one division per axis for a vertex's screen position; a strip's shared vertices keep the previous polygon's Z. Exact (the same vertices, polygons, GXSTAT, FIFO timing and matrix and test results). `MELONDS_GEOMFAST=0` turns it off |
| `render.2d` | `banana` | the 2D renderer: `banana` (the Banana 2D engine, gpu2d/: melonDS's picture, pixel for pixel, in about a third of the time) or `melonds` (melonDS's own) |
| `render.2d_thread` | `off` (`on` at `render.scale=2`) | `on` draws the Banana 2D engine's lines (`render.2d=banana`) on a thread of their own (`bananads-2d`), the same pictures: the emulator's thread only snapshots each line (its registers, the 3D layer's line, and the VRAM, palette and OAM changes since the last line, through a copy log) and waits for the thread at the end of the frame. While display capture is on (a game's DISPCAPCNT), that frame's lines are drawn on the emulator's thread, as with `off`. It logs what it did when the game quits (`[2d]`) |
| `render.2d_fast` | `on` | the Banana 2D engine's fast paths (`render.2d=banana`, on either thread of `render.2d_thread`): NEON (aarch64) for the output stage (master brightness, 6-to-8-bit expansion, display modes 2-3), 16-colour text BGs (palette lookups with `tbl`, 8 pixels per `st4`), 256-colour text BGs, direct-colour bitmaps, colour effects (blending, brightness) and sprite resolving; the 3D layer read by the compositor without a pass of its own; affine BGs 4 pixels per step; per-line sprite lists; partial clears of the sprite line. `off`: the portable code. The same pixels either way. Also `BANANADS_RENDER_2D_FAST=off` |
| `render.3d_share` | `on` | at `render.scale=2` with Gengis, the 2D thread reads Gengis's rows of the 3D frame instead of a 4 KB copy of each line's quarters (Gengis keeps two frames' rows). Exact: the same frames and audio |
| `render.2d_lazy` | `on` | with the 2D thread (`render.2d_thread`), a frame's last lines may be drawn while the emulator runs the next frame: at a frame's end the emulator waits only for the frame before's, and whoever reads a frame (the KMS presenter, `top()`/`bottom()`, a savestate's picture) waits for its own. Same pictures |
| `render.3d_wait2d` | `on` | with `render.3d_share`, the 2D thread waits for each line's Gengis bin before compositing it, not the emulator's thread; the emulator waits for the 3D frame only at VBlank (before the geometry engine's next frame). Same pictures |
| `render.vramtrack_skip` | `on` | VRAM tracking's per-line walk (the Banana 2D engine asks it every line) returns at once when no VRAM mapping changed and its banks have no dirty bit, as the full walk would (patches/melonds/0010). Exact. Bit 4 of `MELONDS_MEMFAST` |
| `render.renderer` | `gengis` | the 3D renderer: `gengis` (Gengis Engine, SuperDrastic's rasterizer, on several threads; aarch64 builds) or `soft` (melonDS's software renderer) |
| `render.scale` | `1` | `2` renders the 3D layer at 2x and gives 512x384 screens (`render.renderer=gengis` with `render.2d=banana`; otherwise 1x, with a warning). Gengis Engine renders 512x384 from melonDS's sub-pixel vertex positions; the Banana 2D engine draws its layers at 1x and, on lines where BG0 shows the 3D layer, composites them once for each of the 2x frame's four 256-pixel lines (rows 2y and 2y + 1, even and odd pixels: DraStic's quarters), with blending, windows and brightness; other lines and the bottom screen are doubled. Display capture reads quarter 0. HeartGold, RG DS Plus at 1992 MHz: 65.7 fps against 75.9 at 1x |
| `render.edge_marking` | `on` | Gengis Engine draws the outlines games ask for round their 3D models (DISP3DCNT's edge marking). `off` leaves them out |
| `render.fog` | `on` | Gengis Engine draws the distance fog games ask for (DISP3DCNT's fog). `off` leaves it out |
| `render.gengis_threads` | `3` | Gengis Engine's render threads (the emulator's thread also renders a bin when it has to wait for one) |
| `render.gengis_defer` | `off` | Gengis Engine's deferred shading of opaque polygons |
| `render.gengis_help` | `1` | when the emulator's thread needs a Gengis bin that isn't drawn yet: `2` it draws one itself (as before), `1` only once a render thread has converted the frame (its polygons and textures stay off the emulator's thread), `0` it waits. With no render threads it always draws. Same pictures |
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
| `mic.threshold` | `0.05` | how far above the room's noise a sound must be to reach the game (full scale 1). Settings > Nintendo DS > Microphone: High 0.02, Medium 0.05, Low 0.15 (a blow, hardly a word). `0` turns the gate off |
| `mic.gate` | `3` | the echo gate's factor over the learned speaker leak; `0` turns it off |
| `mic.coupling_max` | `4` | the most leak it will learn |
| `mic.hold_ms` | `120` | the shortest time the gate stays open |
| `audio.out_device` | `default` | the ALSA output device |
| `audio.latency_us` | `30000` | ALSA's buffer |
| `audio.realtime` | `on` | the sound thread at SCHED_FIFO 10, when allowed |
| `audio.servo` | `on` | the rate servo: when the game runs below full speed, the sound is stretched smoothly (it slows and lowers with the game) instead of crackling; only the sound changes, never the emulation |
| `audio.servo_target` | `768` | the backlog of game sound (in 48 kHz frames) the servo holds: more absorbs longer dips, adds latency |
| `audio.stats` | `off` | a log line every 10 s: short chunks (the crackle), the servo's stretch range, the backlog |
| `sdl.scale` | `2` | the window's size |
| `sdl.present` | `queue` | `queue`: the emulator runs on a thread of its own at the DS's rate and the window shows its finished frames in order, one per vsync, each about one vsync after it is done (a steady picture when frames alternate between slower and faster than a vsync, as HeartGold's do at 1104 MHz; one frame more latency). `direct`: each frame shown as soon as it is done, on the emulator's thread (a frame slower than a vsync misses it) |
| `sdl.stats` | `0` | with `sdl.present=queue`, every this many vsyncs log the vsync rate, the emulated frames per second, the vsyncs that showed no new frame and the frames never shown |
| `sdl.vsync` | `off` | with `sdl.present=queue`: `on` paces the window's presents by SDL's vsync; `off` by a clock at `sdl.refresh` Hz (under sway on the RG DS Plus SDL's vsync let only ~18 presents a second through; a Wayland compositor shows the newest buffer at its own refresh, so nothing tears) |
| `sdl.refresh` | `60` | the presents per second with `sdl.vsync=off` (the display's refresh: the RG DS Plus's panels run at 60.003 Hz) |
| `kms.top` | `$BANANADS_TOP`, else `DSI-2` | the connector of the panel that shows the top screen; the other connected panel of the same type shows the bottom one (`Virtual-2` in QEMU) |
| `kms.touch` | `$BANANADS_TOUCH`, else `fe5e0000.i2c` | the bottom panel's touchscreen: the input device whose sysfs path contains this |
| `kms.menu` | `on` | the in-game menu (src/menu.c: SuperDrastic's, in BananaOS's colours): L3 pauses the game and opens it on both panels. Resume, Save and Load (8 slots with pictures; a load can be undone), Quick settings (volume, brightness, the microphone's meter and a blow), Restart game, Quit game. `kms.menu_test=<n>` opens it 3 s in, on the main page (1), Save (2), Load (3), Quick settings (4) or the quit question (5), for tests |
| `kms.queue` | `2` | the most finished frames waiting for the screen (1-3); past that the oldest is dropped. Frames are committed one per refresh, so a game whose frames take 19 and 12 ms in turn still shows a new one at every refresh |
| `kms.rt` | `on` | the presenter (the thread that commits the frames) at `SCHED_FIFO` 5, below the sound thread, as SuperDrastic's: a late wake-up finds the last flip still pending and the frame waits a refresh |
| `kms.stats` | `0` | every this many frames shown, log the rate of new frames on the panels, the frames emulated per second, the refreshes that showed no new frame and the frames dropped |
| `cpugov` | `on` | with `frontend=kms`: the lowest CPU clock that keeps the game at full speed, set through `scaling_max_freq` under the `performance` governor and put back on exit (src/cpugov.cpp says how it decides: one step up whenever the emulator falls a frame behind the DS's schedule at the clock it's at, and one step down after 2 s without, never onto a clock that was late recently); off when the governor isn't `performance` |
| `cpugov.min_khz`, `cpugov.max_khz` | `1104000`, the hardware's top | its bounds |
| `cpugov.repeats` | `on` | a refresh the panels showed without a new frame counts as a late frame: the clock goes up (and the lower one is banned) for the stutter the player sees, which a frame on time for the DS's schedule can still cause. Only while the busiest thread is at least half busy (a light game's missed refresh is a load, not the clock) |
| `cpugov.tail` | `1.4` | a step down only while the slowest frames (the 99th percentile of the last ~10 s), scaled to the lower clock, would take under this many frames (16.67 ms); a game that alternates heavy and light frames otherwise steps down until each heavy one misses its refresh. `0`: no check |
| `cpugov.max_soft` | `off` | `on`: past `cpugov.max_khz` while the game can't keep up |
| `cpugov.memory` | `on` | remember per game (and scale) the clocks that were late, in `cpugov.dir` |
| `cpugov.dir` | `$BANANADS_DATA/cpugov` | |
| `cpugov.log` | `off` | a log line per 250 ms decision window |
| `cpugov.sysfs`, `cpugov.window_ms` | the CPU's `cpufreq/policy0/`, `250` | for tests (tests/cpugov_test.cpp): another cpufreq folder, another window |
| `power.cpu_latency_us` | `0` | with `frontend=kms`: hold the cores out of idle states that take longer than this to leave, for the game's lifetime (PM QoS through `/dev/cpu_dma_latency`; the RK3566/RK3568's `cpu-sleep` takes 220 us, and 100 here kept HeartGold at 2x ~1.5% faster). `0`: the kernel decides. Costs battery: measure before keeping it on |
| `headless.frames` | `600` | how many frames `headless` runs |
| `headless.dump_dir`, `headless.dump_every` | none, `60` | write both screens as `frame<N>.ppm` every N frames |
| `headless.direct` | `off` | `on`: every frame drawn straight into a ring of the frontend's buffers (`Emu::draw_into`), as `frontend=kms` draws into the scanout buffers; the pictures must be the same as without (the Banana 2D engine only) |
| `headless.reload_at` | none | after this frame, save the state to slot 3 and load it straight back (a test of the savestate path) |
| `headless.audio_dump` | none | write the raw audio the SPU hands the frontend (`SPU::ReadOutput`: stereo s16 at the output rate, 48000 Hz) to this file, so two builds' audio compares byte for byte with `cmp` |
| `headless.measure_from` | `0` | also report the fps from this frame on (a game's scene, after loading) |
| `headless.window` | `0` | from `headless.measure_from` on, log the fps of every this many frames, then the slowest window and the slowest frame: how steady the speed is, not only its average |
| `headless.input` | none | scripted presses: `<frame>:<keys>[:<frames held>]`, comma-separated, keys joined by `+` (`a b select start right left up down r l x y`), held 6 frames by default: `600:start,900:a:10` |
| `log.file` | none | a copy of the log (stderr always gets it) |
| `log.level` | `1` | 0 debug, 1 info, 2 warnings, 3 errors |
| `prof.hz` | `0` | a sampling profiler of the emulator's thread (`src/prof.cpp`), for a handheld without `perf`: `N` samples a second of that thread's CPU time (a `CLOCK_THREAD_CPUTIME_ID` timer signalling that thread alone; the kernel checks it at its tick, so it gets at most `CONFIG_HZ`, and the report says what it got). At the end a report: the share in JIT-generated code, by module, the top functions, the same by C++ class, and with `MELONDS_JIT_PERFMAP=1` the hottest JIT blocks (`mj9_`/`mj7_<guest address>`) and stubs. The emulation doesn't see it: the same frames, RAM and sound. `0`: off |
| `prof.source` | `auto` | what makes `prof.hz`'s samples: `task-clock` (a perf event on the thread's CPU time: a high-resolution timer, any rate), `cycles` (the core's cycle counter, `prof.hz` samples per 10^9 cycles), `timer` (a POSIX CPU-time timer, which the kernel checks only at its tick: at most `CONFIG_HZ`, 250 on the RG DS). `auto`: `task-clock`, else `timer` (the report says which it used, and why) |
| `prof.pmu` | `on` | with `prof.hz`: the core's own counters on the same thread and frames (`perf_event_open`, as `perf stat`): cycles, instructions, branch misses, L1I and L1D misses, iTLB misses, per frame and per 1000 instructions, the IPC and the clock while running; a line in the report. Needs the kernel's `CONFIG_HW_PERF_EVENTS` and the A55's PMU (BananaOS's kernel has them) |
| `prof.from`, `prof.frames` | `0`, `0` | the first frame sampled (e.g. `headless.measure_from`'s), and how many (`0`: up to the quit; the report is made then) |
| `prof.out` | the log | the report's file |
| `prof.syms` | the binary itself | the file to take function names from: the unstripped build of this same binary (Buildroot strips the one on the device; the report warns when the file is another build's). Without names the report gives `bananads+0x<offset>` |
| `prof.raw` | none | also every sampled address with its count and the modules' ranges, to resolve offline (`addr2line -f -C -e <unstripped binary> <address>`) |
| `prof.top`, `prof.samples` | `40`, `262144` | the functions listed; the samples kept (8 bytes each, allocated when sampling starts; later ones are counted as dropped) |

## The handheld's buttons (`frontend=kms`)

The pad as the DS's (A east, B south, X north, Y west, L, R, Start, Select, the d-pad), read and never grabbed: the
volume keys and the lid stay the system's. F (`BTN_MODE`) + Start: quit (with the resume state). F + R: save to slot
1. F + L: load slot 1. While F is held the game sees no buttons. A keyboard works too: arrows, Enter (A), Backspace
(B), X, Y, Page Up / Page Down (L / R), S (Start), Tab (Select), F1 (F); Esc quits. `SIGTERM`, `SIGINT` and `SIGHUP`
quit the same way, writing the resume state and the save.

## The window's keys (`frontend=sdl`)

Arrows: the D-pad. X / Z: A / B. S / A: X / Y. Q / W: L / R. Enter: Start. Right Shift or Backspace: Select. The
mouse on the bottom screen: the stylus. F1: blow into the mic. F5-F7: save to slot 1-3. F9-F11: load slot 1-3.
Esc: quit (with the resume state).
