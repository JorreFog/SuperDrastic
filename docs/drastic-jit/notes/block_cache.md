# DraStic JIT: block cache, lookup, SMC invalidation, translation-cache memory

> **Errata (from the consistency review in review.md, checked against the binary; already folded into the text below):**
> - The ARM9 cycle adjustment is at sys+0x3b2a988, not 0x362e988.
> - The ITCM literal-folding gate is the reverse of what an earlier revision said: only words with a modification
>   count above 7 get folded (a likely DraStic bug).
> - Translation state +0x478 is the CPU type byte (copied from cpu+0x210c, 1 = ARM9), so literal folding is ARM9-only.
> - Instruction record +0x04 is never overwritten.
> - ARM7 main-RAM coarse and fine code bitmaps are swapped (0x1b284).

Scope: cpu_block_* (0x29f10-0x2f840), translation_cache_* / initialize/reset_translation_cache (0x968a0-0x96984),
asm store handlers that call cpu_block_check_allocation*, DMA code-modification path, literal folding hook.
All addresses are file VAs.
Confidence tags: [C] confirmed from code, [L] likely, [G] guess.

---------------------------------------------------------------------------------------------------------
## 0. Key objects and who owns them

| thing | where | evidence |
|---|---|---|
| ARM9 cpu struct | nds_system+0x15c7d50 (id `[cpu+0x210c]`=1) | initialize_system 0xf8cc..0xf8fc calls initialize_cpu(ns+0x15c7d50, ns, 1, ns+0x25ce340) [C] |
| ARM7 cpu struct | nds_system+0x25ce340 (id 0) | same, second call with type 0 [C] |
| `T` = translation base | `[cpu+0x2088]` = nds_system+0x8c000 (file VA 0x488000, .bss), **shared by both CPUs** | initialize_cpu (0x28804/0x28824) stores ns+0x8c000 at cpu+0x2088 in both calls; initialize_translation_cache(ns+0x8c000) at 0xf914 [C] |
| T in JIT code | x11 (reloaded from x9-0x70 = `[cpu+0x2360]`) | block_itcm_branch_* add the table offset to x11; recompiler_entry_direct 0x8ccb4 [C] |
| other CPU | `[cpu+0x22a0]` | cpu_block_flush uses it to flush the partner's front cache/ITCM [C] |
| memory map | `cpu+0x23d0` (= x9 in JIT): 2 KB-page table, 2^21 entries x 8 B (16 MB) | arm64_store_* index it with addr>>11 scaled by 8 [C] |
| region descriptors | `[cpu+0x23d0+0x1000000]`, 0x60 B per 8 MB region (index addr>>23); +0x00 u32 mask, +0x48 fn get_coarse_bitmap(M,desc,addr), +0x50 fn get_fine_bitmap(M,desc,addr) | cpu_block_fill_allocation 0x2a234-0x2a268 [C] |
| memory struct M | `[cpu+0x23d0+0x1000008]` (== `[cpu+0x2260]`) | fill_allocation 0x2a1d0, reset_cpu_block uses cpu+0x2260 for same offsets [C] |

Page-table entry (8 bytes, used by all fast-path loads/stores) [C]:

| bits | meaning | evidence |
|---|---|---|
| 61..0 | (host_ptr - guest_page_addr) >> 2; host = (entry<<2) + guest_addr, the flags fall off in the shift | all load/store stubs |
| 62 | slow path (I/O, or page holds translated code) | tested first by every store handler |
| 63 | page holds translated code -> SMC check on store | tested e.g. in arm64_store_block1 at 0x8e364 |

`cpu_block_fill_allocation` sets bits 63|62 on every 2 KB page a new block covers (unless bit 62 already set) [C 0x2a1f8-0x2a204];
cpu_block_flush clears bits 63:62, but only on entries whose bit 63 is set (i.e. negative entries) [C 0x2f054-0x2f064].
`execute_cpu_load_pc_page_ptr(cpu,pc)` (0x27d30) is the interpreter's fetch helper on the same table: when the entry
has any of bits 61..0 set it returns entry<<2, otherwise it asks `extended_memory_get_page_ptr(cpu+0x23d0)` and
subtracts pc [C]. Not used by the JIT lookup.

---------------------------------------------------------------------------------------------------------
## 1. Translation-cache memory layout (relative to T)

From reset_translation_cache / translation_cache_flush_* (0x96950, 0x968a0..) and cpu_translate_block (0x95e20) [C]:

| T+off | size | content |
|---|---|---|
| 0x0000000 | 16 MB | **main cache** (guest 0x02xxxxxx): host code grows UP from T, block headers + PC-metadata/stubs grow DOWN from T+0x1000000 |
| 0x1000000 | 1 MB | **ITCM cache** (ARM9 only, guest pc < 0x2000000): code up from +0x1000000, meta down from +0x1100000 |
| 0x1100000 | 2 MB | **alternate cache** (everything else: ARM7 BIOS/WRAM, shared WRAM, ARM9 BIOS 0xffff0000, VRAM, GBA slot...): code up from +0x1100000, meta down from +0x1300000 |
| 0x1300000 | 0x80000 | main hash: 0x8000 buckets x 16 B, idx=(pc>>2)&0x7fff |
| 0x1380000 | 0x20000 | alternate hash: 0x2000 buckets x 16 B, idx=(pc>>2)&0x1fff |
| 0x13a0000 | 0x80000 | translation stack: branch records (0x28 B each) of the blocks being translated; bump pointer at T+0x14fa140, reset to T+0x13a0000 by every cpu_block_lookup_base miss; overflow at T+0x1420000 prints "Translation stack overflow!" and continues |
| 0x1420000 | 6 x u64 | cache pointers: +0x00 main code_ptr, +0x08 main meta_ptr, +0x10 itcm code, +0x18 itcm meta, +0x20 alt code, +0x28 alt meta |
| 0x1420030 | 0x8000 x 16 B | delayed-link list: {u64 host addr of B/BL to patch; u32 target pc; pad} |
| 0x14a0030 | u32 | delayed-link count (zeroed per lookup_base miss) |
| 0x14a0034 | 0x8000 x u32 | recursion stack: pc\|thumb of blocks currently being created |
| 0x14c0034 | u32 | recursion depth |
| 0x14c0038 | 0x8000 | ITCM ARM table: 0x2000 x u32 (T-relative entry offset), idx (pc>>2)&0x1fff (= `[ARM9+0x2270]`) |
| 0x14c8038 | 0x10000 | ITCM Thumb table: 0x4000 x u32, idx (pc>>1)&0x3fff (= `[ARM9+0x2278]`) |
| 0x14d8038 | 0x2000 | ITCM per-word modification counters (u8: bits0-6 saturating count<=15, bit7 "hot") |
| 0x14da038 | 16 x 16 B | ITCM "polymorphic word" cache {u32 value; u32 addr; u64 block_entry} |
| 0x14da138 | 0x20000 | main-RAM "word was modified after translation" bitmap (1 bit / 4 B, 4 MB) |
| 0x14fa138 | u32 | ITCM poly cache count (0..16) |
| 0x14fa140 | u64 | translation-stack pointer |
| 0x14fa148/150 | 2 x u64 | zeroed by initialize_translation_cache (unknown) |

Evidence for table/ITCM offsets: initialize_cpu 0x288a0-0x288bc (type 1 only: `[cpu+0x2270]` = ns+0x154c038 = T+0x14c0038,
`[cpu+0x2278]` = ns+0x1554038 = T+0x14c8038) [C]; reset_cpu_block memsets [C]; cpu_block_modify_itcm 0x2e238-0x2e2a4 [C].

Protection: initialize_translation_cache mprotect(T,16MB,RWX) + mprotect(T+16MB,1MB,RWX); reset_cpu_block re-does
both plus mprotect(T+0x1100000, 2MB, RWX) [C]. The whole thing lives inside nds_system .bss.

**No cache-full check exists.** The only readers of T+0x1420000 are lookup_base (icache maintenance), translate_block,
log_all and the flush/reset helpers; nothing compares code_ptr against meta_ptr [L: exhaustive search for every
use of the 0x1420000 offset]. Flushes are driven only by SMC/DMA/cheat/lua/savestate (section 4).
Main cache = 16 MB for 4 MB of main RAM, so in practice it is never filled before an SMC flush.

---------------------------------------------------------------------------------------------------------
## 2. Guest PC -> host code

### 2.1 Region classification (same in lookup_base, lookup_recursive, block_create, translate_block) [C]

| condition (checked in this order) | lookup structure | code cache |
|---|---|---|
| ARM9 (cpu type 1) and pc < 0x02000000 | ITCM tables (direct-mapped, no hash) | ITCM cache |
| pc>>24 == 0x02 | main hash (0x8000 buckets) | main cache |
| anything else | alternate hash (0x2000 buckets) | alternate cache |

(cpu_translate_block 0x95e20; cpu_block_lookup_base 0x2db38-0x2db50 / 0x2e094.)
The key is `pc | thumb` (bit 0 = Thumb). **There is no CPU id in the key**: ARM9 and ARM7 share T, both hash tables
and the main/alternate caches [C]. Separation relies on the two CPUs never executing the same address
(ARM7 BIOS at 0 vs ARM9 BIOS at 0xffff0000 hash to the same bucket but differ in key) [L]. If both CPUs ran code at
the same main-RAM address, one would execute the other's translation (different memory handlers/cycle tables) [G].
Only the ITCM tables and the L0 front cache are per-CPU.

### 2.2 Level 0: per-CPU direct-mapped front cache (cpu+0x80) [C]

| field | location | content |
|---|---|---|
| tag[0x400] | cpu+0x0080, u32 each | full pc\|thumb |
| off[0x400] | cpu+0x1080, u32 each | T-relative entry offset |
| index | | (pc >> 2) & 0x3ff (block_indirect_branch masks the PC with 0xffc to get idx*4 directly) |

Probed inline by `block_indirect_branch` (0x8cb3c); filled by cpu_block_lookup_base on every successful lookup;
cleared (0x2000 B, both CPUs) by every cpu_block_flush and reset_cpu_block; single tag entries cleared by
cpu_block_modify_itcm. lookup_recursive does NOT fill it.

### 2.3 Hash bucket (16 B) and chaining [C]

| offset | field |
|---|---|
| +0x0 | u32 pc of slot 0 |
| +0x4 | u32 offset of slot 0 (entry - T; 0 = empty) |
| +0x8 | u32 pc of slot 1 |
| +0xc | u32 offset of slot 1 |

Two inline slots. Overflow entries are chained through the *block headers*: the header of slot 1's block (found via the
u32 header offset stored 4 bytes before that block's entry) holds the first `next` link, and each further header links
to the next. Insertion appends the new block's header at the end of that chain (lookup_base 0x2e1bc-0x2e1e0,
lookup_recursive the same).

### 2.4 Block layout in the cache (written at the end of cpu_translate_block 0x95e20) [C]

Code area (grows up):

| location | content |
|---|---|
| entry-4 | u32 hdr_off (T-relative offset of the block header) |
| entry+0 | cycle check: if w12 >= 0 (bit 31 clear) continue at entry+8 (encoding 0x36f8004c) |
| entry+4 | call recompiler_cpu_next_action_arm9_to_arm7 (0x8cbb0) or ..._arm7_to_event_update (0x8cc40) |
| entry+8 | body; each instruction that writes PC is preceded by a cycle charge, a subtract-immediate on w12 (0x5100018c \| imm<<10) |
| tail | depends on the end reason (2.6) |

Meta area (grows down); header = 0x18 bytes at old_meta_ptr-0x18:

| offset | type | field |
|---|---|---|
| +0x00 | u32 | next: chain link (T-relative header offset), 0 = end |
| +0x04 | u32 | guest pc \| thumb |
| +0x08 | u32 | entry_off = entry - T |
| +0x0c | u16 | live_regs_in (liveness of r0-r15 at entry; consumed by blocks that link here) |
| +0x0e | u16 | live_flags_in (NZCV liveness at entry) |
| +0x10 | u16 | length in halfwords & 0xfff \| flags<<12; flags: bit12 Thumb, bit14 idle-loop [L], bit15 ARM9 [L] |
| +0x12 | u16 | host code size in bytes (entry..end) |
| +0x14 | u32 | pcmeta_off: T-relative offset of the PC-metadata table (n x u32 below the header, n = number of instructions with record flag byte +0x1a bit2; each entry = hostWordIdx<<16 \| (pc - block_pc)) |

cpu_block_load_pc (0x2e980): takes the saved x30 (= entry+8, the return address of the entry call), steps back 12 bytes
to entry-4, reads the header offset there, and copies the header's +0x04 pc into [cpu+0x23bc] [C].
cpu_translate_get_pc(host_addr, T) (0x96830): scans back for 0x36f8004c, then searches the PC-metadata
table for the exact host word index and returns header pc + low 16 bits of that entry [C].
cpu_block_log_from_hash reads a u64 exec counter at hdr+0x18 that is never written in this build (it overlaps the
previous block's meta) — profiling remnant [L].

### 2.5 cpu_block_lookup_base (0x2db10): how a PC is resolved
Input: cpu, pc (with Thumb in bit 0). Steps:

1. Compute the front-cache slot `(pc >> 2) & 0x3ff`; T comes from [cpu+0x2088].
2. **ITCM case** (ARM9 and pc < 0x02000000): read the Thumb table at index (pc>>1)&0x3fff if bit 0 is set, otherwise
   the ARM table at (pc>>2)&0x1fff. A nonzero offset is a hit.
3. **Hash case**: pick the bucket (main hash at T+0x1300000 for pc>>24 == 2, index (pc>>2)&0x7fff; otherwise the
   alternate hash at T+0x1380000, index (pc>>2)&0x1fff). Compare slot 0's pc, then slot 1's pc; if slot 1 is in use,
   walk the header chain from slot 1's block and compare each header's pc. A match gives the entry offset (hit).
4. **Hit**: write pc and offset into the front-cache slot and return T + offset.
5. **Miss - start a translation session**: remember the six cache pointers (T+0x1420000..+0x28); reset the
   translation-stack pointer to T+0x13a0000; zero the delayed-link count and the recursion depth.
6. Call `cpu_block_create(cpu, pc & ~1, pc & 1)` (create re-derives the Thumb bit from its third argument).
7. If an entry came back, insert offset = entry - T:
   * ITCM: store it in the Thumb or ARM table. For ARM state, if that word's modification counter is above 8,
     set the counter's bit 7 ("hot").
   * Hash: use slot 0 if empty, else slot 1 if empty, else append the new block's header to slot 1's chain.
8. Call `cpu_translate_link_delayed_targets(cpu)`, which patches the imm26 field of the delayed B/BL links.
9. For each of the three caches, do cache maintenance (DC CIVAC, DSB, IC IVAU, DSB, ISB) over the code range that
   grew, [old, new) of the code pointer, and the meta range [new, old) of the meta pointer. Line sizes come from
   CTR_EL0 and are cached in .data at 0x160210.
10. Write pc and offset into the front-cache slot and return the entry.

cpu_block_lookup_recursive (0x2bd50) is the same without the L0 cache fill, without resetting the per-session state
and without cache maintenance; it is only called from cpu_block_create to translate link targets [C].
Callers of lookup_base: block_indirect_branch (miss path), recompiler_entry_direct, all SMC store handlers,
cpu_alerts_dma_modified_code*, arm64_spsr_restore, arm64_update_coprocessor_control, cpu_block_flush (re-resolve),
cpu_block_modify_itcm, link_delayed_targets, reset_system/system_load_savestate (vector blocks), lua/cheat writers [C].

Indirect dispatch stubs (asm, 0x8cb0c-0x8cba8) [C]:

| stub | probe | hit | miss |
|---|---|---|---|
| block_itcm_branch_arm | ITCM ARM table at (w0>>2)&0x1fff | nonzero offset: jump to T+offset | common slow path |
| block_itcm_branch_thumb | ITCM Thumb table at (w0>>1)&0x3fff | nonzero offset: jump to T+offset | common slow path |
| block_indirect_branch | front-cache tag at (w0>>2)&0x3ff equals w0 | jump to T+off[idx] | common slow path |

Common slow path: spill w12-w18 and NZCV, call cpu_block_lookup_base(cpu, w0), reload, jump to the result.

### 2.6 Block creation (cpu_block_create 0x2bf70, size 0x1ba0)
1. Region gate [C]: unless (ARM9 && pc >= 0xffff0000) or pc < 0x2000000, calls region desc+0x48
   (coarse-bitmap getter); NULL -> return 0 (no code allowed in that region).
2. Decode pass [C]: the records array starts as a 0x200-byte malloc (16 records), is doubled with realloc when full,
   and is shrunk to n*0x20 at the end. ARM: fetched with load_memory32, record pc value = pc+8; Thumb: fetched with
   load_memory16 and converted by convert_thumb_instruction_to_arm, record pc value = pc+4 (word-aligned for
   PC-relative forms). Thumb BL/BLX halves are fused: the first half becomes a NOP (0xe1a00000), the second an ARM
   BL (0xeb......) or BLX (0xfa000000) with the combined offset. Condition codes equal to the condition of the
   preceding run of same-condition instructions ("IT-like" run) are folded.
3. Per-instruction record, 0x20 bytes [C for layout, L for some semantics]:

| offset | type | field |
|---|---|---|
| +0x00 | u32 | arm_opcode (Thumb already converted) |
| +0x04 | u32 | pc_value (pc+8 ARM / pc+4 Thumb); never overwritten |
| +0x08 | u64 | host_code_ptr (filled in by cpu_translate_block; separate from +0x04) |
| +0x10 | u16 | internal_branch_target_index (forward branch inside block) |
| +0x12 | u16 | regs_read mask (bit15 = PC) |
| +0x14 | u16 | regs_written mask (bit15 set => instruction writes PC -> emits cycle charge) |
| +0x16 | u16 | regs_live_after (backward liveness; 0x7fff = all live; at block end/unknown exits = 0x7fff) |
| +0x18 | u8 | low nibble flags_read, high nibble flags_written (NZCV) |
| +0x19 | u8 | flags_live_after (0xf = all) |
| +0x1a | u8 | bit1 = target of an internal branch; bit2 = needs a PC-metadata slot (may leave the block mid-way: memory/MSR ops etc.); bit2 also excludes idle-loop detection |
| +0x1b | u8 | extra cycle cost (ARM7: cycles += 2*x; ARM9: cycles += x + the adjustment at sys+0x3b2a988) |
| +0x1c | u8 | "ends block" (instruction cannot be continued past, e.g. undefined/system ops) [L] |

4. Block end conditions (state byte +0x2a = end reason, consumed by cpu_translate_block) [C]:
   - 0: unconditional instruction that writes PC (cond >= 0xe and regs_written bit15) — the instruction emits the exit.
   - 1: 0x800 instructions (max block = 8 KB ARM / 4 KB Thumb) -> tail loads next_pc|T into w0 and branches to
     block_indirect_branch.
   - 2: record +0x1c set; 4 consecutive zero opcodes (backs off 2 instructions); ARM7 branch target in
     0x00800000-0x01ffffff -> tail via block_indirect_branch.
   - 3: loop split: if some backward branch targets a point inside the block (not its start), the block is truncated
     at the lowest such loop head and ends with a direct link (fallthrough) to the loop-head block. This is the only
     exit type that charges the pending cycles at the end; types 1, 2 and 4 drop them.
   - 4: (ARM9, ARM state, ITCM) the next word's modification counter >= 9 ("hot") -> tail loads next_pc into w0 and
     branches to block_itcm_branch_arm.
5. Branch records (0x28 B, on the translation stack at T+0x13a0000) [C]:

| offset | type | field |
|---|---|---|
| +0x00 | u64 | target entry (resolved) |
| +0x08 | u64 | host address of the emitted B |
| +0x10 | u64 | delayed-list entry |
| +0x18 | u32 | target pc\|thumb |
| +0x1c | u32 | source pc |
| +0x20 | u32 | flags (below) |
| +0x24 | u16 | instruction index |
| +0x26 | u16 | internal target index |

   | flag bit | meaning |
   |---|---|
   | 0 | jump to target+8 (skip the entry cycle check; used for the loop-split fallthrough) |
   | 1 | target is in ITCM and the source is not (or the target word is hot) -> via block_itcm_branch_{arm,thumb} |
   | 2 | source and target are in different cache classes and one is "alternate" -> via block_indirect_branch |

   (cpu_block_create; cpu_translate_direct_link 0x90220.) Rationale [L]: caches flush independently, so
   direct links are only allowed where the target cannot be flushed without the source: main<->main,
   ITCM->main/ITCM (and a main flush also flushes ITCM, see 4), alt->alt.
6. Liveness [C]: a backward pass in which a register is live before an instruction if it is read by it, or live after
   it and not written by it (for conditional instructions the reads are simply added). Exits are seeded with the
   target block's header live_regs_in/live_flags_in (or 0x7fff/0xf when unknown/indirect).
7. Idle-loop detection [L]: if the first branch record targets the block start, no instruction in the loop has
   record flag +0x1a bit2, and no register that is read is written later in the loop, header flag bit14 (state+0x29
   bit2) is set. cpu_translate_direct_link then places a "w12 = -1" instruction (0x1280000c) right before that
   backward branch (the first branch entry, exit type 0), so the loop burns the rest of the timeslice and exits to
   the event loop at the entry check.
8. `cpu_block_fill_allocation(cpu, pc, byte_len)` marks code presence (section 3.1).
9. Push pc|thumb on the recursion stack. For every branch record with flag bits 1-2 clear whose target is outside the
   block (target <= start, >= end, or backward): if the target is on the recursion stack, make a delayed-link entry
   (patched by cpu_translate_link_delayed_targets after the outermost create returns); otherwise call
   `cpu_block_lookup_recursive(cpu, target)` — i.e. **eager depth-first translation of all statically reachable
   direct-branch targets** in linkable regions; a failure also becomes a delayed entry. Forward targets inside the
   block become internal branches (record +0x10, target record +0x1a |= 2). Pop the recursion stack.
10. `cpu_translate_block(&state, cpu, pc, thumb)` emits code and the header and bumps the cache pointers.
   No back-patching of older blocks ever happens (only same-session delayed links) [L].

---------------------------------------------------------------------------------------------------------
## 3. Code-modification (SMC) detection

### 3.1 Allocation bitmaps (in memory struct M, per code-capable region) [C]
Each region has a coarse bitmap (1 bit per 2 KB, u32 per 64 KB) and a fine bitmap (1 bit per 2 bytes, u32 per 64 B),
returned by region desc callbacks +0x48 / +0x50. Known instances (offsets in M; from cpu_block_flush/reset_cpu_block/fill_allocation):

| region | coarse | fine | fine size -> coverage |
|---|---|---|---|
| ITCM (ARM9, pc < [M+0xfd4d8]) | M+0xaf178 (1 word) | M+0xef1b8 | 0x800 -> 32 KB |
| main RAM | M+0xaf070 (64 words) | M+0xaf1b0 | 0x40000 -> 4 MB |
| 32 KB region (shared WRAM [L]) | M+0xaf17c | M+0xef9b8 | 0x800 -> 32 KB |
| 64 KB region (ARM7 WRAM [L]) | M+0xaf180 | M+0xf01b8 | 0x1000 -> 64 KB |
| VRAM [L] | M+0xaf184.. (11 words) | M+0xf11b8 | 0xa400 -> 656 KB |
| optional big region (GBA slot ROM? [G]) | ptr [M+0xaf170] | ptr [M+0xef1b0] | 0x200000 -> 32 MB, only if [M+0xfba38]/[M+0xef1b0] != 0 |

**ARM7 main-RAM bitmaps are swapped** [C for the swap, L for consequences]: initialize_memory_map_arm7 (0x1b284) stores,
for regions 4/5 (0x02000000-0x02ffffff), the main *fine* bitmap M+0xaf1b0 as the coarse base (desc+0x38) and the main
*coarse* bitmap M+0xaf070 as the fine base (desc+0x40); ARM9 has them the right way round. For ARM7 code in main RAM
(commercial ARM7 binaries usually load at 0x02380000) fill/check_allocation then touch bits up to 256 KB past the
coarse array, overlapping the ITCM/WRAM/VRAM coarse words and the main fine bitmap shifted by 0x140 B. ARM7's own checks
use the same getter, so ARM7 self-modification is mostly still detected, but the bits seen by ARM9 stores, DMA
(memory_check_code_region) and cpu_block_flush's clearing loops are wrong.

cpu_block_fill_allocation(cpu, addr, len) (0x2a190): sets bits 63|62 on the covered 2 KB page-table entries of the calling CPU's
map (skipped if bit 62 already set), then (addr < 0x10000000 only) ORs the coarse and fine bit ranges; splits recursively at region
mask boundaries. Also called by cpu_translate_memory_op for PC-relative literal loads (3.5).
Note [C/L]: the page bits are set only in the *translating* CPU's page table (cpu+0x23d0), while the bitmaps are
shared. So a store by the other CPU (e.g. ARM7 writing into main-RAM code translated for ARM9) takes the fast path
and is not detected, unless that page was already code-flagged in its own map; DMA writes are always checked.

### 3.2 Store fast path -> check (asm, e.g. arm64_store_memory32_arm9 0x8d610) [C]
1. Read the page entry for addr>>11; host delta = entry<<2.
2. Bit 62 clear: store directly and return (fast path).
3. Bit 62 set, bit 63 clear: I/O or extended store (see asm_runtime.md section 4).
4. Bit 63 set: compare the old word with the new value; equal -> return (same-value writes get no SMC handling).
5. Otherwise store, spill, record the address at [cpu+0x2334], and ask cpu_block_check_allocation32(cpu, addr).
   A zero answer -> return.
6. Recover the resume PC of the current block: cpu_translate_get_pc(saved x30, T) -> [cpu+0x23bc].
7. cpu_block_flush(cpu, addr).
8. Jump to cpu_block_lookup_base(cpu, [cpu+0x23bc]) after reloading registers.

Same for 8/16 (arm9/arm7) [C for 16 by symbol pattern]. STM: arm64_store_block1..16 (0x8e33c..) test only the
first address's page, write all words (no same-value test), call cpu_block_check_allocation_blockN, then the same
flush+relookup [C].

### 3.3 cpu_block_check_allocation* [C]
`check_allocation{8,16,32}(cpu, addr)`; the bit mask tested is 1 bit for 8/16-bit stores and 2 adjacent bits for
32-bit stores, at bit position (addr>>1)&31 of the relevant fine-bitmap word.

| address | fine word used | bit set -> result | bit clear |
|---|---|---|---|
| < 0x02000000 (ITCM; no cpu-id check) | ITCM fine table, word (addr>>6)&0x1ff | return cpu_block_modify_itcm(cpu, addr & ~3) | return 0 |
| 0x02000000-0x02ffffff (main RAM) | region fine getter (M, desc, addr & ~1) | also set the "modified after translation" bit for this word in T+0x14da138 (word (addr>>7)&0x7fff, bit (addr>>2)&31), then return 1 | return 0 |
| other | region fine getter | return 1 | return 0 (also when the getter returns NULL) |

`check_allocation_blockN(cpu, addr)` (N = 1..16) tests the fine bits of the whole range [addr, addr+4N-1] (at most 2
fine words), using the ITCM fine table below 0x2000000 and the region getter otherwise. It has no ITCM poly path.
Granularity: 2 bytes (fine bitmap); 8/16-bit stores test one bit, 32-bit stores two bits.

### 3.4 ITCM special handling: cpu_block_modify_itcm (0x2e220) [C]
Input: cpu, word-aligned ITCM address. Uses the per-word counter byte at T+0x14d8038 + ((addr>>2)&0x1fff).

1. Compute the incremented count (low 7 bits + 1, saturating at 15).
2. **Normal word** (bit 7 clear): store the new count and return 1, so the caller flushes the ITCM cache.
3. **Hot word** (bit 7 set): up to 16 translations are kept, keyed by (new word value, address). The value just
   stored is read back from ITCM data (`[M+8]`).
4. Search the poly cache (count at T+0x14fa138, entries at T+0x14da038) for a matching (value, address). On a match:
   point the ITCM ARM table entry at that entry's block, clear the front-cache tag for this PC, return 0 (no flush).
5. Poly cache full (16 entries): store the new count (bit 7 kept) and return 1 -> flush.
6. Otherwise: add (value, address) as a new entry, clear the ITCM ARM table entry and the front-cache tag, translate
   the new variant immediately with cpu_block_lookup_base(cpu, addr), store the result as the entry's block, bump the
   count and return 0 (no flush).

A word becomes hot when a block is created at it (ARM state) while its counter > 8 (lookup_base/recursive set
bit 7). Hot words: blocks stop before them (end reason 4), and branches to them go through block_itcm_branch
(flag bit1), so only the ITCM table entry needs swapping. ITCM flush clears bit 7 but keeps the counts, and resets
poly_count to 0 [C cpu_block_flush 0x2ef48..].

### 3.5 Literal-pool folding hook (cpu_translate_memory_op, VA 0x90dd0-0x91598) [C]
For `LDR Rd,[PC,#imm]` (P=1, W=0, base PC, literal addr < 0x4000000, aligned) when the translation-state byte +0x478
is 1 — that byte is the CPU type copied from cpu+0x210c, so folding is **ARM9-only** — the translator reads the
literal at translate time, emits it as an immediate (cpu_translate_set_imm32), and calls
`cpu_block_fill_allocation(cpu, lit_addr, 1/2/4)` so a later write to the literal triggers the SMC flush.
Gates:
* **ITCM**: a word whose modification counter is <= 7 is *not* folded; the translator emits a run-time load of the
  literal through the fastmem base x10 instead (0x90e54, 0x9153c..). Only counters > 7 fold (unless the modified bit
  below is set). This is the opposite of what makes sense — almost certainly an inverted compare (DraStic bug [L]): a
  folded hot ITCM word is handled by cpu_block_modify_itcm's poly cache, which only swaps the table entry of the block
  *starting* at that word, not the block holding the folded constant. Also, the modified bitmap is indexed with the
  main-RAM layout ((addr>>7)&0x7fff) and is never set for ITCM writes, so for ITCM it tests an aliased main-RAM bit
  [C code, L impact].
* **addr <= 0x2ffffff**: folding is skipped if the word's bit is set in the modified bitmap T+0x14da138 (i.e. this
  literal was written after an earlier translation). That bitmap is set only by check_allocation8/16/32 and cleared
  only by reset_cpu_block [C].

### 3.6 DMA-modified code [C]
dma_transfer (VA 0x1f5c0) calls `memory_check_code_region(coarse, fine, offset, len)` (0x13370) for the destination
before copying; that function returns nonzero if any coarse bit (2 KB) in range is set, refined with fine bits only
when the range stays within one 2 KB chunk. If set: alert value 0x01 is ORed into [cpu+0x22a8] and it logs "DMA modified
code" with the two addresses. The next handle_cpu_alerts (0x8dca4; four-way branch table on alerts&3: 0 none,
1 dma_modified_code, 2 check_interrupts, 3 both) runs cpu_alerts_dma_modified_code (0x8dd58), which recovers the PC
into [cpu+0x23bc] with cpu_translate_get_pc(x30, T), calls cpu_block_flush(cpu, 0xffffffff), and jumps to
cpu_block_lookup_base(cpu, pc) — DMA into code always flushes **all** caches.

---------------------------------------------------------------------------------------------------------
## 4. Invalidation = whole-cache flush (cpu_block_flush 0x2e9a0) [C]

There is no per-block invalidation. A hit flushes the entire cache class of the written address. Sequence:

1. Call get_ticks_us (timing only; results discarded). Find the other CPU via [cpu+0x22a0].
2. Zero both CPUs' L0 front caches (0x2000 B each).
3. **Alternate class** — taken when addr>>24 != 2 and not (ARM9 and addr <= 0x02000000):
   a. zero the alternate hash (0x20000 B at T+0x1380000);
   b. clear page bits 63:62 for both CPUs (summary bitmap words from addr 0x3000000, 104 x 2 MB) (*);
   c. clear the coarse+fine bitmaps of the shared-WRAM / ARM7-WRAM / VRAM / (GBA) regions (only words flagged in
      the coarse bitmap);
   d. translation_cache_flush_alternate(T): alt code pointer = T+0x1100000, meta pointer = T+0x1300000;
   e. if the other CPU is suspended in alternate code: reload its PC (load_pc) and set its resume address to
      lookup_base(other, pc)+8;
   f. re-translate the vector blocks — ARM7 at 0x8/0x18, ARM9 at base+8/base+0x18 (base = the vector base at
      CP15 state +0x10, via [ARM9+0x2250]) — and store them at cpu+0x2288 (SWI) / cpu+0x2280 (IRQ, used by
      cpu_alerts_check_interrupts);
   g. unless addr == 0xffffffff ("flush everything"), stop here.
4. **ITCM** (always part of a main flush), applied to the ARM9 struct (the caller if it is ARM9, else the other CPU):
   zero the ITCM fine bitmap (M+0xef1b8, 0x800 B) and the ITCM ARM table; clear bit 7 of every ITCM counter (counts
   kept); zero the ITCM Thumb table (0x10000 B); poly_count = 0; translation_cache_flush_itcm(T) (ITCM code pointer =
   T+0x1000000, meta = T+0x1100000). If the caller is ARM9 and addr < 0x02000000 (ITCM-only write), stop here.
5. **Main**:
   a. zero the main hash (0x80000 B at T+0x1300000);
   b. clear page bits 63:62 (ARM9: addresses 0..0x3000000; ARM7: summary from 0x2000000, 8 x 2 MB) (*);
   c. clear the main-RAM coarse/fine bitmaps (flagged words) and the ITCM coarse/fine bitmaps;
   d. translation_cache_flush_main(T): main code pointer = T, meta = T+0x1000000;
   e. if the other CPU is suspended in main or ITCM code: re-resolve its PC and resume address (as in 3e).

Callers: all 6 arm64_store_memory{8,16,32}_{arm9,arm7}, arm64_store_block1..16, cpu_alerts_dma_modified_code(*),
arm64_update_coprocessor_control (ITCM/DTCM remap), process_cheat, lua_set_ds_memory_*, system_load_savestate [C].
The caller re-translates its own resume PC with lookup_base afterwards.

(*) Page-bit clearing uses the "directly mapped page" bitmaps of the memory map (fine: map+0x1000018, 1 bit/2 KB
page, u32 per 64 KB; summary: map+0x1004018, 1 bit per fine word, u32 per 2 MB; set by map_memory_page,
cleared by unmap_memory_page_region_direct 0x13b70). In the ARM9 loop the summary start (word 0) matches the page/
fine start (address 0). In the ARM7-main loop (summary from word 16 = 0x2000000) and the alternate loop (summary
from word 24 = 0x3000000) the page and fine pointers still start at address 0 (0x2ea60-0x2eabc, 0x2eda8-0x2edf4).
That looks like a base mismatch (likely a DraStic bug) [L]: the code bits get cleared on the wrong pages
(0..16 MB / 0..0xd000000 masked by bits of other ranges), and ARM7's main-RAM page bits are never cleared.
Possible consequences [G]: stale bit 63 only costs slow-path stores; but a wrongly *cleared* bit 63 on a main-RAM
code page would let stores skip SMC detection.

Over-clearing: reset_cpu_block memsets 0x100000 at T+0x1300000 and 0x40000 at T+0x1380000, and cpu_block_log_all
walks 0x10000/0x4000 buckets — twice the sizes the lookup masks use (0x8000/0x2000) [C]. Harmless; suggests the
hash sizes were halved at some point [G].

reset_cpu_block (0x2f5f0, from reset_system) clears: both hash tables, ITCM tables+counters, both L0 caches, all
coarse/fine bitmaps, poly count, the modified bitmap, and re-mprotects the three caches; reset_translation_cache
then resets the six pointers [C].

---------------------------------------------------------------------------------------------------------
## 5. Tunables, limits, profiling

| item | value | where |
|---|---|---|
| main / ITCM / alternate cache size | 16 MB / 1 MB / 2 MB (code+meta share each) | translation_cache_flush_* [C] |
| hash buckets main / alt | 0x8000 / 0x2000, 2 inline slots + header chain | lookup_base [C] |
| L0 front cache | 0x400 entries per CPU | block_indirect_branch [C] |
| ITCM tables | 0x2000 ARM + 0x4000 Thumb entries | lookup_base [C] |
| max insns per block | 0x800 | cpu_block_create instruction-count limit [C] |
| block header | 0x18 B + 4 B per PC-metadata slot | translate_block [C] |
| translation stack | 0x80000 B (0x28 B/branch record), overflow only warns | cpu_block_create [C] |
| delayed links / recursion stack | 0x8000 entries each, no bound check seen | [L] |
| ITCM hot threshold | counter > 8 (mark), >= 9 (end block), > 7 (literal folding *allowed* — inverted gate, 3.5); counter saturates at 15 | [C] |
| ITCM poly cache | 16 (value,addr) variants total | modify_itcm [C] |
| zero-word stop | 4 consecutive 0x00000000 opcodes | cpu_block_create [C] |
| cache-full flush | none found | [L] |

Profiling/logging: cpu_block_log_all(ns, filename) (0x2f350, called from step_debug and update_input, i.e. a
debug hotkey) walks both hash tables via cpu_block_log_from_hash, collects per block the exec count (hdr+0x18),
exec count x instruction count, instruction count, host bytes and entry, sorts the blocks by exec count in
descending order (qsort with cpu_block_execution_compare_function), and prints a summary line (number of blocks and
bytes), one line per block with those statistics plus cpu_translate_log_block, and finally a listing of the
secondary translation instructions (stubs) via cpu_translate_log_translated_code over the main meta area [C]. The
exec counter is not maintained in this build (no increment emitted; hdr+0x18 is the next header) [L]. Globals
block_lookup_cycles (0x169054) and block_lookup_depth (0x169058) have no references at all (dead) [C].
cpu_block_flush calls get_ticks_us three times but discards the results [C].
