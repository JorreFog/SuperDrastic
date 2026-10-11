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

3. **`MELONDS_SLICE=n`** overrides `kMaxIterationCycles`, melonDS's scheduler slice of 64 bus cycles. At 64 that
   is about 8,750 trips per frame through `Execute`, block lookup, timers, GPU3D and `RunSystem`.

melonDS's A64 quirks, left as they are because changing them changes timing:
- `Comp_AddCycles_C(true)` adds the not-taken path's cycles to the whole block.
- `Comp_AddCycles_CI(c, reg, shift)` adds its constant twice and ignores the register part.

## Levers not pulled yet, largest first

1. **The scheduler slice.** 64 → 256 bus cycles gives +8.5% and 512 gives +10%. It's an accuracy trade: coarser
   ARM9/ARM7 interleaving, which matters for IPC handshakes and anything timing-sensitive between the CPUs.
   - Make it a setting, default melonDS's 64.
   - Test many games before changing the default.
   - Or make it adaptive: long slices while neither CPU touches IPC or shared memory.
2. **2D off the main thread** (15.5%). Render lines on a worker from per-line snapshots of the 2D registers.
   Palette, OAM and VRAM changes need a write log or copy-on-write, since games change them mid-frame. The
   engine already keeps `PaletteDirty`/`OAMDirty`. melonDS flattens VRAM per renderer through `VRAMTrackingSet`.
3. **IO paths** (14.5%):
   - The 3D command stream comes from STMs into the GX FIFO: `SlowBlockTransfer9` → `ARM9Write32` →
     `ARM9IOWrite32` → `GPU3D::Write32` → `WriteToGXFIFO`, per word. A direct path would cut that chain.
   - LDMs from IE/IF.
   - Find which other slow accesses are hot: count them by address.
4. **3D geometry** (11.9%): NEON for the matrix and vertex work and the clipping, and less per-command FIFO
   overhead. It has to stay synchronous: GXSTAT, FIFO stalls and matrix reads depend on its timing.
5. **JIT dispatch** (9.7%):
   - Count why exits go back to the loop: `StopExecution` (an IRQ pending while masked keeps it set), the slice
     end, a region change (`SetupExecutableRegion`, e.g. ITCM ↔ main RAM), or a lookup miss.
   - `DynExit` could know more than one region.
   - Find why `Compiler::Reset` runs during gameplay: near or far memory full? The far region is only 4 MB, and
     link paths now use it too.
6. **JIT code** (~28%):
   - Guest registers resident across linked blocks; BananaJIT had this.
   - The fast-memory base kept in x26 instead of reloaded per block (three instructions).
   - Fewer R15/CodeCycles/RegionCodeCycles stores at exits.
   - Two known exits for a conditional branch at a block's end (it uses `DynExit` now).
7. **Audio** (3.8%) and **DMA** (2.4%).

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
- Keep runs short, and ask the user before long benchmarks.
