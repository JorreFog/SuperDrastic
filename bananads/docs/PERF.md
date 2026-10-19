# BananaDS's speed: where the time goes, what's done, what's next

The goal (2026-10-11): BananaDS as fast as SuperDrastic (DraStic) on the RG DS and RG DS Plus (Cortex-A55).

## The target

SuperDrastic 0.4.0-beta.2 holds HeartGold at full speed down to 1104 MHz on the RG DS Plus. That figure was
measured with the main emulation thread as the bottleneck, and the whole process used about 27 ms of CPU per frame
at 1104 MHz. BananaDS runs its HeartGold overworld scene at 65.4 fps at 1992 MHz, so it would need about
1830 MHz for 60 fps. Matching SuperDrastic takes roughly a third less main-thread time per frame.

The two numbers aren't from the same device or scene. Measuring SuperDrastic on BananaDS's scene means stopping
EmulationStation on the handheld, so ask the user first.

## Measurements

The setup for every number here:
- RG DS (not the Plus), A55 at 1992 MHz with the performance governor.
- HeartGold (USA), from its in-game save.
- `tools/device/hgquick.sh`: 2100 frames, the fps of frames 1500-2100 (the overworld).
- Gengis on 2 threads.

| Build | fps |
|---|---|
| melonDS JIT, melonDS 2D renderer, Gengis 3D (where this started) | 48.4 |
| + the Banana 2D engine (`render.2d=banana`, gpu2d/) | 61.7 |
| + block linking (patch 0001) | 63.3 |
| + host-flag tracking, flags with `mrs`, deferred C cycles (patch 0002) | 65.4 |
| + `MELONDS_SLICE=256` (patch 0003; **not** the default) | 70.2 |
| + `MELONDS_SLICE=512` / `1024` | 71.2 / 71.5 |

**Exactness.** Both checks use `--set rtc.fixed=2026-10-11_12:00:00` and compare 42 frames, one every 50 of 2100:
- Patched melonDS JIT (0001+0002) against stock melonDS's JIT: identical pixels.
- The Banana 2D engine against melonDS's 2D renderer: identical pixels.

`tools/device/quick.sh` passes too. The slice changes timing, so its frames aren't expected to match stock.

## Where the main thread's time goes

The profile is of patches 0001+0002 with the Banana 2D engine, using `perf record` on the overworld. The main
thread is about 90% of the process's CPU; each Gengis thread is about 5%. Shares of the main thread:

| Share | What |
|---|---|
| ~28% | JIT code |
| 15.5% | 2D (the Banana engine: BG text, composite, sprites, output) |
| 14.5% | IO and slow memory (see below) |
| 11.9% | 3D geometry: GPU3D's command FIFO, transforms, clipping, polygon setup |
| 9.7% | JIT dispatch and compile (see below) |
| 6.8% | the scheduler (`NDS::RunFrame` itself) |
| 3.8% | audio (SPU channels, mixing) |
| 2.4% | DMA |
| ~4% | kernel and libc |

The IO and slow-memory share breaks down as:
- `SlowBlockTransfer9` loads and stores: 3.5%.
- `ARM9IOWrite32`: 2.2%.
- `ARM9Read32`: 1.9%.
- `ARM9Write32`: 1.2%.
- `SlowRead9` / `SlowWrite9`: 1.1% / 0.8%.

The JIT dispatch share breaks down as:
- `ARMv5::Execute` and `ARMv4::Execute`: 2.3% and 1.4%.
- `LookUpBlock`: 1.5%.
- `Compiler::Reset`: 0.7%. The whole block cache gets thrown away during gameplay, and stock melonDS does this too.
- `JumpTo`, `UpdateMode`, `HaltInterrupted`, `SetupExecutableRegion`, `ARM_Dispatch`, `ARM_Ret`: about 0.5% each.

Within the JIT code, before patch 0002 (shares of the JIT code's samples):
- Flags: 12.5%. Each flag-setting instruction took up to four `cset`/`bfi` pairs.
- Cycle `add`s: 7.6%.
- Guest register loads and stores at block boundaries: 15%.
- Fast-memory accesses: 12%.
- Calls: 8%.
- The fast-memory base (`mov x26` plus two `movk`) at every block's entry: 7.6%. That's where each block's entry
  cost lands.

Patch 0002 removed most of the flag and cycle work.

## The patches to melonDS (patches/melonds/)

CMake applies them in order to the melonDS tree. `-DBANANADS_MELONDS_PATCHES=OFF` builds stock melonDS in a build
directory of its own, for A/B runs.

1. **Block linking** (ARMJIT_A64). A block's exit does the JIT loop's work itself:
   - It checks `StopExecution` and does `Timestamp += Cycles` against the target.
   - Then it goes straight on into the next block.
   - Exits with a constant target get a slot, a `b` that `ARM_Dispatch` patches to the target block once the
     loop has dispatched it (`melonds_jit_link`).
   - Two framework hooks in ARMJIT.cpp unlink the slots when the target block is retired or invalidated.
   - Other exits look the next block up in `FastBlockLookup`.
   - The checks live in shared per-CPU stubs (`ExitCheck`, `DynExit`), so an exit costs 1-2 instructions in the
     block. A first version inlined them: instructions went down 1%, but L1I refills rose 22% and iTLB walks
     43%, and it was slower.
   - Link paths live in the far region, which needs an explicit I-cache flush.
   - `MELONDS_JIT_NOLINK=1` turns linking off.
   - `MELONDS_JIT_PERFMAP=1` writes `/tmp/perf-<pid>.map` for perf and the blocks' code to
     `/tmp/melonjit-<pid>.code`, for `tools/perf/mjcls.py` and `mjblock.py`.
2. **Flags and cycles** (ARMJIT_A64 and the Dolphin emitter):
   - The emitter counts the instructions that write NZCV (`FlagsEpoch`). Calls count, and so does every fast
     memory access, since a fault patches it into a call.
   - `CheckCondition` branches on the host's flags when they still hold the guest's, and uses melonDS's 6-instruction
     table otherwise. At a conditional instruction's join, only the flags valid both ways are kept.
   - Two or more flags from an arithmetic instruction are written as `mrs nzcv` + `and` + `orr`, instead of a
     `cset`/`bfi` pair each.
   - C cycles of unconditional instructions are added once per block, as melonDS's x64 compiler does. Only
     interpreter fallbacks read a CPU's `Cycles` mid-block, so nothing observable changes.
   - A dead `mov` in `SUB rd, rn, #imm` is gone.
   - A55 microbenchmarks, in cycles:

     | Sequence | Cycles |
     |---|---|
     | `cmp` + four `cset`/`bfi` | 8.4 |
     | `cmp` + `mrs`/`and`/`orr` | 4.2 |
     | table condition check | 3.2 |
     | `msr nzcv` + `b.cond` | 4.2 (no better than the table) |
     | `b.cond` on live host flags | ~1 |

3. **The scheduler slice** (`NDS::SetSlice`): `core.slice`, the adaptive mode and `kIterationCycleMargin`, in the
   section below. At 64 bus cycles that is about 8,750 trips per frame through `Execute`, block lookup, timers,
   GPU3D and `RunSystem`.
4. **`fast_gxdma` and `fast_spu`** (NDS, SPU): exact shortcuts, below.
5. **IO statistics and direct routes** for the JIT's slow memory paths (`NDS_IOFast`, ARMJIT), below.
6. **`fast_sched`** (NDS): the scheduler keeps its next event's time, below.
7. **The AArch64 JIT's code** (ARMJIT_A64, `jit.codegen`): fewer host instructions per block and per exit, below.
8. **Block loads on the fastmem path**, and `LookUpBlock`/`HaltInterrupted` inline, below.
9. **The slice loop for the JIT inline** (`core.fast_loop`), timers run when they overflow (`core.fast_timers`), below.
10. **Fewer layers for the JIT's slow memory accesses** (`jit.vramfast`, `jit.iodirect`, `render.vramtrack_skip`), below.

melonDS's A64 quirks, left as they are because changing them changes timing:
- `Comp_AddCycles_C(true)` adds the not-taken path's cycles to the whole block.
- `Comp_AddCycles_CI(c, reg, shift)` adds its constant twice and ignores the register part.

## Pulled since (2026-10-11, second session): three levers, each behind a toggle, none measured on a device

Three agents worked in parallel, one lever each, on qemu only. Every change is exact on everything that can be
checked here (pictures and main RAM) and sits behind a toggle whose default is today's behaviour unless said
otherwise. **The fps numbers are still to be taken**, on the device, with `tools/device/hgquick.sh`; the A/B
lines are below each lever. Run them one lever at a time, then all together.

### The scheduler slice as a setting, adaptive mode, and three exact shortcuts (patches 0003, 0004, 0006)

Patch 0003 is rewritten: `NDS::SetSlice(slice, idle, hold)` replaces `kMaxIterationCycles` (`MELONDS_SLICE` is
gone; `BANANADS_CORE_SLICE=256` in the environment does the same). `kIterationCycleMargin` (8) is unchanged.

| Key | Values | Default |
|---|---|---|
| `core.slice` | 1-16384, or `auto` | `64` (melonDS's) |
| `core.slice_idle` | the slice away from IPC | `512` with `auto`; otherwise = `core.slice` |
| `core.slice_hold` | bus cycles the short slice stays after an IPC access | `4096` |
| `core.slice_stats=N` | logs IPC accesses per frame and the share of time near IPC, every N frames | `0` |
| `core.fast_gxdma`, `core.fast_spu` | on/off | `off` |
| `core.fast_sched` | on/off | `on` since patch 0009 (its slice loop needs it; `off` before) |

`auto` hooks the IPC case labels of the twelve `ARM9/ARM7IORead/Write8/16/32` handlers (IPCSYNC, IPCFIFOCNT,
SEND, RECV) and WRAMCNT writes; other IO addresses cost nothing, and `NextTarget` gains one load, compare and
select (its comment says what is safe and what isn't). Patch 0004: `fast_gxdma` sends main-RAM → 0x04000400 DMA
words straight to `GPU3D::Write32`; `fast_spu` makes `Mix` skip channels that are off. Patch 0006: `fast_sched`
keeps the next event's time (`SchedNext`) instead of walking the event list twice every slice. All three are
exact, and default off only by the toggle rule: after the device A/B they can become the default.
`tools/ipcrom/` (FIFO, main-RAM mailbox and IPCSYNC round trips with a halting ARM7, plus the divider) and
`tools/dmasnd/` (GX FIFO and immediate DMA, all 16 sound channels with both capture units writing to RAM,
divider/sqrt with restarts) are the test ROMs; `build.py` in each.

- Checked (qemu): stress L5 ARM/Thumb/ARM7-Thumb pictures identical to the previous build at 64, 256, 512 and
  `auto`, and with the three fast toggles on (the stress ROMs never touch IPC, so they can't tell slices apart);
  dmasnd identical at 64, `auto` and with the fast toggles; difftest `--every` main RAM identical with each fast
  toggle, JIT on and off, slice 64 and `auto`; breaking a shortcut on purpose is caught; a checker comparing
  `SchedNext` with a full walk at every `NextTarget` found 0 mismatches in about 4M calls. ipcrom round trips in
  120 frames: 64 → 12,060, 256 → 11,713, 512 → 11,299, `auto` → 11,896, with 0 FIFO/SYNC/mailbox errors at
  every slice; `auto` was near IPC 84% of the time on that ROM. callgrind, PC interpreter, x86 instructions per
  frame: slice 512 −4.8%, `fast_sched` −1.4% (`NextTarget` + `RunSystem` 1.08M → 0.39M), dmasnd with all fast
  toggles −2.1%.
- Device, one line at a time against a plain run of the same binary:
  `EXTRA="--set core.slice=256"`, `=512`, `=auto`, `"--set core.slice=auto --set core.slice_hold=1024"`,
  `"--set core.fast_sched=on"`, `"--set core.fast_gxdma=on --set core.fast_spu=on"`, then all together. Run
  once by hand with `--set core.slice=auto --set core.slice_stats=300` and read the `[emu] slice:` lines: how
  often HeartGold touches IPC decides what `auto` can gain. For the fast toggles also run `tools/device/quick.sh`
  and the 42-frame HeartGold `rtc.fixed` compare with them on. For a slice other than 64, the games to test are
  the IPC-heavy and timing-sensitive ones: Pokémon HG/SS and Platinum (sound, PXI), Mario Kart DS and anything
  with local or Wi-Fi play, Mario 64 DS, Metroid Prime Hunters, Phantom Hourglass/Spirit Tracks (touch and mic
  through the ARM7), and games that boot through card or save locks. Listen for audio glitches, look for hangs.
- Risks: with a longer slice each CPU sees the other's writes up to a slice late; events the ARM7 schedules
  inside a slice run at its end; `auto` can't see handshakes done purely in main RAM, and the first IPC access
  after a quiet spell still lands in a long slice; shared WRAM is never visible to both CPUs at once, so only
  WRAMCNT hand-overs are hooked. Audio exactness is checked only through the capture units.
- Next: if HeartGold's near-IPC share is high, hook only the polling reads (IPCSYNC/FIFOCNT), not FIFO sends;
  inline `RunSystem`'s early-out into `RunFrame`; keep the next due time for `RunTimers` and `GPU3D::Run` too.

### 2D off the main thread: `render.2d_thread=on` (default `off`)

The Banana 2D engine's `DrawScanline`/`DrawSprites` are split: `Prepare*` runs on the emulation thread at the
line's time (a snapshot of both units' registers into `Banana2DRegs`, the line's side effects on the GPU, the
3D line copied when DISPCNT_A bit 3 is set) and `Draw*From` draws from the snapshot alone. With the setting on,
each line is a job in a 128-slot ring for a worker thread (`bananads-2d`); the engines draw from their own copies
of the VRAM flats, palette and OAM, fed by a 4 MB copy log of what changed (dirty 512-byte VRAM blocks, a palette
half, the OAM), about 13-28 KB a frame on the stress ROMs. The emulation thread waits for the worker in
`SwapBuffers`, at reset/stop, before savestates, and when the ring or the log is full. Display capture draws
unit A in place (capture writes VRAM at the line's time); unit B stays on the worker. `gpu2d/`, `src/emu.cpp`,
`src/gengis_renderer.h`; `tests/ds2dtest/` is a 2D test ROM (text, affine and ext-palette BGs, every sprite kind,
windows, mosaic, blending, mid-frame palette/VRAM/OAM/mapping changes, capture); `headless.reload_at=N` saves a
state and loads it straight back, for the savestate joins.

- Checked: on = off pixel for pixel, every 10th frame of 300, L5/L1/dsswap/Thumb L5/ds2dtest, PC and qemu with
  Gengis on 2 threads; difftest `--every` main RAM identical; ThreadSanitizer clean (the two reports it gives
  are melonDS's own soft-3D thread, with `render.threaded=on`).
- Device: `EXTRA="--set render.2d_thread=on"` against `off`. Also `on` with `render.gengis_threads=1`: with 2 Gengis
  threads, the emulation thread and the worker, all four A55s are busy. `top -H` should show `bananads-2d` at
  about 15% of the main thread's CPU, and the main thread down by about that. The `[2d]` line at exit counts the
  waits for the worker: many waits mean the worker is behind or the log is full. The worker is woken about 25
  times a frame (`KickBatch` of 16 jobs, untuned). Expected: up to 15% of main-thread time back, less about 0.3%
  per line of snapshot work.
- Next: capture's inputs to the worker too; tune the batch.

### IO: direct routes for the JIT's slow paths, and IO statistics (patch 0005; `jit.iofast`, default `on`)

Patch 0005 (`src/NDS_IOFast.{h,cpp}`, `ARMJIT.cpp`, counting sites in `NDS.cpp` and `ARMJIT_Memory.cpp`):
`SlowRead9`/`SlowWrite9`/`SlowRead7`/`SlowWrite7` and `SlowBlockTransfer9` route hot addresses straight to
what the `ARM9Read*` → `ARM9IO*` chain ends up doing, with no call per word for a block transfer. The routes
copy the chain line for line and apply only with ConsoleType 0 (the DSi overrides those functions). The JIT
charges a slow access's cycles in the compiled code, so the routes can't change timing. `MELONDS_IOFAST_ROUTES`
is a mask (default 0xF): 1 main RAM (with `CheckAndInvalidate` on writes; every LDM is a slow block transfer,
and pages holding code fault into the slow path), 2 VRAM 16/32-bit through `SyncVRAM_*`/`ReadVRAM_*`/
`WriteVRAM_*`, 4 the GX FIFO (0x04000400-43F → `WriteToGXFIFO` when `GeometryEnabled`) and the command ports
(0x440-5FF → `GPU3D::Write32`), 8 IO reads: DISPSTAT+VCOUNT, IME, IE, IF as 32 bits; DISPSTAT, VCOUNT,
KEYINPUT (clears `LagFrameFlag` as melonDS does), IME and the IE/IF halves as 16 (32-bit KEYINPUT left out:
`KeyCnt` is protected). `MELONDS_IOFAST=0` or `--set jit.iofast=off` turns them all off. `MELONDS_IOSTATS=n`
(count from frame n) prints the slow accesses at exit, by exact IO address and by region, with per-frame rates;
one never-taken branch per site when off. `tests/iorom/` is the stress ROM with LDM/STM through the slow paths
(the stress ROMs have none).

- Checked (qemu): pictures identical, routes on vs off and each route alone, L5 ARM/Thumb, busy-ARM7 L5, L10,
  iorom; difftest `--every --frames 300` main RAM identical on L5, ramp, Thumb L5, busy-ARM7 L5, iorom L5 and
  ramp; sabotage builds (a wrong IF read, a dropped GX word, a wrong LDM word, a wrong VRAM write) are caught.
  Stats on L5 per frame: 2,810 VRAM 16-bit slow writes, 1,218 VCOUNT reads, 1,335 + 668 + 668 GX command-port
  writes (0x48C, 0x484, 0x488); Thumb L5 adds 364 16-bit main-RAM writes into pages that hold code; no ARM7 slow
  access even with the ARM7 busy.
- Device: `MELONDS_IOFAST=0 tools/device/hgquick.sh <bin>` against plain; one route at a time with
  `MELONDS_IOFAST_ROUTES=1`, `2`, `4`, `8`. The GX route and main-RAM LDMs should matter most (`SlowBlockTransfer9`
  3.5% and `ARM9IOWrite32` 2.2% in the profile). Then the stats on HeartGold, which hgquick's grep hides, so run
  the binary with hgquick's settings: `MELONDS_IOSTATS=1500 MELONDS_IOFAST=0 <bin> --frontend headless ...
  2>&1 | sed -n '/IO stats/,$p'` (with `IOFAST=0` the "IO register (all callers)" table is complete). Route
  whatever tops that list, with the same checks.
- Not done: ARM7 routes (nothing hot on the stress ROMs), the JIT's static-address IO calls (`GetFuncForAddr`,
  still through `NDS::ARM9IO*`; `GPU3D_*` already skips it), per-word batching inside GPU3D. In a difftest, set
  `jit.iofast` on both `--a` and `--b`, or one run's value carries into the next.

CMake now finds the newest patch that reverse-applies and applies only the ones after it (before, reconfiguring
a patched tree failed, since 0001's reverse check breaks once 0002 is on).

## The JIT path, measured without a device (2026-10-11, third session)

The goal set for this session: the JIT path twice as fast, then a third of its cost. Three agents worked in
parallel, split by file (the generated code, the per-slice runtime and scheduler, the memory accesses behind the
JIT), plus the coordinator's block-load change. Everything is exact and behind toggles. **No fps number was taken:
the handheld can't be reached from here**, so the measure is host instructions per emulated frame under qemu.

### On the device: HeartGold, RG DS Plus, 1992 MHz (the owner's run of 2b1d9bf)

**With everything on, HeartGold went to a black screen at its first overworld frame** (frame 1200 of hgquick's
walk; hgquick then reported ~105 fps of a hung game). Bisected: every new toggle off fixes it, `jit.codegen=old`
alone fixes it, and inside 0007 `MELONDS_JIT_NOLOOPREGS=1` alone fixes it (so do the parts LOOPREGS depends on:
SELFLOOP, CONDEXIT, EXITSTUB, EXITPC, LINK). **LOOPREGS is now off by default** (`MELONDS_JIT_LOOPREGS=1` turns it
on, for finding the cause). Its cause isn't known: every test ROM's pictures and RAM are identical with it on, so
HeartGold has a loop shape the tests don't. Ruled out by reading: a register written later in the loop and stale at
an earlier exit (every written register counts as dirty from the loop's start). The repro on the device:
```
<bin> --frontend headless --set save.dir=/tmp/hgb --set state.dir=/tmp/hgb --set resume.on_start=off \
  --set resume.on_quit=off --set wifi.dns=off --set headless.frames=1200 --set render.gengis_threads=2 \
  --set headless.dump_dir=/tmp/hgt --set headless.dump_every=1200 --set headless.input=$(cat walk.txt) hg.nds
md5sum < /tmp/hgt/frame1200.ppm   # good 12e33782..., bad 8fa8c332...
```

Speed (local clang builds, ~4% slower than CI's gcc builds; compare only with each other):

| Build | HeartGold fps |
|---|---|
| stock melonDS JIT | 59.0 |
| mj5 (0001-0002 and the host flags) | 61.8 |
| 2b1d9bf, `MELONDS_JIT_NOSELFLOOP=1` | 66.9 |
| 2b1d9bf, `jit.codegen=old` | 66.7 |

**The qemu counts below don't carry over to this game.** 0001-0010 give about +8% over mj5, and 0007's code
generation (59% of the JIT code's instructions on cpubench) adds nothing measurable on HeartGold. What did show is
the C++ side (0008, 0009, 0010). So the next work goes by a device profile of the current build, not by qemu
counts: `tools/device/hgprofile.sh`.

### How it was measured

- `tools/perf/qemu-count/` (its README): a qemu TCG plugin counts the main thread's AArch64 instructions per
  function and per JIT block, frames 100-300, exactly reproducible. Instruction counts aren't A55 time: they miss
  cache misses and stalls (the inlined exit checks of the first session counted 1% fewer instructions and ran
  slower), so the device has the last word.
- `tests/cpubench/`: game-like ARM9 work that never waits (a bytecode interpreter, entities, a sprite sort, LZ77,
  text tiles, CRC32 in ARM, calls, the divider, a routine in ITCM), as Thumb, as ARM, and with a busy ARM7. Its
  startup turns the protection unit and caches on, as games do: melonDS's direct boot leaves them off, and uncached
  the ARM9 ran 8 times less code per frame and the JIT was 10% of the main thread instead of the profile's 28%.
- `tests/membench/`: a frame of a game's memory traffic (BG/OBJ VRAM, OAM, palette, the divider and square root,
  IO polling, IME/IF, IPC with a halting ARM7, the GX FIFO, stores into a code page).

### Results under qemu (thousands of host instructions per frame; before = the branch before 0007-0010; LOOPREGS on, which is now off by default: +329k on cpubench Thumb)

| ROM | main thread | JIT code | JIT runtime + scheduler | memory paths | CPU emulation (the last three) |
|---|---|---|---|---|---|
| cpubench Thumb | 10,555 → 4,860 (2.17x) | 4,070 → 2,418 | 3,966 → 1,100 | 1,002 → 182 | 9,038 → 3,700 (2.44x) |
| cpubench ARM | 10,089 → 4,372 (2.31x) | 3,520 → 1,897 | 3,973 → 1,085 | 1,088 → 231 | 8,581 → 3,212 (2.67x) |
| cpubench busy ARM7 | 10,797 → 5,297 (2.04x) | 4,402 → 2,705 | 3,774 → 1,156 | 1,002 → 182 | 9,178 → 4,043 (2.27x) |
| membench | 10,349 → 6,931 (1.49x) | 190 → 107 | 3,559 → 941 | 426 → 180 | 4,175 → 1,228 (3.40x) |

(JIT code includes the exit stubs. "JIT runtime + scheduler" is `Execute`, the lookup and dispatch and
`RunFrame`/`RunSystem`/`RunTimers`/`NextTarget`, counted together because 0009 moves the first into the second.
membench's main thread is mostly the software 3D renderer and the 2D engine, untouched here.) Twice as fast is
reached on the CPU emulation of every ROM; a third is reached by the runtime/scheduler part on Thumb and ARM and
by the memory paths on cpubench, not by the JIT's generated code (59%, 54% and 61% of before).

### What changed

- **0007, the JIT's code** (`jit.codegen=new|old`, default new; `MELONDS_JIT_NO<name>=1` turns one part off; with
  `old` the code is byte for byte the previous one). Thumb jit-code with that part off alone (it includes what
  depends on it), from 2,434k: CONDEXIT (a block-ending conditional branch with a known target gets a linkable exit
  each way) +1,000; EXITSTUB (ExitCheck/DynExit: one extended add, `ARM_Ret` from a literal, a variant without the
  CPSR store, a shorter lookup) +695; EXITPC (a constant exit's R15 stored by the stub) +577; SELFLOOP (a jump back
  to the block's own start checks inline, through a slot linked from the start) +440; LOOPREGS (such loops keep
  their guest registers in host registers round the loop; **off by default since the HeartGold hang**,
  `MELONDS_JIT_LOOPREGS=1`) +329; KEEPFLAGS (fastmem call functions keep NZCV) +193;
  LAZYFLAGS (flags stay in NZCV until needed) +168; FOLDEXIT +97; DEAD (dead `cls`/`clz`, CodeCycles stores and R15
  loads) +85; FASTENTRY (x26 set by a 3-instruction prologue at the block's entry, skipped by linked exits) +46;
  DYNADDR +34; DYNCACHE (a per-site cache for `bx lr`/`pop {pc}`/`ldr pc`, linked like a known exit) +24; MEMADDR
  +21; CODESTORES +14. Block code is 1-4% smaller; the far region (cold link paths) larger.
- **0008, block loads on the fastmem path** (`MELONDS_JIT_NOFASTLDM=1` off): the A64 compiler compiled the fastmem
  path for block stores only, so every LDM and POP called `SlowBlockTransfer9` (~100 instructions). A fault anywhere
  in the sequence patches all of it into the slow call and runs it again from the address. Plus `LookUpBlock` and
  `HaltInterrupted` inline. cpubench Thumb: -1,005k on the main thread.
- **0009, the slice loop** (`core.fast_loop`, `core.fast_timers`, both default on; `core.fast_sched` now defaults
  on, which `fast_loop` needs): `NDS::RunSlicesJIT` does `NextTarget`, the `Execute<JIT>` block loop and lookup, a
  halted CPU, `GPU3D::Run`'s idle case and the event walk inline, with the scheduler's and the CPUs' fields reached
  through addresses in registers (the NDS object is ~94 MB, so GCC rebuilt far addresses at every use). Timers run
  only when one is due to overflow. `RunSystem` walks only the set events; cancelling the next event marks it for
  recompute. Main thread, cpubench Thumb: all on 7,118k; `fast_timers` off 7,993k; `fast_loop` off 8,216k; only
  `fast_sched` 8,990k; all off 9,595k.
- **0010, the memory accesses behind the JIT** (`MELONDS_MEMFAST` mask, default 7: `jit.vramfast` 1,
  `jit.iodirect` 2, `render.vramtrack_skip` 4): VRAM with one bank at a 16 KB page straight to the bank through a
  per-page table (dirty bit and capture check included; empty while VRAM holds compiled code), OAM and palette
  straight to the GPU; the divider, square root, IPC and IME/IE/IF straight to their state; other IO registers
  without the virtual dispatch; one function per hot register for the JIT's static-address calls; `SlowRead9` and
  `SlowWrite9`'s common cases inline; the Banana 2D engine's per-line `VRAMTrackingSet::DeriveState` returns at once
  when nothing was remapped and its banks aren't dirty. Per access: a VRAM 16-bit write 155 → 41 instructions, an
  IO read 125 → 52, a static-address IO write 74 → 21, a hardware divide (memory side) ~550 → ~175.

### Checks (aarch64 under qemu, on the merged branch with 0001-0010 applied to pristine melonDS)

- Pictures at frames 150 and 300 identical to the build before 0007-0010 on cpubench (Thumb, ARM, busy ARM7), the
  stress ROMs (L5 ARM, Thumb, busy ARM7; L10), iorom, ipcrom and dmasnd; the ARMv5TE differential ROMs pass;
  `bananads-tests` passes. With the defaults, with every new toggle off at once, and with FOLDEXIT and DEAD off
  alone; each agent also ran it with each of its toggles off alone.
- `bananads-difftest --every --frames 300`, every new setting off against every one on: main RAM identical every
  frame on cpubench Thumb, cpubench busy ARM7, membench and ipcrom.
- The agents' own: sabotage builds caught by difftest; membench and ds2dtest with `render.2d_thread` off and on;
  savestate reloads; `core.slice=auto` and `256` identical to the previous build at the same settings; the JIT's
  membench checksums equal the interpreter's.

### On the device

`tools/device/hgquick.sh` on HeartGold, each line against a plain run of the same binary (all new parts are on by
default, so these turn them off):
```
EXTRA="--set jit.codegen=old" tools/device/hgquick.sh <bin>                                   # 0007
MELONDS_JIT_NOFASTLDM=1 tools/device/hgquick.sh <bin>                                          # 0008's block loads
EXTRA="--set core.fast_loop=off --set core.fast_timers=off --set core.fast_sched=off" tools/device/hgquick.sh <bin>   # 0009
EXTRA="--set jit.vramfast=off --set jit.iodirect=off --set render.vramtrack_skip=off" tools/device/hgquick.sh <bin>   # 0010
EXTRA="--set jit.codegen=old --set core.fast_loop=off --set core.fast_timers=off --set core.fast_sched=off --set jit.vramfast=off --set jit.iodirect=off --set render.vramtrack_skip=off" MELONDS_JIT_NOFASTLDM=1 tools/device/hgquick.sh <bin>   # all of it
```
Then `tools/device/quick.sh` and the 42-frame `rtc.fixed` HeartGold compare with the defaults. If 0007 is slower
on the device than its count says, `perf stat -e L1-icache-load-misses,iTLB-load-misses` against `jit.codegen=old`
first: its far region grew. A single 0007 part is `MELONDS_JIT_NO<name>=1`.

### What's left, largest first (estimates are of the part they touch)

1. **A cycle budget in a register** across blocks instead of Timestamp/Target in memory: the exit check would be
   `subs budget, budget, #cycles; b.le out` instead of ~9 instructions. Needs a runtime contract change (0009) and
   the JIT (0007) together: the runtime forces the budget to 0 when it sets StopExecution, and Timestamp is brought
   up to date at ARM_Ret, before interpreter fallbacks and before every C call from JIT code. -15 to 20% of JIT code.
2. **Guest registers resident across linked blocks**, then loops over several blocks (lz_decompress, the sprite
   sort): -6 to 8%, then -5% of JIT code. Needs fixed home registers for all 15 guest registers and a second entry
   for "registers loaded".
3. **The JIT emitting VRAM/OAM/palette stores inline** with 0010's per-page table: ~150k/frame on cpubench.
4. **The divider**: 5 `StartDiv` (cancel + schedule) per divide, 36-100k/frame on cpubench; must stay exact for
   `NextTarget`.
5. **A busy ARM7's turn**: ~16 loop + 22 dispatch instructions per turn (cpubench busy ARM7 is at 35% of its
   runtime/scheduler, not a third). Continuing from `ARM_Ret` without returning to the loop was tried, exact, and
   net worse (+144k with the busy ARM7): dropped.

## Aimed at the device (2026-10-11, fourth session)

After HeartGold showed that qemu's instruction counts in JIT code don't carry over, this round went for real work
on the main thread, as HeartGold's A55 profile spreads it: 3D geometry (11.9%), 2D (15.5%), audio and DMA (6.2%), and
two build settings for the in-order A55. All exact (`check.sh` FAILS=0 with everything on and with every toggle
off; each agent's own checks below), all on by default, **none measured on the device yet**.

| Change | Toggle (default on) | qemu-count, main-thread work it targets |
|---|---|---|
| 0011: the geometry engine's fast paths: GX writes and the command loop inline (no calls per FIFO entry), NEON matrix multiplies and vertex transforms (bit-exact s64), view-volume pre-test and one division per axis in polygon setup, enabled lights only | `render.geom_fast` (`MELONDS_GEOMFAST=0`) | geometry on stress L5/L10/ramp: 59% of before; per vertex 2,190 -> 1,306 instructions, 714 -> 443 loads/stores, ~40 -> ~2.5 calls; divisions per strip quad 12 -> 6 |
| `gpu2d/`: NEON output stage, 16- and 256-colour text BGs (tbl/lane loads), direct-colour bitmaps, blending and brightness 4 pixels at a time, per-line sprite lists, affine BGs 4 pixels per stage, the compositor's 3D and backdrop passes folded in | `render.2d_fast` | 2D bucket: membench 60%, cpubench 61%, ds2dtest 80% of before; also with `render.2d_thread=on` |
| 0013: only the sound channels that are due run (NEON finds them and sums the mix); sound DMA and capture straight to main RAM; main RAM DMA and GX FIFO DMA direct | `core.fast_mix`, `core.fast_dma` | audio: sndbench 71%, dmasnd 94%; DMA: dmasnd 73% |
| melonDS's core with `-fno-semantic-interposition` (it is built position-independent, so GCC kept every core function a call) | CMake `BANANADS_CORE_NOINTERPOSE` | main thread -1.1% (cpubench), -1.3% (membench) |
| `-mcpu=cortex-a55` for aarch64 builds: scheduling for the A55's in-order pipeline | CMake `BANANADS_CPU` (empty: generic) | none (qemu doesn't model the pipeline; the gain, if any, shows only on the device) |
| LTO for aarch64 builds | CMake `BANANADS_LTO` | main thread -0.6% (cpubench), -5% (membench); binary 7% smaller |

New test ROMs: `tests/geomtest` (matrix ops, CLIPMTX/VECMTX reads, POS/VEC/BOX tests, logged to RAM), `tests/sndbench`
(16 channels, every format, both captures), and `headless.audio_dump=<file>` for comparing the sound output byte for
byte. Checks per change: pictures (every 10th or 30th frame of 300, soft and Gengis, `render.2d_thread` off and on,
savestate reloads), `bananads-difftest --every` toggle off against on, the audio dump, random unit tests of the NEON
code against the C code (with sabotaged builds caught), and CI's PC and BananaJIT builds.

On the device, `tools/device/hgprofile.sh` now also A/Bs `render.geom_fast`, `render.2d_fast` and
`core.fast_mix`/`core.fast_dma`. The build settings need two builds: `-DBANANADS_CPU=` (generic) and
`-DBANANADS_LTO=OFF` against the defaults.

Later rounds the same night (all exact, same toggles; qemu-count unless said):
- **0011 geometry:** one reciprocal per new vertex for its screen X/Y and Z (`umulh` by ceil(2^64/W), exact for
  W in 2..0xFFFF, else the old divisions): 64-bit divisions per strip quad 6 -> 2 (stock melonDS: 12), per separate
  triangle 9 -> 3; the pipeline helpers forced inline under LTO; the strip's shared vertices read in place unless
  the polygon is clipped; no IRQ recompute while IF's GX FIFO bit stays; the vertex path without a stack canary
  (113 -> 99 instructions per vertex). Tried and dropped: one ring buffer for the GX FIFO and pipe (exact with
  savestate conversion, but more instructions), clearing GXSTAT's busy bits only after a command (not exact: a
  mid-run savestate differed while pictures and RAM didn't). Geometry on L5: 59% -> 57% of before 0011.
- **`render.2d_fast`:** 256-colour text BGs (lane loads), affine BGs 4 pixels per stage (fewer dependent-load
  stalls; the count barely moves), rotscale sprites clipped to the pixels whose source lands in the sprite
  (-40 to -63%), OBJ mosaic only over the block span that has it, per-line dirty sets tested once, the register
  snapshot written straight into the engine, the 3D line copied for a 2D-thread job only when BG0 shows it.
  membench 2D: 55% of before (thread off); with `render.2d_thread=on` the main thread's 2D cost -22%.
- **0013 audio/DMA:** the Mix's tail skipped when no output can change; ARM9 DMA of 16- and 32-bit units straight
  to main RAM, palette, OAM and VRAM (the 0010 page table) with `UnitTimings9` unchanged. sndbench (now with 11 ARM9
  DMAs a frame to VRAM/palette/OAM) main thread -18%. Only the audio dump catches small mixing errors (a sabotaged
  pan term passed pictures and RAM): keep it in the checks for SPU work.

- **0012 (GPU):** `VRAMTrackingSet::Unchanged`, 0010's early-return test on its own, so the 2D engine skips deriving
  (and building a 128-byte empty set for) each of a line's six VRAM tracking sets when nothing changed. Main thread's
  2D cost on membench: -13% with `render.2d_thread=on`, -4% off.
- **The HeartGold black screen with LOOPREGS: not reproduced.** `tests/jitloops/` (IRQ-driven wait loops: polling
  memory the handler writes, VCOUNT/IF/IPCSYNC/GXSTAT, IntrWait in a loop, mid-loop exits, VRAM and GX stores, an
  ARM7 loop in IRQ mode) is byte-identical with LOOPREGS on and off and equals the interpreter. LOOPREGS stays off.
  To bisect it on the device: `MELONDS_JIT_LOOPREGS=1` plus one of `MELONDS_JIT_NOLOOPTS=1`,
  `MELONDS_JIT_NOLOOPSIGNED=1`, `MELONDS_JIT_NOLOOPLAZY=1`, `MELONDS_JIT_NOLOOPXFER=1`; and once with
  `render.gengis_threads=0` and the 2D thread off (if LOOPREGS is exact, only a race in a renderer thread could turn
  a different host speed into a different picture). Not covered by the tests: a loop whose own block is invalidated
  while it runs.

Left, by agent: the geometry's FIFO ring buffer (savestates hold both FIFOs) and its vertex copies; rotscale
sprites and OBJ mosaic, skipping unchanged lines in 2D; the mix's per-sample tail and batched ARM9 DMA for VRAM.

## The slice loop on the emulator's thread (2026-10-18, second session; none measured on a device)

HeartGold at 2x on the RG DS Plus is bound by the emulator's thread (99% busy); its older profile has the scheduler
(`RunFrame`, the slice loop) at 10% and the JIT's dispatch and exit stubs at 7%. Melon's slice is 64 system cycles,
~8.7k slices a frame, and a longer slice changes timing (DEAD-ENDS). These changes keep every slice end where it
was and make the slices that only keep time cheaper. Each is exact and has a toggle; qemu counts below only show
that the path got cheaper.

### The ARM9's slice ends inside its block chain (patch 0030, `jit.slice_chain`, default `on`)

When an ARM9 exit stub (ExitCheck, ExitCheckPC, DynExit, DynExitA, a self loop's inline check) found Timestamp >=
Target, the chain went back through `ARM_Ret` (12 registers restored, a mispredicted return) to `RunSlicesJIT`'s
iteration, then the loop's block lookup and `ARM_Dispatch` (12 registers saved, an indirect branch into the block).
With the ARM7 halted, as it is most of a game's frame, that iteration only keeps time. Now the stub calls
`NDS::SliceChain9`, which makes the loop's own checks (no event or timer due by the ARM9's time, no GX stall, DMA or
sleep, the geometry engine idle, the ARM7 ahead or halted with no IRQ pending, Running) and, if they hold, does the
iteration's bookkeeping (GPU3D's time, the ARM7's time and target, SysTimestamp) and `NextTarget`'s ARM9Target; the
chain then goes on into the exit's next block. Otherwise it changes nothing and the stub returns to the loop.
`MELONDS_JIT_SLICECHAIN=0` or `jit.codegen=old` turn it off.

- qemu-count (main thread, instructions a frame): cpubench Thumb 4,896k -> 4,696k (-4.1%: the slice loop 962k ->
  352k, ARM_Dispatch/ARM_Ret 192k -> 45k, SliceChain9 +510k); membench 6,032k -> 6,022k (its ARM9 halts most of the
  frame, which 0031 is for).
- Checked: `evidence/jit/exact-suite.sh` off vs on, FAILS=0 (pictures of 17 ROMs, the dmasnd audio dump, difftest
  `--every` main RAM on 9, `bananads-tests`); pictures and audio dump of sndbench, ds2dtest, geomtest, bigcode and
  L1; `core.slice=auto` (ipcrom, cpubench busy ARM7, dmasnd) and `render.scale=2` (cpubench, L5); difftest on ipcrom.
  A sabotaged build (the ARM7's pending IRQ ignored) is caught by ipcrom's difftest (main RAM differs).
- Expected on the device: what it removes is a share of "scheduler 10%" and "dispatch and exit stubs 7%" for the
  slices where the ARM9 runs and the ARM7 waits; the call it adds is ~60 instructions of loads and compares.
  `EXTRA="--set jit.slice_chain=off" hgquick.sh <bin>` against plain.

### Both CPUs halted: the times go to the next event in one step (patch 0031, `core.halt_skip`, default `on`)

While a game waits for VBlank both CPUs are halted (the ARM9 in CP15's wait for interrupt, the ARM7 in HALTCNT),
and every 64-cycle slice until the next event only set the CPUs', GPU3D's and the system's times to its end
(~45 instructions a slice in `RunSlicesJIT`). Now, at the loop's top, with both halted, no IRQ pending for either,
no DMA, GX stall or sleep and the geometry engine idle, the loop steps the slice ends as `NextTarget` would (same
steps, same margin) up to the last one before the next event or timer and sets the times once; the next iteration
runs the slice that reaches the event as before. Nothing runs in between, so nothing can see the skipped slices.
`MELONDS_HALTSKIP_STATS=1` logs `[core] halt_skip: N skips in F frames` at exit (how often a game's both CPUs wait).

- qemu-count: membench (both CPUs halt, as a game's) 6,032k -> 5,635k instructions a frame (-6.6%; the slice loop
  920k -> 523k); ROMs whose ARM9 never halts (cpubench, the stress ROMs, sndbench, ds2dtest) don't skip, and pay
  ~1 instruction a slice for the test (L1 +12k of 19.6M).
- Checked: `exact-suite.sh` off vs on, FAILS=0; jitloops (IntrWait loops: 16.7k skips in 200 frames) and membench
  (1.3k skips a frame) are the ROMs that exercise it. A sabotaged build (the pending-IRQ test dropped) is caught by
  jitloops' difftest (main RAM differs from frame 5).
- Expected on the device: the share of HeartGold's frame in which both CPUs wait, times the slice loop's cost there.
  Run `MELONDS_HALTSKIP_STATS=1` with hgquick's settings first: skips per frame x ~16 slices each is what it removes.
  `EXTRA="--set core.halt_skip=off" hgquick.sh <bin>` against plain.

## Mario Kart DS's race start: the emulator thread's costs beside the geometry (2026-10-18, third session, `mkside`)

Mario Kart DS at 2x drops to 52-57 fps through every race's intro and countdown on the RG DS Plus at 1992 MHz; the
emulator's thread is 93.5% busy (BananaLinux `perf/bananads/session3/`). Its device profile, beside the geometry
engine (31%) and the JIT's code (29%): the frame loop `main::{lambda()#1}` 8.0% (with LTO it is `run_headless` with
`RunFrame`, `RunSlicesJIT`, `RunEventsDue`, `ExecuteJITInline` and `HaltInterrupted` inlined: the slice loop and the
scheduler, read from a `-g1` build's line table, which has the same code), the SPU 7.7% (its event 3.7%,
`RunValid<2>` 3.0%, `RunValid<1>` 0.5%, `blip_add_delta` 0.7%), `DMA::Run9` 5.3%, `pthread_cond_signal` 0.9%.
Counted in the scene (headless, `mk-scene.sh` in the hand-over, frames 1680-2100, per frame): 547 Mixes running
~10.4 channels each (4.6k ADPCM calls taking 6.1k samples, 1.1k PCM16 calls), 3.8 of them after a sound register
write; events LCD 526, SPU 547, RTC 548, Div 162, Sqrt 85; DMA3 in GX FIFO mode 70 starts and 31.6k words, the only
other DMA 4 immediate DMA3 starts and 906 words (so `DMA::Run9` is the geometry's). None of this is measured on the
device; qemu counts only show that a path got cheaper.

### A due channel's samples with its state in registers (patch 0070, `core.fast_samples`, default `on`)

- `RunValid<type>` keeps nothing of the channel in registers across the FIFO's stores and its refill call: ~110
  instructions a call for ~1.3 samples. `RunSamples<1/2>` are leaves with the state in locals that take the plain
  samples and hand any other (start, header, end, loop, a FIFO refill due) with the rest of the Mix's samples to
  `RunValid` (a tail call), so those stay melonDS's code; ADPCM's step comes from two tables built from melonDS's.
- qemu, MK frames 1680-2100: `RunValid<1>` + `RunValid<2>` 570k -> 435k instructions a frame (-24%).
- Checked: `mk-scene.sh` frames 1500-2100 and its audio dump identical off/on and to the base build; main RAM every
  frame 1500-2100 identical (`bananads-difftest --every --from 1500 --input <the scene's>`, off vs on);
  `exact-suite.sh` off vs on plus sndbench (all formats, looping and one-shot ADPCM) pictures, audio and difftest,
  dmasnd and ipcrom difftests: FAILS=0. A sabotaged build (ADPCM's loop-start state not saved) changes MK's audio
  dump (caught by cmp).
- Expected on the device: these two functions are 3.5% of the thread in MK's race start; a quarter of that is
  ~+0.8%. `EXTRA="--set core.fast_samples=off"` against plain.

### The 2D thread woken every 64 jobs, not 16 (`render.2d_kick`, default `64`; no melonDS patch)

- `pthread_cond_signal` is 0.9% of the emulator's thread in MK's race start: `BananaRenderer::Kick`'s
  `notify_one`, a futex wake whenever the 2D thread (30% busy) has caught up and sleeps, every 16 of a frame's 383
  jobs. Counted in the scene under qemu (2100 frames): 27.9k kicks found the thread asleep at 16, 8.2k at 64.
- The 2D thread draws the same jobs in the same order whenever it is woken, and the emulator's thread wakes it
  before every wait for it (VBlank, the swap, a full ring or log, display capture), so the pictures can't depend
  on the batch.
- Checked: `mk-scene.sh` frames and audio identical at 16 and 64 and to the base; `exact-suite.sh` with
  `render.2d_thread=on` on both sides, 16 vs 64, plus ds2dtest and geomtest pictures: FAILS=0.
- Expected on the device: ~24 -> ~6 wakes a frame, so most of the 0.9%: ~+0.6%. The 2D thread finishes a frame's
  last 32 lines after VBlank instead of the last 8 (~1 ms of its work): with `render.2d_lazy` the next swap waits
  for it, so watch `kms.stats` for repeats. `EXTRA="--set render.2d_kick=16"` against plain.

### A CPU leaving its code region swaps back to the one it left (patch 0071, `jit.region_pair`, default `on`)

- The slice loop's block lookup sets the code region up again (`SetupExecutableRegion`: `ClassifyAddress`,
  `GetMirrorLocation`) whenever the PC leaves it: in MK's race start ~1500 times a frame for the ARM9 (ITCM <->
  main RAM calls) and ~370 for the ARM7 (BIOS <-> WRAM for IRQs and SWIs). Those functions are ~1.1% of the
  emulator's thread on the device. Each CPU now keeps the region it left when that region's setup is a pure
  function of the address and of state the swap checks (ARM9: ITCM, main RAM; ARM7 on a DS: BIOS, main RAM, the
  WRAM at 0x038), and swaps back to it: 79% of the ARM9's and 40% of the ARM7's setups become swaps.
- qemu, MK frames 1680-2100: the setup functions 205k -> 122k instructions a frame.
- Checked: `mk-scene.sh` frames and audio identical off/on and to the base; main RAM, both CPUs' registers and
  GPU3D's timing state every frame 1500-2100 identical off vs on (`bananads-difftest --regs --timing`);
  `exact-suite.sh` off vs on: FAILS=0. A sabotage (the kept mirror's start off by a word) hangs MK at boot. Note:
  a region with the wrong size or lookup only costs speed (the lookup entries are tagged with the block's address,
  so a wrong region misses and compiles or sets up as before): a sabotage giving the wrong size passed every check,
  which is the design working, not a hole.
- Expected on the device: ~40% of ~1.1%: ~+0.4%. `EXTRA="--set jit.region_pair=off"` against plain.

### Session 2's slice loop commits on MK (patches 0030 `jit.slice_chain`, 0031 `core.halt_skip`, cherry-picked)

- In MK's race start both CPUs are halted for much of the frame (537 halt skips a frame, frames 1680-2100). qemu:
  the frame loop (`run_headless` with the slice loop inlined) 1151k -> 858k instructions a frame with both on,
  `SliceChain9` +158k, JIT code and stubs -66k: about -200k a frame (halt_skip -150k alone; slice_chain about
  even in instructions, but it removes `ARM_Ret`/`ARM_Dispatch` round trips with their mispredicted returns).
- Checked on MK: frames and audio identical with each off and both on; main RAM every frame 1500-2100 identical
  for each off vs on; with every change of this section on, the full savestate at frames 1850 and 2000 is
  byte-identical to the base build's (`headless.reload_at`, the GX DMA lesson of this session's geometry work:
  RAM and registers at frame ends can hide timing).
- Expected on the device: the frame loop is 8.0% of the thread; a quarter less: +1.5-2.5%.
  `EXTRA="--set core.halt_skip=off --set jit.slice_chain=off"` against plain.

## Mario Kart DS's race start after the restart: beside the geometry (2026-10-19, third session's phase 2, `mk2cpu`)

Counted in the scene from the checkpoint (8e98258: qemu, `mk-scene.sh`, frames 1680-2100, per frame, 13.56M
instructions on the emulator's thread): the SPU ~1.1M (its event 582k, `RunSamples<2>` 341k, capture 72k,
`blip_add_delta` 65k, already vectorised by GCC), the frame loop 801k (`run_headless` with the slice loop and
`RunEventsDue` inlined: ~60 instructions a slice over ~5,500 ARM9 slices, ~75 an event over ~1,900 events), the
ARM7's slow accesses to its WRAM 245k (`ARM7Write32` 141k), the event handlers small (`ScheduleEvent` 34k, Sqrt 32k,
RTC 24k, `StartScanline` 29k). None of this is measured on the device; qemu counts only show that a path got cheaper.

### The ARM7's slow accesses to its own WRAM straight to it (patch 0072, `jit.wram7fast`, default `on`)

- MK's sound driver keeps its state in the ARM7's WRAM next to its code, so the JIT's stores there take the slow
  path (a store into a page holding compiled code faults once and stays a call): per frame ~1,040 stores, ~450 STMs
  (~2,000 words), ~170 loads and ~60 LDMs (~450 words) (`MELONDS_IOSTATS`, frames 1700-1899). Each word went
  `SlowWrite7` -> `NDS::ARM7Write32` (a virtual call through `NDS::Current`, three register pairs saved, the
  region switch) -> the code check -> the store. On the device `ARM7Write32` was 0.76% of the thread,
  `SlowBlockTransfer7` 0.23%, `ARM7Read32` 0.24%.
- Now, on the DS, `SlowRead7`/`SlowWrite7`/`SlowBlockTransfer7` do `ARM7Read*`/`ARM7Write*`'s case 0x03800000
  themselves: the same `CheckAndInvalidate<1, memregion_WRAM7>` and the same store or load at
  `addr & (ARM7WRAMSize - 1)`; a block whose first and last words are both at 0x038 goes word by word in the same
  order with the code index loaded once. Anything else takes the old path.
- qemu, MK frames 1680-2100: `ARM7Write32/16/8` and `ARM7Read32` -181k instructions a frame, `SlowWrite7` and
  `SlowBlockTransfer7` +19k: **-162k (-1.2% of the thread's instructions)**.
- Checked: `mk-scene.sh` frames and audio identical to the base with it on and off; MK's main RAM and both CPUs'
  registers every frame 1500-2100 off vs on (`bananads-difftest --regs`); `exact-suite.sh` off vs on (sndbench,
  dmasnd and ipcrom pictures, audio and difftests included); a sabotage (an STM's last word stored a word late)
  changes MK's countdown frame.
- Expected on the device: ~1.2% of the thread is these paths, -60..70% of it: **+0.6..0.8%**.
  `EXTRA="--set jit.wram7fast=off"` against plain.

### The 2D job ring's counters on cache lines of their own (`render.2d_ring`, default `split`; no melonDS patch)

- On the device `BananaRenderer::Submit` is 0.54% of the thread for 383 jobs a frame: ~420 cycles a call for ~10
  instructions, which only memory can cost. Head (written by the emulator's thread every job), Tail (written by the
  2D thread every job), `Kicked`, `FenceCur` and the start of the mutex were on one cache line, so every job moved it
  between the cores twice; `NewJob` also read Tail (acquire) every job and `LogReserve` the copy log's `LogRead`
  for every copy, each a miss while the 2D thread writes them.
- `split`: Head on a line of its own, Tail and `LogRead` on another; the emulator's thread keeps the last Tail and
  `LogRead` it read (`TailSeen`, `LogReadSeen`) and reads them again only when the ring or the log looks full by
  those. They only grow, so a stale value can only say "full" too early, and the acquire that read it orders the
  slots and log space it freed as before. The 2D thread draws the same jobs in the same order.
- qemu can't show this (it doesn't model caches): the instruction counts are unchanged.
- Checked: `mk-scene.sh` frames and audio identical to the base with `split` and `shared`, and 210 frames (every 10th)
  identical between them; `exact-suite.sh` with `render.2d_thread=on` on both sides, `shared` vs `split`, plus
  ds2dtest and geomtest pictures: FAILS=0. A sabotage (the 2D thread publishing Tail on the old line) hangs MK
  before its first dump. The full-ring path is rare under qemu (the 2D thread keeps up), so
  `render.2d_test_delay=150` (tests only) slows the 2D thread until the ring fills every frame (~330k waits in the MK
  scene): with it, `split` and `shared` give the same 210 MK frames (every 10th), the same audio and the same frames
  as without the delay, and ds2dtest's 30 frames the same; a sabotage (the ring bound 128 jobs too large: undrawn
  jobs overwritten) changes 28 of the 210 MK frames and 15 of ds2dtest's 30. Without the delay that sabotage
  changed nothing.
- Expected on the device: NewJob's and LogReserve's misses gone, Submit's store no longer fighting Tail's writes:
  ~+0.3..0.5% (half of Submit's 0.54% and a miss a job in the job preparation). `EXTRA="--set render.2d_ring=shared"`
  against plain; watch `kms.stats` as for `render.2d_kick`.

## Levers not pulled yet, largest first (1-3 above are pulled, unmeasured)

1. **The scheduler slice.** *Pulled: `core.slice`, above.* 64 → 256 bus cycles gives +8.5% and 512 gives +10%. It's an accuracy trade: coarser
   ARM9/ARM7 interleaving, which matters for IPC handshakes and anything timing-sensitive between the CPUs.
   - Make it a setting, default melonDS's 64.
   - Test many games before changing the default.
   - Or make it adaptive: long slices while neither CPU touches IPC or shared memory.
2. **2D off the main thread** (15.5%). *Pulled: `render.2d_thread`, above.* Render lines on a worker from per-line snapshots of the 2D registers.
   Palette, OAM and VRAM changes need a write log or copy-on-write, since games change them mid-frame. The
   engine already keeps `PaletteDirty`/`OAMDirty`. melonDS flattens VRAM per renderer through `VRAMTrackingSet`.
3. **IO paths** (14.5%): *Partly pulled: patch 0005, above; the stats on HeartGold say what is next.*
   - The 3D command stream comes from STMs into the GX FIFO: `SlowBlockTransfer9` → `ARM9Write32` →
     `ARM9IOWrite32` → `GPU3D::Write32` → `WriteToGXFIFO`, per word. A direct path would cut that chain.
   - LDMs from IE/IF.
   - Find which other slow accesses are hot: count them by address.
4. **3D geometry** (11.9%): NEON for the matrix and vertex work and the clipping, and less per-command FIFO
   overhead. It has to stay synchronous: GXSTAT, FIFO stalls and matrix reads depend on its timing.
5. **JIT dispatch** (9.7%): *Pulled: 0008, 0009, above (the third session).*
   - Count why exits go back to the loop: `StopExecution` (an IRQ pending while masked keeps it set), the slice
     end, a region change (`SetupExecutableRegion`, e.g. ITCM ↔ main RAM), or a lookup miss.
   - `DynExit` could know more than one region.
   - Find why `Compiler::Reset` runs during gameplay: near or far memory full? The far region is only 4 MB, and
     link paths now use it too.
6. **JIT code** (~28%): *Partly pulled: 0007 and 0008, above; what's left is listed there.*
   - Guest registers resident across linked blocks; BananaJIT had this.
   - The fast-memory base kept in x26 instead of reloaded per block (three instructions).
   - Fewer R15/CodeCycles/RegionCodeCycles stores at exits.
   - Two known exits for a conditional branch at a block's end (it uses `DynExit` now).
7. **Audio** (3.8%) and **DMA** (2.4%). *Started: `fast_spu` and `fast_gxdma`, above.*

## The JIT's code footprint (2026-10-18, second speed session, agent jitcode; untested on device)

The JIT's generated code is 32% of the emulator thread in HeartGold (device profile, 2026-10-12). 0007 cut ~40% of
its *instructions* on cpubench and nothing showed on the device; on a game-sized code footprint (tests/bigcode) the
JIT's code runs at ~0.25 instructions per cycle (rgds-sim, first session): instruction fetch, not instruction count,
is the likely limit in games. These changes make the blocks smaller without changing what they do. The measure is
static and deterministic: the bytes of the blocks (`MELONDS_JIT_PERFMAP=1`, the `mj9_`/`mj7_` symbols of the
code dump; the far region, `mj9f_`/`mj7f_`, holds cold paths), tests/bigcode-thumb, first 60 frames (3863
blocks), device build (`build-a64-device.sh`), counted with the first session's `census.py`:

| Build | Block bytes | Bytes/block | Instructions in blocks |
|---|---|---|---|
| base (0001-0020) | 1,170,716 | 303 | 292,679 |
| + 0040 `jit.entry_x26` | 1,124,360 (-4.0%) | 291 | 281,090 |
| + 0041 `jit.dyncold` | 1,096,536 (-6.3%) | 284 | 274,134 |
| + 0042 `jit.flagstub` | 1,060,344 (-9.4%) | 274 | 265,086 |

Expected device gain (an estimate, by reasoning only): the first session's rgds-sim run of 0024 (= 0040) on bigcode
gave ~0.4% less main-thread time per 1% less JIT code (4% -> -1.7%; JIT code 60% of that thread). If HeartGold's
JIT code (32% of its thread) is fetch-bound like bigcode's, 1% fewer block bytes is ~0.2% of the emulator thread:
0040 ~+0.8% fps, 0041 ~+0.5%, 0042 ~+0.6%, all three ~+2%. If HeartGold's hot code fits the A55's 32 KB L1I, ~0.
The device decides.

- **0040, `jit.entry_x26`** (`MELONDS_JIT_NOENTRYX26=1` off): the first session's 0024 (perf/jit), renumbered.
  ARM_Dispatch takes the CPU's fast memory base as a third argument and sets x26, so blocks lose 0007's FASTENTRY
  prologue (`mov/movk/movk x26`, 12 bytes each, 4.0% of the blocks' bytes) and a dispatch runs three instructions
  fewer. Everything else entering a block (a linked slot, DynExit, a self loop, the fault handler's restore) comes
  from the same CPU's code, where x26 already holds the base.
- **0041, `jit.dyncold`** (`MELONDS_JIT_NODYNCOLD=1` off): a return through a register (`bx lr`, `pop {pc}`;
  0007's DYNCACHE) ends its block with the hit's way (`ldr w1,<key>; cmp w4,w1; b.ne; bl ExitCheck; b <slot>`)
  and then 20 bytes of the miss's way (another target than last time, or the first time). Those go to the far
  region after the exit's learn path, a `b` to them stays: 16 bytes less per such exit (1739 of 3863 blocks on
  bigcode-thumb, 2.4% of the bytes). A miss runs one `b` more.
- **0042, `jit.flagstub`** (`MELONDS_JIT_NOFLAGSTUB=1` off): with LAZYFLAGS the guest's flags stay in NZCV and
  every exit put them in RCPSR (`mrs x17,nzcv; and w27,w27,#0x0fffffff; orr w27,w27,w17`) before its
  `bl ExitCheck(PC)` (3016 of 3863 blocks on bigcode-thumb, 3.1% of the bytes). Two new stub entries do it
  instead: ExitCheckPCF (the three, falling into ExitCheckPC with the CPSR store) and ExitCheckF (the three and a
  `b` to ExitCheck with the CPSR store). Only for a BL'd exit (a known target, not a self loop) with CPSRDirty, so
  the stub's CPSR store is the one the block would have chosen. The same instructions run (+1 `b` on ExitCheckF).

Checks: `evidence/jit/exact-suite.sh` (pictures every 50 frames to 300 on 17 ROMs incl. jitloops, irqmask, iorom;
dmasnd's audio; `bananads-difftest --every` main RAM on 9 ROMs; `bananads-tests`), each toggle off against on,
device build: FAILS=0 for each. Sabotage: 0041 with one cycle added on the miss way: FAILS=12. 0042: two
sabotages (no CPSR store in ExitCheckF; ExitCheckPCF without its AND) were *not* caught: the test ROMs' flags at
those exits are dead. 0042 is exact by construction, but its check is weak (see Mario Kart DS below).

### Mario Kart DS's race start (2026-10-18, third speed session, agent mkjit; untested on device)

The three on Mario Kart DS (USA), Figure-8 Circuit's race start from a fresh save (session 3's `mk-scene.sh`:
2100 frames, the countdown at 1780-1965), device build under qemu. The device profile of this scene has the JIT's
code at 29.1% of the emulator's thread, IPC 0.49, 9.9 L1I and 4.15 iTLB misses per 1000 instructions.

| Build (all of the scene's blocks, census of the perf map) | Block bytes | Bytes/block | Instructions |
|---|---|---|---|
| all three off | 4,371,356 | 303 | 1,092,839 |
| + 0040 `jit.entry_x26` | 4,198,376 (-4.0%) | 291 | 1,049,594 |
| + 0041 `jit.dyncold` | 4,093,128 (-6.4%) | 284 | 1,023,282 |
| + 0042 `jit.flagstub` | 3,934,836 (-10.0%) | 273 | 983,709 |

14,415 blocks in each build (the same code compiled). The far region grows by 131,568 bytes (0041's miss ways).
The blocks call 0042's stubs at 13,191 exits: ExitCheckF at 5,920 (ARM9 5,276, ARM7 644), ExitCheckPCF at 7,271
(ARM9 6,398, ARM7 873).

Exactness, each toggle off against on (the other two on): the scene's eight pictures (1500-2100) and its audio
identical to the base build's, and to every cumulative combination above; `bananads-difftest --every --from 1600`
(main RAM every frame from 1600 to 2100): the same for each.

Sabotages of 0042 (MELONDS_JIT_SABFS=n in a sabotage build), on the scene's pictures, audio and main RAM:

| Sabotage | Caught |
|---|---|
| ExitCheckF without its AND (old flags OR'd with the new) | yes (pictures from 1500 on) |
| both stubs without their AND | yes |
| both stubs' flags inverted; both without the ORR (flags not stored) | yes |
| the blocks BL the plain stubs (flags never put in the CPSR) | yes |
| ExitCheckF into the c=0 ExitCheck (no CPSR store) | **no** (jitcode's first) |
| ExitCheckPCF without its AND | **no** (jitcode's second) |
| ExitCheckPCF's flags inverted; ExitCheckPCF without the ORR | **no** |
| (the old code: every exit to the c=0 stubs, no CPSR store) | yes |

So in this scene the flags at ExitCheckF's exits are live and checked, and those at ExitCheckPCF's 7,271 exits (the
exits whose R15 the stub stores from the slot's literal) are all dead: no check of ours sees a mistake there. **So
`jit.flagstub` is off by default** (BananaDS's setting; the patch's own default is unchanged): an uncaught toggle
is not turned on. A test whose flags live across an exit after a non-branch last instruction would be the check
it needs.

Expected gain on the race start (with 0042 off: -6.4% of the bytes; with it on -10.0%): only instruction fetch can
change (the instructions run are the same but 0040's three per dispatch and 0041's one B per miss), so at most the
JIT code's share of the fetch stalls times the bytes saved. The PMU's 145.6k L1I misses a frame at ~10-12 cycles
(an L2 hit) are ~5-6% of the frame's 29.95M cycles, its 61k iTLB misses ~1-2% more; the JIT's code (29.1% of the
time) holding half to all of them and 6-10% fewer of its lines missing gives **+0.2-0.7% of the emulator thread**,
within the device's run-to-run noise. Only `perf stat -e L1-icache-load-misses` on the device would show the
mechanism.

## Mario Kart DS's race start: where the JIT code's time goes (2026-10-19, third speed session's third phase, agent mk3jit; untested on device)

The device profile (mkbase, frames 1650-2149) puts 29.1% of the emulator thread's samples in JIT code. Under qemu
(MK scene, frames 1650-2150, the mk2 head 3a743f3) the JIT code runs **1.54M of the thread's 13.27M instructions a
frame (11.6%)**: its cycles per instruction on the A55 are about twice the thread's average (IPC ~0.2 against
0.49), so the JIT's cost is mostly stalls, not instructions. Per block (`MELONDS_JIT_PERFMAP=1` + the qemu per-TB
counts, 8,888 blocks ran in the window):
- **the shared stubs are a third of it**: ExitCheck 163k + 37k (ARM9, ARM7), the ARM9's JumpTo 140k (~7,000 register
  jumps a frame, 20 instructions each), DynExit 59k + 10k, the patched memory calls' save/restore 60k;
- the hottest guest block (a word-copy loop at 0x0214d048, ~1,900 iterations a frame) is 71k, then a long tail:
  the top 50 blocks are 55%;
- the dynamic mix: ALU 30%, constants 13% (the exit check's timestamp address alone is three MOV/MOVKs at ~18k exit
  checks a frame), loads/stores of CPU state 15%, flags 7%, cycles 5%, guest memory 4%.
Outside the code: the JIT compiling the race's new code (~4.5 blocks a frame, ~214k instructions a frame), the
interpreter's MCRs (~1,000 a frame) and the GX FIFO port's STRs through the patched memory stubs (~1,830 a frame).

### The JIT's code memory on 2 MB huge pages (patch 0085, `jit.hugepages`, default `on`)

Session 1's jit patch 0021, never merged into BananaDS, ported unchanged: the 32 MB code slice aligned to 2 MB and
`madvise(MADV_HUGEPAGE)`'d before Reset's first touch (BananaLinux's kernel: THP `madvise`, 4 KB pages). MK's blocks
are 4.4 MB (~1,100 4 KB pages, mkjit's census) spread over the slice and its far region; the profile has 61k L1
iTLB misses a frame (4.15 per 1000 instructions) and the A55's 1024-entry L2 TLB is shared with the data side
(main RAM's fastmem pages alone are 1,024). With huge pages the code takes a few L2 TLB entries. qemu can't see it
(no TLB). Exact by construction: no emulated state moves, only the slice's address. **Expected +0.3..1.5% of the
emulator thread** (half of the iTLB misses in JIT code at ~5 cycles each if they hit the L2 TLB, ~25 if they
walk; more if the data side's walks drop too). The log line `[jit] code memory at <addr>, 32768 kB, N kB on huge
pages` says on the device whether the kernel gave them; `prof.pmu`'s iTLB line is the check of the mechanism.

### An ARM9 MCR that CP15Write ignores, compiled to its cycles (patch 0086, `jit.cp15nop`, default `on`)

NitroSDK's `DC_FlushRange`/`DC_StoreRange`/`DC_InvalidateRange` loops issue an `MCR p15` per 32-byte line, and MK's
race start runs ~1,000 a frame (c7,c10,4 drain and c7,c14,1 clean+invalidate ~394 each, c7,c10,2 ~123, c7,c6,1
~71: `MELONDS_JIT_PERFMAP` + the qemu counts at the A_MCR call sites). The JIT had no compiler for MCR, so each went
through the interpreter's call: R15, the instruction and CodeCycles stored, the cycles and the CPSR saved, every
guest register flushed, `A_MCR` -> `CP15Write` (a switch that does nothing for c7's operations here: melonDS
doesn't emulate the caches) -> `AddCycles_CI(2)` through a virtual call, then the cycles and the CPSR reloaded
and the registers loaded again. With 0086 an ARM9 `MCR p15` whose operation is c7's and not wait for interrupt
(0x704/0x782) or an instruction cache invalidation (0x750-0x752, which end the block and do something) is compiled
to `Comp_AddCycles_CI(2)`, the same cycles, after a two-instruction check of the mode bits: in user mode `A_MCR`
takes the undefined instruction exception, so mode bits `0x?0` go to the far region, which does what the old call
did (the same stores, `A_MCR`, every loaded register reloaded) and takes this instruction's cycles back when they
were counted in `ConstantCycles`. R13/R14 holding known constants keep the old call. qemu, MK 1650-2150, the
thread without Gengis: **12.590M -> 12.516M instructions a frame (-74k, -0.6%)**: `A_MCR` -32k, `CP15Write` -24k,
`AddCycles_CI` -9k, the JIT code -9k.

Exact: MK's 8 pictures and audio identical to the base with it on and off; main RAM, both CPUs' registers and the
geometry engine's timing (with the ARM9's timestamp) every frame 1500-2100 off vs on (`bananads-difftest --regs
--timing`): SAME; full savestates at 1850 and 2000 identical to the checkpoint's (`a57737e0...`, `f476c39e...`);
`tests/cp15nop` (the MCRs in DC_FlushRange's loop with every register live, conditional ones, timer reads, then
user-mode MCRs with R13/R14 known and not known, 1,000 times): main RAM and registers every frame off vs on,
SAME. Sabotages: the fast path's cycles one short changes MK's audio and every picture from 1740 on, and
`tests/cp15nop`; the far region's cycles not taken back changes `tests/cp15nop`'s timer (MK never runs an MCR in
user mode). **Expected +0.3..0.5% of the emulator thread** on the race start: ~75 instructions, two calls (one
virtual) and a register flush/reload per MCR, ~1,000 a frame, at the thread's ~2 cycles an instruction and some
more for the calls. Untested on device.

### Two code regions kept for the swap back (patch 0087, `jit.region_trio`, default `on`)

0071 (`jit.region_pair`) keeps the one code region a CPU last left. Counting `SetupExecutableRegion`'s calls by
region on MK's race start (frames 1650-2150, an instrumented build) still gave ~640 setups a frame: the ARM9's
ITCM 191, BIOS 147 and main RAM 103 (an IRQ goes main RAM -> the BIOS at 0xFFFF0018 -> the ITCM handler -> the BIOS
-> main RAM: three regions, and the pair doesn't keep the BIOS), the ARM7's shared WRAM 118, BIOS 77 and WRAM 8
(its sound driver runs at 0x037Fxxxx, shared WRAM, which the pair doesn't keep), each ~115 instructions
(`ClassifyAddress`, `GetMirrorLocation`'s switch, `SetupExecutableRegion`). Many of them were in the slice after a
CPU wakes up (`ARMv5/ARMv4::Execute<JIT>`, which didn't use the pair): the ARM9 halts for VBlank and the IRQ's
round runs there. With 0087 two regions are kept (the last left and the one before), the ARM9's BIOS (a 4 KB
mirror, memoryOffset 0, under the same ITCM/DTCM checks as main RAM) and the ARM7's shared WRAM can be kept, and
`Execute<JIT>` swaps too. The shared WRAM's lookup, start and size are a function of `SWRAM_ARM7`'s Mem and Mask,
so a kept one is swapped back only while Mem and Mask are those its lookup and size say. A swap gives exactly what
`SetupExecutableRegion` would. Setups left: ~20 a frame (by its instruction count). qemu, MK 1650-2150: the region functions **-34k
instructions a frame** (`SetupExecutableRegion` -27k, `GetMirrorLocation` -24k, `ClassifyAddress9/7` -11k, the
setup calls -7k; the swap itself +34k: ~35 instructions a swap against the pair's ~24, ~1,650 ARM9 swaps a frame).

Exact: MK's pictures and audio = base on and off; main RAM, registers and timing every frame 1500-2100 off vs on:
SAME; savestates at 1850/2000 = the checkpoint's; `exact-suite.sh` off vs on: FAILS=0. Sabotage: a swapped-in
BIOS/shared WRAM/WRAM region's start 4 bytes off hangs MK at boot. Not caught by any check here: a wrong shared
WRAM Mem/Mask check (MK doesn't remap its shared WRAM in the scene). **Expected +0.2..0.4% of the emulator thread**:
on the device `GetMirrorLocation` + `SetupExecutableRegion` + `ClassifyAddress9` were 1.07% on mkbase, 0071 took
~40% of that; this takes nearly all of the rest (and `GetMirrorLocation`'s indirect branch), less the swap's own
cost. Untested on device.

### The emitter's CountLeadingZeros by CLZ (patch 0088, `jit.fastclz`, default `on`)

During the race start the JIT compiles ~4.5 new blocks a frame, ~0.2M instructions a frame of the emulator thread
(qemu). `IsImmLogical` was 43k of it from only ~39 calls a frame (counted by caller in an instrumented build: nearly
all from `ANDI2R`): Dolphin's portable `CountLeadingZeros`, called four times per check, is a bit-by-bit loop of up
to 64 rounds, ~1,100 instructions a call. 0088 makes `CountLeadingZeros(value, 64)` `__builtin_clzll` (64 for zero),
the same count. qemu, MK 1650-2150: **-38k instructions a frame** (`IsImmLogical` -40k, `CountLeadingZeros` +2k).
Exact by construction, checked: MK's scene compiled with `MELONDS_JIT_PERFMAP=1` without ASLR (`setarch -R`), the
code dump of every block and stub (5.7 MB) and the perf map byte-identical between `jit.fastclz` on and off;
pictures and audio = base both ways. Sabotage (CLZ one too many for one value) emits different code and the run
dies on an illegal instruction. **Expected +0.1..0.3%** of the emulator thread in the race start (the device
profile's "JIT compiler" area is 1.2%; this was ~a fifth of the compiler's instructions, a predictable loop), only
while new code is being compiled. Untested on device.

## BananaJIT

BananaJIT (jit/, `-DBANANADS_JIT=banana`) is parked. Its block-timing mode reached about 44 fps in this scene,
against melonDS's JIT's 48, before the 2D engine. Its gap was instruction fetch: 41% more L1I refills and 90%
more iTLB walks. Its block chaining with a countdown cycle budget and its resident registers are worth reading
before doing (6) in melonDS's compiler.

## The geometry engine after 0011 (2026-10-18, second speed session, agent geom; untested on device)

What a FIFO entry still costs in `GPU3D::Run` (qemu, per-TB counts inside the geometry functions, device build):
dsstress L5 (CPU-paced lit quad strips): ~2,850 entries and ~710 vertices a frame; `Run` 385k instructions
(~135 per entry, lighting included), polygon setup 195k (~550 per quad), vertex 69k, clipping 48k, the FIFO
writes 122k. memio's gxstm (NNS-style draws by STM/DMA): ~2,700 entries, `Run` 292k (~103 per entry). No single
hot spot is left: the loop head and pipe read ~20, the pipe refill with the DMA and IRQ checks ~25-40 every
other entry, a parameter's store and its cycle ~25, a command's cycle accounting ~25. A count of the
stream shows why batching inside `Run` gains little: the stream is CPU-paced, `Run` is called ~1,000 times a frame
with 2.7 (L5) to 5.3 (gxstm) entries each, and its entry and finish (FinishWork, GXSTAT bits) cost ~90
instructions per call, a fifth of `Run` on L5.

- **0050 (`render.geom_batch`):** the DMA and IRQ checks once per `Run`. qemu: `Run` -3.7% (L5), -7% (gxstm),
  geomtest -5.6%; geometry -2..-4%.
- Tried and dropped: a command's middle parameters (a matrix's 2nd-15th) in a short loop with their cycles added
  at once (exact): gxstm `Run` -0.1%: the parameters arrive a few per `Run` call (0.6 per batch).

## Mario Kart DS's race start: the GX FIFO DMA and the command loop (2026-10-18, third speed session, agent geomlazy; untested on device)

The device profile of Mario Kart DS's race intro and countdown (BananaLinux `perf/bananads/session3/`) has 3D
geometry at 31% of the emulator's thread. The stream, counted on the scene (frames 1700-2150, per frame): ~28,900
FIFO entries and ~1,330 polygons; ~31,700 words sent by the GX FIFO DMA (mode 7, through `core.dma_block`'s block
path) in ~1,260 `DMA::Run9` calls; ~2,180 `GPU3D::Run` calls with 13.4 entries each; the FIFO holding ~62 entries
(under half, so the DMA restarts at every burst's end); GXSTAT's FIFO IRQ in mode 2 (empty), its IF bit toggling
~270 times a frame (not in IE); GXSTAT read 7 times; RAM_COUNT, the matrices and the tests never. Commands: texcoord
7,500, VTX_16 3,770, normal 4,600 (about one light each), the other vertex formats 4,800, begin/end 900 each.

qemu, device build, the emulator's thread over frames 1650-2150 (mkbase: 16.2M instructions a frame): `Run` 3.79M
(131 per entry), `WriteToGXFIFOBody` 2.08M (66 per word), `DMA::Run9` 1.51M (47 per word), polygon setup 1.14M,
vertices 0.78M.

- **0050 (`render.geom_batch`, session 2's, cherry-picked):** here the FIFO is under half all the time, so every
  refill of the pipe ran the four DMAs' checks and the IRQ check: `Run` 3.79M -> 3.09M.
- **0060 (`render.gx_dmafast`):** the DMA's block unpacked with its state in registers: `WriteToGXFIFOBody` +
  `DMA::Run9` 3.58M -> 1.86M + 0.15M (the CPU's own GX writes).
- **0061 (`render.geom_fifo`):** `Run`'s loop reads the pipe and the FIFO as one queue (no moves between the rings
  every other entry, GXSTAT's push/pop and test bits cleared only after a push/pop or test command): geometry and
  the GX DMA 7.38M -> 7.14M. GCC now keeps the lighting out of line (`CalculateLightingBody`, 0.64M).
- **0062 (`render.gx_dmafast` too):** the block's burst timing loop with ARM9Target and the table in registers
  (~13 -> ~10 instructions a word): `DMA::Run9` 1.86M -> 1.77M. Timing only, so main RAM can't check it: the full
  savestates at frames 1850 and 2000 are byte-identical to mkbase's, and a sabotage (a burst's first unit timed as
  sequential) changes the savestate.
- Together: the emulator's thread 15.78M -> 13.25M instructions a frame (-16%; without the Gengis bins and 2D lines
  it draws itself, which vary with thread timing under qemu: 0.3-0.9M), geometry and the GX DMA 9.64M -> 7.05M.

Lazy geometry (running the engine only when something observes it) was checked against this stream and not built:
`Run`'s per-call cost is ~2,180 calls x ~90 instructions, ~1% of the thread, and in this scene nearly every slice end
is an observation point (the GX FIFO DMA restarts whenever the FIFO is under half, and melonDS's timing is defined by
when `Run` is called, so a deferred engine has to replay every call point with the FIFO as it was then). The
observers and the reasoning: BananaLinux `perf/bananads/session3/geomlazy-evidence/observers.md`.

## Mario Kart DS's race start after the checkpoint: the CPU's GX stores and the polygon sort (2026-10-19, third speed session's second phase, agent mk2geom; untested on device)

qemu, device build, the emulator's thread over frames 1650-2150 of the scene (`mk-scene.sh`), from the checkpoint
(8e98258: 12.88M instructions a frame without the Gengis bins it draws itself). Where the GX FIFO's words come from
in this scene (`MELONDS_IOSTATS=1700`, frames 1700-1899, per frame): ~31,700 by the GX FIFO DMA, **2,076 by the
ARM9's single STRs to 0x04000400** and 16 by STMs (2 of them). So the CPU's stores are ~6% of the words; the
device profile's `WriteToGXFIFOBody<true>` (6.4%) was the DMA's per-word path, which 0060 replaced (2.08M -> 0.15M
instructions a frame; the 0.15M left are the STRs).

### The JIT's STMs into the GX FIFO in one call (patch 0063, `render.gx_batch`, default `on`)

Session 2's 0021 (memio), ported: `SlowBlockTransfer9` hands an STM into the GX FIFO to
`GPU3D::WriteGXFIFOWords`, which runs 0060's block loop (now `WriteGXFIFOWordsFast`, shared with the DMA's block;
the words go on after one that stalls, as word by word). Session 2's own loop and its DMA hook are not needed (0060
does the DMA). On MK it does nothing (2 GX STMs a frame) and costs the ~210 other STMs a frame (into the DTCM) ~16
instructions each for the test: +3.4k a frame (+0.03%). It is for HeartGold, where the 2026-10-12 profile had STMs
into the GX FIFO at 5.7% of the thread (session 2's estimate: +1..2.5% fps there). Exact: tests/gxstm (session 2's
ROM, added here) main RAM every frame and pictures off vs on; Mario Kart's frames, audio and savestates. Sabotage (an
STM's push/pop commands not counted): gxstm's difftest DIFFERENT at frame 1 (Mario Kart doesn't catch it).

### The JIT's STRs to the GX ports with fewer layers (patch 0064, `render.gx_str`, default `on`)

Session 2's 0022 (memio), ported unchanged (its `GXStr` read as the GPU3D is created). MK's 2,076 STRs a frame to
the FIFO port: `WriteToGXFIFOBody` 153.9k -> 103.1k, `GXWrite9Route` 23.6k -> 0, `WriteToGXFIFO` 7.9k -> 57.3k (the
quick parameter path inline), `SlowWrite9` +9.4k: **-15.6k a frame (-0.12%)**; command words and last
parameters still take the decoder. Expected on the device: ~-0.1%. Exact: tests/gxstm, geomtest, dsstress
difftests and pictures; Mario Kart's frames, audio, main RAM every frame (with registers and the geometry timing
state) and savestates at 1850/2000. Sabotage (a quick parameter into an empty FIFO put in the FIFO, not the pipe):
Mario Kart's savestate at 1850 differs, gxstm hangs.

### VBlank's polygon Y-sort as a counting sort (patch 0065, `render.geom_sort`, default `on`)

MK's race start sorts ~1,330 polygons a frame with `std::stable_sort` (`__move_merge`, `__insertion_sort`,
`__merge_adaptive`: 108.7k instructions a frame, comparisons whose branches can't be predicted, and a buffer
allocated and freed every frame). A stable counting sort on the 17-bit `SortKey` in two passes (low 8 bits, then
high 9) gives the same order: `YSortCounting` 39.5k, **-69.2k a frame (-0.54%)**. Session 1 dropped this patch
because the test ROMs draw too few polygons (DEAD-ENDS.md); sets under 64 keep `std::stable_sort`. Expected on the
device: -0.5..-0.8% of the thread (the profile has the sort at 0.9%; the merges' mispredicted branches go too).
Exact: the sorted order is in the savestate, and Mario Kart's savestates at 1850 and 2000 are byte-identical to the
checkpoint's; frames and audio too. Sabotage (the first pass unstable): the savestate at 1850 differs.

### The GX FIFO DMA's block path in a function of its own (patch 0066, `render.gx_dmarun`, default `on`)

MK calls `DMA::Run9` ~1,240 times a frame for the GX FIFO DMA, each call ending at the slice's end after one block,
and each paid ~185 instructions of `Run9`'s setup for all its cases. `Run9` now checks first for the one case where
the whole call takes the GX block path with `render.gx_dmafast`'s timing (32-bit FastDMA from main RAM going up, to
main RAM's end for all its remaining units, into the fixed 0x04000400, block paths and GeomFast on) and runs it in
`Run9GX`, the same statements; everything else goes to the old body (`Run9Full`). `DMA::Run9` 1,772.8k ->
`Run9GX` 439.0k + `WriteGXFIFOBlock` 1,135.7k (now out of line) + `Run9Full` 57.5k + the check 76.2k: **-64.4k a
frame; the thread -69.7k (-0.55%)**. Expected on the device: -0.4..-0.6% (fewer instructions per call, and a
340-instruction function instead of `Run9`'s ~1,500 between the JIT's blocks for the L1I).

Exact: Mario Kart's frames, audio, main RAM every frame with registers and geometry timing, and full savestates at
1850 and 2000 identical off vs on and to the checkpoint's. A trace build (every `Run9` call's ARM9 timestamp,
source, counts, burst table position and state folded into a hash; not committed) gives the same hash over all
676,042 calls of the scene off and on. Sabotage (one cycle more when a block starts at a burst's second unit): the
trace, the savestate at 1850 and the audio differ. Two other sabotages (a DMA's first unit not started as a new
burst; a burst restarting inside a block) left even the trace identical: MK's GX DMA never reaches those branches
(they are the old code's statements, unchanged). Test ROMs and exact-suite.sh as 0063's.

Together (0063-0066): the emulator's thread 12.88M -> 12.71M instructions a frame (-1.3%). The geometry that is
left has no single hot spot by the per-TB counts: polygon setup 1.14M (3,300 calls a frame, ~340 instructions each,
no block over 11%), vertices 0.78M (8,100 a frame, ~95 each, 40% the NEON transform), lighting 0.64M (4,600 normals,
~140 each), `Run` 2.21M (~20 a FIFO entry plus the commands), `DMA::Run9` 1.77M (~37 a word, plus ~185 per call over
~1,240 calls before 0066).

## Measuring

The handheld runs everything; cloud agents can't reach it. Put each change behind a toggle (a setting or an
environment variable), so it can be A/B'd on the device, and say what to measure.

- `tools/device/quick.sh <bananads> <difftest>` is the correctness pass. It compares the stress ROMs' pictures
  with JIT on and off, and runs the diff ROMs' results through difftest. Its kit lives on the handheld in
  `/storage/bananads-device`; docs/DEVICE-TEST.md has it.
- `tools/device/hgquick.sh <bin>...` is the short HeartGold run (600 measured frames at 1992 MHz).
  - `EXTRA="--set ..."` passes settings.
  - It uses `walk.txt`, whose `left` presses are mangled into no-ops (`1500eft:90`). The scene walks right for
    90 frames every 180. It stays as it is because every number above used it; `walk-lr.txt` is the corrected one.
  - The ROM and save aren't in the repo.
- Frame-exact checks use `--set rtc.fixed=YYYY-MM-DD_HH:MM:SS` with `headless.dump_dir`/`headless.dump_every`,
  then `cmp`.
- Test ROMs that need no game: `stressrom/` (3D), `bananads/tests/ds2dtest/` (2D), `bananads/tests/iorom/` (the JIT's
  slow paths), `bananads/tools/ipcrom/` (IPC) and `bananads/tools/dmasnd/` (DMA, sound, capture). `python3 build.py` in
  each, needs clang and ld.lld.
- **A profile without perf** (BananaOS has none): `--set prof.hz=1000 --set prof.from=1500` samples the emulator's
  thread from frame 1500 and reports at the quit (SETTINGS.md `prof.*`; `tools/device/hgprofile.sh` runs it on
  HeartGold). A perf event on that thread's CPU time (`task-clock`, its overflows signalled to that thread alone
  through `F_SETOWN_EX`), or where there is none a timer on its CPU clock (`timer_create(CLOCK_THREAD_CPUTIME_ID)`
  with `SIGEV_THREAD_ID`), sends it `SIGPROF`; the handler stores the interrupted PC in a preallocated array (`SA_RESTART`,
  errno kept), nothing else, so the emulation is exact by construction (checked: the exactness suite with it on,
  FAILS=0). At the end the PCs are resolved: the binary's own `.symtab` (or `prof.syms`, the unstripped twin:
  Buildroot strips the device's binary; keep `output/build/bananads-<version>/bananads/buildroot-build/bananads` or the CI's
  unstripped artifact), `dladdr` for the libraries, and anything outside a module's code is JIT-generated code
  (named per block with `MELONDS_JIT_PERFMAP=1`). The timer is checked only at the kernel's tick (250 Hz on the
  RG DS); the perf event isn't, so 1000 a second works (a 1200-frame HeartGold run at 1104 MHz: ~17000 samples).
  The handler costs a few microseconds a sample: the fps of a profiled run is not a benchmark. `prof.pmu` (on) adds
  the A55's counters for the same thread and frames: cycles and instructions (IPC), branch, L1I, L1D and iTLB
  misses, what `perf stat` would give. With LTO, `NDS::RunFrame`'s slice loop is inlined into its caller: in headless runs it shows as
  `main::{lambda()#1}` (the scheduler/RunFrame share of earlier profiles).
- Keep runs short, and ask the user before long benchmarks.
