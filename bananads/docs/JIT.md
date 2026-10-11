# BananaDS's CPU: melonDS's JIT, and what the DraStic JIT study can bring to it

BananaDS runs the DS's two CPUs on **melonDS's JIT** (`ARMJIT_A64`, on by default; `jit=off` for its
interpreter). That JIT is GPL code, maintained upstream, and already in this build.

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
