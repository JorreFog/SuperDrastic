## Context (shared by all porting agents)

We are building a bit-exact replacement for the 3D rasterizer of DraStic r2.5.2.2 (closed-source Nintendo DS
emulator, Linux aarch64 build, PIE, not stripped). Your job is to port a group of DraStic's 3D routines to exact C,
verified bit-for-bit against the originals by unit tests that run inside DraStic's own process.

Resources (all on this machine):
- DraStic binary: /tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad/dsrun/drastic
- Full disassembly: /tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad/re/drastic.dis
  (llvm-objdump; addresses are file offsets). Hand-written NEON functions are size-0 symbols named *_asm; they run
  to the next symbol. Local data tables (e.g. iota_0_3_value) sit between them.
- Decompiler (angr, approximate, fine for C functions, poor for NEON):
  `cd /tmp/claude-0/-home-user/2214c741-dca5-5de9-abc6-6b47f5d47ba5/scratchpad/re && ../angrenv/bin/python decomp.py ../dsrun/drastic <symbol> [...] 2>/dev/null`
  Some already decompiled in re/core.c. Symbols: `llvm-nm -S --defined-only <binary> | grep ...`
- Unit-test harness: /home/user/SuperDrastic/tools/rast/ut/ut.h and run.sh. A test is a C file defining
  `void ut_main(void)`; run it with `/home/user/SuperDrastic/tools/rast/ut/run.sh <test.c> <your spec .c files>`.
  It is compiled for aarch64 and preloaded into DraStic under qemu-aarch64; ut_main runs in a constructor before
  DraStic's main, then the process exits. Call an original routine with `DS(type, 0xOFFSET)(args...)`. Helpers:
  rnd(n), rndr(lo,hi), rndfill(), ut_cmp(what, drastic_buf, ours_buf, n) (counts into ut_fail). UT_SEED env var
  picks the random seed. Startup is under a second, so iterate freely. If a routine reads DraStic globals (e.g.
  reciprocal_table, filled by initialize_video_3d), get their address from the symbol table and initialise them
  in your test as DraStic would.
- Shared helpers: /home/user/SuperDrastic/src/rast/rast.h, ds3d.h (layout notes; read, don't change).

Rules:
- Write your port(s) to /home/user/SuperDrastic/src/rast/spec/<group>.c (+ <group>.h with prototypes) and tests to
  /home/user/SuperDrastic/tools/rast/ut/t_<group>.c. Touch no other files. Do not git commit.
- Name each port spec_<original name without _asm suffix> with the original's argument list and memory behaviour,
  so tests can run original and port on identical copies of all input/output memory and compare everything they
  write (outputs AND any in-place updated inputs, AND bytes past the end that the original may write as padding:
  NEON code often writes whole 8/16-lane vectors past the logical count; replicate that exactly).
- Plain C is preferred; use arm_neon.h intrinsics where float rounding or NEON-specific instructions (frecpe,
  frecps, fcvtzs with fraction bits, saturating ops, etc.) must match exactly.
- Tests: randomized inputs over realistic AND extreme ranges, all tail lengths (counts not multiple of 8/16,
  counts of 0 or 1 if the original allows them), several thousand calls per routine, at least 3 seeds. A port is
  done only when all pass.
- Never kill other processes (no pkill/killall); other agents run tests concurrently. Use timeouts.
- In each spec file's header comment, document per routine: what memory it reads (offsets/strides relative to
  its pointer arguments) and writes, and the exact per-pixel/per-element math. This documentation is the main
  deliverable alongside the verified code: it will be used to build a fused per-pixel rasterizer.

Known layout so far: per-line span data is a struct of arrays with 44 entries of 4 bytes per array (0xb0 bytes per
array): e.g. +0x000, +0x0b0, +0x160, +0x210, ... +0x630 holds the u16 pixel count per line (stride 4), +0x580 and
+0x6e0 are also used. Hi-res scanline buffers are 512 pixels x 4 bytes (0x800 bytes per line). Pixel colour format
in scanline buffers: r6 | g6<<8 | b6<<16 | a5<<24 (| flag<<31). Batches handed to the per-pixel stages are up to
512 pixels spanning several lines.

Final answer: list each routine ported, test status (calls x seeds, pass/fail), and a concise semantic summary
per routine (inputs with offsets, math, outputs) plus anything surprising.
