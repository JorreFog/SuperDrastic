# Dynamic tests, round 2: SMC scope, Thumb and ARM7 differential tests, stored PC value, timing and idle loops

Harness: the one from `dynamic.md` (DraStic r2.5.2.2 under qemu-aarch64 with the jitdump.so LD_PRELOAD shim), copied to
`$S/work/dyn2` (run.sh uses Xvfb :99). One DraStic instance ran at a time. Every result below comes from a run here.
Confidence tags: **[C]** = confirmed by a run (results and/or the log), **[L]** = likely, **[G]** = guess.
"exp" is the value my model expects (ARMv5TE for the ARM9, ARMv4T for the ARM7). Where the model is only my reading of
the ARM ARM or GBATEK and was not checked on hardware, the tables say so.

## 0. What changed in the harness

- The harness inline-hooks two more functions with the same kind of entry patch (now merged into `tools/dyn/jitdump.c`,
  enabled with `JITDUMP_FLUSH=1`):
  - `cpu_block_flush` (VA 0x2e9a0). One of the four displaced prologue instructions (the 4th) is a PC-relative page
    address computation that leaves page 0x15f000 in x23; it cannot be replayed from another address, so our trampoline
    loads that constant into x23 with movz/movk instead. Each call writes `F <cpu 1=ARM9/0=ARM7> <addr> ph=<phase>`.
  - `cpu_block_modify_itcm` (VA 0x2e220). It writes `I <cpu> <addr> ret=<return value> ph=<phase>` after the call returns.
  - Each `B` (block translated) line now also ends in `ph=<phase>`. The phase is the guest word at 0x02200008, read
    through the memory-map base. The test ROMs write their phase number there.
- `smc/mkrom.py`: builds a ROM from any ARM9 .s (load address 0x02000000) plus any ARM7 .s (load address 0x02380000).
  `blocks.py <jit_raw.txt>`: lists translations, flushes and ITCM events per phase. Format: `PC(ninsn)`, `T` prefix =
  Thumb, `/7` suffix = ARM7.
- `tdiff/tgen.py <9|7> <name> <groups>`: generator for differential ROMs (groups: thumb, storepc, shift, alu, mem, immc).
  It writes expected values to `tdiff/out/<name>.tests.txt`. `diff/compare.py` is the same comparer as before, with
  better grouping into classes.
- Rerun: `cd $S/work/dyn2; python3 smc/mkrom.py smc smc/smc9.s smc/arm7mb.s; cp smc/out/smc.nds drastic/roms/;
  OUT=$PWD/o_smc_jit JITDUMP_LOG= ./run.sh drastic/roms/smc.nds 90 --recompiler; python3 blocks.py o_smc_jit/jit_raw.txt`.
  For the interpreter use `--interpreter`. Each ROM finishes in about 4-6 s.
- Outputs are kept in `$S/work/dyn2/o_<rom>_{jit,int}/` (result.txt, jit_raw.txt, drastic.log) and in
  `o_<rom>_compare.txt`. ROMs: smc, smc2, t9, a9, c9 (ARM9), a7, t7 (ARM7), idle.

## 1. SMC invalidation: granularity and scope

ROM `smc/smc9.s` + `smc/arm7mb.s`. The functions, each `mov r0,#imm; bx lr`:
- fA at 0x02000800 and fB at 0x02000900 share one 2 KB page.
- fL (0x02000a00, `ldr r0,[pc]` from a literal at 0x02000a08) is in the same page.
- fC at 0x02020000 is 128 KB away.
- dataD at 0x02020400 is a data word in fC's 2 KB page that no block covers.

The main loop runs in page 0x02000000. Each phase n first calls every function (phase n1), then does one store (n2),
then calls them all again and records r0 (n3).

### 1.1 Results (127 words; interpreter vs JIT differ only in the two ARM7-store words)

| phase | action | static claim | flush log (JIT) | retranslated afterwards | int result | JIT result |
|---|---|---|---|---|---|---|
| 1 | `str` new value into fA (fB is in the same page) | whole main cache flushed | `F 1 02000800 ph=12` | fA, fB, fL, fC, all main-loop blocks, **and the ARM7 block 02380010** | fA=0x11 | 0x11 |
| 2 | `str` into fC (a different page, 128 KB away) | whole main cache | `F 1 02020000 ph=22` | fA, fB, fL, fC, main loop, ARM7 02380010 | fC=0x33 | 0x33 |
| 3 | `str` into dataD (a code page, but not inside any block) | no flush (fine bitmap is 2-byte granular) | none | none | ok | ok |
| 4 | `str` of the unchanged value into fA | ignored (old == new) | none | none | ok | ok |
| 5 | `strb` into fB's immediate byte | flush (check_allocation8) | `F 1 02000900 ph=52` | everything | 0x5B | 0x5B |
| 6 | `str` into fL's literal (it was constant-folded) | flush (fill_allocation on the literal) | `F 1 02000a08 ph=62` | everything; fL is now a real load | 0x66 | 0x66 |
| 7 | second `str` into the same literal | the word is in the modified bitmap, so no folding | **none** | none | 0x77 | 0x77 (correct, because it is now a runtime load) |
| 8 | **ARM7** `str` into fA (mov r0,#0x7a), then ARM9 calls fA | suspected NOT detected | **none** (no `F` from either CPU) | none | **0x7A** | **0x11 (stale code)** |
| 9 | ARM9 `str` into fC (forces a flush), call fA again | - | `F 1 02020000 ph=92` | everything | 0x7A | 0x7A (now picked up) |
| 10 | DMA3 copies 1 word over fB | "DMA modified code" + full flush | `F 1 ffffffff ph=102`; drastic.log: `DMA modified code (2000904 for 1).` | everything, **including the ARM9 BIOS (ffff0108, ffff0008, ffff0628, ffff0018) and the ARM7 BIOS (00001080, 00000008, 00001f00, 00000018)** | 0x8B | 0x8B |
| 11 | ITCM: copy fI to 0x01ff8000, call it, then rewrite its `mov r0,#imm` 24 times (a0/a1 alternating, a2 once) and call it after each rewrite | first rewrites flush ITCM; after more than 8 the word becomes "hot" (poly cache) | `I ... ret=1` + `F 1 01ff8000` **9 times**, then `I ... ret=0` **14 times**, with no flush | 9 ITCM retranslations (one per flush), then only 3 new variants (a1, a0 and a2 values); main-RAM blocks are not retranslated | all 25 correct | all 25 correct |

The interpreter run has no `F`/`I`/`B` lines at all, and drastic.log has no "DMA modified code" line. The SMC machinery
exists only in recompiler mode [C].

Phase timeline from `blocks.py o_smc_jit/jit_raw.txt` (excerpt):
```
ph=12   [FLUSH cpu1 02000800] 02380010(9)/7 02380000(4)/7 02000800(2) 020000c4(5)
ph=13   02000900(2) 020000d8(2) 02020000(2) 020000e0(2) 02000a00(2) 020000e8(2) 020000f0(4)
ph=22   [FLUSH cpu1 02020000] 02380010(9)/7 02000800(2) 02000130(5)        <- fC's page written, fA retranslated
ph=31.. 0200016c(2) ...         ph=33 020001b0(2) ...                       <- dataD store: no flush, fA/fB/fC stay cached
ph=81   ... 020003c4(8) 020003a0(9)    ph=83 020003e4(2) ...                <- ARM7 store: nothing
ph=102  [FLUSH cpu1 ffffffff] 00001080(11)/7 00000008(1)/7 00001f00(4)/7 00000018(1)/7 ffff0108(11) ffff0008(1) ffff0628(6) ffff0018(1) 02380010(9)/7 02000800(2) 020004c0(8)
ph=112  02000520(8) 02000540(7) [ITCMmod 01ff8000 ret=1]x9 [FLUSH cpu1 01ff8000]x9 02000538(2) 01ff8000(2)x12 [ITCMmod 01ff8000 ret=0]x14
```
ITCM sequence in detail (raw log). A `B` line printed just before `I ret=0` is the variant that cpu_block_modify_itcm
translates itself:
```
I 1 01ff8000 ret=1 ph=112 / F 1 01ff8000 ph=112 / B 1 0 01ff8000 2 0x7f98aef48004 ...      (x9: flush + retranslate at the same host address)
B 1 0 01ff8000 2 0x7f98aef48020 ...  I 1 01ff8000 ret=0                                     (10th write: new poly entry, value a1)
B 1 0 01ff8000 2 0x7f98aef4803c ...  I 1 01ff8000 ret=0                                     (11th write: poly entry a0)
I ret=0 x9                                                                                  (poly hits: no translation, no flush)
B 1 0 01ff8000 2 0x7f98aef48058 ...  I 1 01ff8000 ret=0                                     (value a2: 3rd entry)
I ret=0 x3
```

Literal-folding excerpt (fL), first translation vs after the literal was written (phase 62):
```
before:  [02000a00] +008 mov w13,#85                 ; ldr r0,[pc] folded to the constant 0x55
after:   [02000a00] +008 mov w1,#0xa08 ; +00c movk w1,#0x200,lsl#16 ; +010 ldr w13,[x10,w1,uxtw]   ; real load of 0x02000a08
```
After the write, the literal is loaded at run time (fastmem with a constant address, so there is no range check). It is
no longer registered with fill_allocation, so the second literal store (phase 7) flushes nothing [C].

### 1.2 ROM `smc/smc2_9.s` + `smc2_7.s` (more scope checks; int and JIT agree except where noted)

| phase | action | flush log | retranslated | int | JIT |
|---|---|---|---|---|---|
| 2/3 | main-RAM `str` into fA while an ITCM block (0x01ff8000) is cached | `F 1 02000800` | ITCM block 01ff8000 retranslated in ph3, and ARM7 main-RAM blocks 02390000/02380014 too | ok | ok |
| 4/5 | `stmia r0,{r1,r2}` over fA (new values) | `F 1 02000800` (first address) | everything | 0x21 | 0x21 |
| 6/7 | the same `stmia` with **identical** values | `F 1 02000800` again | everything | 0x21 | 0x21 |
| 8/81 | Thumb `strh` into Thumb code fT (cached) | `F 1 02001000` | everything | 0x55 | 0x55 |
| 10/11 | **ARM9** `str` into **ARM7** code f7 (0x02390000, translated by the ARM7) | none | none | 0x71 | **0x70 (stale)** |
| 121/13 | ARM9 SMC store into its own fA (forces a flush) | `F 1 02000800` | 02390000/7 retranslated | 0x71 | 0x71 |

(First-run pitfall: a store into code that an earlier flush had already evicted triggers nothing, because the fine bits
were cleared. Calling the target first fixes the test.)

### 1.3 Static claims: confirmed or refuted

- **Whole main cache flushed on any detected main-RAM code write, at any distance** [C]. A store 128 KB away (phase 2)
  retranslates fA, and a store into fA retranslates fC. The flush addr argument is the stored address.
- **The main cache is shared by both CPUs**: every main-RAM flush started by the ARM9 also retranslates the ARM7's
  main-RAM blocks (02380010, 02390000) [C].
- **The main flush includes ITCM** (smc2 ph3) [C]. **An ITCM-only write does not flush main**: during the 9 ITCM flushes
  no main-RAM block was retranslated. The only exception is 02000538, created once as the resume point after the store [C].
- Detection granularity is 2 bytes (fine bitmap): a data word in a code page (phase 3) causes no flush [C].
- A single store of an unchanged value is ignored (phase 4) [C]. Block stores (STM) have no same-value test (smc2 ph6) [C].
- strb (8-bit) and Thumb strh (16-bit) into code are detected [C].
- Literal folding: the first write into a folded literal flushes. Afterwards the literal is never folded again and is no
  longer watched [C].
- **(d) The ARM7 writing ARM9 code in main RAM is NOT detected. The ARM9 keeps running stale translated code until some
  unrelated flush** [C: interpreter 0x7a, JIT 0x11; no `F` line]. The mirror case is also confirmed: the **ARM9 writing
  ARM7 code is not detected** [C]. This matches "code flags live only in the translating CPU's page table" [L for the
  mechanism; the behaviour is C].
- **(e) DMA over code: "DMA modified code" plus a full flush (addr 0xffffffff, which includes BIOS/alternate blocks for
  both CPUs)** [C]. The message printed 0x2000904 for a 1-word DMA to 0x2000900, so it seems to print the address after
  the transfer [L].
- **(f) ITCM hot-word poly cache confirmed**: 9 modifications flush (ret=1), after which every further write returns 0
  without a flush. Only first-seen (value, addr) pairs are translated: 3 variants for 3 distinct values [C]. Static
  analysis said a hot block would be 1 instruction (exit type 4). Here the hot blocks had 2 instructions (`mov; bx lr`),
  but this test cannot tell them apart because the 2nd instruction ends the block anyway [L].
- New: the jitdump snapshot is taken before `cpu_translate_link_delayed_targets`. A self-loop / delayed link therefore
  shows as `b <translation-cache base>` (e.g. the ARM7 `subs/bne` loop at 02380020 -> 0x7f2d60000000) [C as an
  observation; L that it is patched later, since the loop runs correctly].

## 2. Thumb differential tests (ARM9: ROM t9, 1184 words; ARM7: ROM t7, 1147 words)

The ARM side presets the registers and NZCV, calls the snippet with `ldr r12,=T; mov lr,pc; bx r12`, then stores the
registers and CPSR>>27. Full list: `o_t9_compare.txt`, `o_t7_compare.txt`.

| class | words (t9) | int != exp | JIT != exp | notes |
|---|---|---|---|---|
| lsls/lsrs/rors Rd,Rs (by register) | 184/184/168 | 0 | 0 | |
| **asrs Rd,Rs** | 184 | 0 | **20** | **The ASR carry bug hits Thumb too** (same stub; see below) [C] |
| imm shifts (lsl #0/#1, lsr #1/#32, asr #31/#32) | 24 | 0 | 0 | |
| adds/subs/adcs/sbcs/negs/cmp/cmn (reg, imm3, imm8) | 72+72+24+24+24+25+12 | 0 | 0 | |
| ands/eors/orrs/bics/mvns (C/V kept) | 5x24 | 0 | 0 | |
| muls (v5: C kept) | 24 | 0 | 0 | |
| **tst Rn,Rm** | 12 | 0 | **12** | **The JIT clears C and V** (bare host `ands`) [C] |
| hi-reg add/mov/cmp with r8 | 3 | 0 | 0 | |
| ldr r0,[pc,#4], add r0,pc,#4, mov r0,pc, add r0,pc at T+0 and T+2 | 8 | 0 | 0 | pc alignment correct in both |
| BL pair (callee result, return, LR = next\|1) | 3 | 0 | 0 | |
| **lone BL suffix (0xF802 after `mov lr,r3`)** | 2 | 0 | **2** | **JIT lands at next+4 instead of LR+4**: the static guess is confirmed [C] |
| BLX imm / BLX reg to ARM and back, LR | 4 | 0 | 0 | ARM9 only |
| BX reg -> ARM / -> Thumb, BX PC, MOV PC,Rm, ADD PC,Rm | 5 | 0 | 0 | |
| ldmia r0!,{r1,r2}; stmia (wb, base first in list) | 9 | 0 | 0 | |
| ldmia r0!,{r0,r1} (base in list) | 1 | 1 | 1 | int = JIT = 0x02210008 (writeback wins); my model expected the loaded value. Both DraStic paths treat it like ARM LDM on ARMv5 ("writeback unless base is last") [hardware behaviour not verified] |
| push/pop {.., lr}; pop {r0,pc} -> Thumb; -> ARM (v5 interworking) | 9 | 0 | 0 | |
| Thumb ldr unaligned (rotated), ldrh/ldrsh at odd addresses | 5 | 0 (t9) | 0 (t9) | on the ARM7 (t7) ldrh/ldrsh odd: int = JIT = ARMv5 behaviour (see section 4) |

The ARM7 run (t7) shows the same picture: asrs 20, tst 12 and the lone suffix 2 are JIT-only mismatches; nothing else
differs between int and JIT.

Excerpts (from `o_t9_jit/jit_raw.txt` through jitdis.py):
```
Thumb 02009574: 4677 mov r7,lr ; 469e mov lr,r3 ; f802 (lone BL suffix)
  +008 and w20, w27, w27            ; r7 = lr
  +00c and w27, w16, w16            ; lr = r3   (r3 = w16)
  +010 sub w12, w12, #5
  +014 mov w27, #0x957b ; +018 movk w27, #0x200, lsl #16   ; LR := next|1 = 0x0200957b ...
  +01c mov w27, #0x957b ; +020 movk w27, #0x200, lsl #16   ; ... emitted twice
  +024 add w0, w27, #4              ; target from the NEW LR  (should be old LR + 4)
  +028 orr w0, w0, #0x1 ; +02c b block_indirect_branch
Thumb 02005d6c: 4208 tst r0,r1
  +008 ands w3, w13, w14            ; no mrs/bfxil wrapper -> C=V=0 (compare Thumb ands at 02005c40, which is wrapped)
Thumb 02000040: 4108 asrs r0,r1
  +008 mrs x2,nzcv ; mov w1,w13 ; and w3,w14,#0xff ; bl arm64_shift_reg_flags_asr ; ands w13,w1,w1 ; mrs x1,nzcv ; bfxil w1,w2,#0,#30 ; msr nzcv,x1
```

## 3. Stored PC value (ARM mode; ROM t9 for the ARM9, a7 for the ARM7)

Stored value minus the address of the storing instruction:

| test | expected (GBATEK, from memory: +12 on ARM7TDMI and ARM946E-S) | ARM9 int | ARM9 JIT | ARM7 int | ARM7 JIT |
|---|---|---|---|---|---|
| `str pc,[r4]` | 12 | 8 | 8 | 8 | 8 |
| `str pc,[r5,#4]!` | 12 | 8 | 8 | 8 | 8 |
| `stmia r4,{pc}` | 12 | 8 | 8 | 8 | 8 |
| `stmia r4,{r0,pc}` | 12 | 8 | 8 | 8 | 8 |
| `stmdb sp!,{r0,pc}` | 12 | 8 | 8 | 8 | 8 |
| `mov r1,pc` (read) | 8 | 8 | 8 | 8 | 8 |
| `add r1,pc,r3,lsl r3` (register-shift read of PC; ARM7TDMI gives +12, the ARM ARM calls it unpredictable) | (12) | 8 | 8 | 8 | 8 |

**Interpreter and JIT agree: DraStic stores PC+8 for STR/STM on both CPUs** [C]. Real hardware (both CPUs are
"store PC+12" cores per GBATEK; melonDS's interpreter also adds 4 for STR Rd=15) would give +12. That last point is
recalled, not re-verified here [L]. So this is an emulator-wide deviation, not a JIT bug.

## 4. ARM7 differential (ROM a7, 1250 words: shift, alu (+TST/TEQ), mem, storepc; ARM9 idles in `b .`)

| class | words | int != exp | JIT != exp | int != JIT | notes |
|---|---|---|---|---|---|
| movs LSL/LSR/ROR Rs | 3x168 | 0 | 0 | 0 | |
| **movs ASR Rs** | 168 | 0 | **20** | 20 | same JIT carry bug as on the ARM9 (shared stub arm64_shift_reg_flags_asr) [C] |
| adds/adcs/subs/sbcs/rsbs/rscs; ands/eors/orrs/bics/mvns/movs rrx with shifted operands | 6x24 + 6x24 | 0 | 0 | 0 | |
| mul/mla/umull/smull/umlal/smlal (results; flags not modelled on v4) | 24+24+4x36 | 0 | 0 | 0 | |
| **tst** (reg, `#0xF0000000`, `lsl #0`, `#0xFF`) | 48 | 6 | **36** | 30 | see the shifter-carry table below (identical to the ARM9) |
| teq (reg, `lsr #1`) | 24 | 6 | 0 | 6 | the interpreter ignores the shifter carry of TEQ with a shifted register |
| ldr unaligned (rotated), ldrsb, swp, ldrh/ldrsh at even addresses | 12 | 0 | 0 | 0 | |
| ldrh/ldrsh at odd addresses | 2 | 2 | 2 | 0 | int = JIT = ARMv5 behaviour (aligned halfword) where GBATEK documents ror 8 / LDRSB on the ARM7 [model from memory] |
| ldmia r1!,{r1,r2} / stmia r1!,{r0,r1} (base in list; 3 other LDM/STM words match) | 2 | 2 | 2 | 0 | int = JIT = ARMv5 rules (writeback / store old base) instead of the ARMv4 rules |
| STOREPC | 7 | 5 | 5 | 0 | +8, see section 3 |

**Apart from the bugs it shares with the ARM9 JIT (ASR carry, TST), the ARM7 JIT matches the ARM7 interpreter
exactly** [C]. ARM7 loads are never inlined: every load is
`mov w0,addr; bl arm64_load_memory{32,16}_unsigned; mov wD,w0`, SWP is load call + `arm64_store_memory32_arm7`. On the
ARM7, **PC-relative literals are not constant-folded** (the ARM7 `ldr r1,=..` became `mov w0,#lit_addr; bl arm64_load_memory32_unsigned`) [C; excerpt below].
```
ARM7 02386fd4: e1d400b5 ldrh r0,[r4,#5]  ->  add w0,w17,#5 ; bl arm64_load_memory16_unsigned ; mov w13,w0
ARM7 02386f9c: ldr (pc-relative literal)  ->  mov w0,#0x70a4 ; movk w0,#0x238,lsl#16 ; bl arm64_load_memory32_unsigned ; mov w18,w0
ARM7 0238700c: swp r0,r1,[r2]            ->  mov w0,w15 ; bl arm64_load_memory32_unsigned ; mov w13,w0 ; mov w0,w15 ; mov w1,w14 ; bl arm64_store_memory32_arm7
```
The guest-register mapping on the ARM7 is the same as on the ARM9 (r0 = w13 ...).

### 4.1 Shifter carry-out of logical ops (ROM c9 on the ARM9, plus the alu group on both CPUs; ARM7 results identical)

| operand form | expected C | interpreter | JIT |
|---|---|---|---|
| ANDS/EORS/ORRS/BICS/MOVS/MVNS Rd, Rm, \<imm shift\> | shifter carry | correct | correct |
| ANDS/MOVS/ORRS/MVNS Rd, **#rotated imm** (0x80000000, 0xFF000000, 0xF0000000, 0x3FC, 0xFF00) | imm[31] | **always 0** | **always 0** (`and w2,w2,#0xdfffffff` before the op) |
| ... #unrotated imm (0xFF) | unchanged | correct | correct |
| **TST/TEQ #rotated imm** | imm[31] | **unchanged** | **always 0** |
| **TST Rn, Rm{, \<imm shift\>}** (incl. plain register and lsl #0) | shifter carry (Rm: unchanged), V unchanged | **C unchanged** (wrong when the shift produces a carry) | **C=0 and V=0** (bare `ands w3,..`) |
| TEQ Rn, Rm, \<imm shift\> | shifter carry | **C unchanged** | correct |
| TEQ Rn, Rm | unchanged | correct | correct |

Counts in c9: tst 24 words (int 11 wrong, JIT 16 wrong), teq 24 (11/6), ands/movs/mvns/orrs 14 each (6/6, int = JIT).
Excerpt (c9, ARM9):
```
02000030 e3110102 tst r1,#0x80000000 -> mrs x2,nzcv ; and w2,w2,#0xdfffffff ; ands w3,w14,#0x80000000 ; mrs x1,nzcv ; bfxil w1,w2,#0,#30 ; msr nzcv,x1
020000a8 e3b00102 movs r0,#0x80000000 -> mrs x2,nzcv ; and w2,w2,#0xdfffffff ; mov w13,#0x80000000 ; ands w13,w13,w13 ; mrs ; bfxil ; msr
02000654 movs r0,#0x3fc               -> same `and w2,#0xdfffffff` (C forced to 0 whatever imm[31] is)
a9 02006c70 e1110002 tst r1,r2        -> ands w3,w14,w15                      (no wrapper)
a9 02006d10 e13100a2 teq r1,r2,lsr #1 -> mrs x2,nzcv ; ubfx w3,w15,#0,#1 ; bfi w2,w3,#29,#1 ; eor w3,w14,w15,lsr #1 ; ands w3,w3,w3 ; mrs/bfxil/msr   (correct)
```

## 5. Timing and idle-loop detection (ROM `smc/idle9.s` + `smc/arm7delay.s`)

Timer0 and Timer1 cascaded at 33.5 MHz (1 tick = 2 ARM9 cycles). The ARM7 spins N iterations and then sets a flag
(ARM7 cost = 4 cycles per iteration = 4 ticks).

| test | interpreter | JIT | notes |
|---|---|---|---|
| tight `subs r0,#1; bne` x 1,000,000: ticks | 2,000,014 | 2,000,015 | 4 ARM9 cycles/iter in both (JIT: `sub w12,#4` = 2 insns + 2 branch penalty) |
| iterations of `add; ldrh [VCOUNT]; cmp #100; bne` over 100 lines | **70,999** | **60,856** | the same 213,000 ticks (100 x 2130) in both. Interpreter 6 cycles/iter, JIT 7 (`sub w12,w12,#7`) |
| idle loop `ldr r0,[flag]; cmp #0; beq` until ARM7 sets the flag (ARM7 delay 200,000) | 800,065 ticks | 800,063 | emulated time is set by the ARM7. Idle detection does not change it |
| same loop with `add r6,#1` (not idle): ticks / iterations | 800,059 / **266,684** | 800,059 / **228,586** | interpreter 6, JIT 7 cycles per iteration |
| same loop with a `str` inside | 800,062 | 800,058 | |
| `ldrh [VCOUNT]; cmp #150; bne` from line 0 | 319,500 | 319,500 | |

Idle-loop marker `movn w12,#0` (0x1280000c, w12 := -1, ends the time slice) in the loop's block:

| loop | movn emitted? | static rule |
|---|---|---|
| `ldr r0,[r4]; cmp r0,#0; beq` (ARM9 idleA) | **yes** | invariant base, load only: idle |
| `ldrh r0,[VCOUNT]; cmp; bne` (vwait0, vwait150) | **yes** | polling I/O counts as idle |
| ARM7 mailbox poll `ldr r1,[r0]; cmp r1,#1; bne` | **yes** (ARM7 too) | |
| `add r6,#1; ldr; cmp; beq` | no | r6 varies |
| `ldr; str; cmp; beq` | no | store disqualifies |
| `subs r0,#1; bne` | no | r0 varies |

```
ARM9 02000340: ldr r0,[r4] ; cmp r0,#0 ; beq 02000340
  +008 and w1,w17,#0xfc000003 ; cbz w1,+8 ; b <slow stub> ; ldr w13,[x10,w17,uxtw]
  +018 subs w3,w13,#0
  +01c sub w12,w12,#6 ; b.ne +12 ; mov w12,#-1 (movn) ; b <self, linked later>
```
The static description of idle detection is confirmed [C]. The 6-vs-7-cycle difference shows that the JIT and the
interpreter charge different cycle counts for loads (or for the taken branch) in loops that contain a load. The JIT's
per-block constant is insns + 2 + extra for the load [L about the exact split]. Effect: interpreter and JIT disagree on
how many loop iterations fit in a given emulated time (about 14% fewer under the JIT here).

## 6. Summary: static claims checked

| claim (block_cache.md / translator_core.md) | verdict |
|---|---|
| any detected code write flushes the whole main cache + ITCM | **confirmed** (distance does not matter; ARM7 main-RAM blocks are flushed too) |
| ITCM write flushes only the ITCM cache | **confirmed** |
| same-value single stores ignored; STM has no same-value test | **confirmed** both |
| 2-byte fine granularity (data in a code page is not a hit) | **confirmed** |
| literal folding plus watch; after one write it is never folded again and no longer watched | **confirmed** |
| other-CPU stores not detected (code flags only in the translating CPU's page table) | **confirmed** both directions: ARM7->ARM9 code and ARM9->ARM7 code run stale under the JIT |
| DMA into code: message + flush(0xffffffff) of all caches | **confirmed** (BIOS blocks of both CPUs retranslated) |
| ITCM hot word after >8 modifications, 16-entry (value,addr) poly cache, no flush | **confirmed** (9 flushes, then poly; 3 variants translated) |
| hot ITCM block = 1 instruction | not observable with this test |
| unpaired Thumb BL suffix uses the overwritten LR | **confirmed** (lands at next+4) |
| Thumb is translated through ARM conversion; ARM-side bugs carry over | **confirmed** (ASR carry); new: Thumb/ARM **TST reg** clobbers C/V in the JIT |
| idle loop `movn w12,#0` for invariant polling loops | **confirmed** (memory and I/O polling, ARM9 and ARM7) |
| ARM7 JIT uses non-inline arm64_load_memory* | **confirmed**; also no literal folding on the ARM7 |

New bugs found here (beyond dynamic.md):
1. JIT: TST with a register operand clears C and V [C].
2. JIT: the carry of a rotated immediate in TST/TEQ is forced to 0 [C].
3. Interpreter and JIT: the carry of a rotated immediate in ANDS/MOVS/ORRS/MVNS is forced to 0 [C].
4. Interpreter: TST/TEQ ignore the shifter carry, both for immediate-shifted registers and for rotated immediates [C].
5. Both: STR/STM of PC store +8 instead of +12 [C for DraStic's value; the hardware value is from memory].
6. Both, on the ARM7: ARMv5 semantics for LDRH/LDRSH at odd addresses and for LDM/STM with the base in the list
   [C for DraStic's values; the ARMv4 behaviour is from memory].
7. JIT: cross-CPU SMC is not detected [C].
8. JIT charges 7 cycles where the interpreter charges 6 for a 4-instruction loop with a load [C].

---------------------------------------------------------------------------------------------------------
## Round 3: testing the static review's predictions (review.md, coverage.md)

Sources: `$S/work/dyn2/r3/r3_9.s` (ARM9 tests, one ROM per `-defsym` variant, built as `smc/out/r3_<V>.nds`),
`r3/r3_7.s` + `r3/wd9.s` (ARM7 tests; the ARM9 is a watchdog that writes DONE after 40 frames), and `r3/run3.sh <rom>`
(runs JIT and interpreter with a 20-30 s timeout and prints DONE/crash plus the results). The shim gained
`JITDUMP_PARTIAL=<n>`, which keeps `result.txt.partial` updated, so a hung or crashed run still leaves its phase number
and results. Outputs are in `o3_<rom>_{jit,int}/`.

ARM9 exception handlers: the free DraStic BIOS vectors (0xffff0004/0c) are `b .` loops. To observe UND/abort, the ROM
puts `ldr pc,[pc,#0x18]` vectors in ITCM at 0 and clears CP15 control bit 13 (V). This works in the interpreter. **It
does not work under the JIT, which exposed a new bug (R3-0 below).** So under the JIT, an exception shows up as a hang
in the BIOS loop, and the jitdump log tells which vector it was (block ffff0004 = UND, ffff000c = prefetch abort).

| # | test | spec (ARMv5TE ARM9 / ARMv4T ARM7) | interpreter | JIT | static prediction |
|---|---|---|---|---|---|
| R3-0 | `mcr p15,0,r0,c1,c0,0` clearing V (bit 13), then `mrc` read-back | control = 0x10078, low vectors | 0x00010078, exceptions go to the ITCM vectors | **0x00012078: the write is lost**, exceptions still go to 0xffff00xx | **new**. The JIT helper arm64_update_coprocessor_control (VA 0x8f678) builds the new control value in a register but never writes it back to the CP15 state. It then calls update_coprocessor_control (VA 0x295b0), which takes the control word from offset 0x14 of the CP15 state (load at VA 0x295c0), i.e. the old value. **So under the JIT, every MCR to the CP15 control register has no effect** [C: read-back + disassembly] |
| R3-1a | ARM `mov r0,#0; mov pc,pc; add r0,#1; add r0,#2` | r0 = 2 | 2 | **crash: SIGILL** at host 0x...80bc = 4 bytes after the end of block 02000028 (it ends at the `mov pc,pc`), i.e. the next block's header word | **confirmed**: no code emitted, the host runs off the end of the block |
| R3-1b | `moveq pc,pc` with Z=1 (taken) | 2 | 2 | **3** (acts as a NOP) | **confirmed** |
| R3-1c | `movne pc,pc` with Z=1 (not taken) | 3 | 3 | 3 | - |
| R3-1d | Thumb `mov pc,pc` (0x46ff) then `adds #1; adds #2` | 2 | 2 | **crash: SIGILL** at host 0x...817c = the end of the 1-insn Thumb block 02000128 (12 bytes) | **confirmed** |
| R3-2a | ARM7 `clz r0,r1` (r0 preset 0x55) | UND | **0x55 (NOP)** | **0x55 (NOP)**: no host code at all for 02380028, and it stays inside block 02380000 (14 insns), so it is *not* isolated in its own block | NOP **confirmed** (the interpreter does the same). The "isolated by rec+0x1c" part is refuted |
| R3-2b | ARM7 `blx thumb_fn` (cond NV) | UND / unpredictable (ARM7TDMI: never) | **executed** (r0 = 0x77) | **executed** | **confirmed** (both paths) |
| R3-2c | ARM7 `ldrd r2,r3,[r4]` | UND / unpredictable | **executed** (aaaa0001, bbbb0002) | **executed** | **confirmed** (both paths) |
| R3-2d | ARM7 `strd r2,r3,[r4]` | UND / unpredictable | **executed** | **executed** | **confirmed** (both paths) |
| R3-2e | ARM7 Thumb `blx arm_fn` (suffix 11101) | UND | **executed** (r0 = 0x67) | **executed** | **confirmed** (both paths) |
| R3-3a | ARM9 ARM `bkpt #0` | prefetch abort, vector 0x0C, lr = insn+4 | abort taken, lr - insn = 4, SPSR 0x600000df | abort taken: jumps to **ffff000c** (prefetch-abort vector; hangs in the BIOS loop only because of R3-0) | raise_exception(3,4) confirmed |
| R3-3b | Thumb `bkpt #3` (0xbe03) with 0x1111, 0x2222 pushed | prefetch abort; registers and sp unchanged | **r0 = 0x1111, r1 = 0x2222, sp +8, no abort** (popped) | **same: popped, no abort** | **confirmed: BKPT decoded as POP {r0,r1}**, in the interpreter too |
| R3-3c | Thumb 0xde01 (undefined-cond B), then `adds #1; adds #2; adds #4` | UND (r0 = 7 after the handler returns) | **r0 = 4, no UND** (branch taken) | **r0 = 4, no UND** | **confirmed: always-taken branch**, in the interpreter too |
| R3-4 | interpreter `qdadd/qdsub`, Rn < -2^30 (see below) | saturate 2*Rn to -2^31 | doubled value saturated to **-0x40000000** | different wrong values (JIT ignores the doubling) | **confirmed** |
| R3-5a | `strd r2,r3,[fA]`: the **first** store hits translated code | continue at the next instruction; r5 = 0x11, fA() = 0x42 | correct (0x11, 0x42) | flush `F 1 02000804`, then **resume at a garbage PC 0x0200e92d (Thumb)**: about 200,000 one-instruction Thumb blocks over zeroed RAM (0x0200e92c, ..., 0x0207038a), then **SIGILL** | **confirmed**: wrong resume PC from cpu_translate_get_pc (no PC-metadata entry for the first store) |
| R3-5b | `strd` where only the **second** store hits code | 0x11, 0x43 | correct | correct | consistent (that store has a metadata entry) |
| R3-5c | `swp r4,r1,[r4]` (Rd == Rn), the store hits code | r4 = old word 0xe3a00001; fA() = 0x44 | correct | **r4 = 0x02000804 (Rd never written**: still the address); fA() = 0x44 | **confirmed** |
| R3-5d | `swp r1,r1,[r4]` (Rd == Rm), the store hits code | r1 = old word 0xe3a00044; fA() = 0x45 | correct | **r1 = 0xe3a00045 (Rd never written**: still the stored value) | **confirmed** |
| R3-6a | Q=0, `msr cpsr_c,Rm` with Rm bit 27 = 1, then read Q | Q = 0 (only bits 7:0 are written) | 0 | **1** | **confirmed: whole word stored, Q set** |
| R3-6b | Q=1 (after a saturating qadd), `msr cpsr_c,Rm` with bit 27 = 0 | Q = 1 | 1 | **0** | **confirmed: Q cleared** |
| R3-6c | `msr cpsr_c,Rm` with NZCV bits set in Rm | NZCV unchanged (0) | 0 | 0 | N/Z/C/V live in host NZCV, so the stale image in [x28+0x23c0] is not visible here |
| R3-6d | as 6a, then an unrelated `adds` and mrs | Q = 0 | 0 | 1 | Q set by 6a stays sticky |

QDADD/QDSUB values (result, Q). Interpreter and JIT columns are observed:

| op | Rm (r1) | Rn (r2) | spec | interpreter | JIT |
|---|---|---|---|---|---|
| qdadd | 0 | 0x80000000 | 0x80000000, 1 | **0xc0000000**, 1 (predicted exactly) | 0x7fffffff, 1 |
| qdadd | 0x10 | 0xA0000000 | 0x80000010, 1 | **0xc0000010**, 1 | 0x7fffffff, 1 |
| qdadd | 0 | 0xC0000000 (= -2^30, no saturation) | 0x80000000, 0 | 0x80000000, 0 | **0x7fffffff, 1** |
| qdadd | 5 | 0xBFFFFFFF | 0x80000005, 1 | **0xc0000005**, 1 | 0x7fffffff, 1 |
| qdsub | 0 | 0x80000000 | 0x7fffffff, 1 | **0x40000000**, 1 | 0x7fffffff, 1 |
| qdsub | 0x10 | 0xA0000000 | 0x7fffffff, 1 | **0x40000010**, 1 | **0x60000010, 0** |

These complete the round-1 picture. Interpreter: Rn >= 0 uses -1 as 2*Rn, and Rn < -2^30 uses -2^30 (should be -2^31).
JIT: sat(Rm + zext(Rn)) and sat(Rm - Rn), with the doubling unused.

Round-3 conclusions:
- Every static prediction tested was confirmed, except one detail: on the ARM7, CLZ is a NOP but is *not* split into an
  isolated block.
- Several of the "JIT" predictions turned out to be **shared with the interpreter**:
  - Thumb BKPT -> POP.
  - Thumb 0xDExx -> branch.
  - ARM7 executing CLZ (as a NOP), BLX imm, LDRD/STRD and the Thumb BLX suffix.

  These are decoder-level behaviours of DraStic as a whole.
- JIT-only:
  - `mov pc,pc` (ARM unconditional and Thumb): crash (SIGILL).
  - Conditional `mov pc,pc`: NOP.
  - STRD first-store SMC: runaway at a wrong PC, then crash.
  - SWP with Rd aliasing, store hitting code: Rd not written.
  - MSR CPSR_c: can set/clear Q.
  - **New**: MCR to the CP15 control register is lost entirely (R3-0).
