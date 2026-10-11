# DraStic's JIT, reverse engineered

This is a map of the ARM9/ARM7 → AArch64 dynamic recompiler in **DraStic r2.5.2.2**: the Linux aarch64 build
that ROCKNIX ships, md5 `17550db727f3b59d36b57746ad1944be`.

**How it was done.** The binary is a PIE that was never stripped, so every static function and asm label keeps
its name. It was analysed statically with [REA](https://github.com/morluto/rea) 6.4.0 on top of Ghidra 12.1.4,
plus llvm-objdump and llvm-mc. A dynamic harness ran it under qemu-aarch64 (see *Dynamic verification*).

**Where the evidence is.** Each claim in `notes/` has an address and a confidence tag:

| Tag | Meaning |
|---|---|
| C | confirmed |
| L | likely |
| G | guess |

This file is the overview. The detail lives in:

| File | Covers |
|---|---|
| `notes/translator_core.md` | pipeline, analysis pass, register map, conditions, Thumb, cycles, linking, pseudocode |
| `notes/emitters.md` | what AArch64 each ARM instruction class becomes, with every encoding decoded |
| `notes/block_cache.md` | translation-cache layout, PC→code lookup, block creation, self-modifying-code detection, flushing |
| `notes/asm_runtime.md` | hand-written asm stubs, CPU state struct, scheduling, memory handlers, alerts |
| `notes/startup.md` | JIT selection, CLI switches, the fastmem window |
| `notes/coverage.md` | how every ARMv5TE ARM and Thumb instruction class is handled (inline, stub, UND or nothing), ARM9 vs ARM7 |
| `notes/review.md` | cross-check of all notes, answers to the open questions, and errata. Where it conflicts with an earlier note, review.md wins; the affected notes start with an errata box |
| `notes/dynamic*.md` | qemu harness and real translated blocks |

All addresses here are file VAs (base 0). Ghidra and REA addresses are VA + 0x100000.

The copyright on DraStic belongs to its author, Exophase. Everything in this directory is our own work.

- The notes describe DraStic's behaviour in our own words: addresses, offsets, layouts, ISA encodings and test
  results.
- There are no decompiler output, no copies of DraStic's machine code and no pseudocode that mirrors its functions.
- The only generated code shown is what DraStic's JIT produced for our own test ROMs, in short annotated excerpts.
- The patches and tools are code we wrote. They check the DraStic build by its SHA-256 instead of embedding any of
  its bytes.
- The DraStic binary itself is not included; bring your own copy.

---

## 1. The big picture

```
main ─ _setjmp ─▶ recompiler_entry(nds_system, fastmem_base)      (never returns; menu/load-state longjmp back)
                    │
          ┌─────────▼──────────┐   w12 < 0 at a block entry
          │  translated blocks │──────────────▶ recompiler_cpu_next_action_arm9_to_arm7 / arm7_to_event_update
          │  (AArch64, in a    │                 ARM9 slice → ARM7 slice → execute_events → ARM9 …
          │  48 MB RWX area    │
          │  inside .bss)      │── indirect branch / miss ─▶ block_indirect_branch (1024-entry front cache)
          └──▲──────────┬──────┘                              └▶ cpu_block_lookup_base (hash / ITCM tables)
             │          │                                          └▶ cpu_block_create  (decode + analysis)
             │          │ loads/stores                                  └▶ cpu_translate_block (codegen)
             │          ▼                                                    └▶ cpu_translate_instruction …
             │   inline fastmem (base + addr)  or  arm64_load_* / arm64_store_*_{arm9,arm7} stubs
             │          │ store to a page flagged "has code"
             └──────────┴── cpu_block_check_allocation* → cpu_block_flush (whole cache) → re-lookup
```

The design resembles Exophase's earlier recompilers (gpSP), rebuilt for AArch64. This is an observation, not something the binary confirms:

- Guest registers are statically mapped to host registers.
- Guest flags live in the host NZCV register.
- Blocks are linked directly, and translation runs eagerly and recursively.
- Guest memory is reached through a fastmem window with per-page handler tables.
- Self-modifying code is handled by flushing the whole cache.

There is no IR: each ARM instruction is translated straight into AArch64 words. The JIT **never falls back to the
interpreter** (`coverage.md`): each instruction is emitted inline, calls an `arm64_*` stub, raises UND, or emits
nothing.

**ARM9-only optimisations.** Inline fastmem loads, SP-relative fast paths and literal-pool constant folding are
used on the ARM9 only. The ARM7 goes through stub calls for all of these. The translator checks the CPU type byte
at translation state +0x478.

## 2. Selecting the JIT

`process_arguments` takes getopt long options. Their table is at 0x160010:

| Option | What it does |
|---|---|
| `--recompiler` | selects the JIT (the default) |
| `--interpreter` | selects the interpreter (`execute_arm_*`) |
| `--benchmark N`, `--bench-display N`, `--bench-full N` | benchmark runs |
| `--fast-forward` | fast-forward |
| `--debug-arm9`, `--debug-arm7` | step debugger |
| `--breakpoint-arm9/7 HEX`, `--countdown-arm9/7 HEX` | breakpoints |
| `--color-depth N` | colour depth |
| `--input-record F`, `--input-playback F` | record or replay input |

`--recompiler` and `--interpreter` write the use-recompiler byte at `nds_system+0x3b2a9a8`. `main` reads that
byte after `_setjmp(nds_system+0x3b2a840)` and calls either `recompiler_entry` or the interpreter loop.

## 3. Host register convention inside translated code (C)

| Host | Role |
|---|---|
| `x28` | current CPU state struct (ARM9 = `nds_system+0x15c7d50`, ARM7 = `nds_system+0x25ce340`) |
| `x9` | `x28+0x23d0`: that CPU's memory page table, also the base for negative-offset spill slots |
| `x10` | fastmem base: 64 MB host window mirroring DS physical space 0x0–0x4000000 |
| `x11` | translation cache base T (`nds_system+0x8c000`) |
| `w12` | cycle countdown; the CPU hands off when it goes negative |
| `w13`–`w27` | guest **r0–r14**, fixed (`host = rn + 13`). r15 is never in a register: reads become constants, writes go to `w0` and then an exit |
| host NZCV | guest NZCV. CPSR minus NZCV is at `cpu+0x23c0`; Q and T are tracked in memory |
| `x0`–`x4` | scratch registers and stub arguments (address in `w0`, store value in `w1`, load result in `w0`) |
| `x30` | link. `x29` and `sp` are never touched |

Translated code makes no prologue or epilogue calls. Stubs save only the C caller-saved subset (`w12`–`w18`,
`x30`, NZCV) around calls into C. A full register-file spill happens only on a CPU switch, mode change, IRQ or
halt.

## 4. CPU state struct, the most useful offsets (from `asm_runtime.md` §2)

| Offset | Meaning | Offset | Meaning |
|---|---|---|---|
| 0x0080 / 0x1080 | indirect-branch front cache: 1024 tags / 1024 code offsets | 0x2290 | saved cycle counter |
| 0x2088 | translation cache base | 0x2298 | resume address (block entry + 8) |
| 0x2090.. | banked r13/r14 | 0x22a0 | the other CPU's struct |
| 0x20e8 + 4·mode | SPSRs | 0x22a8 | alert bits |
| 0x2104 | mode index: 0 USR/SYS, 1 FIQ, 2 IRQ, 3 SVC, 4 ABT, 5 UND | 0x22b0 | LDM/STM staging buffer |
| 0x2108 | IRQ pending | 0x2358 / 0x2360 | saved fastmem base / translation cache base |
| 0x210c | CPU type (1 = ARM9) | 0x2370–0x23a8 | spilled r0–r14 |
| 0x2110 | halt state (0 run, 1 halt, 2 sleep) | 0x23b0 | next_action function pointer |
| 0x2250 | CP15 pointer (null on ARM7) | 0x23b8 | spilled NZCV |
| 0x2258 | `nds_system` | 0x23bc | PC (bit 0 = Thumb) |
| 0x2270 / 0x2278 | ITCM block tables, ARM / Thumb | 0x23c0 | CPSR |
| 0x2280 / 0x2288 | cached IRQ / SWI vector blocks | 0x23d0 | page table: 2 M × 8 B entries, 2 KB pages |

## 5. Memory: fastmem window + page table (C)

**The window.** `initialize_memory` creates a shm file and reserves a 64 MB "dynamic memory map low buffer". It
maps main RAM into that buffer four times, at 0x2000000–0x3000000, in 16 KB `MAP_FIXED` pages. Directly mapped
guest memory is then simply at host address fastmem base + guest address.

**The page table.** Each CPU has one at `cpu+0x23d0`, with one 8-byte entry per 2 KB page:

- the entry value is `(host_page − guest_page) >> 2`;
- **bit 62** means stores to the page need special handling;
- **bit 63** (set together with 62) means the page holds translated code.

**Inline vs out-of-line access.**

- ARM9 loads are emitted inline. A mask test checks that the address is in a directly mapped region and suitably
  aligned; if it is, the value is loaded straight from the fastmem window. Anything else branches to an out-of-line
  stub that calls `arm64_load_ext*`, which handles IO and unaligned-LDR rotation.
- ARM9 SP-relative accesses go straight to the fastmem window with no check when "DTCM in mapped memory" is set
  (`cp15+0x3c`).
- ARM7 loads and LDRD always `bl arm64_load_memory*`.
- All stores `bl arm64_store_memoryN_{arm9,arm7}`.

**Fast paths** are about six instructions and leave the flags alone. The slow paths are:

| Case | Handled by |
|---|---|
| unmapped load | `extended_load*` |
| IO store (`addr>>24 == 4`) | `store_io_register_*`; the ARM9 geometry FIFO goes straight to `queue_geometry_command` |
| store to a page with translated code | see §8 |

**LDM/STM.** The stubs return a host pointer, so the block is then moved with `ldp`/`stp`. When the range is
not directly mapped, the data is staged through `cpu+0x22b0` instead.

## 6. Translation pipeline (translator_core.md §0, §7)

1. **`cpu_block_lookup_base`** (0x2db10) resets the per-translation state (branch stack, delayed-link list),
   calls `cpu_block_create`, inserts the result into the tables, patches delayed links, then cleans the caches
   with `DC CIVAC` and `IC IVAU`.
2. **`cpu_block_create`** (0x2bf70) is the front end and the analysis pass.
   - It fetches each instruction. Thumb goes through `convert_thumb_instruction_to_arm`, and BL pairs are fused
     into one instruction.
   - It fills a **0x20-byte record per instruction**: opcode, PC value, host address, in-block branch target,
     registers read / written / live-out, flags read+written / live-out, flag bits (needs PC metadata, is a
     load), cycle cost, and "must be alone in its block".
   - A block ends:
     - at an unconditional PC write;
     - after 0x800 instructions;
     - after 4 zero words;
     - at a loop head, where the block is split;
     - before a "hot" ITCM word.
   - Passes over the block:
     - backward register and NZCV liveness, merged with the successor block's live-in;
     - **known-condition folding**: after a backward conditional branch, later instructions with the same
       condition become NOPs and those with the inverse condition become unconditional;
     - **idle-loop detection**.
   - There is no constant propagation.
   - Direct-branch targets are translated *recursively and eagerly* (`cpu_block_lookup_recursive`, no depth
     limit). Targets already in progress go on a delayed-link list.
3. **`cpu_translate_block`** (0x95e20) emits the code.
   - It emits a two-instruction prologue: if the cycle counter is negative, call the CPU's next-action stub.
   - For each instruction it does the cycle accounting and calls `cpu_translate_instruction`.
   - It writes the block exit for the exit type:

     | Exit type | Meaning |
     |---|---|
     | 0 | branch |
     | 1 | instruction limit |
     | 2 | special instruction |
     | 3 | loop-head fall-through link |
     | 4 | hot ITCM |

   - It then writes the 0x18-byte block header and the PC-metadata trailer, and patches in-block branches.
4. **`cpu_translate_instruction`** (0x93320) is a big switch over ARM instruction classes. It calls the emitters
   `cpu_translate_alu_op_imm`, `_memory_op`, `_block_memory_op`, `_msr_op`, `_mrs_op`, `_bx_op` and
   `_raise_exception`. They write AArch64 instruction words through an output cursor in the translation state (+0x408).

## 7. What the code looks like (emitters.md §6)

**Condition codes.** A conditional instruction is wrapped as `b.<inverse cond> skip ; …body… ; skip:`. Guest
NZCV *is* host NZCV, and AArch64's carry convention for subtraction matches ARM's (C = NOT borrow), so no
inversion is needed.

**Data processing**

- Arithmetic ops (`ADD`, `SUB`, `ADC`, `SBC`, `RSB`, `RSC`, `CMP`, `CMN`) map to `ADDS`/`SUBS`/`ADCS`/`SBCS`.
  RSB and RSC swap the operands.
- Logical ops with S set N and Z through a flag-setting AND of the result. The shifter carry is merged into NZCV
  (read flags, insert the bit, write flags back) only when liveness says C is still needed.
- Rotated imm8 operands:
  - a valid bitmask immediate is used directly as a logical immediate;
  - ADD/SUB use imm12 (optionally LSL #12) when it fits;
  - otherwise the operand is `ROR #2r` of the raw imm8, or a constant built with movz/movn/movk.
- Shifts by register use inline `lslv`/`lsrv`/`asrv` with a fix-up for amounts of 32 or more. When the carry
  is needed they call `arm64_shift_reg_flags_*`.

**Writes to PC.** The value is put in w0 and aligned (ARM) or tagged with the Thumb bit, then execution jumps to
`block_indirect_branch`. With S set it branches to `arm64_spsr_restore` instead.

**Stores and SMC metadata.** After every store, MSR and CP15 write, the translator records a
(pc delta, host offset) metadata entry. `cpu_translate_get_pc` uses it to recover the guest PC in the middle
of a block, e.g. after a store invalidated the code it is running in.

**Exceptions and CP15**

- SWI, undefined and BKPT are emitted inline: mode switch, LR, SPSR, vector (from `cp15+0x10` on the ARM9),
  then `b block_indirect_branch`.
- CP15 c1 and c9 writes call stubs that update the control register and the DTCM/ITCM mapping.
- Wait-for-interrupt sets the halt state and branches to `block_halt`.

**Cycles.** Costs build up during the block and are subtracted from the counter only before PC-writing
instructions and before the fall-through link.

- ARM9 cost = recorded cycles + a per-game adjustment (`apply_cycle_adjustment_hacks`).
- ARM7 cost = recorded cycles × 2. The unit is ARM9 clocks.
- Idle loops set the counter to −1 before the backward branch, so the loop gives up the rest of its time slice.

## 8. Block cache and self-modifying code (block_cache.md)

**Translation cache layout.** T = `nds_system+0x8c000`, and the area is mprotected RWX. Both CPUs share it.
Each of the three caches grows code upward and headers/metadata downward.

| Cache | Size | Holds |
|---|---|---|
| main | 16 MB | guest 0x02xxxxxx |
| ITCM | 1 MB | ARM9, pc < 0x2000000 |
| alternate | 2 MB | BIOS, WRAM, VRAM and everything else |

There is **no "cache full" check**. Only code changes cause flushes.

**PC → code lookup**

1. The per-CPU front cache: tags at `cpu+0x80`, offsets at `cpu+0x1080`, indexed by `(pc>>2)&0x3ff`.
2. The hash tables: 16-byte buckets with two ways, chained through the block headers. Main has 0x8000
   buckets and alternate 0x2000. The key is `pc|thumb`. The CPU id is not part of the key.
3. ITCM has direct tables instead.

**SMC detection**

- `cpu_block_fill_allocation` sets page bits 62/63 plus coarse (2 KB) and fine (2 B) bitmaps for the code
  each block covers.
- A store to a flagged page first compares against the old value and does nothing if it is unchanged.
- Otherwise it calls `cpu_block_check_allocation{8,16,32,_blockN}`. On a hit:
  1. `cpu_block_flush` throws away the **whole** cache for that region;
  2. the PC is recovered from the PC metadata;
  3. the code is looked up again and execution resumes there.

**Flush scope**

| Write to | Flushes |
|---|---|
| main RAM | main + ITCM |
| ITCM | ITCM only |
| anything else | alternate, then re-translates the IRQ/SWI vector blocks |
| DMA over code | everything (`memory_check_code_region` → alert bit 0x01 → `cpu_alerts_dma_modified_code`) |

**ITCM polymorphism.** Each ITCM word has a write counter. After more than 8 writes a word becomes "hot", and
up to 16 translations keyed by (value, address) are cached, so rewriting a hot word swaps an entry instead of
flushing.

**Literal pools.** PC-relative loads are constant-folded at translate time and registered for SMC detection. A
bitmap of "words modified after translation" (`T+0x14da138`) turns that folding off for words that are known to
change.

## 9. Scheduling and alerts (asm_runtime.md §3, §5)

The slice length is the number of cycles until the next event. Each slice runs in this order:

1. The ARM9 runs the slice.
2. The ARM7 runs the same slice.
3. `execute_events` runs.

**Overshoot** carries over in `cpu+0x2290`. A halted CPU is charged the whole slice.

**Handing off.** A CPU only hands off at a block entry with `w12 < 0`. A slice can also end early through:

- alert bit 0x04;
- `block_halt`;
- an idle loop.

**Alerts** (`cpu+0x22a8`) are polled only after IO-store slow paths:

| Bit | Meaning |
|---|---|
| 0x01 | DMA modified code |
| 0x02 | check interrupts |
| 0x04 | end slice |
| 0x10 | halt |

## 10. DraStic JIT bugs

"Dyn" means checked dynamically: the same homebrew ROM was run with `--interpreter` and `--recompiler` under
qemu, and the results were compared against ARM-spec values cross-checked with `qemu-arm -cpu arm946`. The full
mismatch list is `notes/dynamic_diff_mismatches.txt`.

| # | Bug | Where | Status |
|---|---|---|---|
| 1 | `MOVS Rd, Rm, ASR Rs`: shifter carry is bit *n* instead of bit *n−1*. The stub computes n−1 and then shifts by n anyway; the LSR stub uses n−1 correctly | `arm64_shift_reg_flags_asr`, 0x8f8f4 | **C + Dyn**: 24/24 cases wrong in the JIT, interpreter correct |
| 2 | QADD zero-extends Rn (the emitted ADD uses the UXTW extend option) while QSUB sign-extends it, so `QADD 5, -1` gives 0x7fffffff with Q set instead of 4 | `cpu_translate_instruction` | **C + Dyn** |
| 3 | QDADD/QDSUB compute the doubled operand into a scratch register that nothing reads: `QDADD 1, 0x10` gives 0x11 instead of 0x21. *The interpreter is wrong here too*, in a different way (it saturates the doubled value) | same | **C + Dyn** |
| 4 | SMLAxy results are right, but Q is set wrongly when the accumulator is negative (it is zero-extended before the 64-bit add) | same | **Dyn** |
| 5 | SMLALxy extracts the operand halves wrongly (shifted by one instead of sign-extending the selected half), so the high word is wrong. *The interpreter is also wrong*, with a different high word | same | **Dyn** |
| 6 | `MSR CPSR_f` (flags field only) never updates Q in the JIT; it only writes host NZCV | `cpu_translate_msr_op` | **Dyn** |
| 7 | The SPSR saved on an exception is built from the in-memory CPSR, so its NZCV can be stale | `cpu_translate_raise_exception` | C (static) |
| 8 | `cpu_block_flush` clears page bits with mismatched base addresses in the ARM7-main and alternate loops | 0x2ea60, 0x2eda8 | L |
| 9 | Code flags are set only in the translating CPU's page table, so the ARM7 writing ARM9 code (or the reverse) goes undetected and the JIT keeps running stale code | `cpu_block_fill_allocation` | **Dyn**: the ARM7 store left the ARM9 running the old code |
| 10 | IO-store slow paths reload `w12`, so a cycle change a C handler writes to `0x2290` is lost | store stubs | L |
| 11 | An unpaired Thumb BL suffix overwrites LR before using it, so it lands at next+4 | Thumb conversion | **Dyn** |
| 12 | STR/STM of PC stores PC+8, the same as the interpreter. This is an emulator-wide choice, not a JIT bug | — | Dyn (not a JIT divergence) |
| 13 | Cycle costs pending at exit types 1, 2 and 4 are dropped | `cpu_translate_block` | L |
| 14 | `TST Rn, Rm{, shift}` (ARM and Thumb, both CPUs) becomes a bare host `ands`, which clears **C and V**. ARM keeps V and sets C from the shifter | TST register emitter | **Dyn** |
| 15 | Logical ops with a rotated immediate (ANDS/MOVS/ORRS/MVNS/TST/TEQ, rot ≠ 0) force C = 0 (the generated code clears the carry bit explicitly); ARM says C = imm[31]. *The interpreter is wrong too*: it reads bit 2r−1 of the rotated value, and leaves TST/TEQ's carry unchanged (0x25014) | `cpu_translate_alu_op_imm` | **Dyn + C** |
| 16 | The ASR carry bug (#1) also hits Thumb `ASRS Rd, Rs` on both CPUs, because Thumb is converted to ARM | — | **Dyn** |
| 17 | `mov pc, pc` (ARM and Thumb) is removed as a no-op `mov Rd, Rd`. Unconditionally, the block has no exit and the JIT **crashes with SIGILL**; `moveq pc,pc` taken becomes a NOP | ALU register path, 0x93c9c | **C + Dyn** |
| 18 | ARMv5-only instructions on the ARM7: CLZ is a NOP; BLX imm, LDRD, STRD and the Thumb BLX suffix execute instead of raising UND. *The interpreter does the same*, so this is a DraStic decoder choice | decoders | **Dyn** (both cores) |
| 19 | Thumb BKPT is decoded as `pop`, and Thumb 0xDExx becomes an always-taken branch. *The interpreter does the same* | Thumb decoding | **Dyn** (both cores) |
| 20 | The first store of an STRD has no PC-metadata entry. If that store hits translated code, the JIT resumes at a garbage PC and **crashes** | memory emitter | **C + Dyn** |
| 21 | Alert 0x10 (halt) discards a simultaneous "DMA modified code" alert | `handle_cpu_alerts` | C (static) |
| 22 | ITCM literal folding is inverted: only words modified more than 7 times are folded | `cpu_translate_memory_op` | C (static) |
| 23 | SWP with Rd==Rn or Rd==Rm whose store hits translated code never writes Rd | SWP emitter | **Dyn** |
| 24 | `msr cpsr_c, Rm` stores Rm[31:8] too, so bit 27 of Rm sets or clears Q. The interpreter is correct | `arm64_msr_cpsr_low` | **C + Dyn** |
| 25 | **CP15 c1 (control register) writes are lost in the JIT.** `arm64_update_coprocessor_control` computes the merged value but never stores it, and `update_coprocessor_control` then reloads the old word from cp15+0x14. Clearing the V bit has no effect, so exceptions keep going to 0xffff00xx. This mostly goes unnoticed because DraStic's default value already suits DS games | 0x8f678, 0x295b0 | **C + Dyn** |

SMULxy, SMULWy, SMLAWy, QSUB, and the LSL/LSR/ROR register-shift carries all matched the spec in both cores. The
Thumb ALU, BL pairs, BLX/BX, PC-relative loads, LDM/STM and PUSH/POP{pc} matched too. Apart from #1, #14 and #15,
the ARM7 JIT matches the ARM7 interpreter.

**SMC behaviour, measured (`notes/dynamic2.md`)**

- Any detected write to main-RAM code flushes main + ITCM, even 128 KB away from the code, and including the ARM7's
  blocks.
- A data word in a code page, or a store of an unchanged value, causes no flush. An identical STM does flush.
- DMA over code flushes everything.
- An ITCM word becomes "hot" after 9 flushes, then switches to the polymorphic cache.

**Timing.** A load-containing loop costs 7 cycles per iteration in the JIT and 6 in the interpreter. Idle-loop
detection fires for memory- and VCOUNT-polling loops.

The cp15 main ID register reads as 0x41009561, not the real ARM946E-S value 0x41059461.

### Fixes (patches/)

Twelve JIT bugs (#1–#6, #11, #14, #15, #17, #24, #25) have prototype **runtime patches** that a preloaded library such as
SuperDrastic could apply from its constructor. The code is `patches/fixes.c`; turn it on with `JITFIX=1`, or a
bit mask to pick fixes. Root causes and patch locations are in `patches/README.md`.

| Mask | Bug | Patch |
|---|---|---|
| 0x01 | #1 ASR-by-register carry (ARM + Thumb, both CPUs) | 1 word in the asm stub, 0x8f904 |
| 0x02 | #2 QADD | 1 emitter constant, 0x957b8 |
| 0x04 | #3 QDADD/QDSUB | pass after translation + helper |
| 0x08 | #4 SMLAxy Q | 1 emitter constant, 0x937b0 |
| 0x10 | #5 SMLALxy | 7 emitter instructions, 0x95068 |
| 0x20 | #6 MSR CPSR_f Q | pass after translation + helper |
| 0x40 | #14 TST register clears C/V | 1 word at 0x93894. The check that skips the C/V-preserving wrapper for arithmetic ops also matched TST; the fix narrows its bit mask so only arithmetic ops match |
| 0x80 | #15 rotated-immediate carry | 1 word at 0x920a4: the carry is taken from bit 31 of the rotated immediate instead of bit 2r−1 |
| 0x100 | #11 lone Thumb BL suffix | pass after translation that reorders the emitted words (same length) |
| 0x200 | #25 CP15 control writes lost | the call at 0x8f6b0 is redirected to our helper, which stores the merged value at cp15+0x14 first, then tail-calls `update_coprocessor_control` |
| 0x400 | #17 `mov pc,pc` | 1 word at 0x93ca0: the no-op-move check compares the mapped host registers instead of the raw register fields, so a move involving PC is never treated as a no-op |
| 0x800 | #24 `msr cpsr_c` changes Q | pass after translation that sends `bl arm64_msr_cpsr_low` to helpers that merge only the written fields |

**How the helpers work.** Where the emitter can't produce a needed call (#3, #6, #24), a pass runs after
`cpu_translate_block` and rewrites the emitted code in place. The new code calls small helpers placed in zero
padding at the end of the R-X segment (VA 0x14c800).

**Safety.** Nothing is written unless the SHA-256 of the running executable matches DraStic r2.5.2.2 aarch64. Each patch site also has a 32-bit hash guard over its original bytes, so a mismatch refuses that patch. Every instruction we write is built by our own A64 encoders. Static patches are applied in
the constructor, before any translation, and flushed with `__builtin___clear_cache`. The post-passes run before
DraStic's own DC/IC maintenance.

**Verified under qemu, all twelve on (`JITFIX=1`, mask 0xfff)**

| ROM | Result |
|---|---|
| base differential (1311 words) | 0 JIT≠spec (was 162) |
| extended differential (1659 words) | 0 (was 335) |
| generated ARM9 ARM+Thumb (2531 words) | 0 in every fixed class |
| generated ARM7 ARM + Thumb + immediate (1147 + 1250 + 104 words) | 0 in every fixed class |
| round 3 ROMs (CP15, SWI after clearing V, `mov pc,pc`, MSR c, QDADD edge cases) | fixed JIT = interpreter = spec. The unpatched JIT crashed with SIGILL/SIGSEGV on several |

- What remains matches the interpreter: STR PC+8, and ARMv5-style LDM base-in-list and odd LDRH.
- Each fix changes only its own instruction class.
- Emitted code diffs (`rawdiff.py`) show changes only in the targeted sequences.
- The 3D stress ROM still runs.

**Not yet done.** Testing on real aarch64 hardware (the RG DS). Unpatched: #7 (stale SPSR flags on exceptions; now visible because BKPT reaches its handler), #20 and #23 (STRD/SWP self-modifying-code paths). The interpreter's own bugs (QDADD, SMLALxy, the
rotated-immediate and TST/TEQ carries) are documented but not patched.

## Dynamic verification (notes/dynamic.md, dynamic_examples.md)

**How it runs**

- DraStic runs headless on x86-64 under `qemu-aarch64`, with SDL on Xvfb and the free DraStic BIOS.
- The qemu binary comes from the Docker Hub image `tonistiigi/binfmt`. The arm64 Ubuntu sysroot comes from
  ports.ubuntu.com. Scripts are in `tools/dyn/`.
- That qemu build takes the first guest argument as argv[0], so the program name is passed twice:
  `qemu-aarch64 ./drastic ./drastic rom.nds`.

**The `jitdump.so` shim**

- `cpu_translate_block` is only called internally, so LD_PRELOAD can't interpose it. The shim patches its first
  16 bytes inline and jumps back through a trampoline.
- For each block it logs the guest PC, CPU, Thumb flag, guest opcodes with their host offsets, and the host code.
- `jitdis.py` turns that into side-by-side guest/host listings with branch targets resolved to DraStic symbols.
- A watcher thread reads results the test ROM writes to guest RAM (fastmem base at `nds_system+0x36d1e20`).

**Observations that confirm the static model**

- The register map matches (r0–r12 in w13–w25, sp in w26, lr in w27).
- Literal pools are constant-folded.
- Loads are inlined; stores always call a stub.
- One store into translated code retranslated every ARM9 main-RAM block, and execution resumed in a new block at
  the instruction after the store.
- An I-cache-invalidate `mcr` emits no host code.

**Example.** Part of a real translated block from the test ROM, ARM9 at 0x02000060. The full listing and
eight more are in `dynamic_examples.md`:

```
guest                         host (AArch64)
                              tbz  w12, #31, #8           ; cycles left? else
                              bl   recompiler_cpu_next_action_arm9_to_arm7
mov  r0, #0x7f000000          mov  w13, #0x7f000000       ; r0 = w13
mov  r1, #0x81000000          mov  w14, #0x81000000       ; r1 = w14
adds r2, r0, r1               adds w15, w13, w14          ; guest NZCV = host NZCV
adcs r3, r0, #1               mov  w1, #1 ; adcs w16, w13, w1
subs r4, r2, r3               subs w17, w15, w16          ; no carry inversion needed
rsbs r6, r5, #0               negs w19, w18
eors r7, r6, r0               eor  w20, w19, w13 ; ands w20, w20, w20   ; logical S: N/Z via ANDS
cmp  r10, r9                  subs w3, w23, w22           ; result to scratch
```

## 11. Hook points for SuperDrastic

| Hook | Address | Use |
|---|---|---|
| `cpu_translate_block` | 0x95e20 | the only codegen entry; called only from `cpu_block_create` |
| `cpu_block_lookup_base` | 0x2db10 | the miss path; wrap it to log or prefetch translations |
| `block_indirect_branch` | 0x8cb3c | indirect-branch dispatch; w0 = target PC, bit 0 = Thumb |
| `cpu_translate_get_pc(cpu[0x2368], cpu[0x2360])` | — | recovers the guest PC from inside a memory slow path |
| `cpu_block_flush(cpu, addr)` | 0x2e9a0 | `addr = 0xffffffff` flushes everything; needed after patching guest code from outside |

Inside a memory slow path, guest r0–r5 are spilled at `cpu+0x22f4..0x2308` and r6–r14 are still in host
`x19`–`x27`.

## Reproducing

`tools/` holds the setup used here:

| File | Purpose |
|---|---|
| `reamcpd.py` + `reaq` | a persistent REA MCP session, so Ghidra imports once (~70 s) and then answers in milliseconds |
| `dump.py` | batch-decompiles a symbol range to local files for private analysis. Its output is DraStic-derived: keep it out of the repo |
| `dpull.py` | pulls Docker Hub image layers, which is how Ghidra 12.1.4 was obtained (`blacktop/ghidra:12.1.4`) when GitHub release downloads were blocked |
| `env.sh.example` | environment |

```sh
npm i -g rea-agents          # REA 6.4.0
python3 tools/dpull.py blacktop/ghidra 12.1.4 layers && tar xzf layers/layer08.tgz   # -> ghidra/
. tools/env.sh               # GHIDRA_INSTALL_DIR, JAVA_HOME (JDK 21), REA_ANALYSIS_PROVIDER=ghidra
python3 tools/reamcpd.py /tmp/rea.sock &     # reaq reads $REA_SOCK (default /tmp/rea.sock), then:
reaq open_binary '{"path":"/abs/drastic","provider_id":"ghidra"}'
reaq procedure_pseudo_code '{"procedure":"cpu_translate_block"}'
```
