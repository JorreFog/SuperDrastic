# Runtime fixes for twelve DraStic r2.5.2.2 JIT bugs

This directory holds a prototype of runtime fixes that a preloaded library (for example SuperDrastic) can apply to the
DraStic r2.5.2.2 Linux aarch64 build from its constructor. The code is `fixes.c`. During testing it is `#include`d by
the qemu preload shim `tools/dyn/jitdump.c`, which also provides the hook on `cpu_translate_block` that the
post-translation fixes need.

**Target build.** The fixes apply to exactly one file: DraStic r2.5.2.2, ROCKNIX Linux aarch64, 1,581,200 bytes,
SHA-256 `dab3275ef772451b9235b78e64b2d0d041a28ac22937668231088c76b709e218`
(MD5 `17550db727f3b59d36b57746ad1944be`). All addresses below are file virtual addresses. At run time they are added to
the load base.

**Turning it on.** Set the environment variable `JITFIX`:
- `JITFIX=1` or `JITFIX=all` enables all twelve fixes (mask 0xfff);
- `JITFIX=0x<hex>` enables only the fixes in the mask;
- unset, empty or `0` changes nothing.

## Overview

| Mask | Bug | Symptom | Kind of fix | Where |
|---|---|---|---|---|
| 0x01 | #1 | MOVS Rd,Rm,ASR Rs gives the wrong carry | static, 1 word in a flag stub | 0x8f904 |
| 0x02 | #2 | QADD treats a negative Rn as a large positive number | static, 1 word in the emitter | 0x957b8 |
| 0x04 | #3 | QDADD/QDSUB never double Rn | post-translation rewrite + helper | hook on `cpu_translate_block` |
| 0x08 | #4 | SMLAxy sets Q wrongly when the accumulator is negative | static, 1 word in the emitter | 0x937b0 |
| 0x10 | #5 | SMLALxy uses the wrong operand halves | static, 7 words in the emitter | 0x95068..0x95080 |
| 0x20 | #6 | MSR CPSR_f (without the c field) never writes Q | post-translation rewrite + helper | hook on `cpu_translate_block` |
| 0x40 | #14 | TST Rn,Rm{,shift} clobbers C and V (ARM and Thumb, both CPUs) | static, 1 word in the translator | 0x93894 |
| 0x80 | #15 | Logical S ops, TST and TEQ with a rotated immediate always clear C | static, 1 word in the translator | 0x920a4 |
| 0x100 | #11 | A lone Thumb BL/BLX suffix branches relative to the new LR | post-translation reorder (no helper) | hook on `cpu_translate_block` |
| 0x200 | #25 | Writes to the CP15 control register are lost | static, 1 call redirected to a helper | 0x8f6b0, helper at 0x14ca80 |
| 0x400 | #17 | `mov pc,pc` is dropped as a no-op | static, 1 word in the translator | 0x93ca0 |
| 0x800 | #24 | MSR CPSR_c also writes CPSR bits 31:8, including Q | post-translation rewrite + helpers | hook on `cpu_translate_block`, helpers at 0x14ca90 / 0x14cab0 |

Which CPU each fix affects:
- 0x01, 0x40, 0x80, 0x100, 0x400 and 0x800 act on both CPUs.
- 0x04 and 0x20 rewrite ARM9 code only.
- 0x200 matters only on the ARM9, because the ARM7 has no CP15.

## How the code is organised

`fixes.c` contains only code we wrote. It does not contain any of DraStic's machine code:

- **AArch64 encoders.** Every instruction we write, and every emitted instruction we look for, is built from
  small ISA encoders with named fields, such as `a64_lsr_reg(sf, rd, rn, rm)`, `a64_add_ext(sf, rd, rn, rm, option)`,
  `a64_movk(sf, rd, imm16, hw)`, `a64_tst_imm(sf, rn, value)` and `a64_bl(offset)`. Logical immediates are encoded
  from their value by a bitmask-immediate encoder. When the post-passes match emitted logical immediates, they decode
  the immediate and compare values, because an emitter may spell the same immediate with different `immr` bits.
- **Build identity.** `jf_identity_ok()` computes SHA-256 of `/proc/self/exe` (falling back to `AT_EXECFN`) with
  our own SHA-256 implementation, and compares it with the digest above. If the file can't be read or the digest
  differs, nothing is patched and the shim does not hook anything.
- **Per-site guard.** Each static patch site, and each function entry we hook, also has a 32-bit FNV-1a hash of its
  original window (8 to 28 bytes around the word we change). If the hash of the bytes in memory differs (because
  something else already changed them), that one patch is refused and logged as `REFUSED`.
- **Helper area.** The helpers go into the zero padding after the end of the R-X LOAD segment (the segment's file
  size ends at 0x14c7ef and its last page ends at 0x14d000; the helpers start at 0x14c800). This is within `bl`
  range of the translation cache, which lives in the `nds_system` .bss (roughly VA 0x488000..0x1600000). Before
  writing, the code checks that 0x300 bytes there are all zero. If they aren't, the helper-based fixes (0x04,
  0x20, 0x200 and 0x800) stay off.
- **Post-translation passes.** These run on every block that `cpu_translate_block` returns. They only change code
  inside the host range of one guest instruction, never change its length, and do nothing unless the exact expected
  shape is found. If it isn't found they log `not found` and leave the code alone. They also skip any rewrite whose
  helper would be out of `bl` range.
- **Timing.** All static patches are written from the constructor, before DraStic's `main` and so before any
  translation. Each write is followed by `__builtin___clear_cache`, and the page is set back to R-X.
- **Hooking.** `jf_inline_hook()` replaces the first four instructions of a function with a literal load of the hook
  address into x16 followed by a branch to x16. The trampoline runs the four displaced instructions, copied from
  memory at run time, then jumps back to the function at +16. An ADRP among the copied instructions is rebuilt as
  MOVZ/MOVK of the page address it computes. Any other PC-relative instruction makes the hook refuse.

The guest-side facts the post-passes rely on (all confirmed in `notes/dynamic.md`):
- the block descriptor points at +8 to an array of 0x20-byte instruction records (+0 the guest opcode, +8 the host
  address of that instruction's code), with the record count at +0x20;
- the block's host byte count is the u16 at +0x12 of its metadata;
- the CPU type is at +0x210c of the CPU state (1 = ARM9);
- the guest CPSR word is at offset 0x23c0 from x28 in JIT code;
- guest register Rn lives in host register n+13.

## The fixes

### 0x01: carry of MOVS Rd,Rm,ASR Rs (bug #1, stub `arm64_shift_reg_flags_asr` at 0x8f8f4) [confirmed]

- **Root cause.** For a register shift amount n in 1..31, the shifter carry is bit n-1 of Rm. The ASR flag stub
  computes n-1 into a scratch register, but its next instruction shifts by n instead of by that register. So C
  comes from bit n. The matching LSR stub does use n-1.
- **Fix.** Replace the word at 0x8f904 with `LSR W4, W1, W4` (shift by the n-1 register). The paths for n = 0 and
  n >= 32 are separate and unchanged.
- **Risk.** Very low. Only one register field changes, and the stub is shared, so the ARM7 benefits too.

### 0x02: QADD sign extension (bug #2, QADD/QSUB/QDADD/QDSUB emitter in `cpu_translate_instruction`) [confirmed]

- **Root cause.** The saturating-add emitter forms the 64-bit sum with an ADD (extended register) template whose
  extend option is UXTW. A negative Rn is therefore zero-extended, so the overflow test sees a large positive
  value. The subtract template correctly uses SXTW.
- **Fix.** The template is loaded with a MOVZ (low half) and a MOVK (high half). The MOVZ at 0x957b8 is rebuilt to
  load the low half of `ADD Xd, Xn, Wm, SXTW`; the high half is the same for both options. The emitted add for guest
  `qadd` changes from `add x13, x14, w15, uxtw` to `add x13, x14, w15, sxtw`.
- **Risk.** Very low. Only the extend option of a template changes, and only QADD and QDADD use it.

### 0x04: QDADD/QDSUB doubling (bug #3, same emitter plus a post-pass) [confirmed]

- **Root cause.** For the doubling forms (opcode bit 22), the emitter writes `2*Rn` to a scratch register, but
  the add or subtract that follows still reads the original Rn. So the doubled value is never used. The doubling
  is also neither saturated nor allowed to set Q.
- **Why a post-pass.** A correct sequence needs more instructions than the emitter writes, and a call whose offset
  is not a constant.
- **Fix.** For each guest QDADD/QDSUB record in an ARM9 ARM-mode block, the pass looks for this shape in the record's
  host code (N, M and D are the host registers of Rn, Rm and Rd):
  1. `add w1, wN, wN`
  2. `sxtw xM, wM`
  3. `add|sub xD, xM, wN, uxtw|sxtw`
  4. `sub x0, xD, wD, sxtw`

  Then it rewrites two words: word 1 becomes `bl dbl[N]`, and word 3 becomes `add|sub xD, xM, w1, sxtw`. That is
  correct whether or not fix 0x02 is on. The existing saturation and Q logic after the sequence then handles the
  second saturation. Example from our test ROM (guest `qdadd r0, r1, r2`): `add w1, w15, w15` becomes a `bl` to the
  helper, and `add x13, x14, w15, uxtw` becomes `add x13, x14, w1, sxtw`.
- **Helper `dbl[R]`.** There is one 4-instruction entry per host register, plus a shared tail:
  1. `SBFIZ X1, XR, #1, #32` computes the exact doubled value.
  2. `SUB X0, X1, W1, SXTW` is nonzero exactly when the result doesn't fit in 32 bits.
  3. If it fits, return with the result in w1.
  4. Otherwise the tail picks 0x7fffffff or 0x80000000 from the sign (TBNZ on bit 63, then MOVN or MOVZ), sets
     bit 27 (Q) in the guest CPSR word, and returns.

  The helper leaves NZCV alone and clobbers only x0, x1 and x30. The emitted QADD sequence already uses x0 and w1 as
  scratch.
- **Risk.** Low. It relies on the record layout and the block byte count, both confirmed. Translations always go
  through `cpu_translate_block`, whose only caller is `cpu_block_create`. Forms that use PC as Rd, Rm or Rn
  (UNPREDICTABLE) are skipped. Each rewritten instruction costs one call.

### 0x08: SMLAxy Q flag (bug #4, SMLAxy emitter) [confirmed]

- **Root cause.** Before the multiply-accumulate, the emitter prepares the accumulator with a 32-bit shift by zero.
  A 32-bit operation zero-extends into the X register, so the 64-bit SMADDL and the overflow test that follows see
  a negative accumulator as a large positive one, and set Q wrongly.
- **Fix.** The template is built with a MOVZ and a MOVK. The MOVK at 0x937b0 is rebuilt to load the high half of
  `SXTW Xa, Wa`. The emitted preparation becomes a sign extension, for example `sxtw x16, w16`.
- **Risk.** Very low. Only the upper half of a host register is written, as the QADD path already does with SXTW.
  Guest state never reads those bits.

### 0x10: SMLALxy operand halves (bug #5, SMLALxy emitter, 0x95028..0x950e0) [confirmed]

- **Root cause.** The operand-extraction templates use the wrong bit positions: the Rs half for y=0 starts at bit 1
  and the half for y=1 at bit 17, instead of bits 0 and 16. The Rm template ignores the x bit and always shifts by 1.
- **Fix.** The seven words at 0x95068..0x95080 that build the templates are replaced by seven of ours. They leave
  the same registers live (w2 = y=0 template, w4 = y=1 template, w6 = Rm template, flags = y bit):

  | Step | Our instruction | Effect |
  |---|---|---|
  | 1-2 | `MOVZ` + `MOVK` into w2 | w2 = template for `SXTH W1, Ws` (bottom half of Rs) |
  | 3 | `ADD W4, W2, #imm, LSL #12` | w4 = template for `ASR W1, Ws, #16` (top half); only the immr field differs |
  | 4 | `TST X20, #0x20` | test the x bit of the guest opcode |
  | 5 | `CSEL W6, W4, W2, NE` | pick the Rm template by x |
  | 6 | `SUB W6, W6, #1` | change its destination from w1 to w0 |
  | 7 | `TST X20, #0x40` | test the y bit; the select after this sequence still uses these flags |

  We checked that w2, w4, w6 and the flags are all dead before 0x95068. In our test ROMs, `smlaltb` now emits
  `asr w0, w14, #16 ; sxth w1, w15`, and `smlalbt` emits `sxth w0, w14 ; asr w1, w15, #16`.
- **Risk.** Low. The change is inside the translator, and only the SMLALxy path reaches it.

### 0x20: MSR CPSR_f does not update Q (bug #6, `cpu_translate_msr_op` at 0x8fb00, plus a post-pass) [confirmed]

- **Root cause.** With the f field set and the c field clear, the emitter writes only the host NZCV. Q is bit 27 of
  the CPSR word in memory, and only the c-field stub writes that word. MRS rebuilds the CPSR from memory, so Q
  stays as it was.
- **Fix (ARM9 only).** For a guest MSR CPSR with f set and c clear (both the immediate and register forms), the pass
  replaces the first `msr nzcv, x0` in the instruction's host code with `bl msrf`. The helper `msrf`:
  1. writes NZCV from x0, as the original instruction did;
  2. loads the guest CPSR word;
  3. inserts bit 27 of the new value (`LSR`, then `BFI` into bit 27);
  4. stores the word and returns.

  The fc form is left alone, because the c-field stub already writes Q.
- **Risk.** Low. The helper clobbers w0 (dead after the write), w1 (scratch everywhere in emitted code) and x30. The
  ARM7 is excluded on purpose: bit 27 is reserved on ARMv4T. The fs and fx forms were tested and behave like f.

### 0x40: TST Rn,Rm{,shift} clobbers C and V (bug #14, ALU-register path of `cpu_translate_instruction`) [confirmed]

- **Root cause.** At 0x93880..0x938b8 the translator decides whether a flag-setting ALU-register op needs its
  wrapper. The wrapper saves C and V around the host op and inserts the shifter carry. Arithmetic ops are meant to
  skip it, and logical ops (AND, EOR, TST, TEQ, ORR, MOV, BIC, MVN) are meant to get it when C or V are live. The
  test for "logical" is `(op & 0xe) == 0 || op == 9 || op >= 0xc`. That covers ops 0, 1, 9 and 0xc..0xf, but not
  op 8 (TST). So TST-register is emitted like CMP, as a bare host ANDS, which sets C=0 and V=0 and has no shifter
  carry. The immediate form goes through a different function whose test includes TST, and Thumb `tst` is
  converted to the ARM register form, so it hits the same bug.
- **Fix.** The mask in the test at 0x93894 changes from 0xe to 0x6 (our word is `TST W24, #0x6`). Ops 0, 1, 8 and 9
  all have `op & 6 == 0`, and every arithmetic op (2-7, 0xa, 0xb) has `op & 6 != 0`. All other ops take the same
  branches as before. TST then gets exactly the code that ANDS and TEQ get: the wrapper when C or V is live, the
  shifter carry for immediate shifts, and the flag stubs for register shifts.
- **Risk.** Low. Liveness analysis still drops the wrapper when C and V are dead, so the 4-instruction cost appears
  only where the flags matter.

### 0x80: shifter carry of a rotated immediate (bug #15, `cpu_translate_alu_op_imm` at 0x91fd0) [confirmed]

- **Root cause.** For an immediate with a nonzero rotation 2r, the ARM shifter carry is bit 31 of the rotated
  value. When the flags are live, the translator computes the carry at translation time and then emits either
  "set C" or "clear C". It takes bit 2r-1 of the *already rotated* value, which is wrong. For the immediates we
  tested, that bit is always 0.
- **Fix.** The word at 0x920a4 becomes `LSR W0, W28, #31`, so the carry is bit 31 of the rotated immediate (held in
  w28). The unrotated case (r = 0, C unchanged) is a separate branch and is not touched. This covers ANDS, EORS,
  ORRS, BICS, MOVS, MVNS, TST and TEQ with an immediate. For example, `tst r1, #0x80000000` now sets C.
- **Risk.** Very low. The wrong value was used only to choose which of the two instructions to emit.

### 0x100: lone Thumb BL/BLX suffix (bug #11, pseudo-op 0xe6000010) [confirmed]

- **Root cause.** When a Thumb BL/BLX suffix half is translated on its own, it becomes a pseudo-op. The suffix
  pseudo-op is 0xe61f0010 | off<<5 for BL and 0xe61d0010 | off<<5 for BLX. Its emitted code first writes the new LR
  (two immediate loads into w27), then computes the target as w27 + off*2, so the target is relative to the new LR
  instead of the old one. Complete BL pairs aren't affected.
- **Why a post-pass.** The add comes after two variable-length immediate loads, so no constant change can reorder
  them.
- **Fix (Thumb blocks, both CPUs).** For each suffix record, the pass finds `add w0, w27, #off*2` followed by
  `orr w0, w0, #1` (BL) or `and w0, w0, #~3` (BLX). It checks that the 2 to 4 words before the add are immediate
  writes to w27 (MOVZ, MOVN, MOVK or ORR from WZR), then moves the add in front of them. The length stays the same,
  the moved instructions don't depend on the PC, and the final branch stays at its address.
- **Risk.** Low. It is a pure reorder within one guest instruction's host code.

### 0x200: CP15 control register writes are lost (bug #25, stub `arm64_update_coprocessor_control` at 0x8f678) [confirmed]

- **Root cause.** The stub merges the written value with the old control word, keeping only the writable bits
  (mask 0xff085). It then calls `update_coprocessor_control` (0x295b0) without storing the merged value. That
  function re-reads the control word from cp15+0x14 itself, derives the vector base (bit 13) and the cache/TCM
  flags, and returns whether the vector base changed (the stub then flushes all blocks). So the write has no
  effect. The interpreter path (`coprocessor_register_store`, c1,c0,0) stores
  `(Rt & 0xff085) | (old & ~0xff085)`, and the DTCM/ITCM stubs store their values before their calls. Only the
  control stub is missing the store.
- **Fix.** The call at 0x8f6b0 becomes `bl cp15c`. The helper `cp15c` (at 0x14ca80) stores w1 to [x0, #0x14], then
  tail-branches to `update_coprocessor_control`. That function returns into the stub, so the flush-on-change path
  runs as before. The stored value is exactly what the interpreter stores.
- **Risk.** Low. CP15 control writes now take effect under the JIT as they do in the interpreter, including clearing
  V (low vectors) and toggling the TCM enables. A game that relied on such a write being ignored would now behave as
  it does in the interpreter, which is the reference behaviour.

### 0x400: `mov pc,pc` is eliminated as a no-op (bug #17, `cpu_translate_instruction` at 0x93c9c) [confirmed]

- **Root cause.** The translator drops a `MOV Rd, Rd` with no shift and no S bit. It compares the raw 4-bit Rd and
  Rm fields, so `mov pc, pc` (and Thumb hi-register `mov pc, pc`, 0x46ff, which becomes the same ARM op) emits no
  code at all. Unconditionally, the host then runs off the end of the block (SIGILL). Conditionally and taken, it
  acts as a NOP.
- **Fix.** The conditional compare at 0x93ca0 becomes `CCMP W26, W23, #0, EQ`, which compares the host register
  numbers the translator has already worked out: w26 holds Rd's host register (0 for PC) and w23 holds Rm's (28
  for PC). These are equal exactly when Rd == Rm and Rd isn't PC. `mov pc, pc` then takes the normal path: Rm = PC
  reads as PC+8 (PC+4 in Thumb) and the Rd = PC epilogue runs.
- **Risk.** Very low. Every other `MOV Rd, Rd`, including the Thumb NOP `mov r8, r8`, is still dropped. In the x9,
  t7, a7, c7, jittest and dsstress runs, adding this fix changed no emitted code apart from the 0x800 call targets.

### 0x800: MSR CPSR_c writes bits 31:8 (bug #24, stub `arm64_msr_cpsr_low` at 0x8f43c) [confirmed]

- **Root cause.** With the c field set, the emitter loads the value into w0, writes NZCV if f is also set, and calls
  `arm64_msr_cpsr_low`. That stub stores all of w0 as the CPSR word. So `msr cpsr_c, Rm` also writes Q (bit 27)
  and the reserved bits from Rm. NZCV isn't visibly affected because the live flags stay in the host NZCV.
- **Fix (both CPUs).** For a guest MSR CPSR with the c field, the pass finds the `bl` to `arm64_msr_cpsr_low` in the
  instruction's host code and points it at one of two helpers. Both tail-branch to the stub, so x30 and the PC
  bookkeeping stay the same:
  - `msrc` (c without f) keeps CPSR bits 31:8 from memory and takes bits 7:0 from w0;
  - `msrfc` (f and c) keeps bits 23:8 and takes bits 31:24 and 7:0 from w0, so Q still comes from the value, as the
    f field requires.

  Each helper is LDR, two ANDs with the masks, ORR, then B to the stub. The s and x fields are ignored, as the
  emitter already does.
- **Risk.** Low. The helpers clobber w1, which the stub clobbers anyway, and add four instructions to an
  already-slow call.

## Verification

All runs use DraStic under qemu-aarch64 with `tools/dyn/jitdump.so` preloaded. Expected values come from our ARMv5TE
model in the ROM generators. Columns: int = interpreter, jit = recompiler, exp = expected.

### Differential ROMs

Base ROM (`tools/dyn/diff/build.py`, 1311 result words):

| Class | Words | JIT≠exp unpatched | JIT≠exp with JITFIX=1 | int≠exp (unchanged) |
|---|---|---|---|---|
| movs asr | 224 | 24 | **0** | 0 |
| qadd | 22 | 10 | **0** | 0 |
| qdadd | 22 | 16 | **0** | 10 |
| qdsub | 22 | 12 | **0** | 10 |
| smla{bb,bt,tb,tt} | 4 x 24 | 4 each | **0** | 0 |
| smlal{bb,bt,tb,tt} | 4 x 36 | 21 / 21 / 20 / 19 | **0** | 18 / 19 / 17 / 17 |
| probe3/4/6 (msr cpsr_f clears Q) | 3 | 3 | **0** | 0 |
| all other classes | rest | 0 | 0 | 0 |

Total JIT≠exp: **162 unpatched, 0 with JITFIX=1**. The interpreter has 91 mismatches, all its own bugs (listed
below).

Each of the first six fixes on its own changes only its own classes:
- 0x01 fixes movs asr (24 words);
- 0x02 fixes qadd (10), and changes 6 qdadd words, which stay wrong until 0x04 is also on;
- 0x04 fixes qdadd and qdsub;
- 0x08 fixes smla* (16);
- 0x10 fixes smlal* (81);
- 0x20 fixes probe3, probe4 and probe6.

Extended ROM (`build.py extra`, 1659 words). It adds:
- register aliasing and high registers for QADD, QDADD and QDSUB, plus conditional forms;
- MSR forms that set Q (f, fs, fx, conditional), and Q staying sticky through a later QADD;
- SMLAxy with Rd = Racc and negative accumulators;
- SMLALxy with aliasing and high registers;
- MOVS ASR with aliasing.

Result: 335 JIT≠exp unpatched, **0 with JITFIX=1**. The interpreter has 189 mismatches, all in the qdadd, qdsub
and smlal classes.

Other checks:
- With `--interpreter`, JITFIX=1 gives the same result as no JITFIX: the fixes only touch JIT code.
- With no mode flag (the JIT is the default), the result equals the `--recompiler` run.

### Generated ROMs (`tools/dyn2/tgen.py`)

- `x9`: ARM9, all groups, 2531 words.
- ARM7, split into three ROMs because the interpreter never finished the combined one: `t7` (Thumb, 1147 words),
  `a7` (shift, ALU, memory, store-PC, 1250 words) and `c7` (rotated immediates, 104 words).

| ROM | Class | Words | int≠exp | JIT≠exp unpatched | JIT≠exp fixed | int≠JIT fixed |
|---|---|---|---|---|---|---|
| x9 | Thumb tst | 12 | 0 | 12 | **0** | 0 |
| x9 | Thumb lone BL suffix | 2 | 0 | 2 | **0** | 0 |
| x9 | Thumb asrs / movs asr | 184 / 168 | 0 | 20 / 20 | **0** | 0 |
| x9 | tst | 72 | 17 | 52 | **0** | 17 (interpreter) |
| x9 | teq | 48 | 17 | 6 | **0** | 17 (interpreter) |
| x9 | ands / movs / mvns / orrs #imm | 38 each | 6 | 6 | **0** | 6 (interpreter) |
| t7 | Thumb tst / lone suffix / asrs | 12 / 2 / 184 | 0 | 12 / 2 / 20 | **0** | 0 |
| a7 | tst / teq | 48 / 24 | 6 / 6 | 36 / 0 | **0** | 6 / 6 (interpreter) |
| a7 | movs asr | 168 | 0 | 20 | **0** | 0 |
| c7 | tst / teq | 24 / 24 | 11 / 11 | 16 / 6 | **0** | 11 / 11 (interpreter) |
| c7 | ands / movs / mvns / orrs | 14 each | 6 | 6 | **0** | 6 (interpreter) |

Some mismatches are the same in the unpatched JIT, the fixed JIT and the interpreter:
- store-PC: DraStic stores PC+8;
- Thumb and ARM LDM/STM with the base in the list, and ARM7 LDRH/LDRSH at odd addresses: our model uses ARMv4
  rules, while DraStic follows ARMv5.

No class has more JIT≠exp mismatches with the fixes than without them.

Each of 0x40, 0x80 and 0x100 on its own (x9):
- 0x40 changes only Thumb tst (12 words) and tst (34);
- 0x80 changes only ands, movs, mvns, orrs and teq (6 each) and tst (18);
- 0x100 changes only the lone suffix (2).

The tst class needs both 0x40 and 0x80.

### Emitted-code comparison (`tools/dyn/rawdiff.py --ignore-b`, which compares branches by target)

- x9: the only guest instructions whose host code changed are TST-register (ARM and Thumb), rotated-immediate
  ANDS/MOVS/MVNS/ORRS/TST/TEQ, the lone suffix, and the MSR-with-c call targets.
- t7: Thumb tst and the lone suffix only.
- a7: tst-register and `tst #0xF0000000` only.
- jittest: the QADD extend option and the MSR-with-c call target only.
- dsstress-L1 (a 3D stress ROM): only the MSR-with-c call target, and it keeps running.

### Round-3 ROMs (`tools/dyn/r3`, `run3p.sh <rom>` runs int, unpatched JIT and fixed JIT)

| ROM | Interpreter | JIT unpatched | JIT fixed |
|---|---|---|---|
| r3n_BASE (control read-back, UND through low vectors) | 10078, und 1 | **12078**, hangs in the BIOS UND loop | 10078, und 1 |
| r3_SWIV (clear V, then SWI into an ITCM handler) | 10078, swi 1, lr+4 | SIGSEGV (took the BIOS SWI vector) | 10078, 1, 4 |
| r3n_MOVPC (ARM `mov pc,pc`) | 2 | **SIGILL** | 2 |
| r3n_MOVPCEQ (condition true) | 2 | **3** | 2 |
| r3n_MOVPCNE (condition false) | 3 | 3 | 3 |
| r3n_THMOVPC (Thumb 0x46ff) | 2 | **SIGILL** | 2 |
| r3n_MSRC (Q after msr cpsr_c with bit 27 = 1 / 0 / NZCV bits / sticky) | 0 1 0 0 | **1 0 0 1** | 0 1 0 0 |
| r3_BKPT | abort taken, lr+4, SPSR 0x600000df | hangs (no low vectors) | abort taken, lr+4, SPSR **0x000000df** |
| r3_QD (QDADD/QDSUB with Rn < -2^30) | interpreter's own wrong values | 7fffffff... | spec values (e.g. qdadd 0, 0x80000000 = 0x80000000 with Q) |
| r3_THBKPT, r3_THUND, r3_STRD2, r37_{CLZ,BLXI,LDRD,STRD,THBLX} | - | = int apart from the control read-back word | = int |
| r3_STRDSMC, r3_SWPSMC | correct | wrong | still wrong (not in scope) |

r3_BKPT's SPSR loses its NZCV bits with `JITFIX=0x200` alone too. This is bug #7: when the JIT raises an exception it
builds the SPSR from the in-memory CPSR without the live host flags. Before this fix it was hidden because the JIT
never reached a low-vector handler. It is not patched.

### Re-verification of this version (identity check and encoders)

This version replaced the old "compare the original words" checks with the SHA-256 identity check and the FNV-1a
site guards, and builds every written word with the encoders. To check that it behaves the same:

- **Patched image.** On the host, the R-X segment patched by the new code (JITFIX=1, and also each static mask on its
  own) is byte-identical to the one the previous version produced.
- **Wrong build.** A file with a different digest is refused, and nothing is written.
- **Same results under qemu.** With JITFIX=1 and with JITFIX unset, these runs gave result words identical to the
  previous version's:
  - the base diff ROM: 0 / 162 JIT≠exp (fixed / unpatched);
  - the extended diff ROM: 0 / 335;
  - x9, t7, a7 and c7: every fixed class at 0. The 6 / 3 / 9 / 0 words that remain are the store-PC and ARMv4-vs-v5
    rows described above.
  - all round-3 ROMs in int, jit and fix modes, except r3_STRDSMC;
  - jittest and dsstress-L1: the same emitted code by `rawdiff.py --ignore-b`, and both kept running until the
    timeout.
- **r3_STRDSMC is not repeatable.** This ROM exercises the unpatched STRD self-modifying-code bug (#20). Its
  outcome changes from run to run with both the old and the new shim, and with or without JITFIX: either a SIGSEGV,
  a SIGILL, a hang, or DONE with the same wrong word (0x12 where the interpreter gives 0x11).
- **Flush hooks.** With `JITDUMP_FLUSH=1`, the SMC ROM (`tools/dyn2/smc9.s` + `arm7mb.s`, built with `mkrom.py`; the
  ROM is byte-identical to the earlier build) logs the same 15 flush and 23 ITCM events, and gives the same results
  and per-phase block list (`blocks.py`) as the earlier dyn2 shim. The entry of `cpu_block_flush` contains an ADRP,
  so this run also exercises the relocation in `jf_inline_hook()`.

## Interpreter bugs seen (not patched)

- QDADD/QDSUB: doubling a positive Rn saturates to 0xffffffff. For example `qdadd 1, 0x10` gives 0 with Q set.
- SMLALxy: the low and high words are wrong for most inputs.
- The rotated-immediate carry is taken from bit 2r-1 of the rotated value (`execute_arm_alu_load_op2_imm`, 0x25014),
  and TST/TEQ with an immediate leave C unchanged.
- TST/TEQ Rn, Rm, <shift> keep the old C instead of the shifter carry.

## Notes for shipping in SuperDrastic

- **Static fixes** (0x01, 0x02, 0x08, 0x10, 0x40, 0x80, 0x400, and 0x200 once the helper area exists) need only the
  load base and a write before the emulator starts. No hook is needed.
- **Post-translation fixes** (0x04, 0x20, 0x100, 0x800) need a callback after `cpu_translate_block`.
  `jf_inline_hook()` provides one. 0x04, 0x20, 0x200 and 0x800 also need helper space within ±128 MB of
  `nds_system`. The segment padding at 0x14c800 is used here, and it is checked to be zero. A page mapped near the
  base would also work.
- **Version guard.** Any other build fails the SHA-256 check and is left untouched.
- **Real hardware.** The passes clear the I-cache for every word they change. qemu doesn't need that, so a test on
  a real aarch64 device is still owed.

## Files

- `fixes.c`: encoders, identity check, static patches, helpers and post-passes.
- `tools/dyn/jitdump.c`: the qemu test shim. It includes `fixes.c` (copy it next to the shim, or build with
  `-I patches`) and calls `jitfix_init()` and `jitfix_post()`. With `JITDUMP_FLUSH=1` it also logs block flushes
  and ITCM write checks.
- `tools/dyn/diff/build.py` (base and `extra` differential ROMs), `tools/dyn2/tgen.py` (generated ROMs),
  `tools/dyn/rawdiff.py` (emitted-code comparison), and `tools/dyn/r3/` (round-3 sources and `run3p.sh`).
