# BananaDS on BananaLinux: what has to change for speed

Research only, 2026-10-12. Sources: BananaLinux `main` (af5f18e), BananaDS on `claude/bananads-2x` (e94f97c,
SuperDrastic repo, `bananads/`), SuperDrastic's `src/dsflip.c`, and measurements on an RG DS Plus running ROCKNIX
(kernel 7.0.2, sway). BananaLinux itself hasn't booted on hardware yet, so nothing here was measured *on*
BananaLinux.

## Done (2026-10-12)

| Item | Where | State |
|---|---|---|
| A1 KMS frontend | `src/frontend_kms.cpp` (`frontend=kms`, the default without a display server) | Both panels in one atomic commit per frame, the plane scaling (the CPU where it can't: QEMU), frames queued one refresh (`kms.queue`), `kms.stats`; evdev pad and touch, never grabbed; F + Start / R / L. The Banana 2D engine draws each line straight into the dumb buffers (`Emu::draw_into`; the copy, 1.5 MB a frame at 2x, only where the plane can't scale or with melonDS's 2D renderer). The presenter at `SCHED_FIFO` (`kms.rt`) with priority-inheriting locks. Run through BananaLinux's menu in QEMU |
| A2 governor | `src/cpugov.cpp` | SuperDrastic's steps, bans, memory and savestate boost, but decided on the frames the emulator falls behind the DS's schedule at the clock it's at (not a linear prediction from the load, which held HeartGold at 1800 MHz): up a step on any, down a step after 2 s without. `tests/cpugov_test.cpp` runs it against a simulated HeartGold (settles at 1104 MHz, never on 816) |
| A3 launcher contract | BananaLinux `run-bananads`; `src/frontend_kms.cpp` | The launcher passes the places and the menu's choices as `BANANADS_<KEY>` (BananaDS already read those); SIGTERM/SIGINT/SIGHUP write the resume state and the save and quit |
| A4 sound | `src/alsa.cpp` | Already 48 kHz, the SPU's own rate: no resampling in `plughw` |
| B1 package | BananaLinux `buildroot/package/bananads` | CMake from this repository, melonDS a second download, the patches applied by Buildroot (`git apply` inside the CI's checkout would skip every file), the external toolchain's `gcc-ar` for LTO; in both defconfigs |
| B2 clock profile | not needed | A2 is in; `run-nds` keeps `superdrastic` (`performance`, which the governor lowers) |
| B3 idle latency | `power.cpu_latency_us` | Off by default until the battery cost is measured |
| B4 kernel options | BananaLinux `linux-banana.fragment` | `DEBUG_SPINLOCK`, `INIT_ON_ALLOC/FREE_DEFAULT_ON`, `HARDENED_USERCOPY`, `STACKPROTECTOR`, `DEBUG_DEVRES`, `DEBUG_BUGVERBOSE`, `RANDOMIZE_BASE` off |
| B5, B6 | | not started (hardware experiments) |

Still to do on the device: the measurements below on BananaLinux itself (the KMS presenter's `kms.stats`, the
governor's log).

## Where BananaDS stands

- HeartGold's overworld, 2x (`render.scale=2`), CPU pinned at 1104 MHz, headless: **64-66 fps**. Each 60-frame
  window is at least 62.9 fps.
- Per frame it alternates **~19 ms / ~12 ms**: the game does its logic and 3D every other frame. A pair always fits
  in two refreshes (31 ms < 33.3 ms), so a presenter that queues one frame shows a steady 60. A presenter that
  shows each frame as soon as it's done misses a vsync on every heavy frame.
- In an SDL window under sway (ROCKNIX), the same scene emulates at only **~56 fps**. Audio through PipeWire costs
  ~2 fps. Presenting costs the rest: the texture upload, the GPU draw and sway compositing both panels. At 30
  presents a second it reaches 59.6; uploading half the data gains only +3%; pinning the emulator thread to a core
  gains +1.5. BananaLinux has no compositor and no sound server, so most of that loss should disappear, **if**
  BananaDS draws straight to the panels the way SuperDrastic does (item A1).

## A. In BananaDS (SuperDrastic repo, `bananads/`). These block running it on BananaLinux at all.

### A1. A KMS frontend: frames drawn straight into scanout buffers (biggest item)
BananaDS has only `--frontend sdl` and `headless`. BananaLinux's contract (`docs/EMULATORS.md`): take DRM master on
`/dev/dri/card0`, both DSI connectors, never change the modes, page-flip on vblank, give everything back on exit.
Use SuperDrastic's `src/dsflip.c` as the model (same hardware, proven):
- **Draw into DRM dumb buffers.** `BananaRenderer` already writes each output line into its own framebuffers
  (`LineDst`, `EmitA/EmitB`, `HiFB` at 2x in `gpu2d/GPU_Banana.cpp`). Point those at mapped dumb buffers
  (XRGB8888, a ring of about 5-8, as dsflip's `NBUF`), so there is no copy, no upload and no GPU. Writes are
  sequential lines (NEON `vst2q`); keep them that way, because dumb buffers are likely write-combined and slow to
  read back.
- **Let VOP2 scale in hardware.** The plane's SRC is 256x192 or 512x384, CRTC is the panel (640x480 on the RG DS,
  1024x768 on the Plus, where 2x is exactly 2:1).
- **One atomic commit for both panels per frame**, `DRM_MODE_ATOMIC_NONBLOCK | PAGE_FLIP_EVENT`, from a presenter
  thread (dsflip uses `SCHED_FIFO` with priority-inheriting mutexes). Queue one frame, as `sdl.present=queue`
  does now (`src/frontend_sdl.cpp`'s `FrameQueue`), because of the 19/12 ms rhythm. dsflip's adaptive latch point
  (`arm_latch`) avoids commits landing right at vblank.
- **Touch from evdev**: the bottom touchscreen is the device whose sysfs path contains `BANANADS_TOUCH`
  (`fe5e0000.i2c`), multitouch protocol B, in panel pixels. **Buttons from evdev** (`rocknix-singleadc-joypad`),
  read only, never `EVIOCGRAB` (bananad reads the same device). Ignore the volume keys and F + volume.
- Display capture and the savestate picture read `Output2D` / the framebuffer: the savestate picture reads the
  framebuffer, so with uncached dumb buffers it should read from a cached copy or tolerate the slow read (it's
  rare).
- Check: no frame repeats or drops at 60.0013 Hz (RG DS) / 59.999 Hz (Plus, after `banana-panel-tune`), logged like
  `sdl.stats`.

### A2. A CPU governor of its own
BananaLinux runs DS games with `perf = superdrastic` (`/usr/bin/banana-perf`): the `performance` governor at the
**maximum clock**, on the assumption that SuperDrastic's governor (`src/cpugov.c`) then lowers it per game.
BananaDS has none, so on BananaLinux it would sit at 1992 MHz for every game: a battery and heat problem, and
the 1104 MHz goal would never be used. Port `cpugov.c` (SuperDrastic's own code: the lowest clock with no dropped
frames, remembered per game, `scaling_max_freq` restored on exit), keyed on BananaDS's frame timing.
`docs/EMULATORS.md` allows this ("an emulator with its own governor may lower `scaling_max_freq` and must put it
back on exit"). Alternatively BananaLinux gets a `bananads` profile in `banana-perf` (see B2), but a per-game
governor saves more.

### A3. The launcher contract
`/usr/libexec/banana/run-bananads` exports `BANANADS_TOP`, `BANANADS_TOUCH`, `BANANADS_DATA`, `BANANADS_LOGDIR`,
`BANANADS_SAVES`, `BANANADS_STATES`, `BANANADS_BIOS` and runs `bananads <rom>`. BananaDS reads none of these: its
settings are `--set key=value` (`save.dir`, `state.dir`, `log.file`, `log.level`, `audio.out_device`, `mic.device`, ...;
`docs/SETTINGS.md`). Either BananaDS reads the `BANANADS_*` environment as defaults, or the launcher translates them
into `--set` arguments. Also needed:
- `--frontend kms` (A1) as the default when there is no display server;
- **`SIGTERM` saves and exits within 5 s** (shutdown while playing). BananaDS installs no signal handlers today;
- `SIGSTOP`/`SIGCONT` around sleep must not upset the audio or frame pacing ("a vblank that took seconds isn't an
  error");
- Settings > Nintendo DS's choices mapped: `drastic.cfg` `hires_3d` → `render.scale`, the renderer → `render.renderer`.
  `render.2d_thread` is on by default at both scales (since 2026-10-18; it was on only at `render.scale=2`).

### A4. Sound
`src/alsa.cpp` (`BANANADS_ALSA`, on by default) already fits: ALSA `default` (bananad points it at `plughw` of the
right card, no dmix, no server), an output thread at `SCHED_FIFO`, and the microphone from `default`'s capture side.
Check it opens at a rate the codec takes natively (48 kHz) so `plughw` doesn't resample. Note the known gap: a
headphone plug-in mid-game only takes effect at the next open.

## B. In BananaLinux

### B1. The BananaDS package can't build as written (blocking)
`buildroot/package/bananads/bananads.mk`:
- `BANANADS_SITE = https://github.com/JorreFog/BananaDS.git`: that repo doesn't exist. BananaDS lives in
  `JorreFog/SuperDrastic` under `bananads/` (branches `claude/bananads*`).
- It's built as a Makefile project. BananaDS is CMake (`$(eval $(cmake-package))`), and its configure step
  **downloads melonDS** (FetchContent). That breaks Buildroot's offline build step: vendor melonDS as a second
  download (pinned commit) and pass `-DBANANADS_MELONDS_DIR=<it>`. `patches/melonds/*.patch` are applied by
  BananaDS's CMake to that tree, so check that the 14 patches (0001-0014) apply.
- Options for the handheld: `-DBANANADS_JIT=melonds -DBANANADS_SDL=OFF -DBANANADS_ALSA=ON -DBANANADS_LAN=OFF`,
  `BANANADS_CPU=cortex-a55` and `BANANADS_LTO=ON` (both defaults on aarch64), `CMAKE_BUILD_TYPE=Release`.
  Dependencies: libdrm, alsa-lib, zlib (already listed).

### B2. Clock profile
If BananaDS has no governor of its own yet (A2), `run-nds` must not give it `superdrastic` (max clock forever):
add a `bananads` profile to `banana-perf` and pick it in `run-nds` when BananaDS runs. On the GPU, BananaDS only
needs Panfrost for shaders; Gengis Engine and the 2D engine are software.

### B3. Keep the cores out of deep idle while a game runs (measured: small, +1.5%)
The A55s have `cpu-sleep` with a **220 µs** wake-up latency (state1; entered ~1.2 M times since boot on the Plus).
BananaDS hands work between threads all the time (the 2D thread drained at every frame end, Gengis bins every 16
lines, the presenter). With state1 disabled on all four cores: HeartGold 2x at 1104 MHz headless went from 64.2 to
65.2 fps, both frame types ~0.2 ms faster. With a real presenter and audio on top it's likely more (unmeasured).
How: while a game runs, hold `/dev/cpu_dma_latency` open with a value under 220 (PM QoS: the kernel then skips
`cpu-sleep`). Keep it open for the game's lifetime: bananad knows the game's pid (`/run/banana/game.pid`), or the
emulator holds it itself. Measure the battery cost before turning it on by default.

### B4. Kernel debug and hardening options (unmeasured, cheap to try)
`docs/ROADMAP.md` says "no hardening overhead", but the kernel config (`board/rgds/linux-rocknix.config`, not
overridden by `linux-banana.fragment`) still has:
- `CONFIG_DEBUG_SPINLOCK=y`: extra checks on every kernel spinlock (scheduler, futexes, DRM, ALSA). It's the one
  most likely to cost real time in a multi-threaded emulator.
- `CONFIG_INIT_ON_ALLOC_DEFAULT_ON=y` (every allocated page zeroed), `CONFIG_HARDENED_USERCOPY=y`,
  `CONFIG_STACKPROTECTOR_STRONG=y`, `CONFIG_DEBUG_KERNEL`/`DEBUG_DEVRES`/`DEBUG_BUGVERBOSE`, `CONFIG_RANDOMIZE_BASE=y`.
Turn them off in `linux-banana.fragment` (at least `DEBUG_SPINLOCK`; `init_on_alloc=0` also works on the command
line) and A/B a game. Leave `CONFIG_PREEMPT` (full) and `HZ=250` alone unless a measurement says otherwise.

### B5. DRAM speed (an experiment with risk, unmeasured)
The DDR init blob is `rk3568_ddr_1056MHz_v1.23.bin` (`banana_rgds*_defconfig`). ROCKNIX on the Plus runs the same,
`clk_scmi_ddr` = 1056 MHz, and there is no DMC devfreq on this chip, so it never changes. The sway measurements show
BananaDS's speed is sensitive to memory traffic. Rockchip's rkbin also has faster DDR init blobs, but whether these
handhelds' LPDDR4 is rated and routed for more than 1056 MHz is unknown. Test on hardware only, with a known-good
card ready to swap in (a bad DDR blob means no boot), and with a long stress run before trusting it.

### B6. Huge pages (an experiment, unmeasured; low priority)
THP is `madvise` and shmem huge pages are `never`. melonDS's fastmem arena is shared memory mapped and protected at
4 KB granularity, so huge pages don't fit it. The JIT's code buffer (32 MB, anonymous) could take
`MADV_HUGEPAGE` (A55 iTLB pressure); that's a BananaDS-side change and needs nothing from BananaLinux.

## What not to redo
- SuperDrastic measured these as worse on this hardware and removed them in 1.3 (`dsflip.c` header): CPU pinning,
  `SCHED_FIFO` for the emulator's own threads, clock lock + PLL pacing, a fixed latch.
- In BananaDS: deferring/batching sound mixing isn't exact (channels read RAM at mix time while games refill
  played buffer halves); longer idle slices (`core.slice_idle` 1024/2048) don't change the frame times; the JIT
  cache doesn't reset during gameplay (profiles that show `ResetBlockCache`/`munmap` were sampling the program's
  exit).
- SDL's vsync under sway on the Plus let only ~18 presents a second through. Irrelevant on BananaLinux (no sway),
  but don't build the KMS path on SDL's KMSDRM backend with vsync before checking it reaches 60.

## Order
A1 → A3 → B1 (BananaDS runs on BananaLinux at all), then A2 or B2 (clocks), then measure on the device with
`headless.window` and the KMS presenter's own stats, then B3 and B4 as A/B runs, B5 last.
