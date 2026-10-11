# Annotated examples of DraStic JIT translations: guest ARM/Thumb to emitted AArch64

> **Errata (from the consistency review in review.md, checked against the binary; these take precedence):**
> - Host x16/x17 are guest r3/r4, not scratch.
> - SP-relative stores are inlined on ARM9 (example 5 shows one).
> - In example 5, `b 0x7f6190000000` is an unpatched delayed self-link, and the `b -60` is the type-3 fall-through to the next block's entry+8.


What these are: the AArch64 code that DraStic r2.5.2.2's JIT produced for **our own homebrew test ROMs**, captured
while it ran under qemu-aarch64 on x86-64 with our `jitdump.so` hook on cpu_translate_block. Setup, findings and rerun
instructions are in `dynamic.md`. We disassembled the captured host code with llvm-mc (aarch64) and the guest code with
llvm-mc (armv6/thumbv5te); our `jitdis.py` resolves branch targets to DraStic symbol names or to other blocks, and uses
the per-instruction records (record+8) filled in by cpu_translate_block to find where each guest instruction's host code
begins.

How to read the excerpts: each one is a short, hand-picked slice of one block, not the full translation. A line
`[guest pc] guest insn -> host insns` gives the guest instruction from our ROM and the host instructions emitted for it
(several per line, separated by `;`). Instruction words in hex and raw branch offsets are left out; branch targets are
written as `<what they point at>`. Everything after `;;` is our annotation. Host addresses differ between runs (ASLR).

Sources: test ROM `$S/work/dyn/rom/jit9.s` (built by `rom/build.py`), differential ROM generator `$S/work/dyn/diff/build.py`.
Full captures (working files, not part of the deliverable): `$S/work/dyn/out_jt/merged.txt`, `$S/work/dyn/out_diff_jit/merged.txt`.

Reproduce:
```
S=$S; D=$S/work/dyn
python3 $D/rom/build.py && cp $D/rom/out/jittest.nds $D/drastic/roms/
OUT=$D/out_jt JITDUMP_MAX=120 $D/run.sh drastic/roms/jittest.nds 15 --recompiler
python3 -I $D/jitdis.py $D/out_jt --cpu 9 > $D/out_jt/merged.txt
```

Register conventions seen in every capture:

| host | role |
|---|---|
| x28 | CPU state |
| x9 | x28 + 0x23d0 |
| x10 | memory-map base (fastmem) |
| w12 | cycle countdown |
| w13..w23 | guest r0..r10 |
| w24, w25 | guest r11, r12 |
| w26, w27 | guest sp, lr |
| w0..w3 | scratch |

(w16/w17, i.e. x16/x17, are guest r3/r4, as the table shows; see the errata.)

## 1. ALU with flags (t_alu_flags, 0x02000060; 17 guest insns -> 152 host bytes)

```
(block entry)                ->  tbz w12,#31,+8 ; bl recompiler_cpu_next_action_arm9_to_arm7   ;; slice check
[02000068] adds r2,r0,r1     ->  adds w15,w13,w14                                                ;; flags 1:1
[0200008c] cmp r10,r9        ->  subs w3,w23,w22                                                 ;; w3 = dummy dest
[02000094] tst r10,#128      ->  mrs x2,nzcv ; ands w3,w23,#0x80
                                 mrs x1,nzcv ; bfxil w1,w2,#0,#30 ; msr nzcv,x1                  ;; keep old C,V
[020000a0] bx lr             ->  sub w12,w12,#19 ; mov w0,w27 ; b <block_indirect_branch>
```

- Every block opens with the cycle check: w12 counts down and turns negative when the time slice is used up, at which
  point recompiler_cpu_next_action_arm9_to_arm7 is called. The block's cost is charged once, at the exit
  (19 = 17 instructions + 2 for the branch).
- Immediates are built with mov/movk; r0 = w13, r1 = w14 and so on.
- Guest NZCV lives in host NZCV, so `adds`/`adcs`/`subs`/`sbcs` translate one to one. ARM and AArch64 `sbcs` agree
  because both use the inverted-borrow carry. `rsbs r6,r5,#0` came out as `negs w19,w18`.
- `adcs r3,r0,#1` first puts the immediate in a scratch register (`mov w1,#1`) and then does a register `adcs`.
- Logical S-ops whose C/V results are dead (`eors r7,r6,r0`, `orrs r9,r8,r1,lsr #4`) become the plain op followed by
  `ands Rd,Rd,Rd`, which sets N/Z and clears C/V. That is only legal because liveness analysis shows a later `subs`
  overwrites C/V; for the same reason the shifter carry of `lsr #4` is not computed.
- `ands r8,r7,#255` maps straight to `ands w21,w20,#0xff`; `bics r10,r9,#15` folds the inverted mask into
  `ands w23,w22,#0xfffffff0`. `cmn` also writes w3 (`adds w3,...`).
- Where C/V must survive (`tst`, `teq`, and `mvns` while cmn's C/V are still live) the op is bracketed by the
  `mrs x2` ... `mrs x1; bfxil w1,w2,#0,#30; msr` sequence: the bfxil puts the old bits 29:0 back, i.e. the old C (bit 29)
  and V (bit 28), while N/Z from the new op are kept.
- `bx lr` hands the target (lr = w27) to block_indirect_branch in w0.

## 2. Conditional execution (t_cond, 0x020000c0; 12 guest insns -> 112 host bytes)

```
[020000c4] cmp r0,#5         ->  subs w3,w13,#5
[020000c8] moveq r1,#1       ->  b.ne +8 ; mov w14,#1                                  ;; skip on inverse cond
[020000dc] streq r0,[r3]     ->  b.ne +16 ; mov w1,w13 ; mov w0,w16 ; bl <arm64_store_memory32_arm9>
[020000e8] bxhs lr           ->  sub w12,w12,#14 ; b.lo +12 ; mov w0,w27 ; b <block_indirect_branch>
```

- A conditional instruction becomes a forward branch over its host code using the opposite condition. Since the flags
  are in host NZCV, ARM condition codes map directly onto AArch64 ones (`movne` -> `b.eq +8`, `addgt` -> `b.le +8`, ...).
- `ldreq r3,=cvar` (PC-relative literal) was read at translation time and became `mov w16,#0x2100000` (under `b.ne +8`).
- `streq` skips an entire store-helper call (w0 = address, w1 = value).
- At the conditional exit `bxhs lr` the cycle charge is applied before the branch; the fall-through `bx lr` path has its
  own charge (`sub w12,w12,#1`).

## 3. Single loads and stores (t_mem, 0x02000100; 18 guest insns -> 300 host bytes)

```
[02000108] ldr r2,[r0]       ->  and w1,w13,#0xfc000003                 ;; range + alignment test
                                 cbz w1,+8
                                 b <slow-path stub, ~16 MB away>
                                 ldr w15,[x10,w13,uxtw]                 ;; fastmem load
[02000118] str r2,[r0,#32]!  ->  add w13,w13,#32 ; mov w1,w15 ; mov w0,w13 ; bl <arm64_store_memory32_arm9>
[02000140] ldr r10,[pc,#-4]  ->  mov w23,#0xff1e ; movk w23,#0xe12f,lsl #16   ;; = encoding of the next insn (bx lr)
```

- Loads are inlined with a fastmem check: put the address in w0 (or use the base register directly), AND it with a mask,
  take the fast path if the result is zero, otherwise branch to an out-of-line slow stub in the cache's stub area. The
  fast path indexes from x10 (map_base + guest address = host address for directly mapped RAM). The mask rejects
  addresses >= 0x04000000 (I/O, VRAM, ...) and misaligned ones: 0xfc000003 for words, 0xfc000001 for halfwords,
  0xfc000000 for bytes. `ldrb`/`ldrh`/`ldrsb`/`ldrsh` use the matching AArch64 load with the same `[x10,wA,uxtw]` form.
- Stores here are never inlined (but see the errata for SP-relative stores): they always call
  `arm64_store_memory{32,16,8}_arm9` with w0 = address and w1 = data, which presumably keeps SMC detection in one place.
- Writeback: for pre-indexed `str r2,[r0,#32]!` the base is updated before the call; post-indexed `str r3,[r0],#-28`
  passes the old base and subtracts afterwards.
- `ldrd r2,r3,[r0,#8]` calls `arm64_load_memory64` (result in w0/w1, then copied to w15/w16). `strd` is two store calls
  with the address spilled to [x9-160] across the first one.
- `ldr r10,[pc,#-4]` is constant-folded: the literal is the next guest instruction word (0xe12fff1e, `bx lr`), read at
  translation time.

## 4. LDM/STM (t_ldm_stm, 0x02000160; 8 guest insns -> 236 host bytes)

```
[02000160] push {r4-r11,lr}  ->  sub w0,w26,#36                         ;; new sp
                                 sub x1,x9,#288                         ;; spill area x9-0x120
                                 stp w17,w18,[x1]  ...                  ;; 3 more stp, then str w27,[x1,#32]
                                 add w26,w0,#0
                                 bl <arm64_store_block9>
[02000168] ldm r0,{r1-r3}    ->  mov w0,w13 ; and w1,w0,#0xfc000003 ; cbz w1,+8 ; b <slow stub>
                                 add x0,x10,w0,uxtw
                                 ldp w14,w15,[x0] ; ldr w16,[x0,#8]
```

- `push {r4-r11,lr}` (STMDB sp!): the new sp is computed into w0, the nine registers are spilled with stp/str into the
  scratch area at x9-0x120 (= x28+0x22b0), sp is updated, and a per-count helper is called (arm64_store_block9; the
  helpers are listed in the arm64_store_block_functions table at 0x167780). `stmda`/`stm` to r0 use the same spill +
  arm64_store_block2 / arm64_store_block10.
- LDM from a non-SP base is inlined with one fastmem check on the start address, then ldp/ldr from `x10 + address`;
  for `ldmib r0!` the captured code updates the base register right after computing the start address, before the
  loads.
- `pop {r4-r11,pc}` is handled the same way and ends the block through block_indirect_branch with the loaded PC in w0.

## 5. Branches: loop, BL, linked blocks (t_branch 0x02000180/0x02000188, main loop 0x02000018)

```
[block 02000180, 2 insns]
[02000180] str lr,[sp,#-4]!  ->  sub w26,w26,#4 ; str w27,[x10,w26,uxtw]          ;; inlined SP store (errata)
[02000184] mov r0,#4         ->  mov w13,#4 ; sub w12,w12,#2 ; b <block 02000188 entry+8>
[block 02000188, 3 insns]
[02000188] subs r0,r0,#1     ->  subs w13,w13,#1
[0200018c] bne 02000188      ->  sub w12,w12,#4 ; b.eq +8 ; b <0x7f6190000000: cache base, delayed self-link>
[02000190] bl leaf           ->  sub w12,w12,#1 ; mov w27,#0x194 ; movk w27,#0x200,lsl #16 ; b <block 020001a0>
```

- `bl` puts the return address in lr (w27 = 0x02000194 here) and links directly to the target block's host code when it
  is already translated. The main loop's `bl` at 0x02000018 does the same (`mov/movk w27,#0x0200001c`, direct `b` to
  block 02000060, cost 3).
- Backward `bne` (0x0200018c): the cycle charge comes first; the taken path is a branch to the translation-cache base,
  i.e. a delayed link that has not been patched yet when our hook takes its snapshot (see errata). The fall-through
  continues with the `bl leaf` code.
- Blocks end at every BL and at branch targets: 0x02000180 has only 2 instructions because 0x02000188 is the loop head,
  and it ends by falling through to that block (entry+8, skipping its cycle check).

## 6. Self-modifying code (t_smc 0x020001c0, resume block 0x020001d4, patched target 0x020001e0)

```
[block 020001c0, 8 insns -> 60 bytes]
[020001c8] orr r1,r1,#0xe3000000 -> mov w1,#0xe3 ; orr w14,w14,w1,ror #8          ;; rotated imm rebuilt
[020001d0] str r1,[r0]       ->  mov w1,w14 ; mov w0,w13 ; bl <arm64_store_memory32_arm9>   ;; writes 0x020001e0
[020001d4] mov r2,#0         ->  movz w15,#0,lsl #16
[020001d8] mcr p15,0,r2,c7,c5,0 -> (no host code)                                  ;; ICache invalidate
[020001dc] b 020001e0        ->  sub w12,w12,#10 ; b <block 020001e0>
[block 020001e0, 2 insns]
[020001e0] mov r0,#imm       ->  movz w13,#imm                                     ;; imm = value just patched in
```

- The store into the code at 0x020001e0 goes through arm64_store_memory32_arm9. In the SMC builds every main-RAM block
  is retranslated on every loop iteration (~8000 translations each in 30 s); without the store each block is translated
  once (see dynamic.md).
- After the store, execution resumes in a new block starting at 0x020001d4 (3 insns -> 20 bytes: the same
  `movz w15,#0` and empty mcr range, then `sub w12,w12,#5` and a branch that jitdis.py resolved to block 02000060's host
  address). The rest of block 0x020001c0 is abandoned because its own code was invalidated.
- `mcr p15,0,r2,c7,c5,0` emits nothing: guest 0x020001d8 has an empty host range.
- `orr r1,r1,#0xe3000000`: the ARM rotated immediate is not a valid AArch64 logical immediate here, so it is
  materialized in w1 and applied through a ROR-shifted register operand (`#0xa00000` likewise uses `ror #12`).
- The patched `mov r0,#imm` at 0x020001e0 becomes `movz w13,#imm`, so the emitted constant changes with every
  retranslation.

## 7. Thumb (t_thumb, 0x02000240; 17 guest insns -> 172 host bytes)

```
[02000242] movs r0,#10       ->  mov w13,#10 ; ands w13,w13,w13                    ;; Thumb MOVS sets N/Z
[0200024e] ldr r0,[pc,#28]   ->  mov w13,#4 ; movk w13,#0x210,lsl #16              ;; literal folded: 0x02100004
[0200025a] beq +0            ->  sub w12,w12,#20 ; b.ne +8 ; b +8         ;; ne: run the adds; eq: jump past it
[0200025e] bl thumb_leaf     ->  sub w12,w12,#3 ; mov w27,#0x263 ; movk w27,#0x200,lsl #16 ; b <block 02000264>
```

- Thumb blocks use the same host idioms as ARM blocks: the metadata thumb flag is set, guest PCs advance by 2, and the
  two halves of the Thumb `bl` (0x0200025e/0x02000260) share one host range.
- `push {r4,lr}` is the spill (`stp w17,w27,[x9-0x120]`) plus `bl arm64_store_block2`, as in ARM code.
- `lsls r4,r3,#2` became `orr w17,wzr,w16,lsl #2` followed by `ands w17,w17,w17`: C is dead, so the shifter carry is not
  computed.
- `beq` over one instruction: cycle charge first, then `b.ne` into the skipped instruction and an unconditional `b` past it.
- `bl thumb_leaf`: lr = 0x02000263 (bit 0 set = return to Thumb) and a direct link to block 0x02000264.

## 8. Register-specified shifts and multiplies (t_shift_mul, 0x02000200; 12 guest insns -> 164 host bytes)

```
[02000208] lsl r2,r1,r0      ->  and w3,w13,#0xe0 ; lsl w15,w14,w13 ; cbz w3,+8 ; mov w15,wzr   ;; amount>=32 -> 0
[0200020c] rors r3,r1,r0     ->  mrs x2,nzcv ; mov w1,w14 ; and w3,w13,#0xff
                                 bl <arm64_shift_reg_flags_ror> ; ands w16,w1,w1 ; (mrs/bfxil/msr merge)
[0200021c] smlal r6,r7,r1,r2 ->  bfi x19,x20,#32,#32 ; smaddl x19,w14,w15,x19 ; lsr x20,x19,#32
```

- `mov r2,r1,lsl r0` (no S) is inline. AArch64 `lsl` uses the amount modulo 32, so the code tests bits 7:5 of the amount
  and forces the result to 0 when the amount is >= 32.
- `movs r3,r1,ror r0` (S, C live) calls a flag helper with w1 = value, w3 = amount & 0xff and w2 = old NZCV, then sets
  N/Z with `ands` and merges in the helper's C with the usual bfxil sequence.
- `mul`/`mla` map to `mul`/`madd`; `umull` maps to `umull x19,w14,w15` plus `lsr x20,x19,#32` for the high word;
  `smlal` first joins the two accumulator halves into x19 and then uses `smaddl`; `clz` maps to `clz`; `smulbb` maps to
  `sxth`, `sxth`, `mul`.
- `qadd r9,r1,r2` shows the Rn zero-extension bug (see 9b), which the differential test confirmed.

## 9. Instructions from the differential test ROM (diff.nds), showing the bugs

All excerpts below come from the translation of our `diff.nds` (see `dynamic.md` for the measured values).

### 9a. `movs r0,r1,asr r2` (block containing 0x02001190)
```
[02001190] asrs r0,r1,r2     ->  mrs x2,nzcv ; mov w1,w14 ; and w3,w15,#0xff
                                 bl <arm64_shift_reg_flags_asr> ; ands w13,w1,w1
                                 mrs x1,nzcv ; bfxil w1,w2,#0,#30 ; msr nzcv,x1
```
The generated code is fine; the carry comes from the helper. Inside arm64_shift_reg_flags_asr (VA 0x8f8f4) the
"amount - 1" value is computed and then immediately overwritten by the value shifted by the full amount, so C is taken
from bit n instead of bit n-1. This matches the 24 ASR flag mismatches.

### 9b. `qadd r0,r1,r2` (0x02004fd8)
```
[02004fd8] qadd r0,r1,r2     ->  sxtw x14,w14 ; add x13,x14,w15,uxtw     ;; Rn zero-extended (bug)
                                 sub x0,x13,w13,sxtw ; cbz x0,<no overflow>
                                 ...                                     ;; saturate + set Q
```
After the add, the code compares the 64-bit sum with its own sign-extended low word; on a difference it picks
0x80000000 or 0x7fffffff from the sign of the sum and ORs bit 27 (Q) into the CPSR word at [x28,#0x23c0]. Because Rn
(w15) is zero-extended, any negative Rn produces a huge positive sum and saturates to 0x7fffffff.

### 9c. `qdadd` / `qdsub` (0x02005350, 0x0200542c)
```
[02005350] qdadd r0,r1,r2    ->  add w1,w15,w15                          ;; doubled Rn, never read
                                 sxtw x14,w14 ; add x13,x14,w15,uxtw     ;; then plain QADD on the original w15
[0200542c] qdsub r0,r1,r2    ->  add w1,w15,w15
                                 sxtw x14,w14 ; sub x13,x14,w15,sxtw     ;; plain QSUB on w15
```
Both continue with the same saturation tail as 9b. The doubled value in w1 is dead.

### 9d. `smlabb r0,r1,r2,r3` (0x02006ab8)
```
[02006ab8] smlabb r0,r1,r2,r3 -> sxth w0,w14 ; sxth w1,w15
                                 asr w16,w16,#0                          ;; writes w16 -> zero-extends acc into x16
                                 smaddl x13,w0,w1,x16
                                 sub x0,x13,w13,sxtw ; cbz x0,<skip>     ;; else set Q in [x28,#0x23c0]
```
The 32-bit result is right, but the overflow (Q) test runs on a 64-bit sum whose accumulator was zero-extended, so it is
wrong whenever the accumulator is negative.

### 9e. `smlalbb r3,r4,r1,r2` (0x02006b1c)
```
[02006b1c] smlalbb r3,r4,r1,r2 -> asr w0,w14,#1 ; sbfx w1,w15,#1,#15     ;; wrong fields (should be sxth of halves)
                                  bfi x16,x17,#32,#32 ; smaddl x16,w0,w1,x16 ; lsr x17,x16,#32
```

### 9f. `mrs r10,cpsr` and `msr cpsr_f,#0` (0x02006eb0, 0x02004dd0)
```
[02006eb0] mrs r10,apsr      ->  mrs x23,nzcv ; ldr w1,[x28,#0x23c0] ; bfxil w23,w1,#0,#28
[02004dd0] msr apsr_nzcvq,#0 ->  movz w0,#0,lsl #16 ; msr nzcv,x0        ;; CPSR word not touched
```
MRS combines host NZCV with bits 27:0 of the saved CPSR word at [x28,#0x23c0], where Q (bit 27) lives. MSR of the flags
field only writes host NZCV, so Q can never be cleared this way.
