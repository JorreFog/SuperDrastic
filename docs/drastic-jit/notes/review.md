# Consistency review of the JIT notes + open questions + interpreter DSP bugs

Reviewer scope: translator_core.md, emitters.md, block_cache.md, asm_runtime.md, startup.md, dynamic.md,
dynamic_examples.md. Static analysis only (disassembly and decompilation of the binary, plus live REA sessions for
initialize_memory_map_arm9/arm7, memory_setup_slot2_ram and map_cartridge_region_slot2_ram). Addresses are file VAs
and point at the code that supports each statement. Tags: **[C]** confirmed in code, **[L]** likely, **[G]** guess.

---------------------------------------------------------------------------------------------------
## 1. Contradictions between the notes, resolved against the binary

| # | topic | what the notes say | what the binary says (evidence) | wrong in |
|---|---|---|---|---|
| 1 | translation-state `ts+0x000..0x3ff` | translator_core 1.4: "unused/legacy padding, no access below 0x400". emitters 0.2: "+0x000 scratch buffer for out-of-line slow paths" | **emitters is right.** cpu_translate_memory_op builds the load slow-path stub in ts+0 and then copies it out: around 0x90c44-0x90c68 it reads the stub pointer ts+0x410, lowers it by the stub size and stores it back; the loop at 0x90cc8 copies the stub in 16-byte chunks to the new location, relocating B/BL instructions (opcode bits 30:26 = 0b00101); at 0x90d98 the saved main emit pointer (ts+0x418) is written back to ts+0x408 [C] | translator_core |
| 2 | `ts+0x410` | translator_core: "metadata cursor (new metadata top)", initialised to mtop-0x18-4*n_meta. emitters: "stub area pointer, grows down" | Both describe the same pointer. It is initialised to mtop-0x18-4*n (stored at 0x95f64; ts lives at sp+0x80) and is then **lowered by every slow-path stub**; at block end the cache's meta pointer is taken from it (read at 0x96290 and stored into the cache record's +0x20/+0x28 pair; for ITCM, 0x9619c copies ts+0x408/0x410 as one 16-byte unit). So per block the meta area holds the trailer, then the PC metadata, then the slow-path stubs. This is why the stub branches in dynamic_examples go +16 MB (the T+0x1000000 region) [C] | translator_core (pseudocode incomplete) |
| 3 | `ts+0x478` | translator_core/emitters: "cpu id, 1 = ARM9". block_cache 3.5: "translator flag [state+0x478]" (unknown) | CPU id: loaded from cpu+0x210c at 0x95e78 and stored as a byte to ts+0x478 at 0x95eec. See 2(b) [C] | block_cache (vague) |
| 4 | ARM9 per-insn cycle adjust | translator_core: sys+0x3b28000+0x2988 = **sys+0x3b2a988**. asm_runtime/block_cache: sys+0x362e988 | At 0x96044 the translator builds the constant 0x3b28000, adds it to the system pointer read from cpu+0x2258 and reads the word at +0x2988, i.e. **sys+0x3b2a988** [C]. The 0x362e988 value is an uncorrected decompiler artefact (0x3b2a988 - 0x4fc000) | asm_runtime 3 "Interleaving", block_cache 2.6 item 3 |
| 5 | trailer +0x10 low 12 bits | translator_core/block_cache: size in halfwords. dynamic.md: "insn count" | Halfwords: 0x96170 reads the u16 at blk+0x22, and cpu_block_create stores there the instruction count (15 bits) shifted left by 1 for ARM blocks and the plain count for Thumb blocks [C] | dynamic.md |
| 6 | exit type 4 | translator_core: "ITCM counter >= 9 -> block = 1 insn". block_cache: "the next word's counter >= 9 -> tail" | block_cache is right. In cpu_block_create the PC variable is advanced by 4 as each instruction is fetched, and the "counter < 9" test (counter index (pc>>2)&0x1fff, ARM state and ARM9 only) is applied to that already advanced PC. The block therefore stops *before* a hot word, so a hot word always starts its own block (needed for the poly cache) [C] | translator_core 6 table |
| 7 | CDP (class 7, bit4=0) | translator_core pseudocode: bit 4 clear -> raise UND. emitters 4.4: "CDP -> no-op on ARM9" | emitters is right: in cpu_translate_instruction the class-7 path goes to the MCR/MRC handling only when bit 4 is set; with bit 4 clear and CP15 present nothing is emitted; on ARM7 (no CP15, cpu+0x2250 == 0) it raises UND (0x93ad4) [C] | translator_core 7 |
| 8 | literal folding gate in ITCM | block_cache 3.5: "folding is skipped if (ITCM) the word's counter > 7" | **Reversed.** At 0x90e54 the translator reads the word's ITCM counter byte (T+0x14d8038+idx) and compares it with 7; a counter <= 7 branches to 0x9153c, which emits a run-time load (constant address in w1, load from [x10, w1, UXTW]); a counter > 7 folds the value unless the word's bit in the T+0x14da138 bitmap is set (0x90e60-0x90e84). So in ITCM only *frequently modified* words get folded. This is almost certainly an inverted compare (DraStic bug [L]): a folded hot ITCM word is handled by cpu_block_modify_itcm's poly cache, which only swaps the ITCM table entry of the block *starting* at that word, not the block holding the folded constant. Also, the "modified" bitmap is indexed with (addr>>7)&0x7fff (main-RAM layout) and is never set for ITCM writes, so for ITCM it tests an aliased main-RAM bit [C code, L impact] | block_cache 3.5 |
| 9 | x10 (fastmem) use | asm_runtime 1: "translator emits `[x10,...]` only for SP-relative ARM9 accesses when cp15+0x3c" | Every ARM9 load gets the inline sequence "AND with 0xfc00000{3,1,0}, CBZ, branch to slow stub, load from [x10, addr, UXTW]" (emitters 2.2, dynamic ex.3); LDM forms its pointer as x10 + UXTW(w0); literal loads use [x10, w1] [C] | asm_runtime |
| 10 | cp15 byte +0x3c | asm_runtime: "fast SP access option byte" | update_coprocessor_dtcm_control (0x29630) sets it to (DTCM base + DTCM size < 0x4000000) and logs "DTCM in mapped memory status changed"; its return value says whether it changed, and on change the stub flushes everything [C] | asm_runtime |
| 11 | "Stores are never inlined" | dynamic.md; dynamic_examples 3 | ARM9 SP-relative stores with cp15+0x3c set are inlined as one STR/STRB/STRH to [x10, wA, UXTW] (memory_op path at 0x90a80; encodings 0xb8204940 / 0x38204940 / 0x78204940). dynamic_examples ex.5 itself shows `str w27,[x10,w26,uxtw]` generated for `str lr,[sp,#-4]!`. These stores bypass the bit62/63 checks and SMC detection [C] | dynamic.md, dynamic_examples 3 |
| 12 | x16/x17 "scratch" | dynamic.md, dynamic_examples header | x16/x17 are **guest r3/r4** (w13+3, w13+4). In the quoted SMLALBB r3,r4 and SMLABB ...,r3 snippets they are RdLo/RdHi/accumulator. The real scratch registers are w0..w4 (+x3 in SMLAWy) [C] | dynamic.md, dynamic_examples |
| 13 | "not-yet-translated target goes through a stub" (ex.5 `b 0x7f6190000000`) | dynamic.md / dynamic_examples 5 | It is an **unpatched delayed link**: the `bne` at 0x0200018c targets its own block (still being translated -> delayed record). direct_link emits a `B` with target 0, so imm26 encodes minus the branch's own address (divided by 4); truncated to 26 bits this lands on the next 256 MB boundary (0x7f618a72842c -> exactly 0x7f6190000000). cpu_translate_link_delayed_targets patches it after cpu_translate_block returns, i.e. after the hook had dumped the code [C] | dynamic_examples 5 |
| 14 | `b #-60` at end of block 0x02000180 "outside this block: stub/other code" | dynamic_examples 5 | Exit type 3 (loop split) fall-through link with flag 1: it jumps to entry **P+8** of block 0x02000188 (0x7f618a728418+8 = 0x7f618a728420) [C] | dynamic_examples 5 |
| 15 | idle-loop `movn w12,#0` | asm_runtime 6: "Type 3 = direct link ... or movn w12,#0 for idle self-loops" | Emitted by cpu_translate_direct_link for the **first branch entry** when bit 2 of blk+0x29 is set (tested at 0x90238), i.e. on the loop's own backward branch (exit type 0), not on the type-3 tail [C] | asm_runtime 6 |
| 16 | cycles "at block end" | asm_runtime 3 handoffs: "subtracted before flagged instructions and at block end" | Only exit type 3 flushes the pending cycles at the end (0x962a4-0x962f4). Types 1, 2 and 4 drop them (translator_core 5 is right) [C] | asm_runtime |
| 17 | mode index numbering | asm_runtime: "0 = user/sys?" / "6? x 8 bytes banked r13/r14". emitters 4.3 table only | See 2(d): 0 USR+SYS, 1 FIQ, 2 IRQ, 3 SVC, 4 ABT, 5 UND, 6 = invalid mode; 7 bank slots [C] | asm_runtime (incomplete) |
| 18 | arm64_msr_spsr guard | emitters 4.2: "skipped in USR/SYS". asm_runtime: "if CPSR & 0xf != 0 (not user mode)" | At 0x8f524 the stub masks the CPSR mode with 0xf and skips the write when the result is zero, i.e. only in USR (0x10). SYS (0x1f) writes SPSR slot 0 (the index shared by USR and SYS) [C] | emitters (now fixed there) |
| 19 | arm64_mrs_cpsr | asm_runtime: "w0 = CPSR[27:0] \| host NZCV" | The stub at 0x8f548 loads the CPSR word and inserts bits 27:0 of the NZCV register into it; since those bits of NZCV are zero, the result keeps CPSR[31:28] (stale flags) and **zeroes** bits 27:0. The operands are the reverse of the inline MRS. Dead code: the only reference is the debug table `disasm_labels` (0x167ca0) [C] | asm_runtime |
| 20 | FIQ bank area | asm_runtime: "0x20c8..0x20dc saved r8-r12 for FIQ" | execute_arm_set_cpu_mode (0x24958-0x24984): on entry to FIQ it saves r8-r11 at 0x20c8, r12/r13 at 0x20d8/0x20dc and r14 at 0x20e0; on leaving FIQ it restores all of r8-r14 from there (and does *not* load the target mode's banked r13/r14). FIQ banking is approximate [C] | asm_runtime |
| 21 | SPSR flags on SWI/UND/BKPT | emitters 4.3: "SPSR built from in-memory CPSR without live NZCV [likely]" | Confirmed: arm64_set_cpu_mode (0x8f7f4) saves NZCV only to 0x23b8 and never merges it into 0x23c0; the code generated by raise_exception then copies the word at x28+0x23c0 into the SPSR. The SPSR receives stale N,Z,C,V, and a handler returning with MOVS pc,lr / LDM^ restores stale flags. IRQ entry is fine (recompiler_entry_direct at 0x8ccc4 and cpu_alerts merge NZCV first) [C] | (upgrade likely -> confirmed) |
| 22 | record +0x04 | block_cache 2.6: "+0x04 pc_value -- later overwritten (+0x08 u64 host ptr)" | Not overwritten. +0x04 (u32 PC read value) and +0x08 (u64 host pointer, written by a 64-bit store at 0x96080) are separate fields [C] | block_cache |
| 23 | IO context vs memory struct | asm_runtime: `[cpu+0x2260]` = "IO context (sys+0x35d4930)". block_cache: `[cpu+0x2260]` = memory struct M (bitmaps, region tables) | Same pointer, two names. The struct passed to load/store_io_register_* is DraStic's memory struct (RAM pointers at +0x00/+0x08/+0x10/+0x18, bitmaps from +0xaf070, region tables at +0xfba98 (ARM9) / +0xfc698 (ARM7), CP15 at +0xfd518) [C for offsets, L for "same object" naming] | naming only |
| 24 | MSR CPSR_c field handling | emitters 4.2: "field c -> bl arm64_msr_cpsr_low (stores whole w0)" | Confirmed, with an unnoted consequence: the stub stores the whole operand into 0x23c0 (0x8f440) without applying the f/s/x masks, so `msr cpsr_c,Rm` also writes Rm bits 31:8 and can set/clear Q (bit 27) and the stale-flag image [C] | (addition, now in emitters) |
| 25 | dynamic: "SMC granularity unknown" | dynamic.md | Resolved by block_cache 4 (consistent with the observation): a hit flushes the whole class: main hash + main cache + **ITCM cache**; for an address that is neither main RAM nor ITCM only the alternate cache. No per-block invalidation [C] | dynamic.md (open item) |
| 26 | asm_runtime "x30 save slot 0x2330" vs translator_core "[x9,#-0xa0] temp" | both | Same word (0x23d0-0xa0 = 0x2330). Users: unaligned-load32 path (x30), SWP alias temp, STRD address across the first store call, LDM^ saved mode (+0x2334 = base). No conflicting overlap found (in SWP the temp is written after any load; block stubs use 0x2334/0x2338/0x2340 but not 0x2330) [C] | (reconciled) |

Smaller notes:
* translator_core 7 pseudocode, class 6: for cond < 0xE with CP15 present the translator still emits the inverted
  condition skip (patched to skip zero instructions); harmless.
* dynamic_examples 1: "+2 for the branch" is really the initial pending count of an exit-type-0 block (ARM9 2 /
  ARM7 4, set at 0x95ef4-0x95f08). It is added at block start, whatever kind of branch ends the block.
* translator_core 1.2/§6 "cond 0xF is unconditional (BLX imm / PLD)": this holds for the other classes too: cond 0xF
  ALU/LDM/STM/MCR are translated as unconditional (no UND); only PLD-space memory ops are dropped (cpu_translate_memory_op
  at 0x90a00 returns immediately when cond is 0xF).

---------------------------------------------------------------------------------------------------
## 1b. Spot-checks of key "confirmed" claims (my own verification)

| claim | where claimed | verdict / evidence |
|---|---|---|
| guest rN = host w(13+N), r15 never resident | all | OK. All emitters derive the host register as guest + 13; arm64_set_cpu_mode spills w13..w27 to x9-0x60..x9-0x28 (0x8f7f8-0x8f814) |
| x9 = x28+0x23d0, x10 = map base, x11 = T | all | OK. recompiler_entry sets them up at 0x8f9d0-0x8f9dc; every stub recomputes x9 = x28+0x23d0 and reloads the x10/x11 pair from x9-0x78 |
| page-table entry: (host-guest)>>2, bit62 slow, bit63 code | asm_runtime 4, block_cache 0 | OK. fill_allocation (0x2a1f8) ORs bits 63:62 into the entry and skips the store if bit 62 is already set; entry i lives at cpu+0x23d0+8*i |
| block header layout (trailer) | translator_core 1.5, block_cache 2.4, asm_runtime 6 | OK (0x96148-0x96190). Header word = trailer - T; trailer: +0 zero, +4 pc\|T, +8 entry - T, +0xc u16 copy of blk+0x26, +0xe u16 copy of blk+0x28, +0x10 (blk+0x22 & 0xfff) \| flags<<12, +0x12 code bytes, +0x14 ts+0x428 - T |
| entry marker 0x36f8004c + BL next_action | all | OK (0x95f70-0x95fdc); the BL target comes from GOT slot 0x15ff20 (ARM9) / 0x15ff98 (ARM7) |
| front cache index `(pc>>2)&0x3ff`, tag incl. Thumb bit | block_cache 2.2, asm_runtime 6 | OK. At 0x8cb3c the lookup masks the PC with 0xffc to form the table offset, loads tag and entry, and compares the tag with the full PC value (so the Thumb bit is part of the tag) |
| ITCM tables `(pc>>2)&0x1fff` / `(pc>>1)&0x3fff` | block_cache, asm_runtime | OK. 0x8cb10 extracts PC bits 14:2, 0x8cb28 extracts bits 14:1 |
| exit types 0-4 and their tails | translator_core 6, block_cache 2.6 | OK (except #6 above). Switch at 0x960d8: type 3 -> cycle subtract + direct_link; 1/2 -> set_imm32(w0, next\|T) + B through GOT 0x15ff58; 4 -> set_imm32(w0, next) + B through GOT 0x15fd98 |
| cycle accounting: ARM9 cost+adjust, ARM7 cost*2, sub before PC-writers, n==0x1000 -> sub #0 | translator_core 5 | OK (0x96020-0x960d4: amounts up to 0x1000 are emitted as a single SUB immediate whose imm12 field is filled from the low 12 bits of the amount) |
| per-insn cost rules (LDM/STM popcount, 1->2, +1 LDM pc, load-use +1, only when cond=AL) | translator_core 1.1 | OK (in cpu_block_create). Independent check: dynamic ex.4 `sub w12,#44` = 9+1+(3+1 interlock r0)+2+(2+1: LDMIB writeback base r0 counts as "loaded")+10+3+(9+1)+2 initial. No note had explained this 44 |
| flush scopes | block_cache 4 | OK. At 0x2ea08: address bits 31:24 == 2 -> main path; CPU id 1 and address <= 0x2000000 -> ITCM path (0x02000000 itself has already taken the main branch) |
| alerts word 0x22a8 bits 0x10/0x04/0x03 | asm_runtime 5, block_cache 3.6 | OK. At 0x8dca4 the handler clears the word, then tests bit 4 (halt), then bit 2 (slice end), then dispatches on bits 1:0 through the jump table at 0x8df90. **Addition:** when bit 0x10 is set the other bits are discarded (the word was already zeroed and the halt path never looks at them), so a simultaneous "DMA modified code" (bit 0) is lost and stale blocks can run after wake-up [C path, L impact] |
| SMC store stub same-value early-out, check_allocation, get_pc, flush, lookup, indirect re-entry | asm_runtime 4, block_cache 3.2 | OK (0x8d784-0x8d80c) |
| LDR pc / LDM {pc}: ARM7 gets the bit fix-up, ARM9 passes the raw value | emitters 0.4 | OK. memory_op at 0x9107c compares ts+0x478 with 1 and skips the fix-up when equal |
| MRS CPSR = MRS NZCV, load 0x23c0, BFXIL bits 27:0 (+ORR #0x20 in Thumb) | translator_core, emitters | OK (cpu_translate_mrs_op, 0x93240) |
| raise_exception args (UND 1,5 / SWI 2,3 / BKPT 3,4) | translator_core, emitters | OK. Calls at 0x93ad4 / 0x93c7c / 0x95414 (for BKPT the vector argument 3 is taken from opcode bits 6:5) |
| QADD UXTW / QDADD unused doubling / SMLAxy ASR #0 / SMLALxy immr | emitters 5 | OK. The emitter constants 0x8b204000, 0x0b000001, 0x13007c00 and 0x13017c00 / 0x13013c01 / 0x13117c01 are present in cpu_translate_instruction's multiply/DSP paths |
| CP15 main ID 0x41009561 | translator_core, emitters | OK (constant in cpu_translate_instruction) |

New issues found while spot-checking (not in any note):
* **`mov pc,pc` (0xe1a0f00f, also Thumb hi-reg `mov pc,pc`) emits no code.** The register-ALU path drops
  `MOV Rd,Rm, LSL #0` with S=0 and Rd == Rm as a NOP (check at 0x93c9c: opcode is MOV, Rd equals Rm, and shift type,
  amount and S are all zero). It compares the raw register numbers, so Rd=Rm=15 matches. cpu_block_create still marks
  the instruction as a PC writer, so when unconditional it is the last instruction (exit type 0) and **no exit is
  emitted**: host execution runs off the end of the block into the next block's header word or zeroed cache ->
  SIGILL [C for code, L for crash]. A conditional `movXX pc,pc` acts as a NOP instead of skipping the next
  instruction. Suggest a qemu test.
* **STRD: only one PC-metadata entry, after the second store call** (memory_op 0x90f74-0x90f88). If the *first* store
  triggers SMC or an IO alert, cpu_translate_get_pc (0x96830) finds no entry with that host index and keeps scanning
  forward past the trailer into older metadata (the scan has no bound; it stops only when an entry's high half matches)
  -> wrong resume PC or a hang [C code, L impact].
* **SWP with Rd==Rn or Rd==Rm**: the loaded value is parked at x9-0xa0 and copied into Rd only *after* the store call
  and its PC-metadata entry (in cpu_translate_instruction the reload into Rd is emitted after store_pc_metadata). If
  that store diverts (SMC/alert), Rd is never written [C code, L impact].
* **ARM7 main-RAM code bitmaps are swapped**: in initialize_memory_map_arm7 the pair store at 0x1b284 puts
  M+0xaf1b0 (the main *fine* bitmap) into desc+0x38 (coarse base) and M+0xaf070 (the main *coarse* bitmap) into
  desc+0x40 (fine base) for regions 4/5 (0x02000000-0x02ffffff). ARM9 has them the right way round
  (initialize_memory_map_arm9 sets coarse = M+0xaf070, fine = M+0xaf1b0). For ARM7 code in main RAM (commercial ARM7
  binaries usually load at 0x02380000), fill_allocation/check_allocation write and read bits at
  M+0xaf070 + (off>>6)*4, up to 256 KB past the coarse array, which overlaps the ITCM/WRAM/VRAM coarse words and the
  main fine bitmap shifted by 0x140 B. ARM7's own check uses the same getter, so ARM7 self-modification is mostly
  still detected, but the bits seen by ARM9 stores, DMA (memory_check_code_region) and cpu_block_flush's clearing
  loops are wrong [C for the swap, L for consequences].

---------------------------------------------------------------------------------------------------
## 2. Open questions

### (a) Which memory the VRAM and 32 MB bitmaps cover [C]
Region descriptor (0x60 B, one per 8 MB; ARM9 table at M+0xfba98, ARM7 table at M+0xfc698):

| offset | content |
|---|---|
| +0x00 | u32 address mask |
| +0x08 | fn get_page_pointer(M, addr) (load) |
| +0x20 | fn, store variant |
| +0x38 | coarse bitmap base |
| +0x40 | fine bitmap base |
| +0x48 | fn get_coarse(M, desc, addr) |
| +0x50 | fn get_fine(M, desc, addr) |
| +0x58 | u16 flags |

Getters (0x10750-0x10960): the `*_direct` variants index by guest address: coarse index = (addr & mask) >> 16, fine
index = (addr & mask) >> 6, entry address = base + 4*index. The `memory_vram_*` / `memory_wram_*` variants first get
the host pointer via desc->get_page_pointer(addr), return NULL if it is the dummy page, and otherwise index by the
offset of that host pointer from the backing store's start (same >>16 / >>6 split).

| bitmap | coarse / fine | covers | used by regions | evidence |
|---|---|---|---|---|
| VRAM | M+0xaf184 (11 u32) / M+0xf11b8 (0xa400 B) | the **656 KB VRAM backing store** (banks A..I laid out contiguously from `[M+0x15020]`: A +0, B +0x20000, C +0x40000, D +0x60000, E +0x80000, F +0x90000, G +0x94000, H +0x98000, I +0xa0000, end/dummy `[M+0x15068]` = +0xa4000), indexed by **physical bank offset**, not guest address; unmapped VRAM pages (pointer == dummy) return NULL = "no code allowed" | ARM9 regions 12-13 (0x06000000-0x06ffffff) and ARM7 regions 12-13, via memory_vram_arm{9,7}_get_page_pointer | initialize_memory (bank layout); getters at 0x107a0 / 0x10810 compare against `[M+0x15068]` and use `[M+0x15020]` as the base; the region 12-13 descriptors are set up in initialize_memory_map_arm9/arm7 |
| 32 MB | ptr `[M+0xaf170]` (malloc 0x800) / ptr `[M+0xef1b0]` (malloc 0x200000) | the **Slot-2 (GBA slot) RAM expansion**, a 32 MB malloc at `[M+0xfba38]`, mapped at guest **0x08000000-0x09ffffff** (4 x 8 MB regions, each with mask 0x7fffff, base +0/+8/+16/+24 MB, bitmap bases +0x200/+0x400/+0x600 coarse and +0x80000/+0x100000/+0x180000 fine) | ARM9 regions 16-19 (M+0xfc098..) and ARM7 regions 16-19 (M+0xfcc98..), `_direct` getters; present only after memory_setup_slot2_ram (otherwise those regions use memory_get_code_bitmap_null) | memory_setup_slot2_ram (0x1a450), map_cartridge_region_slot2_ram (0x1a330) |

For completeness: main RAM coarse M+0xaf070 / fine M+0xaf1b0 (4 MB, mask 0x3fffff; the 0x02400000+ mirrors alias
it); shared WRAM coarse M+0xaf17c / fine M+0xef9b8 (32 KB, `memory_wram_*`, relative to `[M+0x10]`, regions 6-7 on
ARM9, region 6 on ARM7); ARM7 WRAM coarse M+0xaf180 / fine M+0xf01b8 (64 KB, mask 0xffff, `_direct`, ARM7 region 7);
ITCM coarse M+0xaf178 / fine M+0xef1b8 (used directly for addr < 0x02000000; the region 0-3 descriptors are `_null`).
`[M+0x00]` = main RAM, `[M+0x08]` = ITCM data, `[M+0x10]` = shared WRAM, `[M+0x18]` = 16 KB (DTCM), all inside the
0x414000-byte shm file.

### (b) Translator flag at ts+0x478 (gates literal-pool folding) [C]
It is the CPU type byte: cpu_translate_block reads the CPU id from cpu+0x210c (0x95e78; 1 = ARM9, 0 = ARM7) and
stores it as a byte at ts+0x478 (0x95eec; ts = sp+0x80). Nothing else writes it. Readers: memory_op (literal folding
at 0x90de4, the SP fast path at 0x90a80 which requires the value 1, the LDR pc fix-up at 0x91084, the arm9/arm7
store-handler choice), block_memory_op (inline LDM check) and the SWP store-handler choice. So literal folding,
inline fastmem loads and the SP-relative path exist **only for ARM9**; ARM7 always calls arm64_load_memory* (and
ARM7 literal loads are real loads). ts+0x479 is the Thumb flag (stored at 0x95ef0).

### (c) Exact PC resumed after a store diverts the block [C]
* cpu_translate_store_pc_metadata(ts, pc) (0x8fa20) is always called with ts+0x448 = **address of the next guest
  instruction** (cpu_translate_block sets it to pc + 2 for Thumb or pc + 4 for ARM *before* calling
  cpu_translate_instruction, 0x95ff0-0x96004), right after the `BL`. The key is (emit - entry)/4, the host index
  of the return address.
* cpu_translate_get_pc(ret, T) (0x96830) scans backwards to the 0x36f8004c marker, reads trailer+0x14, finds the
  entry whose high half equals (ret - entry)/4, and returns trailer.pc (including the Thumb bit) + the entry's low
  half.
* The store stub writes memory **before** checking (at 0x8d798 the store happens, then check_allocation32 is called),
  and the translator emits the base writeback before the call. So execution continues at **the next instruction;
  the store is not re-executed**, the rest of the old block is abandoned, and that block's pending (unsubtracted)
  cycles are lost. dynamic.md's observation (resume block 0x020001d4 = the instruction after the store at
  0x020001d0) matches.
* The other metadata users follow the same rule: arm64_msr_cpsr_low (MSR CPSR_c), arm64_update_coprocessor_control /
  _dtcm_control, store_block*, IO alerts (handle_cpu_alerts -> halt/IRQ/DMA flush): all resume at the next
  instruction.
* Exceptions to "it works": STRD's first store and SWP with aliasing (see 1b "new issues").

### (d) Mode index numbering [C]
`arm_get_cpu_mode_from_cpsr` (0x248d0) and execute_arm_set_cpu_mode_from_cpsr (0x249b0) map CPSR & 0x1f through the
16-entry table at 0x11d900 = {0,1,2,3,6,6,6,4,6,6,6,5,6,6,6,0} (for modes 0x10..0x1f); invalid modes map to 6.

| index | mode | CPSR bits (cpu_modes_cpsr 0x11d940) | banked r13/r14 | SPSR slot |
|---|---|---|---|---|
| 0 | USR **and SYS** | 0x10 (entry 6 = 0x1f is only used to build a CPSR) | 0x2090/0x2094 | 0x20e8 (exists, written by MSR SPSR in SYS) |
| 1 | FIQ | 0x11 | 0x2098 | 0x20ec |
| 2 | IRQ | 0x12 | 0x20a0 | 0x20f0 |
| 3 | SVC | 0x13 | 0x20a8 | 0x20f4 |
| 4 | ABT | 0x17 | 0x20b0 | 0x20f8 |
| 5 | UND | 0x1b | 0x20b8 | 0x20fc |
| 6 | invalid mode (or SYS when passed as raise_exception's "mode" argument -> 0x1f) | 0x1f | 0x20c0 | 0x2100 |

Current index at cpu+0x2104. FIQ r8-r14 snapshot at 0x20c8-0x20e3 (see table 1 #20). arm64_set_cpu_mode(w1 =
index), raise_exception(vec, index) and LDM^ (index 0 = USR bank) all use this numbering.

### (e) Alert bit 0x08 [C]
Set only by the interpreter's CPSR writers (execute_arm_set_cpsr 0x2577c, execute_arm_spsr_restore 0x258ec,
execute_arm_msr_op 0x2654c) when the new CPSR has I=0 while an IRQ is pending (cpu+0x2108 != 0). The same function
then **enters the IRQ exception inline** (0x257bc-0x25808): LR_irq = pc+4, SPSR_irq (0x20f0) = old CPSR (with T),
mode index 2, PC = vector base + 0x18, CPSR = (CPSR with bits 5:0 cleared) | 0x92. Bit 0x08 tells `_execute_cpu`
that the PC was redirected inside the instruction: at 0x27f00 it zeroes its accumulated-cycle register (w20) when
the bit is set, before re-syncing the budget. The JIT never sets it (its MSR path goes through arm64_msr_cpsr_low ->
cpu_alerts_check_interrupts instead), and handle_cpu_alerts ignores it (it only tests 0x10, 0x04 and 0x03); it is
simply cleared with the word. Meaning: "IRQ taken by a CPSR write (interpreter only)".

---------------------------------------------------------------------------------------------------
## 4. Interpreter QDADD / SMLALxy root causes [C]

### execute_arm_saturating_alu_op (0x26df0)
Operands: Rm = opcode bits 3:0, Rn = opcode bits 19:16; bit 22 selects the doubling forms, bit 21 subtraction.
How the second operand ("addend") is formed:

| case | addend used | Q | correct behaviour |
|---|---|---|---|
| bit 22 clear (QADD/QSUB) | Rn, sign-extended | only from the final clamp | same (correct) |
| QDADD/QDSUB, Rn < -2^30 (0x26e64 / 0x26ebc) | -0x40000000 | set | -0x80000000, Q set |
| QDADD/QDSUB, -2^30 <= Rn < 0 (0x26e70) | 2*Rn | not set here | same (correct) |
| QDADD/QDSUB, Rn >= 0 (0x26e14-0x26e28) | -1 (0xffffffff) | **always set** | 2*Rn, or 0x7fffffff with Q only if Rn >= 2^30 |

The sign test is at 0x26e10. The result is then Rm (sign-extended) plus or minus the addend, clamped to
[INT_MIN, INT_MAX] with Q set on clamping.

Root cause: the doubling-saturation path for non-negative Rn has no overflow comparison and uses the constant -1
instead of 0x7fffffff, so **every QDADD/QDSUB with Rn >= 0 sets Q and uses -1 in place of 2*Rn**
(`qdadd 1,0x10` -> 0 Q=1; `qdsub 1,0x10` -> 2; `qdadd 0x12345678,0x01000000` -> 0x12345677, exactly the dynamic
results). The negative side is right except on overflow: Rn < -2^30 saturates the doubled value to -0x40000000
instead of -0x80000000 (**predicted, not yet tested**: `qdadd 0,0x80000000` should give 0x80000000 Q=1; the
interpreter will give 0xc0000000 Q=1).

### execute_arm_multiply_short_long_op = SMLALxy (0x26300)
What the interpreter computes:
1. First factor: the full 32-bit Rm (opcode bits 3:0); the x bit (bit 5) is never consulted.
2. Second factor: the top or bottom half of Rs as selected by the y bit (bit 6); this part is correct.
3. Both factors are then divided by 2 as signed values (0x26330-0x26344).
4. They are multiplied with a 32-bit multiply (0x26350), so the product is truncated to 32 bits.
5. The sign-extended product is added to the 64-bit RdHi:RdLo (0x26354-0x26364).

Root causes: (1) Rm's half is not selected (x ignored), (2) both factors are halved, (3) the product is truncated to
32 bits before the 64-bit accumulate. The JIT has the same half/halving error, encoded differently: it emits ASR #1
of Rm (SBFM with immr=1) and SBFX Rs, #1, #15 / ASR Rs, #17, i.e. the intended SXTH / ASR #16 encodings with immr off
by one (+0x10000), but it multiplies with a full 32x32->64 SMADDL. That is why dynamic testing saw identical low
words (same small products) but different high words (the interpreter wraps the product at 32 bits). Both paths share
the decoding mistake, so the bug probably sits in a common source macro [G].
For contrast, the interpreter's SMLAxy/SMULxy (execute_arm_multiply_short_op, 0x261f0) select x via bit 5 and y via
bit 6 correctly, and SMLAWy/SMULWy (0x26280) are correct, which matches the dynamic results.

---------------------------------------------------------------------------------------------------
## Corrections other notes need (summary)
* translator_core: ts+0x000 is the slow-path stub buffer and ts+0x410 is lowered by stubs (#1, #2); exit type 4 stops
  *before* the next hot word (#6); CDP with CP15 = no code (#7); the r15 home guess cpu+0x23ac is interpreter-only
  (the JIT PC lives at 0x23bc).
* emitters: arm64_msr_spsr skips only USR, not SYS (#18); add the MSR CPSR_c whole-word store (#24); SPSR stale
  flags are confirmed (#21). (All three are now folded into emitters.md.)
* block_cache: cycle adjust at sys+0x3b2a988 (#4); ITCM literal folding condition is reversed (#8); ts+0x478 = CPU id
  (#3); record +0x04 not overwritten (#22); add the ARM7 main-RAM bitmap swap (1b).
* asm_runtime: cycle adjust offset (#4); x10 used by all ARM9 fastmem loads (#9); cp15+0x3c = DTCM-in-window flag
  (#10); mode numbering (#17, 2d); arm64_mrs_cpsr semantics (#19); FIQ bank layout (#20); block-end cycles (#16);
  idle MOVN (#15); alert 0x10 drops the other bits (1b); alert 0x08 meaning (2e).
* dynamic.md / dynamic_examples: trailer +0x10 = halfwords (#5); SP-relative stores are inlined (#11);
  x16/x17 = guest r3/r4 (#12); ex.5 delayed link and P+8 fall-through (#13, #14); SMC granularity = whole cache (#25).
