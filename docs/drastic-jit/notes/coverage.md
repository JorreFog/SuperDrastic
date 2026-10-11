# JIT instruction coverage matrix (ARMv5TE ARM + Thumb, ARM9 vs ARM7)

Source: static analysis of cpu_translate_instruction (0x93320-0x95b10), the emitters it calls,
convert_thumb_instruction_to_arm (0x28fd0) and the classification pass of cpu_block_create (0x2bf70), cross-checked
by the dynamic tests where noted. Addresses are file VAs. Tags: [C] confirmed in code, [L] likely, [G] guess.

Values used in the "handling" columns:
* **inline**: plain AArch64 placed in the block, no call.
* **stub**: a `bl`/`b` into a hand-written arm64_* / block_* routine in 0x8cb0c-0x8fa20. Those routines may call C
  themselves (extended_load*/extended_store*, IO handlers, execute_arm_set_cpu_mode*, execute_arm_raise_interrupt,
  update_coprocessor_*, cpu_block_*).
* **UND**: cpu_translate_raise_exception(ts, 1, 5): enter UND mode at vector 0x04 (inline CPSR/SPSR update, a call
  to arm64_set_cpu_mode, then a branch to block_indirect_branch).
* **nothing**: no host code at all (behaves as a NOP).
* The JIT **never** hands an instruction to the interpreter. The only execute_arm_* functions reachable from JIT
  code are execute_arm_set_cpu_mode(_from_cpsr) and execute_arm_raise_interrupt (checked over all `bl` targets in
  0x8cb0c-0x8fa20) [C].

Rules that apply to every row [C]:
* Condition: for cond < 0xE an inverted-condition `b.cond` is emitted ahead of the code and patched afterwards to
  skip it (patch at 0x93544). cond == 0xF is never rejected as UND: B/BL turn into BLX, PLD-space memory ops emit
  nothing, and every other class is translated as if cond were AL.
* Block end: an instruction whose destination mask includes bit 15 (writes PC) is preceded by the pending-cycle
  `sub w12`. If it is unconditional it is the block's last instruction (exit type 0) and produces its own exit.
  Instructions flagged at rec+0x1c ("isolate": LDC/STC, empty-list LDM/STM, ARMv5-only ops on ARM7, ...) get a
  block of their own: the preceding block stops in front of them, and their own block ends with exit type 2
  (load the next pc into w0, branch to block_indirect_branch) (cpu_block_create, 0x2bf70).
* Rd/Rn/Rm = r15 as a source is a constant fixed at translate time (PC+8 in ARM, PC+4 in Thumb, PC+12 for Rm in
  register-shift forms). As a destination, the value goes to w0 and an exit follows.

---------------------------------------------------------------------------------------------------
## 1. ARM state

| instruction class | ARM9 (ARMv5TE) handling | ARM7 (ARMv4T) handling | ends block? | dispatch / emitter address | notes, divergences |
|---|---|---|---|---|---|
| Data processing, immediate (AND..MVN #imm) | inline (cpu_translate_alu_op_imm 0x91fd0). ADD/SUB(S) use the A64 imm12 form; logical ops use an A64 bitmask immediate when one exists, otherwise the 8-bit value goes into w1 and the op uses w1 rotated by 2r; ADC/SBC/RSC go through w1; MOV/MVN use set_imm32. NZCV stays in host flags; the C/V of logical S-ops are preserved (save, insert, restore of NZCV) only when live | same | only if Rd = PC (target aligned, then branch to block_indirect_branch; with S=1 branch to arm64_spsr_restore) | class 1 dispatch, call at 0x93584 | |
| Data processing, register, immediate shift | inline shifted-register forms. LSR/ASR #32 become a zero register / #31 shift; ROR on ADD/SUB is first applied into w1 with an ORR from wzr; RRX is built from AND/ADC/ROR #1; the shifter carry is extracted and inserted when C is live; `MOV Rd,Rd` (LSL #0, S=0) emits nothing | same | Rd = PC as above | class 0, ALU path 0x9382c-0x95b0c | **`mov pc,pc` (Rd = Rm = 15) is eliminated as well** (raw register-number comparison at 0x93c9c). Unconditional: the block has no exit and runs off its end [C code / L crash]. Conditional: acts as a NOP instead of skipping one instruction |
| Data processing, register-specified shift | inline: bits 7:5 of Rs are isolated (AND with 0xe0) to detect amounts >= 32, the variable shift is done with LSLV/LSRV/ASRV (RORV for ROR), and a zero test plus fix-up handles the large amounts. When the shifter carry is live (logical S-op): **stub** arm64_shift_reg_flags_{lsl,lsr,asr,ror} (0x8f86c-0x8f938) | same | Rd = PC as above | register-shift branch at 0x938dc (selected by bit 4) | arm64_shift_reg_flags_asr returns bit n instead of bit n-1 as the carry (0x8f904, dynamic-confirmed) |
| MSR CPSR, #imm / Rm | f field: inline write of host NZCV from the value (Q is not written). c field: **stub** arm64_msr_cpsr_low + PC metadata (the whole word is stored to 0x23c0, then execute_arm_set_cpu_mode_from_cpsr and cpu_alerts_check_interrupts; may divert into IRQ). s and x fields ignored. Immediate form with Rd field != 15 -> UND | same | no (may divert at run time via metadata to the next insn) | imm: 0x933f8/0x93404; reg: 0x94824 | CPSR_f cannot clear Q (dynamic); CPSR_c stores Rm[31:8] too, so it can change Q; no privilege check; encodings with bit 21 = 0 ("TST/CMP without S") are also handled as MSR |
| MSR SPSR | **stub** arm64_msr_spsr (w0 = masked value, w1 = keep mask from the psr_mask tables) | same | no | cpu_translate_msr_op 0x8fb00 | skipped only in USR; in SYS it writes SPSR slot 0 |
| MRS CPSR / SPSR | inline. CPSR: host NZCV merged with bits 27:0 of the word at cpu+0x23c0 (T ORed in for Thumb blocks). SPSR: indexed load from the SPSR array using the current mode index at cpu+0x2104 | same | no | 0x94dac -> cpu_translate_mrs_op 0x93240 | |
| MUL / MLA (+S) | inline `madd` with wzr or Rn as addend; S: an `ands Rd,Rd,Rd` for N/Z, C/V preserved if live | same | no | 0x946c0 | costs 1 cycle (no multiply timing); op 0b01x (UMAAL / undefined) is decoded as MUL/MLA |
| UMULL / SMULL / UMLAL / SMLAL (+S) | inline 64-bit multiply (umull/smull), or for the accumulate forms: pack RdHi:RdLo into one X register and use umaddl/smaddl; then split the high half out with a 64-bit shift; S: 64-bit `ands`, C/V preserved via x4 | same | no | 0x94be4/0x94bf4, 0x93bac, 0x93bec | correct (dynamic: umull/smlal fine). Leaves the high product in the upper half of x(RdLo) |
| SMULxy | inline: sign-extend the chosen halves of Rm (x = bit 5) and Rs (y = bit 6) into w0/w1, then `mul` | **UND** (0x94874) | no | DSP block, selected at 0x936dc | correct |
| SMLAxy | inline: halves as above, accumulator extended, `smaddl` into Rd; Q from comparing the 64-bit result with its sign-extended low word | UND | no | DSP block (0x936dc) | result right, **Q wrong** for negative Rn: the accumulator extension is encoded 0x13007c00 (32-bit SBFM, effectively a zero-extend) where SXTW 0x93407c00 is needed |
| SMULWy / SMLAWy | inline 32x32 signed multiply of Rm by the chosen half, keep bits 47:16 (+ sign-extended Rn added, Q check) | UND | no | 0x95164-0x95194, 0x95460 | correct |
| SMLALxy | inline: the operand extraction uses shifts by 1 and 17 and a 15-bit field instead of the intended 16-bit halves, then smaddl and split | UND | no | 0x9506c-0x950b8 | **wrong**: x bit ignored, operands halved (SBFM immr off by one) |
| QADD / QSUB | inline 64-bit add/sub, saturate, set Q (bit 27) in the word at cpu+0x23c0 | **UND** (0x953c8), isolated | no | misc space bits 6:5 = 2 -> 0x953b0, saturation constants 0x957bc/0x957c4 | **QADD wrong** for negative Rn: the add extends Rn with UXTW (0x8b204000) where SXTW (0x8b20c000) is needed; QSUB correct |
| QDADD / QDSUB | as QADD/QSUB plus a doubling of Rn into w1 whose result is never used | UND, isolated | no | same | **wrong**: no doubling and no doubling saturation (w1 only matters when Rn = PC) |
| CLZ | inline `clz` (0x5ac01000) | **nothing emitted** (Rd unchanged); isolated via rec+0x1c [L] | no | 0x943d0 | ARM7 should raise UND |
| LDR / LDRB (imm, reg, scaled reg, pre/post, writeback) | inline address computation (setup_memory_address 0x905e0), then a fastmem test: the address masked with 0xfc000003 (word) or 0xfc000000 (byte); zero -> direct load from x10 + address, non-zero -> out-of-line slow stub that calls **stub** arm64_load_ext*. Rn = SP with DTCM inside the window: unchecked load from x10 + address. Rn = PC literal: folded to a constant (main RAM / WRAM; in ITCM only if the word's modification counter is > 7) or a run-time load from x10 + constant address | always **stub** arm64_load_memory{32,8}_* (no fastmem, no folding) | LDR pc: yes (ARM9 uses the raw value -> interworking; ARM7 clears bits 1:0) | class 2/3 at 0x935fc -> cpu_translate_memory_op 0x90a00 | unaligned LDR rotation is done in the stub; writeback is emitted before the access |
| STR / STRB | **stub** arm64_store_memory{32,8}_arm9 + PC metadata (SMC, IO and alerts handled inside). Rn = SP with DTCM in the window: inline store to x10 + address (no SMC check) | **stub** `..._arm7` + PC metadata | no (may divert -> next insn) | 0x90a00 | STR PC stores PC+8 [G vs. real ARM9] |
| LDRH / LDRSB / LDRSH | inline fastmem (mask 0xfc000001 for halfwords) + slow stub arm64_load_ext16_* / ext8_signed; SP and literal paths as for LDR | stub arm64_load_memory16_* / 8_signed | no (PC destination not meaningful) | halfword branch 0x94134/0x94760 | |
| STRH | stub arm64_store_memory16_* + metadata (SP path inline) | stub `_arm7` | no | same | |
| LDRD | **stub** arm64_load_memory64 (w1 = [a], w0 = [a+4]), no fastmem; odd Rd -> UND | **same (not rejected)** | no | same; odd-Rd UND at 0x93ac4/0x93ad4 | ARMv4T has no LDRD/STRD |
| STRD | two calls to arm64_store_memory32_*, with the address kept at x9-0xa0 across the first; one PC-metadata entry, after the second call only | same (not rejected) | no | 0x90ed0-0x90f88 | the first store has no metadata entry, so get_pc misresolves if it diverts |
| LDM / STM (IA/IB/DA/DB, writeback) | one register and no ^: re-encoded as LDR/STR (and so gets fastmem). LDM: ARM9 inline window check, host address = x10 + address, then paired/single loads (slow path: stub arm64_load_blockN). STM: registers spilled to x9-0x120, then **stub** arm64_store_blockN + metadata | LDM: always stub arm64_load_blockN; STM: as ARM9 | LDM with PC: yes (ARM9 raw value / interworking, ARM7 clears bits 1:0) | class 4 at 0x93528/0x93578 -> 0x91640 | the ARMv5 writeback rule is applied to ARM7 too; empty list -> nothing (isolated); cond 0xF translated as AL |
| LDM/STM with ^ (no PC) | inline: call arm64_set_cpu_mode(USR), do the transfer, call arm64_set_cpu_mode(saved index) | same | no | 0x91640 | |
| LDM with ^ and PC | loads, then branch to arm64_spsr_restore (CPSR = SPSR, mode switch, IRQ check, lookup) | same | yes | 0x91640, tail | |
| SWP / SWPB | call arm64_load_memory{32,8}_unsigned, then arm64_store_memory{32,8}_{arm9,arm7} + metadata; if Rd aliases, the loaded value is kept at x9-0xa0 | same | no | bit-24 branch at 0x940dc, 0x94938/0x94d40; invalid opc -> UND 0x940fc | not atomic (irrelevant here); in the aliasing case Rd is lost if the store diverts |
| B / BL | inline. BL sets w27 = next \| T via set_imm32. cpu_translate_direct_link then emits one of: a direct `b` to the target entry or entry+8 (same cache class); w0 = target and a branch to block_itcm_branch_* (ITCM target); w0 = target and a branch to block_indirect_branch (crossing the alternate cache); a delayed patch when the target is still being translated. Idle loop: `movn w12,#0` before the branch | same | yes (if AL) | class 5, 0x93a00-0x93a50 | |
| BLX #imm (cond 0xF) | inline: w27 = next, then a direct link to target \| 1 (target computed in cpu_block_create including the H bit) | **same, not rejected** | yes | 0x93a78/0x93a80 | ARMv4T: this space is NV/undefined, not BLX |
| BX Rm | inline: Rm copied to w0 (bit 0 selects Thumb), branch to block_indirect_branch | same | yes (AL) | misc bits 6:5 = 0, bit 22 = 0 -> cpu_translate_bx_op 0x903b0 | |
| BLX Rm | inline: Rm copied to w0, w27 = next \| T, branch to block_indirect_branch | **UND** (0x953c8), isolated | yes | 0x953d8/0x953ec | |
| SWI | raise_exception(2, 3): SVC, vector 0x08 (ARM9: exception base from cp15 +0x10, plus 8) | same (vector 0x08) | yes | 0x93c70/0x93c7c | saved SPSR has stale NZCV (live flags not merged into 0x23c0) |
| BKPT | cond == AL: raise_exception(3, 4): ABT, vector 0x0C; otherwise UND | **UND** | yes | 0x953fc-0x95414 | |
| MRC p15 | inline. c1,c0,0 / c9,c1,0 / c9,c1,1: load the cp15 pointer from cpu+0x2250, then its word at +0x14 / +0x18 / +0x1c. c0,c0,{1,2,other}: constants 0x0f0d2112 / 0x00140180 / 0x41009561. Anything else: Rd := 0 | **UND** (no CP15) | no | 0x94f6c | main ID differs from a real ARM946E-S (0x41059461); Rd = PC writes w0 only (no flags) |
| MCR p15 | c7,c0,4 / c7,c8,2: w0 = next \| T, halt flag cpu+0x2110 := 1, branch to block_halt. c1,c0,0: **stub** arm64_update_coprocessor_control + metadata. c9,c1,0: stub ..._dtcm_control + metadata. c9,c1,1: stub ..._itcm_control (no metadata). Everything else (cache ops c7, PU c2/c3/c5/c6, ...) -> nothing | **UND** | halt forms: yes | cp15 path of cpu_translate_instruction; metadata store at 0x94668 | cp# != 15 or opc1 != 0 -> UND (0x93ad4). ICache invalidation needs no code because SMC is caught on the store |
| CDP | nothing | UND | no | cp15 path of cpu_translate_instruction (0x93320) | |
| LDC / STC (incl. cond 0xF LDC2/STC2) | nothing (cond < AL: only an empty skip branch) | UND | isolated (own block) | class 6 (UND sites 0x93658, 0x93aa4) | |
| PLD (cond 0xF, LDR space) | nothing (memory_op returns early for cond 0xF, 0x90a00) | nothing | no | 0x935fc -> 0x90a00 | |
| Undefined: class 3 with bit 4 (media space) | UND | UND | (exception exit) | 0x93ad4 | in Thumb blocks the same encoding is the BL-half pseudo-op |
| Undefined: misc-space leftovers, invalid SWP opc, bad coprocessor | UND | UND | | 0x93ad4 / 0x940fc / 0x953c8 | |

### ARM9 vs ARM7 (ARMv5TE-only instructions on ARMv4T)
The checks are a test of the cpu id (`[cpu+0x210c] == 1`) inside cpu_translate_instruction and/or rec+0x1c
isolation in cpu_block_create:

| v5 instruction | ARM7 result | correct for ARMv4T? |
|---|---|---|
| QADD/QSUB/QDADD/QDSUB | UND (0x953c8) | yes |
| SMLAxy/SMLAWy/SMULxy/SMULWy/SMLALxy | UND (0x94874) | yes |
| BLX Rm | UND (0x953c8) | yes |
| BKPT | UND (0x953c8) | yes |
| CLZ | **no code (NOP)** | no (should be UND) |
| BLX #imm (cond 0xF) | **executed as BLX** | no |
| LDRD/STRD | **executed** | no (not defined on v4T) |
| PLD | NOP | fine (NV on v4 = never) |
| MCR/MRC/CDP/LDC/STC | UND (no CP15 on ARM7) | yes |
| Thumb BLX Rm | UND (converted to ARM BLX Rm) | yes |
| Thumb BLX #imm suffix (11101) | **executed as BLX** (fused in cpu_block_create with no CPU check) | no |

Other ARM7 code-generation differences: no fastmem loads or LDM, no literal folding, no SP path (ts+0x478 = 0),
`_arm7` store handlers, LDR/LDM into pc forced to bits 1:0 clear (ARM state) or bit 0 set (Thumb) with no
interworking (correct for v4T), cycles = record cost * 2.

---------------------------------------------------------------------------------------------------
## 2. Thumb state (convert_thumb_instruction_to_arm 0x28fd0 -> ARM opcode -> ARM translator)

cpu_block_create reads each halfword (`load_memory16`), converts it to one ARM opcode, and the translator then runs
with ts+0x479 = 1. Effects: PC reads give pc+4 (with bit 1 cleared for the PC-relative forms), Rd = PC results get
bit 0 set, and LR values carry bit 0.

| Thumb format | converted ARM opcode | resulting handling | notes |
|---|---|---|---|
| 1 shift by imm (LSL/LSR/ASR #imm5) | `MOVS Rd,Rm,<sh> #imm` 0xe1b00000 | inline ALU (immediate shift); #0 for LSR/ASR means #32 in both encodings | |
| 2 ADD/SUB reg / imm3 | `ADDS/SUBS Rd,Rn,Rm\|#imm3` 0xe0900000/0xe0500000 (+I) | inline | |
| 3 MOV/CMP/ADD/SUB #imm8 | `MOVS` 0xe3b00000 / `CMP/ADDS/SUBS Rd,Rd,#imm8` | inline | |
| 4 ALU: AND/EOR/ADC/SBC/ORR/BIC/TST/CMP/CMN/MVN | S-forms 0xe0100000\|op<<21, MVNS 0xe1f00000 | inline | |
| 4 LSL/LSR/ASR/ROR by reg | `MOVS Rd,Rd,<sh> Rs` 0xe1b00010 | inline, or stub arm64_shift_reg_flags_* when C is live | the ASR carry bug applies |
| 4 NEG / MUL | `RSBS Rd,Rs,#0` 0xe2700000 / `MULS Rd,Rd,Rs` 0xe0100090 | inline | |
| 5 hi-reg ADD / CMP / MOV | `ADD` (no S) 0xe0800000 / `CMP` 0xe1500000 / `MOV` (no S) 0xe1a00000 | inline; Rd = PC -> bit 0 set in w0, branch to block_indirect_branch | `mov pc,pc` -> no code (see the ARM table) |
| 5 BX / BLX Rm | 0xe12fff10 \| H1<<5 | as ARM BX / BLX (BLX: ARM7 UND) | |
| 6 LDR Rd,[PC,#imm8*4] | `LDR Rd,[PC,#imm*4]` 0xe59f0000, aligned PC | ARM9: literal folding / run-time load; ARM7: stub load | |
| 7/8 LDR/STR/LDRB/STRB reg; STRH/LDRSB/LDRH/LDRSH reg | 0xe7800000 (\|L\|B), 0xe1800090 + SH table | as ARM single / halfword transfer | |
| 9/10 LDR/STR(B) imm5, LDRH/STRH imm5 | 0xe5800000, 0xe1c000b0 | as ARM | |
| 11 LDR/STR SP-relative | 0xe58d0000 | ARM9: SP fast path when DTCM is in the window | |
| 12 ADD Rd,PC/SP,#imm | 0xe28f0f00 (aligned PC) / 0xe28d0f00 | inline | |
| 13 ADD/SUB SP,#imm7*4 | 0xe28ddf00 / 0xe24ddf00 | inline | |
| 14 PUSH / POP | `STMDB sp!` 0xe92d0000 (\|LR) / `LDMIA sp!` 0xe8bd0000 (\|PC) | stub store_blockN / inline LDM; POP pc: ARM9 raw value (interworking), ARM7 bit 0 set | **every `1011 x xxx` pattern with bits 11:8 != 0 is decoded as PUSH/POP** (PUSH/POP decode in convert_thumb_instruction_to_arm, 0x28fd0): Thumb **BKPT 0xBExx becomes `LDMIA sp!,{imm8 regs}`** instead of a prefetch abort, and the other v5-undefined 0xB1xx-0xBFxx patterns likewise |
| 15 LDMIA/STMIA Rn! | 0xe8a00000 (\|L), W=1 always | as ARM LDM/STM (ARMv5 Rn-in-list rule) | |
| 16 B<cond> | `cond<<28 \| 0x0a000000 \| sext(imm8)` | direct link | **cond 0xE (0xDExx, undefined in Thumb) becomes an always-taken branch** |
| 17 SWI | `0xef000000 \| imm8<<16` | raise_exception(2,3) | |
| 18 B | `0xea000000 \| sext(imm11)` | direct link | |
| 19 BL/BLX prefix + suffix | pseudo-op `0xe6000010 \| op16<<5`; cpu_block_create fuses a prefix/suffix pair into NOP + `BL` 0xeb... / `BLX` 0xfa000000, with the real target in the branch entry | fused: like ARM BL/BLX imm (direct link, w27 = next \| 1) | unpaired halves: the prefix sets w27 = pc_read + (sext(off) << 12); the suffix computes its target from the LR it has just rewritten (bug, translator_core sec. 4); suffix 11101 is not rejected on ARM7 |

---------------------------------------------------------------------------------------------------
## 3. Summary of coverage gaps / wrong translations (for the qemu harness)
1. QADD, QDADD, QDSUB (JIT): wrong (sign extension / missing doubling). SMLAxy Q flag, SMLALxy (JIT and interpreter).
2. MOVS / logical-S `... ASR Rs` carry (stub arm64_shift_reg_flags_asr, 0x8f904), Thumb ASR-by-register too.
3. `mov pc,pc` (ARM and Thumb hi-reg MOV): emits nothing; the unconditional form leaves the block without an exit.
4. Thumb BKPT -> POP; Thumb `0xDExx` -> branch always.
5. ARM7: CLZ = NOP; BLX imm / Thumb BLX suffix / LDRD / STRD executed instead of UND.
6. MSR CPSR_f cannot clear Q; MSR CPSR_c writes all of Rm[31:8] into the CPSR word.
7. SWI/UND/BKPT save stale NZCV into SPSR.
8. STRD first-store divert and SWP-alias divert (PC-metadata gaps).
9. cond 0xF is never UND (ALU, LDM/STM, MCR/MRC are executed unconditionally).

Suggested quick tests: `mov pc,pc; mov r0,#1; mov r0,#2` (expect r0=2), Thumb `bkpt` with an abort handler,
ARM7 `clz`, `qdadd r0,r1,r2` with r2=0x80000000 (the interpreter was predicted to give 0xc0000000+r1 instead of
saturating).
