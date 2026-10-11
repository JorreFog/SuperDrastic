# BananaDS's CPU: BananaJIT, melonDS's JIT, and the DraStic JIT study

BananaDS runs the DS's two CPUs through a JIT (`jit=off` gives melonDS's interpreter). There are two to pick
from at build time:
- `-DBANANADS_JIT=melonds` (the default) is melonDS's own `ARMJIT_A64` compiler.
- `-DBANANADS_JIT=banana` is **BananaJIT**, BananaDS's own code generator (below).

## BananaJIT

BananaJIT is in `jit/`. It is a new AArch64 code generator inside melonDS's JIT framework. The framework still
does block decoding, the block cache, invalidation and idle-loop detection; BananaJIT replaces only the
`ARMJIT_A64` compiler, through one guarded line that CMake adds to melonDS's `src/ARMJIT_Compiler.h`. It was
written from the Arm Architecture Reference Manual, with no DraStic code.

- **The encoder** (`jit/a64.h`): one function per instruction form. `tests/jit_encodings.sh` checks all 86 forms
  against LLVM's assembler (`ctest`, any host).
- **Registers:**
  - x29 holds the CPU, w28 the cycles, w27 the CPSR.
  - Guest r0-r7 live in w19-w26 and r8-r14 in w9-w15. Each is loaded on first use and stored back only if it
    was written: at exits, and before any call out.
- **Flags:** ARM's condition codes and NZCV bit positions are the same as AArch64's. A condition is
  `msr nzcv` plus one `b.cond`. An S instruction takes its flags straight from the host's arithmetic (`mrs
  nzcv`), and only for the flags a later instruction reads (the framework's SetFlags analysis).
- **Cycles:** each instruction costs what melonDS's interpreter charges for it, taken or skipped.
- **What is native:**
  - ARM data processing: all 16 operations, immediate and immediate-shift operands, RRX and the #32 shift forms,
    with the shifter's carry, when Rd isn't the PC.
  - ARM loads and stores: LDR/STR/LDRB/STRB (immediate and shifted-register offsets, pre/post-indexed,
    write-back) and LDRH/STRH/LDRSB/LDRSH. The address is computed natively. The access goes through the same
    functions the interpreter calls (`DataRead*`/`DataWrite*` and `AddCycles_CD/CDI`), so its timing and
    side effects are the interpreter's.
  - ARM9 loads (all of the above load forms, ARM and Thumb) inline, with no call, for ITCM, DTCM and main RAM.
    It follows `ARMv5::DataRead8/16/32` and `AddCycles_CDI` step for step: the PU_Map read permission,
    DataRegion, DataCycles (1 for the TCMs, else `MemTimings[addr >> 12][1 or 2]`), and the cycle formula.
    melonDS models no ARM9 data cache, so this is exact. Any other address, or no permission, takes the
    call. `jit.fastloads=off` turns it off; with it on or off, `bananads-difftest --every` finds main RAM
    byte-identical after every frame (CI, ARM and Thumb), so it changes no cycle.
  - ARM9 stores inline for DTCM and main RAM (ARMv5::DataWrite*, AddCycles_CD, NDS::ARM9Write*). A main-RAM
    store goes inline only when melonDS's code bitmap (`CodeIndexMainRAM`, which its `CheckAndInvalidate`
    reads) has no compiled code in that 16-byte chunk; then the check would have done nothing. Code
    there, ITCM, no write permission or any other address: the call. `jit.faststores=off`; main RAM is
    identical after every frame with it on or off (CI).
  - ARM7 loads and stores inline for main RAM and ARM7 WRAM (`ARMv4::DataRead/DataWrite*`,
    `NDS::ARM7Read/Write*`, `AddCycles_CDI/CD`). Stores take the code-bitmap check, as on the ARM9. The cost
    reads `CodeCycles` at run time. It is set by the last jump, so it can name a different 32 KB than the PC.
    `jit.fastmem7=off`. Checked with `STRESS_ARM7=arm|thumb stressrom/build.py`, an ARM7 that never halts:
    main RAM is identical after every frame with it on or off (CI). One extra cycle per access is caught
    after frame 1.
  - Block linking: a block's exit does what melonDS's `Execute` loop does between two blocks, in the same
    order: the StopExecution check, `Timestamp += Cycles`, the Target check, the region check and the
    FastBlockLookup lookup. Then it jumps straight into the next block instead of returning. The table is
    read every time, so an invalidated block is never entered. `jit.link=off` turns it off; with it on or
    off, main RAM is identical after every frame (CI, ARM and Thumb).
  - Thumb: every Thumb instruction that means exactly what an ARM instruction means is compiled as that ARM
    instruction (`ThumbToARM`). That covers shifts by an immediate, add/subtract, MOV/CMP/ADD/SUB with
    immediates, the register ALU ops, the high-register ADD/CMP/MOV, PC/SP-relative ADD, ADD SP, and every
    load/store form except PUSH/POP/LDMIA/STMIA. One exception to "exactly": Thumb's PC- and SP-relative LDR
    doesn't rotate an unaligned word, so it uses its own helper.
- **Everything else** runs through melonDS's interpreter, one instruction at a time, using the framework's
  protocol (R15, CurInstr and CodeCycles set first, and the PC stored before the condition check). Today that
  is branches, LDM/STM/PUSH/POP, multiplies, shifts by a register, MSR/MRS, coprocessor and DSP instructions.
- **Fastmem:** off with BananaJIT for now.

**Checked (aarch64 under qemu):** the stress ROMs (`dsstress-L1`, `L5`, `L10`, `dsswap`, `dsstress-ramp`)
give pictures identical to the interpreter's at frames 150, 300, 450 and 600. They were checked both as ARM code
and compiled as Thumb (`STRESS_THUMB=1 stressrom/build.py`); CI checks L5 both ways. Native share of compiled
instructions in `dsstress-L5`:
- **ARM:** 1,533 of 1,598 (96%).
- **Thumb:** 3,782 of 4,266 (89%). Most of the rest is conditional branches, MUL, B and PUSH/POP.

RAM drifts from frame 9 on through block-granularity timing, as with melonDS's JIT (frame 6), with the same
pictures.

**Not checked:** a device, real games, speed. qemu's numbers say nothing about the A55, and under qemu the
stress ROMs are bound by 3D, not by the CPUs.

`BANANAJIT_TRACE=1` logs every compiled block: each instruction's address, kind, and whether it is native.

**Next, in order of what the traces show the interpreter still doing:**
1. Branches natively. Linking is done, but a branch still calls `JumpTo`. Its fetch costs go through the ARM9
   instruction-cache model; melonDS's JIT precomputes them, which is an approximation. A native branch has
   to keep the run-time cost to stay exact.
2. LDM/STM and PUSH/POP.
3. MUL/MLA (the interpreter charges by operand size), shifts by a register.
4. PUSH/POP/LDM/STM: the next big share of what the interpreter still runs. `ARMv4`'s cycle formula depends on the code and data regions at run time.
5. A device: `headless` fps against melonDS's JIT (docs/PLAN.md B).

## The DraStic JIT study (2026-10-11)

A separate session is mapping DraStic r2.5.2.2's ARM-to-AArch64 recompiler with REA. Its write-up is in that
session's `drastic-jit-re/` (README and notes), which **isn't in any repository yet**. What it found, in short:
- **Static register allocation:** guest r0-r14 in w13-w27, the guest's NZCV in the host's.
- **Inline ARM9 fastmem loads** through a 64 MB window.
- **Recursive, eagerly linked blocks**, in a three-part cache.
- **Coarse cycle counting**, paid before PC writes.
- **Idle-loop detection.**
- **Bugs**, found with an interpreter-against-JIT differential ROM: ASR-by-register carry, QADD with a negative
  operand, QDADD/QDSUB, the SMLAxy Q flag, the SMLALxy halves, MSR cpsr_f and Q.

## The rule for BananaDS: ideas yes, DraStic's code no

BananaDS is GPL, and DraStic is closed source. So:
- **Never copied into BananaDS:** anything taken from DraStic's binary. That covers decompiler output (the
  study's `out/c*/`), pseudocode written from it, disassembly listings used as templates, and binary patches to
  DraStic (the study's `fixes.c`, which belongs in SuperDrastic as a runtime patch for DraStic itself).
- **Fine to use:** what DraStic does, described in our own words. That means a technique (static register
  allocation, how idle loops are found), a measurement, or a bug class worth testing. It gets written fresh,
  against melonDS's JIT and the ARM Architecture Reference Manual.

A change to melonDS's JIT is best sent upstream to melonDS as well, so BananaDS's core stays unpatched
(docs/CORE.md).

## Testing a CPU back-end: `bananads-difftest`

`tools/difftest.cpp` runs a ROM on two CPU configurations (default: interpreter, then JIT) and reports the
first difference: the main-RAM bytes, both CPUs' registers, and how many pixels differ.

```sh
bananads-difftest --frames 600 rom.nds                         # all of main RAM at the end
bananads-difftest --every --frames 600 rom.nds                 # stop at the first frame that differs
bananads-difftest --region 0x02300000:0x4000 test.nds          # a test ROM's result block only
bananads-difftest --a jit=on --b jit=on --b <a new option> rom.nds   # JIT against JIT with a change
```

- **The interpreter against itself** gives the same RAM every run (checked on the stress ROM), so the
  harness is deterministic.
- **Interpreter against JIT drifts in timing** on a running game. melonDS's JIT counts cycles per block, so
  the animated stress ROM's RAM differs after a few frames while its pictures don't. Use ROMs that compute a
  result block and stop, with `--region`.
- **The study's differential ROM** (its own builder, in `tools/dyn/`) is that kind of ROM, and is the obvious
  first input once it is pushed.

## Where JIT work can make BananaDS faster (for milestone B)

These are measured on a device first (`perf`, and `headless` fps):
- **Fastmem** (`JITArgs::FastMemory`): on by default; it needs the kernel to allow the large mappings. Check
  it really is on on ROCKNIX.
- **Idle loops:** melonDS detects them too (`ARMJIT` idle-loop optimisation). Compare how many frames' worth
  of loops each finds in real games.
- **Block size** (`MaxBlockSize`, 32 by default) and the literal and branch optimisations: settings worth
  exposing as `jit.*` once B has numbers.
