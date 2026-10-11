# DraStic JIT translator core (ARM9/ARM7 -> AArch64)

> **Errata (from the consistency review in review.md, checked against the binary; these take precedence):**
> - ts+0x000 is the slow-path stub buffer, not unused padding; ts+0x410 is lowered as stubs are added.
> - Exit type 4 stops *before* the next hot ITCM word; it does not create one-instruction blocks.
> - CDP on ARM9 emits nothing (it does not raise UND).
> - The r15 home slot cpu+0x23ac is the interpreter's; JIT code keeps the guest PC at cpu+0x23bc.
>
> All four corrections have been worked into the sections below.

Scope: the analysis front end cpu_block_create (0x2bf70), the code-generation driver cpu_translate_block (0x95e20),
the per-instruction translator cpu_translate_instruction (0x93320), the register and immediate helpers, conditional
skips, direct linking, the link stub, delayed targets, PC metadata, get_pc, bx_op, convert_thumb_instruction_to_arm
(0x28fd0), and the block lookup entry points as far as they matter for linking. Addresses are file VAs (add
0x100000 for the Ghidra view). Confidence tags: **[C]** confirmed in the binary (code or disassembly read directly),
**[L]** likely (strong inference), **[G]** guess.

Notation: `T` is the translation-cache base, the u64 at cpu+0x2088 (also held in host x11 while JIT code runs).
Every cache-area offset below is given as `T + off`. The decompiler view shows these addresses biased by
nds_system's image address (0x4fc000); that bias has been removed throughout.

---------------------------------------------------------------------------------------------------------------
## 0. Pipeline overview

```
miss in block_indirect_branch / block_itcm_branch_*  (asm runtime)
 └─ cpu_block_lookup_base                       0x2db10
     ├─ reset per-translation state: translation-stack pointer = T+0x13a0000,
     │  delayed-target count = 0, in-progress count = 0
     ├─ cpu_block_create                         0x2bf70   front end
     │   ├─ fetch each guest insn (load_memory32, or load_memory16 + Thumb->ARM conversion)
     │   ├─ classify, fill a 0x20-byte record, collect branch entries, decide where the block ends
     │   ├─ resolve branch targets: in-block forward -> internal link; otherwise
     │   │  cpu_block_lookup_recursive (translates missing targets on the spot) or a delayed record
     │   ├─ idle-loop test; backward liveness for registers and flags
     │   └─ cpu_translate_block                  0x95e20   back end
     │       ├─ prologue (cycle check)
     │       ├─ per record: cycle bookkeeping, then cpu_translate_instruction
     │       ├─ exit sequence chosen by exit type
     │       └─ trailer + metadata, patch in-block branches
     ├─ insert into the hash table / ITCM direct table
     ├─ cpu_translate_link_delayed_targets       0x964e0
     └─ cache maintenance (DC CIVAC + IC IVAU, line size from CTR_EL0) over every range that grew
```
Evidence [C]: cpu_block_lookup_base sets the stack pointer to T+0x13a0000, calls cpu_block_create and
cpu_translate_link_delayed_targets, and performs the DC CIVAC / IC IVAU loops. cpu_translate_block has exactly one
caller, at 0x2ce8c inside cpu_block_create (its final action).

Analysis passes that exist:
* decode/classify with per-instruction source/destination register masks, flags-read and flags-written nibbles,
  and a cycle cost [C]
* **known-condition folding** after a backward conditional branch (instructions are NOPed or made unconditional) [C]
  (sec. 3)
* **backward liveness** over 16-bit register masks and the 4-bit NZCV set, merging in a successor block's live-in
  (kept in its trailer) [C]. The code generator only consumes the C/V part of flag liveness (to drop C/V
  preservation); N and Z are always produced when S=1 [L]. Register liveness does not affect codegen because
  registers are statically mapped; it only feeds linking metadata and logging [L].
* **idle-loop detection** (sec. 6) [C]
* no general constant propagation; the only compile-time constants are r15 reads, taken from the record's PC
  field [C]

---------------------------------------------------------------------------------------------------------------
## 1. Data structures

### 1.1 Instruction record (0x20 bytes; array `blk->insns`, first malloc(0x200), grown by realloc doubling, max 0x800 records)
| off | type | meaning | evidence |
|-----|------|---------|----------|
| 0x00 | u32 | ARM opcode. Thumb input is already converted. The front end may rewrite it: NOP 0xe1a00000, condition forced to AL, fused Thumb BL | cpu_block_create (fetch, folding, BL fusion) [C] |
| 0x04 | u32 | value r15 reads as for this instruction: pc+8 (ARM) or pc+4 (Thumb), with bit 1 cleared when the converted Thumb op needs a word-aligned PC (PC-relative LDR, ADD Rd,PC). Never overwritten later | cpu_block_create fetch; review #22 [C] |
| 0x08 | u32* | host address of this instruction's code; cpu_translate_block stores the emit cursor here before translating it | 0x96080 [C] |
| 0x10 | u16 | record index of the in-block branch target (0 = none / external) | cpu_block_create branch resolution [C] |
| 0x12 | u16 | source register mask (bit n = rn read, 0x8000 = r15) | cpu_block_create classify [C] |
| 0x14 | u16 | destination register mask; bit 15 (PC written) marks a branch / exit point | B gives 0x8000, BL 0xc000 [C] |
| 0x16 | u16 | live-out register mask from liveness (0x7fff = all live at unknown exits); cpu_translate_block adds in the source and destination masks | cpu_block_create liveness, cpu_translate_block [C] |
| 0x18 | u8 | low nibble: flags read (N=8, Z=4, C=2, V=1; from condition_source_flags at 0x11dc30, plus C for RRX and register-specified shifts). High nibble: flags written (S bit) | cpu_block_create classify and liveness [C] |
| 0x19 | u8 | live-out NZCV nibble (0xf at unknown exits) | cpu_block_create; emitters test `& 3` [C] |
| 0x1a | u8 | bit flags: 1 = halt MCR (c7 wait-for-interrupt); 2 = target of an in-block branch; **4 = needs a PC-metadata slot** (stores, SWP, STM, MSR CPSR_c, MCR cp15 c1 / c9 DTCM); 8 = load | cpu_block_create; the count of bit-2 records sizes the metadata array [C] (meaning of 1 and 8 [L]) |
| 0x1b | u8 | base cycle cost (default 1) | initialised together with 0x19..0x1c as {0,0,1,0} by one 32-bit store [C] |
| 0x1c | u8 | "isolate" flag (LDC/STC, ARMv5-only ops on ARM7, empty LDM/STM list, ...): the block ends before such an instruction with exit type 2 | cpu_block_create [C] |

`condition_source_flags` (8 x u32, indexed by cond>>1): EQ/NE 0x4, CS/CC 0x2, MI/PL 0x8, VS/VC 0x1, HI/LS 0x6,
GE/LT 0x9, GT/LE 0xd, AL 0 (values in the top nibble of each word) [C] (read from the binary).

Cycle-cost rules applied by cpu_block_create [C unless noted]:
* default 1; ALU with a register-specified shift: 2 (and C is counted as read); LDRD/STRD and SWP: 2 (only when
  cond = AL); LDR into r15: 3 (AL); LDM/STM: number of registers in the list (a single register counts 2), plus 1
  if an LDM loads r15 (AL);
* **load-use interlock**: one extra cycle when the instruction reads a register that the previous instruction
  loaded (the intersection of the previous load's destinations and this source mask is non-empty) [C]. A
  writeback base of LDM counts as "loaded" (review, independent cycle check).

### 1.2 Branch entry (0x28 bytes; array on a global "translation stack" at T+0x13a0000..T+0x1420000)
| off | type | meaning |
|-----|------|---------|
| 0x00 | u64 | host entry of the target block once it is known, else 0 |
| 0x08 | u32* | host address of the emitted B that may need patching (filled by direct_link) |
| 0x10 | u64 | pointer to the delayed-target record (T+0x1420030 + n*0x10) or 0 |
| 0x18 | u32 | target guest pc (bit 0 set for Thumb) |
| 0x1c | u32 | guest pc after the branch instruction |
| 0x20 | u32 | flags: 1 = synthesized fall-through exit (link to target+8, i.e. past the cycle check); 2 = target is in ARM9 ITCM, route through block_itcm_branch_*; 4 = source and target in different caches with one of them the alternate cache, route through block_indirect_branch |
| 0x24 | u16 | index of the branch record |
| 0x26 | u16 | in-block target index (0 = external) |

Evidence: cpu_block_create fills these while collecting and resolving branches; cpu_translate_direct_link (0x90220)
reads +0x00, +0x08, +0x10, +0x18 and +0x20 [C].
The stack pointer is kept at T+0x14fa140. When it reaches T+0x1420000 the front end prints "Translation stack
overflow!" and **carries on**, so it would run into the cache-pointer block at T+0x1420000 [C].

### 1.3 Block descriptor `blk` (a stack object in cpu_block_create's frame, handed to cpu_translate_block)
| off | type | meaning |
|-----|------|---------|
| 0x00 | u64 | T+0x1420030 (delayed-target table) |
| 0x08 | rec* | instruction records |
| 0x10 | branch* | this block's slice of the translation stack |
| 0x18 | u32 | start pc |
| 0x20 | u16 | instruction count |
| 0x22 | u16 | size in halfwords (ARM: count*2, Thumb: count) |
| 0x24 | u16 | branch-entry count |
| 0x26 | u16 | live-in register mask |
| 0x28 | u8 | live-in NZCV |
| 0x29 | u8 | flags: bit3 = ARM9 (whenever the cpu id is 1), bit2 = idle loop; cpu_translate_block adds bit0 = Thumb |
| 0x2a | u8 | exit type (sec. 6) |

Evidence: the fields are laid out contiguously in cpu_block_create's frame; cpu_translate_block reads +0x08, +0x10
and +0x20..+0x2a [C].

### 1.4 Translation state `ts` (0x480 bytes at sp+0x80 inside cpu_translate_block; passed to every emitter)
| off | meaning |
|-----|---------|
| 0x000-0x3ff | **scratch buffer for out-of-line slow paths.** cpu_translate_memory_op assembles a load slow-path stub here, then copies it into the cache's stub area, fixing up the B/BL displacements as it copies (0x90c44-0x90d98) [C] |
| 0x400 | T |
| 0x408 | `u32 *emit`: main output cursor; each emitter appends one word and advances it |
| 0x410 | stub-area pointer. Starts at metadata_top - 0x18 - 4*cap (0x95f64) and is **lowered by each slow-path stub** that gets copied out; at block end the cache's metadata pointer is set from it (0x96290; ITCM path 0x9619c) [C] |
| 0x418 | saved main emit cursor while a stub is being assembled in the scratch buffer (restored at 0x90d98) [C] |
| 0x420 | PC-metadata capacity (number of records with rec+0x1a bit 2) |
| 0x428 | PC-metadata array base |
| 0x430 | PC-metadata write pointer |
| 0x438 | block entry (code start = header + 4) |
| 0x440 | block start pc |
| 0x444 | pending cycles not yet subtracted |
| 0x448 | pc of the NEXT guest instruction (current pc + 4 or 2); used as the LR value and as the resume pc |
| 0x450 | blk* |
| 0x458 | current rec* |
| 0x460 | next branch entry to consume (direct_link advances it by 0x28) |
| 0x468 | cpu* |
| 0x478 | u8 cpu id (1 = ARM9, 0 = ARM7), copied from cpu+0x210c |
| 0x479 | u8 Thumb |

Evidence: cpu_translate_block initialises the struct at 0x95eac-0x95fdc (stores to sp+0x480..0x4f9);
cpu_translate_store_pc_metadata; the dead inlined copy cpu_translate_allocate_pc_metadata; review #1-#3 [C].

### 1.5 Code caches, block layout and trailer
There are three caches. Each has a code pointer growing upward and a metadata pointer growing downward; the
pointer pairs live at T+0x1420000:
| pointer pair | code starts at | metadata top | serves |
|------|-----------|--------------|----------|
| T+0x1420000 / +08 | T | T+0x1000000 | pc>>24 == 2 (main RAM) |
| T+0x1420010 / +18 | T+0x1000000 | T+0x1100000 | ARM9 with pc < 0x2000000 (ITCM) |
| T+0x1420020 / +28 | T+0x1100000 | T+0x1300000 | everything else ("alternate": BIOS, WRAM, VRAM, ...) |

Evidence: reset_translation_cache and the cache selection at the start of cpu_translate_block [C].
cpu_translate_block itself does not test for code/metadata collision [L]; flushing is decided elsewhere.

Layout of one emitted block (by position relative to the entry point):
| position | contents |
|---|---|
| entry - 4 | u32 header: trailer offset (trailer - T) |
| entry + 0 | TBZ on bit 31 of w12, skipping to entry + 8 (encoding 0x36f8004c). This word is also the marker get_pc scans back for |
| entry + 4 | BL to recompiler_cpu_next_action_arm9_to_arm7 (ARM9) or recompiler_cpu_next_action_arm7_to_event_update (ARM7) |
| entry + 8 | block body. The BL returns here, so after the other CPU / event update has run, execution resumes in the body |

The metadata side of a block, from the old metadata top downward:
1. trailer, 0x18 bytes (table below);
2. PC-metadata array, cap * 4 bytes;
3. the block's copied-out slow-path stubs (ts+0x410 moves down past each one). This is why branches from a
   main-RAM block to its slow paths land about 16 MB away, in the T+0x1000000 region [C].

Trailer:
| off | meaning |
|-----|---------|
| 0x00 | hash-chain next (a trailer offset); written as 0, linked by cpu_block_lookup_* |
| 0x04 | start pc, bit 0 = Thumb |
| 0x08 | code offset (entry - T) |
| 0x0c | u16 live-in registers |
| 0x0e | u16 live-in flags |
| 0x10 | u16: low 12 bits = size in halfwords; bits 12-15 = block flags (bit12 Thumb, bit14 idle, bit15 ARM9) |
| 0x12 | u16 code size in bytes (header not included) |
| 0x14 | u32 PC-metadata array offset (relative to T) |

Evidence: cpu_translate_block 0x9613c-0x96190; cpu_block_log reads +4, +0xc, +0xe, +0x10, +0x12; the hash-chain
walk in cpu_block_lookup_base uses +0, +4, +8 [C].

**PC metadata.** Each entry is a u32: low half = (pc of the next guest instruction - block pc), high half = index
of the host instruction just after the call. cpu_translate_store_pc_metadata writes one right after every `bl` to
a handler that may need the guest PC (stores, MSR CPSR_c, cp15 control/DTCM). Exceeding the capacity or a delta
above 0xffff produces a FATAL printf, not an abort [C].

**cpu_translate_get_pc(host_return_address, T)** works as follows [C]:
1. walk backward from the return address to the nearest 0x36f8004c marker (the block entry);
2. read the header word before it to find the trailer;
3. search the metadata array for the entry whose high half equals (return address - entry) / 4;
4. return trailer pc + low half: the pc of the *next* guest instruction, with the Thumb bit preserved.

Callers: arm64_store_memory{8,16,32}_{arm9,arm7}, arm64_store_block1..16, handle_cpu_alerts (passing its return
address and x11), cpu_alerts_*, arm64_update_coprocessor_control [C]. The metadata therefore lets a store that
triggers SMC handling, an IO alert or an IRQ abandon the block and resume at the next guest instruction.

---------------------------------------------------------------------------------------------------------------
## 2. Guest -> host register mapping (static)

Rule: guest rN (N < 15) lives in host w(N+13); r15 gets special handling. The `+13` (0xd) appears in every
register lookup in cpu_translate_instruction, cpu_translate_mrs_op and cpu_translate_bx_op [C]. The spill lists in
recompiler_cpu_next_action_* and arm64_set_cpu_mode (0x8f7f8-0x8f814) save w13..w27 to x9-0x60..x9-0x28, which
matches [C].

| host | role | evidence |
|------|------|----------|
| w0 | scratch; destination when guest Rd = r15 (the result then goes to an exit); target pc (bit 0 = Thumb) for block_indirect_branch and the ITCM stubs; address argument to memory handlers; PC constant slot 0 | the destination lookup substitutes register 0 for r15; bx_op copies Rm into w0 [C] |
| w1 | scratch; PC constant slot 1; store data; mode argument for arm64_set_cpu_mode; MCR value | `mov w1, wN` = 0x2a0003e1 \| N<<16 [C] |
| w2, w3 | scratch; PC constant slots 2 and 3; saved NZCV for C/V fix-ups (MRS of NZCV into x2/x3/x4) | cpu_translate_map_src_reg [C] |
| w4..w8 | short-lived scratch within emitted sequences (w4 = saved NZCV for long multiplies) | [C] |
| x9 | cpu+0x23d0: base for the guest register spill area and temp slots (x9-0xa0 is the SWP/STRD temp) | every asm stub recomputes x9 as x28 + 0x23d0 [C] |
| x10 | guest memory-map base used by the fast paths (e.g. `ldr w0,[x10,w0,uxtw]` = 0xb8604940); saved at cpu+0x2358 | [C]; this is the fastmem window described in startup.md |
| x11 | translation-cache base T (host code = x11 + 32-bit offset); saved at cpu+0x2360 | block_indirect_branch forms targets this way [C] |
| w12 | cycle budget: the block body runs while w12 >= 0; blocks subtract their cost; setting w12 = -1 (MOVN w12,#0) forces a yield | sec. 5 [C] |
| w13..w27 | guest r0..r14 (r13/SP = w26, r14/LR = w27) | [C] |
| x28 | cpu state struct | [C] |
| x29, x30, sp | host frame / return address into the recompiler | [C] |

* **r15 never has a host register.** When read, its value (rec+0x04) is materialised as a constant into a scratch
  slot by `cpu_translate_map_src_reg.part.0` (one MOVZ, MOVN, ORR-bitmask, or MOVZ+MOVK) or by
  `cpu_translate_set_imm32` [C]. When written, the destination is w0 and an exit follows: w0 is aligned (bits 1:0
  cleared in ARM state, bit 0 set in Thumb state) and the block branches to block_indirect_branch; with S=1
  (e.g. MOVS pc,lr) it branches to arm64_spsr_restore instead [C] (register-ALU path ending near 0x95b0c).
* Guest register file in memory: r0..r14 at cpu+0x2370+4n (= x9-0x60..x9-0x28) [C]. The JIT's guest PC slot is
  cpu+0x23bc; cpu+0x23ac is the interpreter's r15 home (review) [C].
  CPSR at cpu+0x23c0, held without the live NZCV and without the T bit while JIT code runs (loaded by
  `ldr w1,[x28,#0x23c0]` = 0xb963c381) [C]. NZCV spill slots: cpu+0x23b8 (CPU switch) and cpu+0x2354 (slow paths)
  [C]. SPSR for each mode at cpu+0x20e8 + 4*mode_index, current mode index at cpu+0x2104 [C]. Halt flag at
  cpu+0x2110 [C]. Coprocessor pointer at cpu+0x2250 (NULL on ARM7); the cp15 object holds +0x10 exception base,
  +0x14 control, +0x18 DTCM, +0x1c ITCM [C].
* **Banked registers.** Since r8-r14 are pinned in w21-w27, a mode change has to exchange host registers with the
  bank storage. The translator never does this inline; it calls `arm64_set_cpu_mode` (w1 = mode index, used by
  cpu_translate_raise_exception), `arm64_msr_cpsr_low` (MSR CPSR_c, followed by PC metadata) and
  `arm64_spsr_restore` (data processing with S and Rd = pc). The analysis gives MSR CPSR_c a source mask of 0x7f00
  (r8-r14) so liveness keeps them, consistent with the stub saving them [C for the calls and mask, L for the
  swapping inside the stub; see asm_runtime notes].
* MRS CPSR builds the value from the live host NZCV (top four bits) merged with bits 27:0 of the stored CPSR word;
  in a Thumb block bit 5 (T) is ORed in from the block's static Thumb flag [C] (cpu_translate_mrs_op 0x93240).

---------------------------------------------------------------------------------------------------------------
## 3. Conditional execution and flags

* Guest NZCV stays in host NZCV for the whole block; it is never materialised. ARM condition codes 0-13 have the
  same numbering and meaning as A64 `B.cond`, and A64 uses ARM's carry convention for subtraction (C = NOT borrow)
  in SUBS/SBCS/CMP, so **no carry inversion is needed** and ADDS/SUBS/ADCS/SBCS map directly [C for the 1:1
  condition mapping and for the direct `subs` 0x6b000000 / `ands` 0x6a000000 emission; the semantic point is
  architectural].
* Conditional skip, for cond < 0xE [C]:
  1. emit a placeholder `B.cond` with the inverted condition (0x54000000 | (cond ^ 1));
  2. translate the instruction;
  3. rewrite bits 23:5 (imm19) of the placeholder with the word distance from it to the current emit position,
     leaving the other bits as they were.

  Until step 3, the imm19 field holds a meaningless value derived from the emit address. The patch happens at the
  end of cpu_translate_instruction (0x93544). `cpu_translate_conditional_skip` (0x932d0) contains the same logic as
  a separate function but has **no callers** (it is inlined everywhere) [C].
* cond 0xF is never treated as UND: B/BL with cond 0xF becomes BLX imm, a PLD-space memory op emits nothing, and
  every other class (ALU, LDM/STM, MCR/MRC, ...) is translated as if cond = AL [C] (memory_op returns early for
  cond 0xF at 0x90a00).
* Pending cycles are subtracted *before* the skip, so a conditional branch costs the same whether taken or not [C].
* **Flag fix-ups depend on liveness** (rec+0x19 & 3, i.e. C or V live afterwards):
  - Logical ops with S (AND/EOR/TST/TEQ/ORR/MOV/BIC/MVN): NZCV is copied to w2 first; the shifter carry is put into
    bit 29 of that copy (bit-field extract + insert from the shifted register, or a constant AND/ORR for rotated
    immediates, encodings 0x12227842 / 0x32230042); the flag-setting A64 op runs; then the new flags are read,
    their bits 29:0 replaced from the copy, and written back. Net effect: N and Z from the operation, C from the
    shifter, V unchanged [C] (cpu_translate_instruction register-ALU path; cpu_translate_alu_op_imm 0x91fd0).
  - MULS/MLAS (C and V unchanged on ARMv5): same pattern with the saved copy in w3 around an `ands Rd,Rd,Rd` that
    sets N/Z; long multiplies do a 64-bit `ands` on the result and keep the copy in w4 [C].
  - When C and V are both dead, none of the save/restore is emitted [C]. N/Z are always computed when S=1 [L].
* **Known-condition folding (front end, cpu_block_create)**: after a *backward* conditional branch on condition c,
  the fall-through path knows c is false. Following instructions with condition c are replaced by `mov r0,r0`
  (0xe1a00000) and those with the opposite condition get cond = AL. This continues until an instruction whose
  flags-written nibble is non-zero [C].
  Possible hazard: an in-block *forward* branch landing after that point arrives with unknown flags, and the
  folding does not consider branch targets [G - potential bug, no test case yet].

---------------------------------------------------------------------------------------------------------------
## 4. Thumb

Thumb code is translated by **first turning each halfword into an equivalent ARM opcode** (cpu_block_create reads
it with `load_memory16` and calls `convert_thumb_instruction_to_arm(op, &needs_aligned_pc)`), then running the ARM
translator with `ts->thumb = 1` [C]. The Thumb-specific parts:
* convert_thumb_instruction_to_arm (0x28fd0) [C], main mappings:
  | Thumb | ARM produced |
  |---|---|
  | shift by immediate | `MOVS Rd,Rm,<sh> #imm` (0xe1b00000) |
  | ADD/SUB register / imm3 | ADDS / SUBS |
  | MOV/CMP/ADD/SUB imm8 | MOVS (0xe3b00000) / CMP / ADDS / SUBS |
  | ALU format | NEG -> `RSBS Rd,Rs,#0` (0xe2700000); MUL -> MULS (0xe0100090); LSL/LSR/ASR/ROR by register -> `MOVS Rd,Rd,<sh> Rs` (0xe1b00010); TST/CMP/CMN via a table; MVN -> MVNS |
  | hi-register ADD / CMP / MOV | ADD, CMP, MOV (MOV without S: 0xe1a00000) |
  | BX / BLX register | 0xe12fff10 \| L<<5 |
  | LDR pc-relative, ADD Rd,PC,#imm | `LDR Rd,[pc,#imm*4]` (0xe59f0000), 0xe28f0f00; both set *needs_aligned_pc* so the record's r15 value becomes (pc+4) with bit 1 clear |
  | ADD Rd,SP / ADD,SUB SP,#imm | 0xe28d0f00 / 0xe28ddf00, 0xe24ddf00 |
  | load/store imm, reg, byte, halfword, signed | ARM single / halfword transfers (0xe5800000, 0xe7800000, 0xe1c000b0, 0xe1800090 + SH table) |
  | LDR/STR SP-relative | 0xe58d0000 |
  | PUSH / POP | `STMDB sp!` 0xe92d0000 (+LR) / `LDMIA sp!` 0xe8bd0000 (+PC) |
  | LDMIA / STMIA | 0xe8a00000, always with W=1 |
  | B<cond> / B / SWI | cond<<28 \| 0x0a000000 \| sext(imm8); 0xea000000 \| sext(imm11); 0xef000000 \| imm8<<16 (comment field placed in bits 23:16, where the ARM SWI path reads it) |
  | **BL/BLX halves** (bits 15:11 = 11101 / 11110 / 11111) | pseudo-opcode 0xe6000010 \| op16<<5: an ARM undefined-space encoding that carries the Thumb halfword (bit16 = low bit of H, i.e. suffix; bit17 = BL(1) / BLX(0); imm11 in bits 15:5) |
* cpu_block_create fuses a prefix+suffix pair: the prefix record becomes a NOP (0xe1a00000) and the suffix becomes
  ARM `BL` (0xeb......) or `BLX imm` (0xfa000000). Target = pc + sext22(hi:lo)*2, then bit 0 set for BL or bits
  1:0 cleared for BLX. The real target is kept in the branch entry; the translator ignores the opcode's offset
  field [C].
* Unpaired halves (block split between prefix and suffix, or entry at a suffix) reach cpu_translate_instruction as
  the 0xe6000010 pseudo-op (class 3 with bit 4; Thumb blocks only, in ARM blocks this space raises UND) [C]:
  - prefix: w27 = rec->pc_read + (sext(imm11) << 12), via set_imm32;
  - suffix (0x93d84-0x93e08) [C]: (a) w27 is set to next|1, and this is emitted twice; (b) w0 = w27 + imm11*2
    (`add w0,w27,#imm` = 0x11000360 | imm<<10); (c) w0 gets bit 0 set (BL) or bits 1:0 cleared (BLX); (d) branch
    to block_indirect_branch.
    **Bug [L]:** LR is overwritten in (a) before (b) reads it, so the target is computed from the return address
    rather than from the prefix's partial LR. Only unfused halves are affected (rare).
* Other Thumb-aware spots: LR values are next_pc | 1; Rd = pc results get bit 0 set; MRS adds T; exceptions store
  CPSR | T into SPSR; the block key is pc | 1; ITCM has a separate direct table for Thumb (cpu+0x2278, one slot
  per halfword) [C]. Thumb branch-target arithmetic, (imm+1)*2 with bit 0 set, is done in cpu_block_create [C].

---------------------------------------------------------------------------------------------------------------
## 5. Cycle accounting

* Cost per instruction (cpu_translate_block 0x96020-0x9605c) [C]:
  - ARM9 (cpu id 1): record cost + the per-game adjustment, a u32 at nds_system+0x3b2a988 reached through the
    system pointer at cpu+0x2258. apply_cycle_adjustment_hacks (0xfa20) sets it from a game-code switch (e.g. to 1)
    [C].
  - ARM7: record cost * 2 (the system clock runs at ARM9 rate and the ARM7 at half) [C/L].
  - Starting pending value: 2 (ARM9) or 4 (ARM7) when exit type == 0, else 0 (a refill penalty added at block
    start whatever the branch kind; 0x95ef4-0x95f08) [C].
* Costs collect in ts->pending and are only emitted as `sub w12,w12,#n` (0x5100018c | n<<10, preceded by
  `sub w12,w12,#(n>>12),lsl #12` = 0x5140018c when n > 0x1000). The flush happens **before any instruction whose
  destination mask has bit 15** (writes PC: B/BL/BX/LDR pc/LDM {pc}/ALU pc/SWI) and before the fall-through link
  of exit type 3 [C].
* Check: the block entry tests w12; while it is >= 0 the body runs; when negative, the BL switches to the other
  CPU or the event update and then returns into this block's body (x30 kept at cpu+0x2298) [C].
* Accounting gaps [C]:
  - exit types 1, 2 and 4 emit no final `sub`, so cycles after the last PC-writing instruction are lost. For
    type 4 this means the tail of any block that stops in front of a hot ITCM word goes uncounted;
  - n == 0x1000 exactly emits `sub w12,w12,#0` (the large-immediate test is strictly greater-than);
  - a block left half-way through by a store alert never subtracts the cycles of its executed part, since only
    branch points subtract [L].

---------------------------------------------------------------------------------------------------------------
## 6. Block ends, exits and linking

### Where a block ends (cpu_block_create) and its exit type (blk+0x2a)
| type | condition | tail emitted by cpu_translate_block after the last instruction |
|------|------|------|
| 0 | the last instruction is an unconditional PC write (B/BL/BX/ALU pc/LDR pc/LDM pc/SWI, ...) | none; the instruction produced its own exit |
| 1 | the 0x800-instruction limit was reached | load next_pc (bit 0 = Thumb) into w0, branch to block_indirect_branch |
| 2 | stopped in front of a record with rec+0x1c set; or 4 consecutive zero opcodes (the count is reduced by 2); or an ARM7 branch into 0x800000-0x1ffffff | same as type 1 |
| 3 | truncated at the earliest *backward*-branch target inside the block, so loop heads start their own blocks | flush pending cycles, then direct_link through a fall-through entry (flag 1) |
| 4 | ARM9, ARM state, and the *following* word (pc already advanced at fetch) has an ITCM modification counter (T+0x14d8038 + index) of 9 or more: the block stops before that word, so a hot ITCM word always begins its own block (required by the poly cache) | load next_pc into w0, branch to block_itcm_branch_arm |

Evidence: tail selection in cpu_translate_block 0x960d8-0x962fc; end decisions in cpu_block_create [C]; type-4
semantics per review #6 [C].

### Resolving branch targets at analysis time (cpu_block_create) [C]
For each branch entry, in this order of preference:
| situation | outcome |
|---|---|
| forward target inside this block | internal link: +0x26 = target index, rec+0x10 set, target rec+0x1a gets bit 1, liveness merged |
| target is currently being translated (in-progress list at T+0x14a0034, count at T+0x14c0034) | delayed record at T+0x1420030 + n*0x10 = {patch site, target pc}, count at T+0x14a0030; live-out = everything |
| otherwise | `cpu_block_lookup_recursive`: hash lookup, **translating the target recursively if absent**, with no depth limit. Found: entry+0x00 = host target and live-out gains the target trailer's live-in. Not translatable: delayed record |

### cpu_translate_direct_link(ts) (0x90220): emits the link for the next branch entry [C]
1. Take the entry at ts+0x460. Start from its host target (+0x00); with flag 1 and a known target, aim 8 bytes
   further (past the target's cycle check).
2. If the block is an idle loop (blk+0x29 bit 2) and this is the block's *first* branch entry, emit
   `movn w12,#0` (0x1280000c) first, so the loop gives up the rest of its time slice (0x90238).
3. Flag 2 (ITCM target): load the target pc into w0 and make the branch destination block_itcm_branch_thumb or
   block_itcm_branch_arm according to the Thumb bit.
4. Flag 4 (cross-cache): load the target pc into w0 and emit B to block_indirect_branch.
5. Otherwise emit a direct B (0x14000000 | imm26) to the chosen destination. If the target is still unknown and
   the entry is neither an internal branch nor ITCM-routed, this B's address is stored in the entry's
   delayed-target record so it can be patched later. An unknown target leaves imm26 meaningless: the unpatched B
   points at the next 256 MB-aligned address above itself (seen in dynamic testing, review #13).
6. Store the B's address at entry+0x08 and advance ts+0x460 by 0x28.

* In-block branches (entry+0x26 != 0) are fixed up at the very end of cpu_translate_block: the B's imm26 is
  rewritten with the word distance to the target record's host address (rec+0x08) [C].
* Delayed branches are fixed up by cpu_translate_link_delayed_targets once the whole recursive translation has
  finished: each record's target is looked up with cpu_block_lookup_base and the imm26 rewritten [C].
  (lookup_base resets the delayed count when it has to translate, while link_delayed_targets loops over a cached
  count [G - potential issue].)
* Region rules: flag 2 when the target is ARM9 ITCM and either the source is not ITCM or the target word is hot;
  flag 4 when source and target sit in different caches and one of them is the alternate cache. Direct `B` links
  therefore only exist main->main, alt->alt, ITCM->ITCM (non-hot) and ITCM->main [C].
* Fall-through links of exit type 3 (flag 1) land on target entry + 8 (review #14) [C].

### Indirect branches [C]
cpu_translate_bx_op copies Rm into w0 (for Rm = pc it materialises pc_read in w1 and copies that); BLX also sets
w27 = next | T; then it branches to block_indirect_branch. That stub looks the pc up in a direct-mapped 1024-entry
cache inside the cpu struct (tag at cpu+0x80 + (pc & 0xffc), code offset at cpu+0x1080 + (pc & 0xffc); the tag
comparison includes the Thumb bit). On a hit it jumps to x11 + offset. On a miss it saves w12-w18 and NZCV, calls
cpu_block_lookup_base, restores, and jumps to the returned address.
`cpu_translate_link_stub` (0x96370; loads pc & ~1, applies the ARM/Thumb fix-up, branches to
block_indirect_branch) has **no callers** [C].

### Idle-loop detection (cpu_block_create) [C]
Applies when the first branch entry targets the block's own start. Initially "invariant" = registers not written
anywhere in the loop. Scanning the body:
* an unconditional instruction that reads only invariant registers makes its destinations invariant too (so a
  load from an invariant address counts: the loop is polling memory);
* any record with rec+0x1a bit 2 (store / side effect), or one that reads a varying register, disqualifies the loop.

On success blk->flags gets bit 2, and direct_link emits `movn w12,#0` before the backward branch, so the loop
yields the CPU immediately.

### Exceptions and special ops in cpu_translate_instruction
* cpu_translate_raise_exception(ts, vector, mode): SWI uses (2, 3 = SVC); UND (1, 5); BKPT on ARM9 (3, 4 = ABT)
  [C]. The emitted sequence does, in order:
  1. switch mode through arm64_set_cpu_mode (w1 = mode index);
  2. set guest LR (w27) to next_pc;
  3. read the CPSR word at cpu+0x23c0 (adding T in Thumb blocks) and store it into that mode's SPSR slot
     (cpu+0x20e8 + 4*mode). The live host NZCV is *not* merged in, so the saved SPSR carries stale flags;
  4. clear the mode bits (AND with 0xffffffc0), add the mode's bits from cpu_modes_cpsr, set I (bit 7), and store
     the result back to cpu+0x23c0;
  5. compute the vector in w0: ARM7 uses the constant vector*4; ARM9 adds vector*4 to the exception base read from
     the cp15 object (+0x10);
  6. branch to block_indirect_branch [C].
* cp15 (ARM9 only; ARM7 or no coprocessor -> UND):
  | operation | translation |
  |---|---|
  | MCR c7,c0,4 / c7,c8,2 (wait for interrupt) | w0 = next pc (bit 0 = Thumb), halt flag cpu+0x2110 := 1, branch to block_halt |
  | MCR c1,c0,0 | Rd copied to w1, call arm64_update_coprocessor_control, PC metadata |
  | MCR c9,c1,0 | call arm64_update_coprocessor_dtcm_control, PC metadata |
  | MCR c9,c1,1 | call arm64_update_coprocessor_itcm_control (no metadata) |
  | MRC c1,c0,0 / c9,c1,0 / c9,c1,1 | load the cp15 pointer from cpu+0x2250, then the word at +0x14 / +0x18 / +0x1c |
  | MRC c0,c0,{0,1,2} | constants 0x41009561 / 0x0f0d2112 / 0x00140180 |
  | other MRC | Rd := 0 |
  | CDP (bit 4 clear) | no code on ARM9 |
  | LDC / STC | ignored on ARM9 (only the empty conditional skip when cond < AL) |

  The real ARM946E-S main ID is 0x41059461; DraStic returns 0x41009561 [C for the value].

---------------------------------------------------------------------------------------------------------------
## 7. Driver and dispatcher, step by step

### cpu_translate_block (0x95e20)
| step | action | state involved |
|---|---|---|
| 1 | Pick the cache: ARM9 with pc < 0x2000000 -> ITCM cache; pc in 0x02xxxxxx -> main-RAM cache; anything else -> alternate cache | T+0x1420000..0x28 |
| 2 | Initialise `ts` (T, start pc, blk, first branch entry, cpu, cpu id, Thumb). Starting pending cycles: 2 / 4 for exit type 0, else 0 | ts+0x400..0x479 |
| 3 | Count records with rec+0x1a bit 2 to get the metadata capacity; reserve trailer (0x18) + 4*cap below the cache's metadata top; metadata pointers and the stub-area pointer (ts+0x410) start at the low end of that reservation | ts+0x410..0x430 |
| 4 | Write the two-word prologue (cycle test + BL to the CPU-switch routine) after the header slot; the emit cursor starts at entry + 8 | ts+0x408, 0x438 |
| 5 | For every record: add its cost (ARM9: cost + adjust, ARM7: cost*2) to pending; store the emit cursor in rec+0x08; OR the source/destination masks into the live-out mask; make it current; if it writes PC, flush pending as `sub w12`; set next pc (+4 or +2); call cpu_translate_instruction | per record |
| 6 | Emit the tail selected by the exit type (sec. 6 table) | |
| 7 | Fill the header word and the trailer (pc \| T, code offset, live-ins, size \| flags<<12, code bytes, metadata offset) | trailer |
| 8 | Publish the cache pointers: code pointer = emit cursor, metadata pointer = ts+0x410 (now below the trailer, the metadata array and any copied-out stubs) | cache pointer pair |
| 9 | Patch every internal branch entry's B to the target record's host address; return the entry address | branch entries |

### cpu_translate_instruction (0x93320): what each opcode class becomes
Every class except the cond-0xF cases goes through the conditional skip of sec. 3 when cond < 0xE. The class is
opcode bits 27:25.

| class (bits 27:25) | covers | handling |
|---|---|---|
| 0 | data processing (register operand), multiplies, SWP, halfword/doubleword transfers, misc space | if bits 7 and 4 are both set: bits 6:5 = 0 -> multiplies (MADD/SMULL/UMULL family with liveness-gated C/V keep) or SWP/SWPB; otherwise LDRH/STRH/LDRSB/LDRSH/LDRD/STRD through cpu_translate_memory_op. Else if it is a "compare without S" encoding (misc space): bit 7 clear -> MRS / MSR register / BX / BLX register / CLZ / QADD-family / BKPT, selected by bit 4, bits 6:5 and bit 22; bit 7 set -> the DSP multiplies (ARM9) or UND (ARM7). Else the shifted-register ALU path, where `MOV Rd,Rd` (no S, no shift) emits nothing and Rd = pc ends the block (sec. 2) |
| 1 | data processing with immediate, MSR immediate | "compare without S" encodings are MSR imm: Rd field != 15 -> UND, otherwise the rotated immediate goes into w0 and cpu_translate_msr_op runs. Everything else -> cpu_translate_alu_op_imm |
| 2 | LDR/STR immediate offset | cpu_translate_memory_op |
| 3 | LDR/STR register offset; media space | with bit 4 set: UND in ARM blocks, the unpaired Thumb BL/BLX half (sec. 4) in Thumb blocks; otherwise cpu_translate_memory_op |
| 4 | LDM/STM | cpu_translate_block_memory_op; cond 0xF is translated as AL |
| 5 | B / BL / BLX imm | for BL or cond 0xF, w27 := next \| T; then cpu_translate_direct_link |
| 6 | LDC/STC | ARM9: no code; no coprocessor: UND |
| 7 | SWI, CDP, MCR/MRC | bit 24 set -> SWI (raise_exception 2, 3); no CP15 -> UND; bit 4 clear (CDP) -> no code; otherwise the cp15 table in sec. 6 |

Misc-space detail (class 0, bit 7 clear, bit 4 set), by bits 6:5 [C]:
| bits 6:5 | ARM9 | ARM7 |
|---|---|---|
| 0 | bit 22 set: CLZ (0x5ac01000); bit 22 clear: BX | CLZ emits nothing; BX |
| 1 | BLX register | UND |
| 2 | QADD/QSUB/QDADD/QDSUB (saturation, Q = bit 27 of cpu+0x23c0) | UND |
| 3 | BKPT when cond = AL (raise_exception 3, 4), else UND | UND |

Details of the ALU and memory emitters are in emitters.md.

---------------------------------------------------------------------------------------------------------------
## 8. A64 encodings emitted by the translator (decoded with llvm-mc)
| purpose | A64 instruction | base encoding (\| fields) |
|---|---|---|
| block prologue / get_pc marker | tbz w12,#31,#+8 | 0x36f8004c |
| handler calls, prologue | bl imm26 | 0x94000000 |
| exits, links | b imm26 | 0x14000000 |
| conditional skip | b.<inverted cond> imm19 | 0x54000000 \| cond^1 |
| argument / result moves | mov wD, wN (orr wD,wzr,wN) | 0x2a0003e0 \| N<<16 \| D (e.g. 0x2a0e03e0 = mov w0,w14 = guest r1) |
| store data / MCR value | mov w1, wN | 0x2a0003e1 \| N<<16 |
| MRC unknown -> 0, MSR helpers | mov wD, wzr | 0x2a1f03e0 \| D |
| set_imm32 | movz / movz lsl16 / movn / movn lsl16 / movk lsl16 | 0x52800000 / 0x52a00000 / 0x12800000 / 0x12a00000 / 0x72a00000 |
| set_imm32 via imm_map_to_logical | orr wD, wzr, #bitmask | 0x320003e0 \| immr<<16 \| imms<<10 |
| cycle flush | sub w12,w12,#imm (lsl #12) | 0x5100018c / 0x5140018c |
| idle loop | movn w12,#0 (w12 = -1) | 0x1280000c |
| ARM / Thumb target fix-up | and w0,w0,#0xfffffffc / orr w0,w0,#1 | 0x123e7400 / 0x32000000 |
| unpaired Thumb BL suffix | add w0, w27, #imm | 0x11000360 \| imm<<10 |
| flag preservation | mrs x2/x3/x4,nzcv ; msr nzcv,x0/x1 | 0xd53b4202/3/4 ; 0xd51b4200/1 |
| keep new N,Z, old C,V | bfxil w0,w3,#0,#30 / bfxil w1,w2,#0,#30 / bfxil w0,w4,#0,#30 | 0x33007460 / 0x33007441 / 0x33007480 |
| insert shifter carry into saved C | bfi w2,w3,#29,#1 | 0x33030062 |
| ALU | ands / and / subs (32-bit) | 0x6a000000 / 0x0a000000 / 0x6b000000 |
| MUL / MLA | mul / madd | 0x1b007c00 / 0x1b000000 |
| long multiply | smull / umull / bfi x,x,#32,#32 / lsr x,x,#32 / ands x | 0x9b207c00 / 0x9ba07c00 / 0xb3607c00 / 0xd360fc00 / 0xea000000 |
| SWP temp | stur w0,[x9,#-0xa0] / ldur w0,[x9,#-0xa0] | 0xb8160120 / 0xb8560120 |
| set Q flag | ldr w1,[x28,#0x23c0] ; orr w1,w1,#0x8000000 ; str w1,[x28,#0x23c0] | 0xb963c381 ; 0x32250021 ; 0xb923c381 |
| halt flag | str w1,[x28,#0x2110] | 0xb9211381 |
| MRC cp15 | ldr xD,[x28,#0x2250] ; ldr wD,[xD,#off] | 0xf9512b80 \| D ; 0xb9400000 \| ... |
| MRS SPSR | sub x1,x9,#0x2e8 ; ldr w2,[x28,#0x2104] ; ldr w0,[x1,w2,uxtw #2] | 0xd10ba121 ; 0xb9610782 ; 0xb8625820 |
| MRS CPSR (+T) | bfxil w0,w1,#0,#28 ; orr w0,w0,#0x20 | 0x33006c20 ; 0x323b0000 |
| exception entry CPSR | and w0,w0,#0xffffffc0 ; orr w0,w0,#0x80 | 0x123a6400 ; 0x32390000 |
| CLZ | clz | 0x5ac01000 |
| fast-path loads (memory emitter) | ldr / ldrh / ldrb / ldrsh / ldrsb w0,[x10,w0,uxtw] | 0xb8604940 / 0x78604940 / 0x38604940 / 0x78e04940 / 0x38e04940 |
| (none) | br x0 | 0xd61f0000 is **never emitted by the C translator**; register-indirect jumps exist only in the asm stubs (block_indirect_branch etc.) |

---------------------------------------------------------------------------------------------------------------
## 9. Notable findings / potential bugs (summary)
1. An unpaired Thumb BL suffix computes its target from the LR it has just overwritten (sec. 4) [L-bug].
2. Exit types 1/2/4 drop pending cycles; `n == 0x1000` emits `sub #0` (sec. 5) [C].
3. "Translation stack overflow!" is only a warning; translation runs on past the end of the stack (sec. 1.2) [C].
4. Known-condition folding ignores in-block forward branch targets (sec. 3) [G].
5. The cp15 main ID register reads 0x41009561 instead of the ARM946E-S value 0x41059461 [C value].
6. cpu_translate_conditional_skip, cpu_translate_link_stub, cpu_translate_allocate_pc_metadata and
   cpu_translate_log_current_arm_instruction have no direct callers (inlined copies / debug) [C].
7. Every direct-branch target is translated eagerly and recursively (no depth limit) via
   cpu_block_lookup_recursive [C].
8. Exception entry (SWI/UND/BKPT) saves stale NZCV into the SPSR (sec. 6) [C].
