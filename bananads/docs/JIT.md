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

- **The encoder** (`jit/a64.h`): one function per instruction form. `tests/jit_encodings.sh` checks all 69 forms
  against LLVM's assembler (`ctest`, any host).
- **Registers:**
  - x29 holds the CPU, w28 the cycles, w27 the CPSR.
  - Guest r0-r7 live in w19-w26 and r8-r14 in w9-w15. Each is loaded on first use and stored back only if it
    was written: at exits, and before any call out.
- **Flags:** ARM's condition codes and NZCV bit positions are the same as AArch64's. A condition is
  `msr nzcv` plus one `b.cond`. An S instruction takes its flags straight from the host's arithmetic (`mrs
  nzcv`), and only for the flags a later instruction reads (the framework's SetFlags analysis).
- **Cycles:** each instruction costs what melonDS's interpreter charges for it, taken or skipped.
- **What is native (phase 1):** ARM data processing (all 16 operations, immediate and immediate-shift operands,
  RRX and the #32 shift forms, with the shifter's carry), when Rd isn't the PC.
- **Everything else** runs through melonDS's interpreter, one instruction at a time, using the framework's
  protocol (R15, CurInstr and CodeCycles set first, and the PC stored before the condition check). So every
  instruction works from the start.
- **Fastmem:** off with BananaJIT for now.

**Checked (aarch64 under qemu):** the stress ROMs `dsstress-L1`, `L5`, `L10`, `dsswap` and `dsstress-ramp`
give pictures identical to the interpreter's at frames 150, 300, 450 and 600 (CI checks L5). In `dsstress-L5`,
869 of the 1,598 compiled instructions are native. RAM drifts from frame 9 on through block-granularity timing,
as with melonDS's JIT (frame 6), with the same pictures. **Not checked:** a device, real games, speed. qemu's
numbers don't say anything about the A55.

`BANANAJIT_TRACE=1` logs every compiled block: each instruction's address, kind, and whether it is native.

**Next, in order of what the trace shows the interpreter still doing:**
1. Loads and stores (LDR/STR immediate, LDRH/STRH, LDM/STM): through melonDS's memory functions, then
   fastmem.
2. Branches (B, BL, BX) natively, and linking blocks with static targets directly.
3. Thumb: the same operations, decoded from Thumb.
4. Shifts by a register, multiplies, and the DSP instructions.
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
