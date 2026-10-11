# DraStic JIT hand-written asm runtime (VA 0x8cb0c-0x8fa20) and CPU state struct

> **Errata (from the consistency review in review.md, checked against the binary; already folded into the text below):**
> - The ARM9 per-instruction cycle adjustment is read from sys+0x3b2a988.
> - x10 (fastmem base) is used by all ARM9 inline fastmem loads (and by inlined SP-relative stores), not only by SP-relative accesses.
> - cp15+0x3c is the "DTCM lies inside the mapped window" flag.
> - Mode index (cpu+0x2104): 0 = USR/SYS, 1 = FIQ, 2 = IRQ, 3 = SVC, 4 = ABT, 5 = UND, 6 = invalid (table at 0x11d900).
> - The FIQ bank holds r8-r14.
> - `arm64_mrs_cpsr` zeroes bits 27:0 and is dead code.
> - Pending cycles are charged at block end only for exit type 3.
> - The idle-loop "w12 = -1" instruction sits on the loop's own backward branch, not on the type-3 tail.
> - Alert bit 0x08 is interpreter-only: CPSR writers set it when unmasking a pending IRQ that they then enter inline.
> - (New in this revision) recompiler_entry's final branch targets 0x8cc78 itself, so execute_events also runs on the
>   very first pass; earlier text claimed it was skipped.

Sources: our disassembly of the JIT runtime region and of the C runtime, plus decompiler-assisted reading of
execute_events (0x1e8a0) and map_memory_page* (0x137b0/0x13820/0x13ad0). All addresses are file VAs.
Confidence tags: **[C]** confirmed (seen directly in code), **[L]** likely (strong inference), **[G]** guess.

---------------------------------------------------------------------------------------------------
## 0. TL;DR

* Two CPU state structs sit inside `nds_system` (VA 0x3fc000, size 0x3b2b000):
  **ARM9 = sys+0x15c7d50, ARM7 = sys+0x25ce340**, stride 0x10065f0 [C] (constants built in recompiler_entry /
  recompiler_entry_direct; cpu_next_action_arm9_to_arm7 calls `_execute_cpu` on sys+0x25ce340).
* Inside JIT code: **x28 = current CPU struct**, **x9 = x28+0x23d0 = that CPU's memory-map table** (and the
  base for spills with negative offsets), **w12 = cycle countdown**, **w13..w27 = guest r0..r14 (fixed
  mapping)**, **x10 = fastmem base ("memory map offset", 64 MB window)**, **x11 = translation cache base**,
  host NZCV = guest NZCV. x0-x4 = args/scratch for runtime calls, x30 = link. [C]
* The JIT never returns to C. `main` calls `recompiler_entry(sys, memory_map_offset)` once; control then
  cycles forever through ARM9 slice -> ARM7 slice -> `execute_events` -> ARM9 ... Leaving the loop (menu, load
  state) is a `longjmp` to main's `_setjmp(sys+0x3b2a840)`, after which main calls recompiler_entry again [C for
  the calls; L for "only exit is longjmp": `menu` 0x801ac calls `__longjmp_chk`, recompiler_entry saves no
  callee-saved registers and has no return path].
* Every block begins with a two-word cycle check: "if w12 bit 31 is clear, skip the next word", then a
  branch-with-link to `recompiler_cpu_next_action_<cpu>`. A negative w12 at block entry therefore hands off
  to the other CPU, and the link register then holds the resume address (block+8), which is saved at
  [cpu+0x2298] [C].
* Memory: one table per CPU at cpu+0x23d0, 2 M x 8-byte entries for 2 KB pages, entry = (host_page -
  guest_page)>>2 plus flag bits 62 (special/slow store) and 63 (page holds translated code -> SMC check) [C].

---------------------------------------------------------------------------------------------------
## 1. Host register convention inside JIT code

| host reg | role | evidence / conf |
|---|---|---|
| x28 | current CPU state struct (ARM9 or ARM7). On the ARM9->ARM7 handoff it is replaced by the other-CPU pointer read from [x28+0x22a0] | 0x8cbe0; [C] |
| x9  | `x28 + 0x23d0`, which is also &memory_map[0]; page-table entries are fetched by indexing x9 with the page number scaled by 8. Spill slots are at negative offsets from x9. Every stub recomputes it from x28 (adding 0x3d0, then 0x2000) after each C call | every stub; [C] |
| x10 | fastmem base = `memory_map_offset` (64 MB host window that mirrors DS physical 0-0x4000000). Saved at [cpu+0x2358]. Used by **all ARM9 inline fastmem loads** (the generated sequence masks the address, takes the slow path when it lies outside the window, otherwise loads from x10+addr), by LDM address formation and by run-time literal loads. SP-relative (Rn == r13) ARM9 **stores** are also inlined off x10 when the CP15 byte +0x3c is set (generated encodings in the 0xb8604940 load / 0xb8204940, 0x38204940, 0x78204940 store families); these inlined stores bypass the bit-62/63 checks and therefore SMC detection | recompiler_entry 0x8f9d0; cpu_translate_memory_op (0x90dd0..0x91598, SP-store path near 0x90a80); [C] |
| x11 | translation cache base = sys+0x8c000 (same value as [cpu+0x2088]). Block tables hold 32-bit offsets from it; dispatch adds the zero-extended offset to x11 and branches to the sum | 0x8f9d4-0x8f9dc, 0x8cb1c; initialize_cpu stores sys+0x8c000 at cpu+0x2088; [C] |
| w12 | cycle counter for the current slice, counting down. The slice is over when bit 31 is set. Blocks charge cycles with a subtract-immediate on w12 (encodings 0x5100018c, or 0x5140018c with the immediate shifted by 12) | cpu_translate_block (0x95e20); [C] |
| w13..w27 | guest r0..r14, fixed mapping `host w(13+n) = guest rN`. Canonical home is [cpu+0x2370+4n] | the full-save stores r0/r1 at x9-0x60 (= cpu+0x2370) up to r14 at x9-0x28 (= cpu+0x23a8); the interpreter uses 0x23a4 = r13 and 0x23a8 = r14; [C] |
| guest r15 | not in a register. The translator materialises constants. The "current PC" is recovered from the return address through PC metadata (`cpu_translate_get_pc`) | [C] |
| NZCV | guest N,Z,C,V are held in the host flags. Runtime fast paths never use flag-setting compares: equality is tested by subtracting/OR-ing into a scratch register and using compare-and-branch-on-zero or test-bit-and-branch. Around C calls the flags are saved/restored via MRS/MSR at [cpu+0x2354] | [C] |
| x0..x4 | arguments and results of runtime stubs: address in w0, store value in w1, load result in w0 (64-bit load: w0 and w1). Stubs clobber x0-x4 (block stubs also clobber x0-x3) | [C] |
| x5..x8 | not touched by any memory fast path. Free temporaries for the translator | [L] |
| x18 | used as guest r5 (no platform use on Linux). It is caller-saved, so it goes to its own slot [cpu+0x2308] | [C] |
| x19..x27 | guest r6..r14. Callee-saved under AAPCS64, so the stubs do **not** spill them around C calls (only around a full state save) | [C] |
| x29 | never touched by JIT or stubs. It still holds main's frame pointer | [C] (no writes in 0x8cb0c-0x8fa20) |
| x30 | return address into JIT code. Stubs that call C save it at [cpu+0x2368] (or 0x2330/0x2338 for nested calls) | [C] |
| sp | main's stack. JIT and stubs never adjust sp; C helpers run on it directly | [C] |

### Saving state around C calls (two levels) [C]

| level | used by | what is written where | restore |
|---|---|---|---|
| **Light save** (caller-saved only) | every slow path that calls C | w12..w17 -> 0x22f0..0x2304 (as pairs, from x9-0xe0); w18 -> 0x2308; x30 -> 0x2368; host NZCV -> 0x2354 | recompute x9 from x28; reload x10/x11 from 0x2358/0x2360; reload w12..w18, x30 and NZCV. w19..w27 survive because C must preserve them |
| **Full save** (canonical guest state) | CPU switch, mode change, IRQ entry, SPSR restore, halt | host NZCV -> 0x23b8; w13..w27 -> 0x2370..0x23a8; w12 -> 0x2290 | the reverse, done by whoever resumes the CPU |

Entering C from JIT never touches callee-saved state on the C side. recompiler_entry was called from `main`
and never returns, so main's x19-x30 are simply dead. [C]

---------------------------------------------------------------------------------------------------
## 2. CPU state struct (`cpu` = x28). nds_system relation

`nds_system` (VA 0x3fc000) contains both CPUs: **ARM9 at +0x15c7d50 and ARM7 at +0x25ce340** [C].
`initialize_cpu(cpu, sys, type, other)` (0x287a0) sets the cross pointers. The struct size is
0x10065f0 [L]. It is dominated by the 16 MB memory map at +0x23d0. The ARM9 ITCM block tables live
elsewhere in nds_system (sys+0x154c038 and sys+0x1554038) [C].

| offset | size | meaning | conf / evidence |
|---|---|---|---|
| 0x0000 | 4 x 0x20 | 4 hardware timers (+0 back-pointer to cpu, +0x10 prescaler 8/16/32/64?, +0x1d index). Each also has an event | initialize_cpu timer loop; reset_cpu writes +0x14/+0x1c/+0x1e per 0x20; [L] |
| 0x0080 | 1024 x u32 | **indirect-branch cache: tag** (guest PC, bit0 = thumb), index `(pc>>2)&0x3ff` | block_indirect_branch 0x8cb3c; cpu_block_lookup_base uses cpu+0x80 as the tag array; [C] |
| 0x1080 | 1024 x u32 | **indirect-branch cache: code offset** (from x11) | same; [C] |
| 0x2080 | ptr | pointer to the IO register backing store. ext16 loads read `[ptr + (addr&0x7fff)]` directly when `(addr & 0x7f00)==0` | 0x8d184, 0x8d208; also used in store_io_register_*; [L] |
| 0x2088 | ptr | translation cache base (= sys+0x8c000 = x11) | initialize_cpu; cpu_block_lookup_base reads it as the cache base; [C] |
| 0x2090 | 7 x 8 | banked r13/r14 per mode index (8 bytes per mode): idx 0 USR/SYS 0x2090, 1 FIQ 0x2098, 2 IRQ 0x20a0, 3 SVC 0x20a8, 4 ABT 0x20b0, 5 UND 0x20b8, 6 invalid 0x20c0 | _execute_cpu IRQ entry copies r13 (0x23a4) into slot 0x2090+mode*8; mode table 0x11d900; [C] |
| 0x20c8..0x20e3 | | FIQ snapshot of r8-r14: r8-r11 at 0x20c8, r12 at 0x20d8, r13 at 0x20dc, r14 at 0x20e0. Written on FIQ entry; on FIQ exit all of r8-r14 are reloaded from here (and the target mode's banked r13/r14 are *not* loaded), so FIQ banking is approximate | execute_arm_set_cpu_mode 0x24958-0x24984; [C] |
| 0x20e8 | 7 x u32 | **SPSR[mode index]** (0x20e8 USR/SYS ... 0x2100 invalid). msr_spsr / spsr_restore address it as x9-0x2e8 + idx*4 | 0x8f530, 0x8f584; [C] |
| 0x2104 | u32 | current mode index: 0 = USR and SYS, 1 = FIQ, 2 = IRQ, 3 = SVC, 4 = ABT, 5 = UND, 6 = invalid. Derived from `CPSR&0x1f` through the 16-entry table at 0x11d900 | 0x8f52c; _execute_cpu; arm_get_cpu_mode_from_cpsr 0x248d0; [C] |
| 0x2108 | u32 | **IRQ pending** (IE & IF, gated by IME). Nonzero => IRQ line asserted | store_io_register_* write it, then set alert bit 1 (0x111b4-0x111c4); [C] |
| 0x210c | u32 | **CPU type: 1 = ARM9**, 0 = ARM7. Selects ITCM path, cycle cost, next_action target | initialize_cpu stores its type argument here; cpu_translate_block picks 0x8cbb0 when ==1; [C] |
| 0x2110 | u32 | **halt state**: 0 running, 1 halted (HALTCNT halt / CP15 wait-for-IRQ), 2 = ARM7 sleep. When ARM7 sleeps, ARM9 gets `|= 2`, and it is cleared again (`-= 2`) when ARM7 wakes | execute_arm_halt_cpu_arm7 0x25548; sleep path 0x115b8-0x115e0 ("Entering sleep mode."); the JIT's CP15 halt sequence stores 1 here (generated encoding 0xb9211381); [C] |
| 0x2118 | ... | debugger state (step_debug(cpu+0x2118, pc, cycles)) | 0x8f980; reset_debug; [C] |
| 0x2220 | u64 | debug instruction counter (interpreter) | _execute_cpu; [L] |
| 0x2249 | u8 | debug flag (1 = no stepping) | _execute_cpu; [L] |
| 0x2250 | ptr | coprocessor (CP15) state. **NULL for ARM7**. +0x10 exception vector base, +0x14 control, +0x18 DTCM, +0x1c ITCM, **+0x3c "DTCM in mapped memory" flag** (true when DTCM base+size < 0x4000000; recomputed by update_coprocessor_dtcm_control 0x29630, whose "changed" return value makes the stub flush everything) | initialize_memory 0x1c2f8/0x1c300; 0x8f694-0x8f7c0; cpu_block_flush; [C] |
| 0x2258 | ptr | **nds_system pointer** (`sys`). sys+0x10 = current slice length | initialize_cpu stores sys here; 0x8cbe8; [C] |
| 0x2260 | ptr | IO context / memory struct M (sys+0x35d4930), first argument of load/store_io_register_*. Same object that block_cache.md calls M | initialize_cpu; 0x8d0d4; [C] |
| 0x2268 | ptr | geometry engine (sys+0x3a28bb0), argument of queue_geometry_command(_packed) | initialize_cpu; 0x8d668; [C] |
| 0x2270 | ptr | ARM9 ITCM block table, ARM state: 8192 x u32 code offsets, index `(pc>>2)&0x1fff` (sys+0x154c038) | 0x8cb0c; initialize_cpu (type 1 only); [C] |
| 0x2278 | ptr | ARM9 ITCM block table, Thumb: 16384 x u32, index `(pc>>1)&0x3fff` (sys+0x1554038) | 0x8cb24; [C] |
| 0x2280 | ptr | cached block for the **IRQ vector** (vector_base+0x18) | cpu_block_flush 0x2ec80-0x2ecb4; used after execute_arm_raise_interrupt; [C] |
| 0x2288 | ptr | cached block for the **SWI vector** (vector_base+0x08) | cpu_block_flush 0x2ec74; [C] |
| 0x2290 | s32 | **cycle counter when not running** (w12 is saved here). Negative = overshoot carried into the next slice. reset = -1 | every handoff; reset_cpu; [C] |
| 0x2298 | ptr | **resume address** in JIT code (block+8, just after the prologue's call). 0 = none (after halt) | 0x8cbb0, 0x8ccac; block_halt clears x30; [C] |
| 0x22a0 | ptr | **other CPU** state | initialize_cpu stores its fourth argument here; 0x8cbe0; [C] |
| 0x22a8 | u32 | **alerts** bitmask (see section 5) | handle_cpu_alerts 0x8dca4; reset_cpu; [C] |
| 0x22b0 | 16 x u32 | LDM/STM transfer buffer (x9-0x120) | arm64_*_block*; [C] |
| 0x22f0 | u32 | light-save slot for w12 (cycles). Alert bit 2 zeroes it | 0x8dd14; [C] |
| 0x22f4..0x2304 | 5 x u32 | light-save slots for w13..w17 (r0..r4) | [C] |
| 0x2308 | u32 | light-save slot for w18 (r5) | [C] |
| 0x2330 | u64 | x30 save for nested stub calls (unaligned load32 path) | 0x8d058; [C] |
| 0x2334 | u32 | scratch: faulting store address / second address of load64 / w19 save in block stubs | [C] |
| 0x2338 | u64/u32 | scratch: x30 (load64), w19 (unaligned load32), x20 (block stubs) | [C] (note: load64 and the unaligned load32 path share this slot) |
| 0x2340 | u32 | scratch: first word of load64, w21 save in block stubs | [C] |
| 0x2354 | u32 | NZCV save during C calls | [C] |
| 0x2358 | u64 | x10 = fastmem base (memory_map_offset) | recompiler_entry stores x10/x11 as a pair at x9-0x78; [C] |
| 0x2360 | u64 | x11 = translation cache base | same; [C] |
| 0x2368 | u64 | x30 save across C calls (JIT return address; used for PC recovery on SMC) | [C] |
| 0x2370..0x23a8 | 15 x u32 | **guest r0..r14** (canonical register file) | [C] |
| 0x23ac | u32 | r15 as read (pc+8), interpreter only | _execute_cpu stores pc+8 here; [C] |
| 0x23b0 | ptr | **next_action function pointer** (ARM9: recompiler_cpu_next_action_arm9_to_arm7; ARM7: ..._arm7_to_event_update; the C versions when interpreting) | block_halt 0x8f66c; the C cpu_next_action_* jump through it; [C] for use, [L] for values |
| 0x23b8 | u32 | NZCV image (bits 31:28) written by JIT full saves. To rebuild CPSR, CPSR bits 27:0 are inserted under this image | [C] |
| 0x23bc | u32 | **guest PC** (current/next), bit0 = Thumb (arm64_spsr_restore sets bit 0 when SPSR.T) | 0x8f5a8; cpu_block_lookup_base(cpu, pc) uses bit0 to pick the thumb table; [C]/[L] |
| 0x23c0 | u32 | **CPSR** (flag bits may be stale while in JIT; 0x23b8/host NZCV hold the real ones). reset = 0x1f | [C] |
| 0x23d0 | 2M x u64 | **memory map** (one entry per 2 KB guest page) = 16 MB, ends at 0x10023d0 | load/store stubs; map_memory_page*; [C] |
| 0x10023d0 | ptr | per-8MB region descriptor table (0x60 bytes each, +0x48/+0x50 = get_code_bitmap fns) | cpu_block_fill_allocation / check_allocation8; [L] |
| 0x10023d8 | ptr | memory subsystem struct (code bitmaps for ITCM etc.) | same; [L] |
| 0x10023e8 | u32[] | "mapped page" bitmap: word = addr>>16, bit = (addr>>11)&31 | map_memory_page_direct (+0x1000018 from map base); [C] |
| 0x10063e8 | u32[] | top-level bitmap: word = addr>>21, bit = (addr>>16)&31 | same; [C] |

nds_system fields seen from the runtime [C]: sys+0x08 u64 total elapsed cycles; **sys+0x10 u32
current slice length**; sys+0x18 scheduler (initialize_event base); **sys+0x318 event list head**
(event: +0 u32 delta, +8 fn, +0x10 arg, +0x18 next); sys+0x8c000 translation cache; sys+0x3b2a840
jmp_buf; sys+0x3b2a988 ARM9 per-instruction cycle adjustment; sys+0x3b2a9a8 use_recompiler byte (from main).

---------------------------------------------------------------------------------------------------
## 3. Entry from C and scheduling

### C side [C]
`main` (0xe3d0) calls `_setjmp(sys+0x3b2a840)`. If the use_recompiler byte at sys+0x3b2a9a8 is set, it logs the
memory map offset (read from sys+0x36d1e20) and the translation cache pointer (sys+0x8c000), then calls
`recompiler_entry(sys, memory_map_offset)` (call site 0xe500). Otherwise it calls the interpreter loop
`cpu_next_action_arm7_to_event_update(sys)` (0xe540). recompiler_entry (0x8f9cc) has exactly one caller, main.

### recompiler_entry (0x8f9cc): what it does
1. Keeps `sys` in x19 and puts the memory_map_offset argument in x10 (the fastmem base).
2. Builds the constant 0x8c000 and sets x11 = sys + 0x8c000 (translation cache base).
3. Points x28 at the ARM7 struct (sys+0x25ce340), derives x9 = x28+0x23d0, and stores x10/x11 into
   ARM7's 0x2358/0x2360 slots.
4. Does the same for the ARM9 struct (sys+0x15c7d50), leaving x28 = ARM9.
5. Jumps (0x8fa10) to recompiler_entry_direct at 0x8cc78, i.e. to its first step, so the first pass
   also runs execute_events [C].

There is no prologue and nothing is spilled: the function never returns.

### recompiler_entry_direct (0x8cc78): the "event update" half of the loop
Entered from recompiler_entry (first time) and afterwards by falling through from
`recompiler_cpu_next_action_arm7_to_event_update` with x19 = x0 = sys. Steps:

1. **Events.** Calls execute_events: global time sys+8 advances by the slice length sys+0x10 and every due
   event on the list at sys+0x318 fires.
2. **CPU pointers.** Sets x20 = ARM7 struct and x28 = ARM9 struct (0x8cc7c-0x8cc90).
3. **ARM9 interrupt / wake check** (0x8cc94-0x8ccfc):
   * If ARM9's IRQ-pending word (0x2108) is zero, go straight to the ARM7 check.
   * If CPSR.I (bit 7 of 0x23c0) is clear, the IRQ is delivered:
     - If a resume address exists in 0x2298, the PC is rebuilt from it: the word 12 bytes before the
       resume address is the block's descriptor offset (resume = P+8, so this is P-4); the descriptor is at
       cache base (from 0x2360) + that offset, and its +4 field is the block start PC, which is written to
       0x23bc. If there is no resume address (halted CPU), 0x23bc already holds the PC.
     - CPSR is rebuilt: top nibble from the NZCV image at 0x23b8, bits 27:0 kept.
     - execute_arm_raise_interrupt is called and the target block becomes the IRQ-vector block in 0x2280.
   * If CPSR.I is set: when the CPU is not halted (0x2110 == 0) nothing happens; when it is halted, it is
     woken at its PC: target block = cpu_block_lookup_base(cpu, [0x23bc]).
   * For either target: resume address 0x2298 = target + 8 and halt state 0x2110 = 0.
4. **ARM7 check** (0x8cd00-0x8cda8): the same logic on x20, plus a call to event_force_task_switch_function(sys)
   and the ARM9 "-= 2" bookkeeping when ARM7 leaves sleep.
5. **Start the ARM9 slice** (0x8cdac-0x8cdf8): the slice length is the delta field of the first event on the
   list at sys+0x318 and is stored to sys+0x10; w12 = ARM9's carried counter (0x2290) + slice; x30 = ARM9's
   resume address; x9/x10/x11, w13..w27 and NZCV (from 0x23b8) are restored.
6. **Decide** (0x8cdfc-0x8ce04): w12 still negative -> go directly to recompiler_cpu_next_action_arm9_to_arm7
   (ARM9 skips this slice); ARM9 halted -> its `_clear_cr` variant (w12 = -1, skip); otherwise jump to the
   resume address and run ARM9 JIT code.

The IRQ-delivery PC is the *block start* PC of the resume point. That is exact because resume points are
only ever block entries (after the cycle check).

### Handoffs [C]
* **Block prologue** (emitted by cpu_translate_block): at P-4 a u32 descriptor offset; at P the cycle check
  (skip the next word if w12 >= 0); at P+4 a call to recompiler_cpu_next_action_arm9_to_arm7 (ARM9) or
  ..._arm7_to_event_update (ARM7); the body starts at P+8. Lookups return P; direct links and indirect
  branches jump to P (or straight to P+8 when link flag bit0 is set), so the cycle check runs at every block
  entry. There is no cycle check inside a block. Cycles are charged (subtract-immediate on w12) before
  flagged instructions, and at block end **only for exit type 3**; exit types 1, 2 and 4 drop the pending
  cycles.
* `recompiler_cpu_next_action_arm9_to_arm7` (0x8cbb0): full save of ARM9 (x30 -> 0x2298, NZCV -> 0x23b8,
  r0-r14, w12 -> 0x2290); x28 becomes ARM7 (via 0x22a0); w12 = ARM7's 0x2290 + slice length (read through
  [0x2258]+0x10); x30 = ARM7's 0x2298; ARM7 registers and NZCV are restored. Then: w12 < 0 ->
  arm7_to_event_update; ARM7 halted -> `_clear_cr` (w12 = -1); otherwise jump to x30.
* `recompiler_cpu_next_action_arm7_to_event_update` (0x8cc40): full save of ARM7; x19 = x0 = sys; falls
  into recompiler_entry_direct (execute_events and the next slice).
* `_clear_cr` variants: set w12 = -1 first, so a halted CPU is charged the whole slice (0x2290 = -1).

### Interleaving model [C, units L]
* Lock-step by **event slices**: slice = delta to the next scheduler event (`*[sys+0x318]`), stored in
  sys+0x10. **ARM9 runs the whole slice, then ARM7 runs the same slice, then execute_events** advances
  global time (sys+8 += slice) and fires events. Per-CPU overshoot (negative w12) carries over through
  0x2290.
* Cycle units: ARM9 instruction cost = byte cost + the ARM9 per-instruction adjustment at sys+0x3b2a988;
  ARM7 cost = 2 x byte cost (cpu_translate_block). The interpreter does the same (doubles the cost unless
  type == 1). So the unit is ARM9 clock ticks (~67 MHz) [L].
* **Keep running or return**: control only changes CPU at a block entry with w12 < 0, or explicitly when
  (a) alert bit 2 zeroes the remaining budget, (b) block_halt / the CP15 halt sequence sets w12 = -1 and
  jumps to [0x23b0], or (c) a self-looping "idle loop" block (block flag byte 0x29 bit2) has its own backward
  branch preceded by an instruction that sets w12 = -1 (encoding 0x1280000c), which burns the rest of the
  slice; cpu_translate_direct_link (0x90220) emits it for the first branch entry when that flag is set, so it
  belongs to an exit-type-0 branch, not to the type-3 tail [C].

### block_halt (0x8f654) [C]
Input w0 = PC to resume at. It stores w0 to 0x23bc, rebuilds CPSR from the NZCV image, clears x30 (so no
resume address is recorded), sets w12 = -1 and jumps through the next_action pointer at [x28+0x23b0].
The translator emits it for CP15 c7,c0,4 / c7,c8,2 as: load the PC (with Thumb bit) into w0, store 1 to
cpu+0x2110, branch to block_halt. handle_cpu_alerts bit 4 also comes here, with w0 =
cpu_translate_get_pc(x30). On wake-up, recompiler_entry_direct sees 0x2298 == 0 and uses
cpu_block_lookup_base(cpu, [0x23bc]).

---------------------------------------------------------------------------------------------------
## 4. Memory handlers

### Memory map entry format (cpu+0x23d0, index = addr>>11) [C]
`entry = (host_ptr_of_page - guest_page_addr) >> 2` (arithmetic shift), plus:
* **bit 62**: "not plain RAM for stores": IO, or pages whose writes need C handling. Set by map_memory_page*
  when its "special" argument is nonzero. Pages with bit 62 and **without** bit 63 go to `extended_store*` in C.
* **bit 63** (always together with 62): "**page contains translated code**". Set by
  `cpu_block_fill_allocation` (both top bits, only if bit 62 was clear). Cleared by cpu_block_flush (both top
  bits masked off).
* entry == 0 after shifting left by 2 (which drops both flag bits) = unmapped: slow-path load.
* Host address = `(entry << 2) + addr` (the guest page base is folded into the entry), so the fast path is
  a single register-offset load with no masking.

A load never looks at the flags, because the left shift by 2 drops bits 62/63. A store tests bit 62 before
the shift.

### arm64_load_memory32_unsigned (0x8cfe0): what it does
1. Computes the page index addr>>11 (in a way that leaves NZCV alone).
2. If the low two address bits are nonzero, goes to the unaligned path.
3. Reads memory_map[page] and shifts it left by 2 (this strips the flags and yields host_page - guest_page).
   A zero result means "unmapped" -> slow path.
4. **Fast path:** loads the word at (shifted entry + addr) and returns it in w0. Six instructions in total,
   flags untouched.
5. **Slow path:** light save (and w12 also to 0x2290), call `extended_load32(memory_map, addr)`, restore, return
   the value in w0.
6. **Unaligned path** (ARM LDR rotate semantics): saves x30 at 0x2330, borrows w19 (guest r6) after saving it at
   0x2338, sets w19 = (addr & 3) * 8, calls the aligned entry recursively with addr & ~3, rotates the result
   right by w19, then restores w19 and x30.

The 8/16-bit variants have the same shape (byte/halfword, zero- or sign-extending). Their slow paths call
extended_load8/16 and re-extend the result. 16-bit forces `addr & ~1`.
`arm64_load_memory64` (LDRD) = two load32 calls, returning w0 = [a] and w1 = [a+4].

### "ext" load variants (arm64_load_ext*) [C]
Used by the translator for instructions it expects to touch IO (cpu_translate_memory_op).
If `addr>>24 != 4` they tail-branch to the normal arm64_load_memory*. For IO they call
**load_io_register_arm9_{8,16,32}(io_ctx=[x28+0x2260], addr & 0x7fffff)** directly. ext16 also has a
fast path: if `(addr & 0x7f00) == 0` (0x04000000-0x040000ff, mirrored) it reads the halfword at
`[x28+0x2080] + (addr & 0x7fff)`. Only ARM9 IO functions are referenced, so these variants are ARM9-only [L].

### arm64_store_memory32_arm9 (0x8d610): what it does
Inputs: w0 = address, w1 = value.

1. Computes the page index, forces the address to word alignment (STR semantics), reads the entry and keeps
   entry<<2 as the host delta.
2. **Bit 62 clear -> fast path:** store the word at host delta + addr and return (plain RAM, no code on page).
3. Bit 62 set and `addr>>24 == 4` -> **IO path** (0x8d62c-0x8d724):
   * the value becomes the third argument, the address is reduced to addr & 0xffffff, light save;
   * **geometry ports** 0x04000400-0x040005ff (offset - 0x400 <= 0x1fc): first argument = geometry engine
     [cpu+0x2268].
     - offsets 0x400-0x43f (GXFIFO): `queue_geometry_command_packed(geo, value)`, restore, and return
       **without** an alert check;
     - offsets 0x440-0x5ff (command ports): `queue_geometry_command(geo, (addr & 0xffffff) >> 2, value)`,
       restore, then the alert check;
   * **other IO**: w12 is stored to 0x2290, `store_io_register_arm9_32(io_ctx=[cpu+0x2260], addr & 0xffffff,
     value)` is called, and w12 is reloaded from 0x2290 - but that reload is dead, because the light restore
     that follows reloads w12 from 0x22f0;
   * **alert check:** after restoring, a nonzero alerts word [cpu+0x22a8] sends control to handle_cpu_alerts;
     otherwise return.
4. Bit 62 set, not IO, **bit 63 clear** -> special non-code page: light save, `extended_store32(memory_map,
   addr, value)`, restore, return (no alert poll).
5. Bit 63 set -> **SMC detection on a page holding translated code** (0x8d784-0x8d80c):
   * read the current word; if it equals the new value, return (same-value writes are no-ops);
   * perform the store, record the address at 0x2334, light save;
   * call `cpu_block_check_allocation32(cpu, addr)` (per-halfword code bitmap test); zero -> restore, return;
   * recover the guest PC: `cpu_translate_get_pc([cpu+0x2368], [cpu+0x2360])` -> 0x23bc;
   * `cpu_block_flush(cpu, addr)` (invalidate the blocks covering addr);
   * `cpu_block_lookup_base(cpu, pc)` (retranslate from the current PC);
   * restore registers except x30 and jump to the returned block, abandoning the current one.

`*_arm7` variants are identical except that IO goes to store_io_register_arm7_* and there is no geometry
shortcut. 8/16-bit variants: same structure, extended_store8/16, check_allocation8/16.
Consequence of the dead reload noted in step 3: an IO handler's change to 0x2290 is lost. Budget changes have
to go through alert bit 2, which edits slot 0x22f0 [C].

### cpu_translate_get_pc (0x96830) [C]
Scans backwards from the return address for the word `0x36f8004c` (the encoding of the block prologue's
cycle check, "test w12 bit 31, branch +8 if clear"). With P = that word's address, `delta = (ret - P)/4`. It
reads the descriptor at `cache + [P-4]`: +0x04 = block PC (|thumb), +0x14 = offset of the PC metadata array.
Each metadata u32 = `pc_delta(16) | host_insn_delta(16)<<16` (cpu_translate_store_pc_metadata). It finds the
entry whose high half equals delta and returns `block_pc + pc_delta`.

### LDM/STM stubs (arm64_load_blockN / arm64_store_blockN, N=1..16) [C]
* load_blockN(addr): page lookup. Fast path returns **x0 = host pointer** (`entry<<2 + (addr&~3)`), and
  the JIT then reads the registers from x0 directly. There is **no page-crossing check** (it relies on the
  host mapping being contiguous). Slow path: `w21` (r8, saved at 0x2340) = N ->
  arm64_load_block_extended, which calls extended_load32 N times into the buffer at 0x22b0 and returns
  x0 = 0x22b0.
* store_blockN(addr): the JIT first writes the registers into the buffer 0x22b0, then calls the stub.
  Fast path copies the buffer to memory inline. Bit 62 without 63 -> arm64_store_block_extended
  (extended_store32 loop, then alert poll). Bit 63 -> copy, then cpu_block_check_allocation_blockN; on a
  hit: get_pc / flush / lookup / jump, as for single stores.

### Status-register / misc stubs [C]
* `arm64_mrs_cpsr`: **dead code** - its only reference is the debug label table `disasm_labels` (0x167ca0).
  Its result keeps CPSR[31:28] (the stale flags) and *zeroes* bits 27:0, i.e. the bit-field insert has its
  operands the other way round from the inline MRS sequence [C].
* `arm64_msr_cpsr_low(w0 = new CPSR)`: stores CPSR. Its test value (old CPSR & 0x1f) is never 0 for a valid
  mode, so it always takes the full-save path -> `execute_arm_set_cpu_mode_from_cpsr` (register banking) ->
  then `cpu_alerts_check_interrupts`. (The fast exit, taken only when the I bit was not cleared, looks
  unreachable. Probably intended to test changed mode bits [L].)
* `arm64_msr_spsr(w0 = bits, w1 = keep mask)`: if `CPSR & 0xf != 0`, `SPSR[idx] = (SPSR[idx] & w1) | w0`. So it
  is skipped only in USR (0x10); SYS (0x1f) writes SPSR slot 0, the slot shared by USR/SYS (0x8f524) [C].
* `arm64_spsr_restore(w0 = new PC)` (MOVS pc / LDM ^): full save; CPSR = SPSR[idx]; if T is set it clears T
  in CPSR and sets PC bit0 (the JIT encodes Thumb in PC bit0); set_cpu_mode_from_cpsr; if an IRQ is
  pending and I is clear -> execute_arm_raise_interrupt and jump to [0x2280]; else lookup_base(PC) and
  jump there.
* `arm64_set_cpu_mode(w1 = mode index)`: full save, execute_arm_set_cpu_mode, full restore, return.
* `arm64_update_coprocessor_{control,dtcm_control,itcm_control}(w1 = value)`: write CP15 fields at
  [0x2250]+0x14 (mask 0xff085) / +0x18 / +0x1c, then call the C updater. If control/DTCM returns nonzero
  (remap, or the DTCM-in-window flag at cp15+0x3c changed) -> flush all blocks (`cpu_block_flush(cpu,-1)`),
  recover PC, re-lookup, jump.
* `arm64_shift_reg_flags_{lsl,lsr,asr,ror}(w1 = value, w2 = flags image, w3 = amount)`: register-shift
  carry-out into bit 29 of w2, result in w1. **ASR bug:** for amounts 1..31 the carry is taken from bit[n]
  (the value is shifted right by the amount itself) instead of bit[n-1]; the amount-1 value is computed but
  never used (0x8f900-0x8f904). The ROR register form does not special-case amounts that are multiples of 32
  beyond the plain "shift by n-1", which happens to give bit 31 for 32 [L].
* `arm64_step_debug`: full save and call step_debug(cpu+0x2118, pc(w1), cycles(w12)) (debugger builds /
  trace mode).

---------------------------------------------------------------------------------------------------
## 5. CPU alerts [C]

Word at **cpu+0x22a8**, set by C code (IO writes, DMA, IPC, geometry), polled **only** at the end of
the IO-store slow paths (store8/16/32 arm9/arm7 IO branch, the geometry command-port branch,
store_block_extended). It is not polled after loads, the packed-GXFIFO path, or extended_store*. Other
asynchronous sources (events raising IRQs) are delivered at slice boundaries by recompiler_entry_direct,
which checks 0x2108 directly.

`handle_cpu_alerts` (0x8dca4) first zeroes 0x22a8 and does a light save, then tests the bits in this order:

| bit | meaning | action |
|---|---|---|
| 0x10 | **halt** (HALTCNT halt/sleep, GX FIFO full in queue_geometry_command) | w0 = get_pc(x30) -> `block_halt`. Because the word was already zeroed and this branch never looks at the other bits, any bits set together with 0x10 are **lost** (e.g. a simultaneous "DMA modified code" bit 0, so stale blocks can run after wake-up) [C path, L impact] |
| 0x04 | **end the timeslice now** (DMA start, IPC, IO with timing side effects) | if w12 >= 0: `sys.slice -= w12`; saved w12 (0x22f0) = 0. Then continue with bits 0-1 |
| 0x01 | **DMA modified code** | branch-table entry 1: get_pc -> 0x23bc; `cpu_block_flush(cpu, -1)` (flush everything); lookup_base(pc); jump |
| 0x02 | **check interrupts** (0x2108 became nonzero, IME/IE/IF write) | entry 2: if 0x2108 != 0 and CPSR.I == 0: merge CPSR, full save, 0x23bc = get_pc(x30), execute_arm_raise_interrupt, jump to [0x2280] (IRQ vector block) with w12 restored from 0x2290; else return |
| 0x03 | both | entry 3: flush, plus IRQ or re-lookup |
| 0x08 | interpreter only: "IRQ taken by a CPSR write". Set by execute_arm_set_cpsr (0x2577c), execute_arm_spsr_restore (0x258ec) and execute_arm_msr_op (0x2654c) when the new CPSR has I = 0 while an IRQ is pending; the same function then enters the IRQ exception inline. `_execute_cpu` reacts by discarding its accumulated-cycle register (0x27f00) | never set by the JIT (its MSR path uses arm64_msr_cpsr_low -> cpu_alerts_check_interrupts); handle_cpu_alerts ignores it and simply clears it with the word [C] |

Dispatch: the low two bits of the alerts value index a four-entry table of branches (cpu_alerts_jump_table,
0x8df90).
Bit-to-setter evidence: store_io_register_arm7_8 0x111b4-0x111c4 (writes 0x2108, then sets 0x02),
0x115b8-0x115e0 ("Entering sleep mode.", sets 0x10, halt = 2); dma_transfer/_gamecard set 0x01 and 0x04;
queue_geometry_command sets 0x10.

---------------------------------------------------------------------------------------------------
## 6. Indirect branches and block lookup [C]

### block_indirect_branch (0x8cb3c): what it does
Input: w0 = target PC (bit0 = Thumb).
1. Forms idx*4 directly as `pc & 0xffc` (so idx = (pc>>2) & 0x3ff) and from it the two addresses
   cpu+0x80+idx*4 (tag) and cpu+0x1080+idx*4 (code offset).
2. Loads both words relative to x28.
3. Compares tag and PC by subtraction into a scratch register (NZCV untouched). On a match it branches to
   x11 + offset, i.e. to the block's P, so the entry cycle check still runs.
4. On a miss: passes (cpu, pc) as arguments, does a light save **without** x30 (this is a tail jump, the
   original return address stays valid), calls `cpu_block_lookup_base`, restores x9/x10/x11, w12-w18 and NZCV,
   and jumps to the returned address.

* Inline part: a **1024-entry direct-mapped cache** inside the CPU struct (tags at +0x80, offsets at
  +0x1080). Thumb targets keep bit0 in the tag. Halfword-aligned Thumb PCs differing only in bit 1 share a
  slot (bit 1 is dropped from the index).
* `block_itcm_branch_arm/thumb` (0x8cb0c/0x8cb24): for ARM9 targets in ITCM (<0x2000000). Direct index
  into [0x2270] (`(pc>>2)&0x1fff`) or [0x2278] (`(pc>>1)&0x3fff`); 0 = not translated -> the miss path of
  block_indirect_branch. Emitted by cpu_translate_direct_link for ITCM targets (ITCM code can change, so
  it is not hard-linked) and as the fall-through exit of type-4 blocks.
* Slow path `cpu_block_lookup_base(cpu, pc)` (0x2db10): ITCM -> ITCM tables; main RAM (pc>>24 == 2) ->
  hash at cache+0x1300000, 0x8000 buckets x 16 bytes (2 inline {pc, offset} pairs + overflow chain via
  the block's descriptor); other memory -> cache+0x1380000, 0x2000 buckets. On a miss: cpu_block_create ->
  cpu_translate_block, cpu_translate_link_delayed_targets, then DC CIVAC / IC IVAU over each
  translation-cache region that grew. Every hit or create also refills the 1024-entry cache.
* Block exits (generated code, cpu_translate_block): types 1/2 (not branches) load `next_pc|T` into w0 and
  branch to block_indirect_branch. Type 3 = direct link: a plain branch to the target's P (patched later if
  the target is not translated yet). Type 4 = ITCM: branch to block_itcm_branch_arm. The idle-loop
  "w12 = -1" belongs to the loop's own backward branch (exit type 0), see section 3.

### Block layout in the translation cache (cpu_translate_block) [C]

| location | content |
|---|---|
| P-4 | u32 descriptor offset (from cache base) |
| P | cycle check "if w12 bit 31 clear, go to P+8" (encoding 0x36f8004c; also the get_pc scan anchor) |
| P+4 | call to recompiler_cpu_next_action_{arm9_to_arm7 \| arm7_to_event_update} |
| P+8 | body (this is the resume address stored in 0x2298) |

Descriptor (0x18 bytes, allocated downward from the region top, PC metadata right below it):

| offset | type | meaning |
|---|---|---|
| +0x00 | u32 | 0 / chain link |
| +0x04 | u32 | guest PC \| thumb |
| +0x08 | u32 | code offset (P) |
| +0x0c | u16 | (live registers in, see block_cache.md) |
| +0x0e | u16 | (live flags in) |
| +0x10 | u16 | flags: low 12 bits \| type<<12 |
| +0x12 | u16 | code size |
| +0x14 | u32 | offset of the PC metadata array |

Translation regions (cache+0xf24000+..: three {code_ptr, meta_ptr} pairs): main RAM, ITCM, other.

---------------------------------------------------------------------------------------------------
## 7. Possible bugs and quirks (for SuperDrastic hooks)
1. ASR-by-register carry uses bit[n] instead of bit[n-1] (0x8f904) [C].
2. arm64_load_memory64 keeps x30 in 0x2338, and the unaligned-load32 path keeps w19 in 0x2338 (and x30 in
   0x2330). They collide only if LDRD hits an unaligned address [C].
3. IO-store paths discard 0x2290 changes made by C handlers (the light restore from 0x22f0 overwrites w12) [C].
4. The packed GXFIFO store path (0x04000400-0x0400043f) skips the alert poll [C].
5. arm64_msr_cpsr_low always takes the slow set-mode path (`old & 0x1f` is never 0) [L].
6. load_blockN / store_blockN fast paths do not check for crossing a 2 KB page [C].
7. Alert bit 0x10 discards any other alert bits raised at the same time (section 5) [C path, L impact].
8. Inlined SP-relative ARM9 stores through x10 bypass SMC detection (section 1) [C].
9. Hooking notes: the guest register file is only valid in memory (0x2370..) after a full save, i.e.
   inside next_action / block_halt / set_mode / spsr_restore / IRQ paths, or after the JIT handed off.
   Inside light-save C calls (memory handlers), r0-r5 live at 0x22f4..0x2308 and r6-r14 are still in
   host x19-x27. A C function called from a memory slow path that wants guest registers must read those
   (or use cpu_translate_get_pc([cpu+0x2368], [cpu+0x2360]) for the PC) [C].
