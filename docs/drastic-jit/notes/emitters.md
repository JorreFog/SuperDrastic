# DraStic JIT: per-instruction code emitters (ARM -> AArch64)

> **Errata (from the consistency review in review.md, checked against the binary; already folded into the text below):**
> - `arm64_msr_spsr` skips the write only in USR mode; SYS writes SPSR slot 0 (section 4.2).
> - `msr cpsr_c` stores the whole word Rm[31:8] into cpu+0x23c0, so it can also change Q (section 4.2).
> - `mov pc,pc` (Rd=Rm=15) is eliminated as a no-op move. When it is unconditional, the block then has no exit
>   (sections 1.2 and 1.3; see coverage.md).

Scope: the following translator functions (file VAs):

| function | address | covers |
|---|---|---|
| cpu_translate_alu_op_imm | 0x91fd0 | data processing with immediate operand |
| cpu_translate_instruction | 0x93320 | ALU-register, multiply, misc/DSP, SWP, halfword, coprocessor, SWI dispatch |
| cpu_translate_setup_memory_address | 0x905e0 | load/store address generation |
| cpu_translate_memory_op | 0x90a00 | single loads/stores |
| cpu_translate_block_memory_op | 0x91640 | LDM/STM |
| cpu_translate_msr_op | 0x8fb00 | MSR |
| cpu_translate_mrs_op | 0x93240 | MRS |
| cpu_translate_bx_op | 0x903b0 | BX/BLX(reg) |
| cpu_translate_raise_exception | 0x92ef0 | SWI/UND/BKPT entry |
| cpu_translate_set_imm32 | 0x900c0 | constant materialisation |
| cpu_translate_imm_map_to_logical | 0x8fd30 | AArch64 bitmask-immediate encoder |
| cpu_translate_map_src_reg | 0x90470 | PC-as-source materialisation |

Every emitted instruction word named below was checked by disassembling it with `llvm-mc --disassemble
-triple=aarch64`. In descriptions of emitted code, "Rd/Rn/Rm/Rs/Rt" mean the host register that holds that guest
register.

Confidence tags: [confirmed] = verified by static analysis of the binary (translator code and the words it emits);
[likely] = strong inference; [guess] = plausible, not verified.

---------------------------------------------------------------------------------------------------
## 0. Conventions used by all emitters

### 0.1 Register mapping (as seen by the emitters) [confirmed]
- Guest r0..r14 live in host w13..w27; every emitter computes the host register as guest number + 13.
- Guest r15 (PC) used as a *source* is never resident. The emitter loads the PC read value (field +4 of the
  current instruction's metadata record, see 0.2) into a scratch register: w0 for Rn of an ALU-immediate op and for
  Rm in address generation, w1 for Rm of an ALU-register op and for Rn in address generation, w2 for PC in an STM
  list, w4 for Rs. When Rm=PC is shifted by a register, the emitter uses that value + 4 (= PC+12, the ARM-correct
  value), which implies the metadata value is PC+8 in ARM mode [likely].
  cpu_translate_map_src_reg(ctx, n) emits a constant load of the PC value into wn and returns n [confirmed 0x90470].
- Guest r15 as a *destination*: the result is produced in **w0** and the PC-write epilogue (0.4) follows.
- Scratch registers: w0..w4. w3 is the throw-away destination of TST/TEQ/CMP/CMN; w1 holds materialised immediates
  and pre-shifted operands; w2 holds a copy of NZCV while the shifter carry is being computed.
- x9 = x28+0x23d0 (spill base). x10 = **memory-map base**: recompiler_entry copies its second argument into x10
  (at 0x8f9d0), and every runtime stub reloads x10/x11 from the pair at x9-0x78. x28 = CPU state pointer.
- Host NZCV holds the guest NZCV. ARM condition codes 0..13 are used unchanged as AArch64 condition codes.

### 0.2 Translation context (ctx, first argument of every emitter) [confirmed unless noted]
| ctx off | meaning |
|---|---|
| +0x000 | scratch buffer in which out-of-line slow paths are assembled before being copied to the stub area |
| +0x408 | current emit pointer (u32*) |
| +0x410 | stub ("secondary translation") area pointer, grows **downward** |
| +0x418 | saved main emit pointer while a slow path is being emitted |
| +0x448 | guest address of the *next* instruction (used for LR values and as the store_pc_metadata key) |
| +0x458 | pointer to the current instruction's metadata: +0 ARM opcode, +4 PC read value, +0x14 s16 flags (bit 15 = instruction writes PC), +0x19 bits 0-1 = flags (C/V) live after this instruction [likely] |
| +0x468 | CPU state pointer (x28 at run time) |
| +0x478 | u8: 1 = ARM9 (selects the *_arm9 store handlers, fastmem, ARMv5 behaviour) |
| +0x479 | u8: 1 = block is Thumb (Thumb instructions are pre-converted to ARM) |

CPU state fields referenced by emitted code:

| offset | meaning |
|---|---|
| x28+0x2104 | current mode index |
| x28+0x2110 | halt state |
| x28+0x20e8 + 4*mode | SPSR array (= x9-0x2e8) |
| x28+0x23c0 | CPSR (its NZCV is stale; the live NZCV is in the host flags) |
| x28+0x2250 | CP15 state pointer (NULL on ARM7) |
| cpu+0x210c | 1 = ARMv5 (ARM9): enables DSP/CLZ/BLX/BKPT |

CP15 struct: +0x10 exception vector base, +0x14 control, +0x18 DTCM region register, +0x1c ITCM region register,
+0x20 DTCM base, +0x2c DTCM size, +0x3c "DTCM in mapped memory" flag. update_coprocessor_dtcm_control (0x29630)
sets that flag to (DTCM base + size < 0x4000000) and logs "DTCM in mapped memory status changed" when it flips.

### 0.3 Condition codes [confirmed]
A conditional instruction begins with a `B.cond` using the inverted condition (cond XOR 1; base encoding
0x54000000) whose offset is left as a placeholder. At the end of cpu_translate_instruction the placeholder's imm19
field is filled with the length of the code emitted for the instruction, so the branch skips it. Cond 0xE emits no
branch; cond 0xF is handled per class (BLX imm / undefined). The same logic exists standalone as
cpu_translate_conditional_skip (0x932d0).

### 0.4 PC-write epilogue (data processing, LDR, LDM) [confirmed]
Applied when the metadata flags (+0x14, bit 15) say the instruction writes PC; the new PC is in w0.
- S=0: ARM block clears bits 1:0 (`AND w0, w0, #0xfffffffc`, 0x123e7400); Thumb block sets bit 0 (`ORR w0, w0, #1`,
  0x32000000). Then an unconditional `B` (0x14000000 | imm26) to block_indirect_branch.
- S=1 (MOVS pc / SUBS pc,lr / LDM ^ with pc): a `B` to arm64_spsr_restore, which copies SPSR into CPSR and jumps to w0.
- LDR pc / LDM {..pc}: the bit-clear / bit-set fix-up is emitted **only on ARM7** (ctx+0x478 != 1). On ARM9 the raw
  loaded value is passed on so bit 0 selects Thumb (ARMv5 interworking) [confirmed in the tails of memory_op and
  block_memory_op].

### 0.5 Immediate materialisation: cpu_translate_set_imm32(emit, reg, imm) @0x900c0 [confirmed]
The first matching form is used:

| condition | emitted | encoding base |
|---|---|---|
| low half is 0 | MOVZ with LSL #16 | 0x52a00000 |
| high half is 0 | MOVZ | 0x52800000 |
| low half is 0xffff | MOVN with LSL #16 | 0x12a00000 |
| high half is 0xffff | MOVN | 0x12800000 |
| valid AArch64 logical immediate | ORR from WZR with the bitmask immediate | 0x320003e0 |
| otherwise | MOVZ low half, then MOVK high half (LSL #16) | 0x52800000 / 0x72a00000 |

cpu_translate_raise_exception and cpu_translate_map_src_reg contain inlined copies of this logic.
cpu_translate_imm_map_to_logical(imm, &immr, &imms) @0x8fd30 decides whether a 32-bit value is a repeating pattern
with element size 32/16/8/4/2, and if so derives the N=0 immr/imms pair from the rotation and run length; it returns
0 when the value is not encodable.

---------------------------------------------------------------------------------------------------
## 1. Data processing

### 1.1 Immediate operand: cpu_translate_alu_op_imm @0x91fd0 [confirmed]
The operand is the ARM rotated immediate (imm8 rotated right by 2*rot). If Rn=PC, w0 is first loaded with PC+8; if
Rd=PC the result goes to w0. Per operation:

| ARM op | strategy | encodings |
|---|---|---|
| ADD/SUB/CMP/CMN | if the value fits imm12 or imm12<<12: one ADD/SUB(S) immediate; else materialise into w1 (0.5) and use the register form | imm: 0x11000000 / 0x51000000 / 0x31000000 / 0x71000000, +0x400000 for LSL #12; reg: 0x0b010000 / 0x4b010000 / 0x2b010000 / 0x6b010000 |
| AND/EOR/ORR/TST/TEQ, BIC (with the inverted value) | if the value is an AArch64 bitmask immediate: logical-immediate form (BIC becomes AND/ANDS of the complement); otherwise **only the unrotated imm8** is put in w1 and the shifted-register form with `ROR #(2*rot)` performs the rotation. Values 0 and 0xffffffff skip the bitmask attempt | imm: AND 0x12000000, ANDS 0x72000000, EOR 0x52000000, ORR 0x32000000; ROR form: AND 0x0ac10000 (+rot<<11), ANDS 0x6ac10000, EOR 0x4ac10000, ORR 0x2ac10000, BIC 0x0ae10000, BICS 0x6ae10000 |
| RSB | value 0: NEG(S) Rd, Rn; else w1 = value and SUB(S) Rd, w1, Rn | 0x4b0003e0 / 0x6b0003e0; 0x4b000020 / 0x6b000020 |
| ADC/SBC/RSC | w1 = value, then ADC(S) Rd,Rn,w1 / SBC(S) Rd,Rn,w1; RSC is SBC(S) with operands swapped (Rd,w1,Rn) | 0x1a010000/0x3a010000, 0x5a010000/0x7a010000, 0x5a000020/0x7a000020 |
| MOV/MVN | set_imm32 into Rd with the value or its complement | any form of 0.5 |
| TST | ANDS into w3 (immediate or ROR form) | 0x72000003 / 0x6ac10003 |
| TEQ | EOR into w3, then ANDS w3,w3,w3 to set N/Z | 0x52000003 / 0x4ac10003, then 0x6a030063 |
| CMP | SUBS into w3 | 0x71000003 / 0x6b010003 |
| CMN | ADDS into w3 | 0x31000003 / 0x2b010003 |

With S=1 on operations that have no flag-setting AArch64 form (EOR, ORR, MOV, MVN), the result is followed by
`ANDS Rd, Rd, Rd` (0x6a000000 | regs) to set N and Z; this also clears C and V, which is repaired below.

**Flags.** Arithmetic S ops use the NZCV produced by host ADDS/SUBS/ADCS/SBCS **directly; there is no carry
inversion anywhere**: AArch64 SUBS sets C = NOT borrow exactly as ARM does, SBC/SBCS compute Rn-Rm-!C like ARM SBC,
and RSC is SBCS with swapped operands.

Logical S ops (AND/EOR/TST/TEQ/ORR/MOV/BIC/MVN) must deliver ARM's shifter carry and leave V unchanged. Only when the
metadata marks the flags as live (+0x19 bits 0-1), the operation is wrapped as follows:

1. The old flags are copied to x2 with `MRS x2, NZCV` (0xd53b4202).
2. If the immediate was rotated (rot != 0), the shifter carry is bit 31 of the rotated value, and bit 29 of w2 is
   cleared (`AND w2, w2, #0xdfffffff`, 0x12227842) or set (`ORR w2, w2, #0x20000000`, 0x32230042) accordingly.
3. The ANDS-based operation runs and sets N/Z.
4. The new flags are read into x1 (`MRS x1, NZCV`, 0xd53b4201), bits 29..0 (C, V and below) are taken from w2 with
   `BFXIL w1, w2, #0, #30` (0x33007441), keeping the new N/Z, and the result is written back with `MSR NZCV, x1`
   (0xd51b4201).

If the flags are not live the wrapper is omitted and host ANDS clobbers C/V, which is harmless by liveness
[likely: meaning of +0x19].

### 1.2 Register operand (inside cpu_translate_instruction, ~0x938xx-0x95600) [confirmed]
Most ops map one-to-one onto AArch64 shifted-register forms:

| ARM | AArch64 form | encoding base |
|---|---|---|
| AND / ANDS | AND / ANDS | 0x0a000000 / 0x6a000000 |
| EOR | EOR (+ANDS for S) | 0x4a000000 |
| SUB / SUBS | SUB / SUBS | 0x4b000000 / 0x6b000000 |
| ADD / ADDS | ADD / ADDS | 0x0b000000 / 0x2b000000 |
| ORR | ORR (+ANDS for S) | 0x2a000000 |
| BIC / BICS | BIC / BICS | 0x0a200000 / 0x6a200000 |
| MOV | ORR from WZR (+ANDS for S) | 0x2a0003e0 |
| MVN | ORN from WZR (+ANDS for S) | 0x2a2003e0 |
| TST / TEQ | ANDS into w3 / EOR into w3 then ANDS w3,w3,w3 | 0x6a000003 / 0x4a000003 + 0x6a030063 |
| CMP / CMN | SUBS / ADDS into w3 | 0x6b000003 / 0x2b000003 |

The ARM shift type (0 LSL, 1 LSR, 2 ASR, 3 ROR) goes into AArch64 bits 23:22 and the amount into bits 15:10.

- Ops with no shifted form or needing swapped operands: RSB becomes SUB(S) Rd, w1, Rn; ADC is ADC(S) Rd, Rn, w1;
  SBC is SBC(S) Rd, Rn, w1; RSC is SBC(S) Rd, w1, Rn. When the ARM operand is shifted, Rm is first pre-shifted into w1
  with an ORR-from-WZR (0x2a0003e1 | fields); unshifted, Rm is used directly.
- ROR by immediate on arithmetic ops (SUB/ADD/CMP/CMN; AArch64 ADD/SUB have no ROR): Rm is first rotated into w1 with
  ORR from WZR, ROR #n (0x2ac003e1).
- Special immediate shift amounts: LSR #0 (meaning #32) replaces the operand with **WZR**; ASR #0 (meaning #32)
  becomes ASR #31; ROR #0 (= RRX) is done in three steps: clear bit 0 of Rm into w1 (AND, 0x123f7801), add the host C
  flag (= guest C) into bit 0 with ADC w1, w1, WZR (0x1a1f0021), then use w1 with ROR #1 in the operation, which moves
  that bit to bit 31.
- NOP elimination: `MOV Rd, Rd` with LSL #0 and S=0 emits nothing. The test compares raw register numbers, so
  `mov pc,pc` (Rd=Rm=15) is also dropped; see 1.3.
- Shifter carry for logical S ops when flags are live: NZCV is copied to x2 with MRS, the carry bit is extracted into
  w3 and inserted into bit 29 of w2 (`BFI w2, w3, #29, #1`, 0x33030062), then the same MRS/BFXIL/MSR merge as in 1.1
  is applied. The extracted bit per shift kind:

| shift | carry source | extraction |
|---|---|---|
| LSL #n | Rm bit 32-n | UBFX (0x53000003 | fields) |
| LSR/ASR #n | Rm bit n-1 | UBFX |
| LSR/ASR #32 | Rm bit 31 | LSR #31 (0x531f7c03) |
| ROR #n | Rm bit n-1 | UBFX |
| RRX | Rm bit 0 | inserted directly from Rm (`BFI w2, Rm, #29, #1`, 0x33030002) |
| LSL #0 | C unchanged | none |

**Register-specified shift (bit 4 = 1).** Rs=PC gives w4 = PC+8; Rm=PC gives w1 = PC+12.
- Logical S op with flags live: Rm is copied into w1 (0x2a0003e1), the amount Rs & 0xff into w3 (0x12001c03), and a
  `BL` (0x94000000) calls arm64_shift_reg_flags_{lsl,lsr,asr,ror} (stubs at 0x8f86c-0x8f938). Stub contract: w1 =
  value (in and out), w3 = amount, w2 = NZCV copy whose bit 29 receives the carry. The operation then uses w1 and the
  usual merge follows.
- Otherwise (no carry needed) the shift is inlined, writing w1 (or Rd directly for a plain MOV):
  - LSL/LSR: w3 = Rs & 0xe0 (0x123b0803); LSLV/LSRV by Rs (0x1ac02000 / 0x1ac02400); if w3 is non-zero (CBZ skip,
    0x34000003) the result is replaced by zero (0x2a1f03e0). This gives 0 for amounts 32..255, which AArch64's
    modulo-32 LSLV/LSRV would not.
  - ASR: same pattern with ASRV (0x1ac02800); for amounts >= 32 the result is replaced by Rm ASR #31 (0x131f7c20).
  - ROR: a single RORV by Rs (0x1ac02c00).
- **Bug (stub)**: in arm64_shift_reg_flags_asr (0x8f900) the stub computes amount-1 into a scratch register but then
  shifts by the original amount, so the carry-out of ASR by register (1..31) is bit n instead of bit n-1 [confirmed
  from disassembly].

### 1.3 Writes to PC
See 0.4. With Rd=PC the result is in w0; Rn=PC operands read PC+8 (immediate / immediate shift) or PC+12 (register
shift). Exception: `mov pc,pc` (Rd=Rm=15, LSL #0, S=0) is removed by the NOP elimination of 1.2 before the PC-write
epilogue is reached. The block builder still treats it as a PC writer, so when it is unconditional it ends the block
and **no exit is emitted** (see coverage.md and review.md); a conditional one behaves as a NOP instead of skipping
the next instruction [confirmed in code; the resulting crash is likely].

---------------------------------------------------------------------------------------------------
## 2. Single data transfer: LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB/LDRSH/LDRD/STRD

Decode in cpu_translate_instruction [confirmed]: cpu_translate_memory_op takes (ctx, opcode, load, byte, half, dual,
signed, addr_mode, imm).
- Word/byte class: load = L, byte = B, half/dual/signed = 0, addr_mode 0 = imm12, addr_mode 2 = scaled register
  (bit 25).
- Halfword class: half = 1, with addr_mode 1 (register, no shift) or 0 with the split immediate (high nibble << 4 |
  low nibble). L=0 with S=1 means LDRD (H=0) or STRD (H=1); an odd Rd raises the undefined exception.
- Cond 0xF: nothing is emitted.

### 2.1 Address generation: cpu_translate_setup_memory_address @0x905e0 [confirmed]
Returns the host register that holds the access address.
- Immediate offset 0 with Rn != PC: no code; Rn's own host register is returned.
- Pre-index (P=1): with Rn=PC the address (PC+8 ± imm) is folded at translate time into a constant load of w0.
  Otherwise, W=0: ADD/SUB immediate (0x11000000 / 0x51000000) from Rn into w0, returning w0; W=1: Rn is updated in
  place and returned (**writeback happens before the access**; for LDR with Rd==Rn the load overwrites it afterwards,
  so ARM semantics hold).
- Post-index (P=0): Rn is copied to w0, then Rn is updated in place; w0 is returned.
- Register offset: ADD/SUB shifted-register (0x0b000000 / 0x4b000000) into w0, or into Rn when W=1; post-index copies
  Rn to w0 first and then updates Rn. Shift special cases: LSR #32 uses WZR; ASR #32 becomes ASR #31; ROR #n first
  rotates Rm into w2 (0x2ac003e2); RRX builds the rotated value in w2 with the AND / ADC-from-carry / ROR #1 trick of
  1.2 (0x123f7802, 0x1a1f0042, 0x2ac207e2). In those cases the offset register is w2. Rm=PC and Rn=PC are
  materialised (w0 and w1 respectively) with PC+8.

### 2.2 Loads [confirmed]
**ARM9 (ctx+0x478 == 1), not LDRD: inline fastmem with an out-of-line slow path.** The inline part, per access:

1. Test the address against a mask into w1: 0xfc000003 for words (0x12261c01), 0xfc000001 for halfwords
   (0x12261801), 0xfc000000 for bytes (0x12261401). A zero result means "below 64 MB and naturally aligned".
2. If zero, skip the next instruction (CBZ, 0x34000041); otherwise branch to the slow-path stub.
3. Fast case: load directly from the host mirror, `[x10, wA, UXTW]`: LDR 0xb8604940, LDRH 0x78604940,
   LDRSH 0x78e04940, LDRB 0x38604940, LDRSB 0x38e04940.

The slow-path stub is assembled in the ctx+0 buffer and copied to the stub area at ctx+0x410 (which grows down);
during the copy every B/BL (opcode bits 30:26 = 0b00101) has its imm26 relocated. The stub copies the address to w0,
calls arm64_load_ext{32_unsigned,16_unsigned,16_signed,8_unsigned,8_signed}, moves w0 into Rt and branches back to
just after the inline load.

So any naturally aligned address in [0, 0x4000000) is read directly from the 64 MB host mirror at x10 (main RAM
mirrors, ITCM, shared WRAM, DTCM when mapped); IO (>= 0x4000000), VRAM etc. and misaligned addresses go to
arm64_load_ext*. arm64_load_ext32_unsigned (0x8d27c) handles misalignment the ARM way: it loads the aligned word and
rotates it right by 8 x (addr & 3) bits; it sends 0x04xxxxxx to load_io_register_arm9_32 and everything else to
arm64_load_memory32_unsigned.

**ARM7 (and LDRD on both CPUs):** the address is copied to w0, arm64_load_memory{32_unsigned,16_unsigned,16_signed,
8_unsigned,8_signed} is called, and w0 is moved into Rt. These stubs walk a 2 KB-page table at x9 + (addr>>11)*8; the
entry shifted left by 2 is the host page pointer, otherwise they fall back to extended_load*.
LDRD calls arm64_load_memory64, which returns the word at addr in w1 and the word at addr+4 in w0; the emitter then
moves w1 into Rt (0x2a0103e0 | Rt) and w0 into Rt+1 (0x2a0003e0 | Rt+1); with Rt=r14 the first move targets w27
(0x2a0103fb).

**SP-relative fast path** (ARM9, Rn == r13, not LDRD/STRD, CP15+0x3c "DTCM in mapped memory" set) [confirmed]:
no check at all; a single load or store against `[x10, wA, UXTW]` (stores: STR 0xb8204940, STRH 0x78204940, STRB
0x38204940). The stack is assumed to be in DTCM/RAM, and these stores bypass the SMC check.

**PC-relative literal loads** (ARM9, Rn==PC, pre-indexed immediate, no writeback, aligned, target < 0x4000000)
[confirmed structure; table semantics likely]: the address is computed at translate time. Per-word metadata decides
between two outputs:
- run-time direct load: constant address into w1, then a load from `[x10, w1, UXTW]` (0xb8614940 etc.);
- **constant folding**: cpu_block_fill_allocation(cpu, addr, size) registers the word for SMC invalidation, the
  value is read at translate time with load_memory32/16/8(), and set_imm32 puts it into Rt.

The metadata consulted is the ITCM per-word modification counter (u8 table at T+0x14d8038, index (addr>>2)&0x1fff)
for targets below 0x2000000, and the main-RAM "modified after translation" bitmap (T+0x14da138, word index
(addr>>7)&0x7fff) for targets below 0x3000000 (see review.md #8 for the ITCM condition). Literals in
0x3000000-0x3ffffff are always folded.

### 2.3 Stores [confirmed]
There is no inline fastmem for stores (apart from the SP path). The emitted sequence: the value goes to w1 (with
Rt=PC, the metadata PC value, i.e. PC+8; real ARM9 stores PC+12 [guess on the exact value]), the address to w0, then
a BL to arm64_store_memory{8,16,32}_{arm9|arm7} (chosen by ctx+0x478). Afterwards
cpu_translate_store_pc_metadata(ctx, next_pc) records the host return address -> guest PC mapping, so the runtime
can recover the PC if the store triggers SMC invalidation or an alert.

SMC detection lives in the stub: bit 62 of a page-table entry marks "slow" pages (IO, or pages that contain
translated code). store32_arm9 (0x8d610) diverts to its slow path when bit 62 is set and otherwise writes straight
through the host pointer. The store-block stubs additionally test bit 63, call cpu_block_check_allocation_* and on a
hit run cpu_block_flush and cpu_block_lookup_base, then jump to the returned code to re-enter at the current PC.

STRD: the first value goes to w1 and the address to w0; the address is saved to the temp slot at x9-0xa0 (STUR,
0xb8160120); store32 is called; the address is reloaded (LDUR, 0xb8560120) and advanced by 4 (0x11001000); Rt+1 goes
to w1; store32 is called again; then a single PC-metadata entry is recorded.

### 2.4 SWP/SWPB [confirmed]
Sequence: address Rn to w0; call arm64_load_memory32_unsigned (or 8_unsigned); if Rd is neither Rn nor Rm, move w0
into Rd; address Rn to w0 and Rm to w1; call arm64_store_memory32|8_{arm9|arm7}; record PC metadata. If Rd aliases Rn
or Rm, the loaded value is parked in the temp slot x9-0xa0 and loaded into Rd only after the store (and its metadata
entry). Not atomic (irrelevant in a single thread).

---------------------------------------------------------------------------------------------------
## 3. LDM/STM: cpu_translate_block_memory_op @0x91640 [confirmed]
n = number of registers in the list (bit_count table). Rn=PC uses w0 = PC+8. An empty list emits nothing.
- **n == 1 and no S bit**: the instruction is re-encoded as a single LDR/STR with offset 4 (P/U derived from the LDM
  mode) and handed to cpu_translate_memory_op, so it gets fastmem etc.
- Start address (from Rn, or w0 for PC): IA uses the base as is (copied to w0 if needed); IB adds 4; DA subtracts
  4n-4; DB subtracts 4n (ADD/SUB immediate, 0x11000000 / 0x51000000).
- Writeback (Rn ± 4n) is emitted **before** the transfer call. For LDM it is suppressed (ARMv5 rule) when Rn is in
  the list, is not the only register, and is the highest-numbered register in the list. When writeback stays on and
  Rn is in the list, Rn's loaded slot is redirected into w1 (discarded) so the written-back value wins. ARM7 uses the
  same rule; there is no ARMv4 special case [confirmed code; the ARMv4 mismatch is a likely inaccuracy].
- **STM**: x1 is pointed at a staging buffer at x9-0x120 (0xd1048121); registers are written there in pairs with STP
  (0x29000020 | fields) or singly with STR (0xb9000020); a PC in the list is first loaded into w2 as PC+8. Then the
  writeback, then a BL through arm64_store_block_functions[n-1] (table at 0x167780 -> arm64_store_block1..16) with
  w0 = address, then store_pc_metadata.
- **LDM**: on ARM9 the inline part tests the address with the word mask (0x12261c01) and CBZ (0x34000001); the
  failing case branches to a stub that calls arm64_load_block_functions[n-1] (table at 0x167700 ->
  arm64_load_blockN) and branches back; the passing case forms the host pointer as x10 + UXTW(w0) in x0 (ADD extended
  register, 0x8b204140). On ARM7 arm64_load_blockN is called directly. Either way x0 ends up pointing at the n words
  (directly into memory, or into the x9-0x120 buffer filled by arm64_load_block_extended via extended_load32). The
  registers are then loaded from [x0, #off] with LDP (0x29400000 | fields) or LDR (0xb9400000). PC is loaded into w0
  and the 0.4 epilogue follows (ARM9: raw value -> interworking).
- **User-bank transfer (^) without PC in the list** (S=1 and not (L and PC in list)): before the transfer the base
  is saved to x9-0x9c (0xb8164120), the current mode index is read from x28+0x2104 (0xb9610780) and saved to x9-0xa0
  (0xb8160120), arm64_set_cpu_mode is called with w1 = 0 (USR bank), and the base is reloaded into w0 from x9-0x9c
  (0xb8564120). After the transfer the saved mode index is reloaded into w1 (0xb8560121) and arm64_set_cpu_mode is
  called again to restore the mode. LDM ^ with PC instead ends with a `B` to arm64_spsr_restore.

---------------------------------------------------------------------------------------------------
## 4. PSR transfer, exceptions, CP15, halt

### 4.1 MRS: cpu_translate_mrs_op @0x93240 [confirmed]
- CPSR: host NZCV is read into Rd (MRS, 0xd53b4200 | Rd); the in-memory CPSR word at x28+0x23c0 is loaded into w1
  (0xb963c381); its bits 27:0 are inserted into Rd with BFXIL #0, #28 (0x33006c20 | Rd). In a Thumb block bit 5 (T)
  is then set with ORR #0x20 (0x323b0000).
- SPSR: x1 = x9-0x2e8 (SPSR array, 0xd10ba121); w2 = mode index from x28+0x2104 (0xb9610782); Rd is loaded from
  x1 + w2*4 (LDR register, UXTW #2, 0xb8625820).

### 4.2 MSR: cpu_translate_msr_op @0x8fb00 [confirmed]
The operand is first put in w0 (a register move from Rm, or set_imm32 of the rotated immediate).
- CPSR, field f (bit 19): `MSR NZCV, x0` (0xd51b4200). Only NZCV changes; Q (bit 27) is not updated by this path
  [likely].
- CPSR, field c (bit 16): BL arm64_msr_cpsr_low, then store_pc_metadata. The stub stores the **whole** w0 into
  x28+0x23c0 (the f/s/x masks are not applied), so `msr cpsr_c, Rm` also writes Rm[31:8], and can set or clear Q and
  the stale flag image. It then calls execute_arm_set_cpu_mode_from_cpsr and re-checks IRQs.
- CPSR fields s/x are ignored. No user-mode privilege check is emitted.
- SPSR: w0 is masked to the selected fields and w1 receives the keep-mask. No fields: w0 = 0, w1 = all ones (MOV from
  WZR, MVN of WZR). All four fields: w1 = 0. Otherwise: AND w0 with the field mask and ORR w1 from WZR with its
  complement, both as bitmask immediates taken from the psr_mask_immr/imms tables (0x127b10 / 0x127b20, encodings of
  the 0xff-per-field masks). Then BL arm64_msr_spsr, which computes SPSR[mode] = (SPSR & w1) | w0. The stub skips the
  write **only in USR mode**; in SYS it writes SPSR slot 0 (the index shared by USR and SYS) [confirmed, review.md
  #18].

### 4.3 Exceptions: cpu_translate_raise_exception(ctx, vec, mode) @0x92ef0 [confirmed]
Callers: undefined/unsupported -> (1, 5) = vector 0x04, UND; SWI (class 7, bit 24) -> (2, 3) = 0x08, SVC; BKPT
(ARM9) -> (3, 4) = 0x0C, ABT. The mode index is turned into CPSR mode bits via cpu_modes_cpsr (0x11d940) =
{0x10, 0x11, 0x12, 0x13, 0x17, 0x1b, 0x1f}.

What the generated code does, in order:
1. Calls arm64_set_cpu_mode with w1 = mode index (bank switch; the stub saves NZCV to x28+0x23b8).
2. Sets LR of the new mode (w27) to the address of the next instruction (no |1 added for Thumb).
3. Loads the in-memory CPSR from x28+0x23c0 (setting bit 5 for a Thumb block) and stores it to SPSR[mode] at
   x28+0x20e8+4*mode (STR unsigned offset, encoding 0xb9000380 | (mode+0x83a)<<10).
4. Builds the new CPSR: clears bits 5:0 (mode and T), adds the mode bits, sets I (0x80), and stores it to x28+0x23c0.
5. Computes the vector: ARM9 loads the CP15 pointer from x28+0x2250, reads the vector base at +0x10 (0 or
   0xffff0000) and adds vec*4; ARM7 just loads the constant vec*4.
6. Branches to block_indirect_branch with the vector in w0.

Note: the SPSR is built from the in-memory CPSR without merging the live host NZCV, so the saved flags are stale. This
was upgraded from [likely] to [confirmed] in review.md #21 (arm64_set_cpu_mode never writes NZCV back into 0x23c0); it
only matters if the handler returns with MOVS pc,lr / LDM^ and the interrupted code depends on the flags.

### 4.4 CP15 [confirmed]
Class 7 with bit 4 = 1. cp# != 15, opc1 != 0, or no CP15 (ARM7) -> undefined; CDP/LDC/STC are no-ops on ARM9.

MCR (bit 20 = 0), source Rt:

| register | emitted |
|---|---|
| c7,c0,4 or c7,c8,2 (wait for interrupt) | w0 = next_pc (with the Thumb bit); halt state x28+0x2110 = 1; `B` block_halt. block_halt (0x8f654) stores the PC to x28+0x23bc, merges NZCV into the CPSR, sets w12 = -1 and jumps through x28+0x23b0 |
| c1,c0,0 (control) | Rt -> w1, BL arm64_update_coprocessor_control, PC metadata |
| c9,c1,0 (DTCM region) | Rt -> w1, BL arm64_update_coprocessor_dtcm_control, PC metadata. The stub masks the value, stores CP15+0x18, calls update_coprocessor_dtcm_control / remap_dtcm; if the "DTCM mapped" status changed it flushes all blocks (cpu_block_flush(-1)) and re-enters through cpu_block_lookup_base |
| c9,c1,1 (ITCM region) | Rt -> w1, BL arm64_update_coprocessor_itcm_control (no PC metadata) |
| anything else (cache ops, PU regions, ...) | nothing |

MRC (bit 20 = 1), destination Rt:
- c1,c0,0 / c9,c1,0 / c9,c1,1: Rt is loaded with the CP15 pointer from x28+0x2250 (0xf9512b80) and then with the
  word at +0x14 / +0x18 / +0x1c (0xb9400000 | imm).
- c0,c0,x: set_imm32 of a constant: opc2 1 -> 0x0f0d2112 (cache type), 2 -> 0x00140180 (TCM size), else 0x41009561
  (main ID, ARM946E-S).
- anything else: Rt = 0 (move from WZR).

### 4.5 Branches: BX/BLX(reg) via cpu_translate_bx_op @0x903b0 [confirmed]
- BX/BLX(reg): Rm is copied to w0 (Rm=PC: w1 = PC+8, then copied); BLX also sets w27 (LR) to next_pc | Thumb bit;
  then `B` block_indirect_branch.
- B/BL: the condition skip, then for BL / BLX-imm w27 = next_pc | Thumb bit, then cpu_translate_direct_link
  (documented elsewhere).
- Thumb BL halves arrive as pseudo-ARM opcodes (bits 25 and 4 set, Thumb blocks only). Prefix: w27 = pc +
  (sign-extended offset << 12). Suffix: w27 = next | 1 (this constant load is emitted twice), then w0 = w27 + off*2
  (ADD immediate, 0x11000360), then bit 0 set (BL) or bits 1:0 cleared (BLX), then `B` block_indirect_branch. The
  suffix derives the target from the *new* LR, which looks wrong for a lone suffix [guess: either a bug or the
  converter pre-adjusts the offset].

---------------------------------------------------------------------------------------------------
## 5. Multiply / DSP / CLZ (cpu_translate_instruction) [confirmed encodings]
"xLo/xHi" = the 64-bit view of the host registers of RdLo/RdHi.

| ARM | emitted code (summary) |
|---|---|
| MUL Rd,Rm,Rs | MUL (= MADD with WZR accumulator, 0x1b007c00) |
| MLA | MADD with Rn as accumulator (0x1b000000) |
| MULS/MLAS | as above, then ANDS Rd,Rd,Rd for N/Z; if flags are live, NZCV is copied to x3 before and C/V are merged back afterwards through x0 (MRS / BFXIL #0,#30 / MSR) |
| UMULL/SMULL | UMULL/SMULL into xLo (0x9ba07c00 / 0x9b207c00), then xHi = xLo LSR #32 (0xd360fc00) |
| UMLAL/SMLAL | xHi is inserted into the top half of xLo (BFI #32,#32, 0xb3607c00), UMADDL/SMADDL accumulates into xLo (0x9ba00000 / 0x9b200000), then xHi = xLo LSR #32 |
| ..LS variants | 64-bit ANDS xLo,xLo,xLo for N/Z (0xea000000), C/V merged back via x4 (0xd53b4204, 0x33007480) |
| CLZ (ARM9) | CLZ (0x5ac01000) |
| QADD/QSUB (ARM9) | Rm is sign-extended to 64 bits (SXTW); Rd(64) = Rm + Rn extended with UXTW (QADD, ADD extended register 0x8b204000) or Rm - Rn extended with SXTW (QSUB, SUB extended register 0xcb20c000). Overflow test: x0 = Rd(64) minus the sign-extension of its low word (SUB extended, SXTW); zero means no overflow (CBZ past the rest). Otherwise bit 63 of x0 (TBZ) selects 0x80000000 or 0x7fffffff for Rd, and Q (bit 27) is set in the in-memory CPSR (load x28+0x23c0, ORR #0x8000000, store) |
| QDADD/QDSUB | same, preceded by w1 = Rn + Rn (0x0b000001); but the add/sub that follows still uses Rn (w1 is only consumed when Rn=PC) |
| SMULxy | each operand's selected half is extracted into w0 / w1 (SXTH 0x13003c00 or ASR #16 0x13107c00, and the w1 variants), then MUL Rd, w0, w1 (0x1b017c00) |
| SMLAxy | halves into w0/w1; an ASR #0 of Rn onto itself (0x13007c00, effectively a no-op); SMADDL xRd = w0*w1 + xRn (0x9b210000); the same 64-vs-32 overflow test; Q set on overflow |
| SMULWy | Rs's half into w0 (SXTH / ASR #16); SMULL xRd = Rm * w0; SBFX xRd, #16, #32 (0x9350bc00) keeps bits 47:16 |
| SMLAWy | SMULL into x3 = Rm * w0; SBFX x3, #16, #32; ADD xRd = x3 + SXTW(Rn) (0x8b20c060); overflow test -> Q |
| SMLALxy | w0 = Rm ASR #1 (0x13017c00); w1 = SBFX Rs, #1, #15 or Rs ASR #17 (0x13013c01 / 0x13117c01); then the same BFI / SMADDL / LSR #32 accumulate as SMLAL |

Observed probable bugs (emitter constants verified in the binary around 0x957bc and 0x95068-0x95080):
- QADD/QDADD extend Rn with **UXTW** (0x8b204000 = ADD extended register, option UXTW) instead of SXTW, so a negative
  Rn wrongly saturates to 0x7fffffff and sets Q (QSUB correctly uses SXTW) [likely bug].
- QDADD/QDSUB: the doubled value (w1) is unused, so they behave like QADD/QSUB (no doubling and no doubling
  saturation) [likely bug].
- SMLAxy: the accumulator is zero-extended (ASR #0 where a SXTW was needed), giving a spurious Q when Rn is negative;
  the result value itself is correct [likely].
- SMLALxy picks the wrong halves (ASR #1 / SBFX #1,#15 / ASR #17; the x bit is ignored) [likely bug].

---------------------------------------------------------------------------------------------------
## 6. ARM -> AArch64 summary table
Host register of a guest register is written as the guest name. "Merge" = the MRS / BFXIL / MSR sequence of 1.1 that
keeps the new N/Z and restores C (shifter carry) and V.

| ARM instruction class | emitted AArch64 (summary) | conf |
|---|---|---|
| any `<cond>` except AL | leading B.cond on the inverted condition, skipping the translated body (0x54000000) | confirmed |
| ADD/SUB(S), CMP/CMN with imm12-encodable value | one ADD/SUB(S) immediate, optionally LSL #12; CMP/CMN write w3 (0x11000000, 0x51000000, 0x71000000, 0x71000003) | confirmed |
| ADD/SUB(S) with other immediates | constant built in w1 (MOVZ, MOVN, ORR-bitmask or MOVZ+MOVK), then the register form, e.g. ADDS (0x2b010000) | confirmed |
| RSB(S) #0 | NEG(S) | confirmed |
| ADC/SBC/RSC immediate | constant in w1, then ADC(S)/SBC(S); RSC uses SBC(S) with operands swapped; NZCV used as is, no carry inversion | confirmed |
| AND/ORR/EOR(S) with a bitmask-encodable value | one logical-immediate instruction (0x12000000 / 0x32000000 / 0x52000000) | confirmed |
| AND/ORR/EOR/BIC with other values | unrotated imm8 in w1, logical shifted-register op with ROR #2r | confirmed |
| MOV/MVN immediate | constant built directly in Rd | confirmed |
| logical op with S | the op, ANDS Rd,Rd,Rd where there is no flag-setting form, plus the merge when flags are live | confirmed |
| op with Rm shifted by immediate | single shifted-register op (LSR #32 -> WZR operand, ASR #32 -> ASR #31; ROR on ADD/SUB pre-rotated into w1) | confirmed |
| op with RRX operand | w1 = Rm with bit 0 cleared plus host C, then the op with w1 ROR #1 | confirmed |
| op with Rm shifted by Rs | inline LSLV/LSRV/ASRV/RORV with an Rs & 0xe0 test that forces 0 (or ASR #31) for amounts >= 32; or a call to arm64_shift_reg_flags_* when the carry is needed | confirmed |
| ALU op writing PC (MOV pc, ...) | result in w0, bits fixed for ARM/Thumb, B to block_indirect_branch; with S: B to arm64_spsr_restore; `mov pc,pc` emits nothing | confirmed |
| LDR/LDRH/LDRB, ARM9 | address into w0 if needed, mask test (AND 0xfc00000x) + CBZ, branch to an out-of-line slow stub, otherwise LDR* from [x10, address, UXTW] | confirmed |
| LDR*, ARM7 | address to w0, BL arm64_load_memory*, result moved to Rt | confirmed |
| LDR*/STR* relative to SP, ARM9 with DTCM mapped | single LDR*/STR* from/to [x10, address, UXTW], no check | confirmed |
| LDR literal (PC-relative), ARM9 | either the folded constant built in Rt, or a constant address in w1 and LDR* from [x10, w1, UXTW] | confirmed |
| STR/STRB/STRH | value to w1, address to w0, BL arm64_store_memoryN_{arm9,arm7}, PC metadata | confirmed |
| LDRD / STRD | BL arm64_load_memory64 then two moves into Rt, Rt+1 / two store32 calls with the address kept in a temp slot | confirmed |
| LDM | start address, writeback, (ARM9: inline mask test) BL arm64_load_blockN, LDP/LDR from [x0, #off] | confirmed |
| STM | registers staged at x9-0x120 with STP/STR, writeback, BL arm64_store_blockN | confirmed |
| MRS / MSR | see 4.1 / 4.2 | confirmed |
| SWI / UND / BKPT | see 4.3 | confirmed |
| MCR/MRC p15 | see 4.4 | confirmed |
| MUL/MLA/UMULL/... and DSP | see 5 | confirmed |
