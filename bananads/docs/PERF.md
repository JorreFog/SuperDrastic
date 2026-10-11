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

Left, by agent: the geometry's FIFO ring buffer (savestates hold both FIFOs) and its vertex copies; rotscale
sprites and OBJ mosaic, skipping unchanged lines in 2D; the mix's per-sample tail and batched ARM9 DMA for VRAM.

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

## BananaJIT

BananaJIT (jit/, `-DBANANADS_JIT=banana`) is parked. Its block-timing mode reached about 44 fps in this scene,
against melonDS's JIT's 48, before the 2D engine. Its gap was instruction fetch: 41% more L1I refills and 90%
more iTLB walks. Its block chaining with a countdown cycle budget and its resident registers are worth reading
before doing (6) in melonDS's compiler.

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
- Keep runs short, and ask the user before long benchmarks.
