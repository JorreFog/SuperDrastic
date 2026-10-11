# Dynamic verification: DraStic's JIT running under qemu-aarch64 (x86-64 host)

> **Errata (from the consistency review in review.md, checked against the binary; these take precedence):**
> - Trailer +0x10 holds the size in halfwords, not the instruction count.
> - SMC invalidation always flushes the whole cache (main + ITCM for main RAM), so the 'granularity unknown' item is resolved (see dynamic2.md).


All scripts and outputs are in `$S/work/dyn/`. Annotated guest-to-host examples are in `dynamic_examples.md`.
Status: goals 1, 2 and 3 were all reached, and so was the differential test the lead asked for (interpreter vs JIT).

## What runs

| piece | source | notes |
|---|---|---|
| qemu-aarch64 10.2.3 (static x86-64) | Docker Hub `tonistiigi/binfmt:latest` (amd64), layer00 `usr/bin/qemu-aarch64` via `$S/tools/dpull.py` | **This build is meant for binfmt_misc preserve-argv0, so it treats the first guest argument as argv[0].** Run `qemu-aarch64 ./drastic ./drastic rom.nds` (program name twice). Without that, DraStic sees argc=1 and opens its file-browser menu. qemu-arm from the same layer is used to cross-check ARMv5TE semantics (`-cpu arm946`). |
| arm64 sysroot (Ubuntu 24.04 + SDL2 and deps) | ports.ubuntu.com (reachable), `setup_sysroot.sh` (the sysroot part of SuperDrastic/tools/sim/setup.sh) | 80 MB |
| DraStic r2.5.2.2 | `$S/bin/drastic` (md5 17550db7…, the same file that is in ROCKNIX/packages drastic.tar.gz) | the free BIOS files `drastic_bios_arm{9,7}.bin` come from that tarball (`system/`). drastic.cfg is ROCKNIX's `drastic.cfg.rgds` |
| display | `Xvfb :97`, `SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software SDL_AUDIODRIVER=dummy` | `--benchmark` was not needed |
| shim | `jitdump.c` -> `jitdump.so` (clang --target=aarch64-linux-gnu --sysroot, lld) | includes simshim.c (SDL_JoystickName(NULL), remap_file_pages, crash handler) |

- With no flag, the JIT is the default: the log shows "Calling recompiler event update handler" and blocks get translated.
  "Using recompiler." is printed only when `--recompiler` is passed (process_arguments). `--interpreter` works too. [confirmed]
- Without Nintendo BIOS/firmware, DraStic loads the free DraStic BIOS ("Can't find Nintendo ARM9 BIOS. Trying free DraStic ARM9 BIOS.").
  Homebrew ROMs boot directly. The first ARM9 blocks it translates are BIOS IRQ-vector code at 0xffff0108/0xffff0008. [confirmed]
- A `timeout` SIGTERM does not stop qemu+DraStic, so run.sh uses `timeout -s KILL`. Speed: the 60 s budget was plenty
  (the diff ROM finishes in a few seconds after boot).

## The hook (jitdump.c)

- cpu_translate_block (VA 0x95e20) is called only from cpu_block_create (bl at 0x2ce8c). It is an internal direct call,
  so LD_PRELOAD symbol interposition cannot reach it. Instead the shim patches it inline from a constructor: base from
  dl_iterate_phdr, a build check (SHA-256 of the executable, plus a hash guard over the 16 entry bytes; see
  patches/fixes.c `jf_inline_hook`), mprotect RWX, then our 16-byte patch (load-literal into x16, branch to x16,
  64-bit hook address) over the entry. Our trampoline
  re-executes the four displaced prologue instructions (none of them depends on the PC) and jumps back to entry+16.
  qemu-user copes with the patched text without problems.
- The signature is `uint32_t *cpu_translate_block(block_desc *b, cpu_t *cpu, u32 pc, u32 thumb)`. It returns the start of
  the host code. Facts used, and confirmed at runtime (output is consistent with DraStic's own logger):
  - `code[-1]` = offset of the block metadata from `*(cpu+0x2088)` (the translation-cache base). Metadata: +4 = pc|thumb,
    +0x10 u16 = insn count | flags<<12, +0x12 u16 = host code bytes. This agrees with the metadata-writing code at the
    end of cpu_translate_block (around VA 0x9613c-0x96144).
  - `*(u16*)(b+0x20)` = number of guest instructions. `*(b+8)` = array of 0x20-byte instruction records, and
    `record+8` = host address where that instruction's code starts (cpu_translate_block stores its current emit pointer
    there just before it translates each instruction with cpu_translate_instruction). jitdump uses this to tag each guest
    instruction with its host offset.
  - `*(int*)(cpu+0x210c)` = CPU type: **1 = ARM9, 0 = ARM7** (cpu_type_str: blocks at 0xffff0108 print "(ARM9)").
  - `cpu+0x23d0` = memory/bus context passed to load_memory32/16 (VA 0x196c0/0x194f0). The shim calls these to read guest code.
  - DraStic's own `cpu_translate_log_block(base, FILE*, code)` (VA 0x96570) works when called with
    `base = *(cpu+0x2088) - 0x8c000` (it computes metadata as base+0x8c000+code[-1] and the CPU as base+0x15c7d50
    (ARM9) / +0x25ce340 (ARM7)). This means `*(cpu+0x2088)` = nds_system+0x8c000. Its output goes to jit_log.txt. DraStic's
    AArch64 disassembler prints some branch targets wrong (e.g. cbz targets offset by 0x800000), so use jitdis.py/llvm-mc instead.
- The result watcher (for test ROMs) is a thread that reads `map_base = *(u64*)(nds_system + 0x36d1e20)`. This is the
  "Memory map offset" that main() prints, and the shim prints the same value. Guest main RAM lives at map_base+guest_addr
  (direct-mapped). When guest 0x02200000 == 'DONE', it dumps the result words and exits. [confirmed]

## How to rerun

```
S=$S; D=$S/work/dyn
# one-time setup (already done): sysroot, qemu, BIOS, cfg
python3 -I $S/tools/dpull.py tonistiigi/binfmt latest $D/dl/binfmt; tar xzf $D/dl/binfmt/layer00.tgz -C $D/dl/binfmt/x usr/bin/qemu-aarch64
$D/setup_sysroot.sh $D            # ports.ubuntu.com -> $D/sysroot
# (drastic/, drastic/system/drastic_bios_*.bin, drastic/config/drastic.cfg: see the table above)
cd $D && clang --target=aarch64-linux-gnu --sysroot=$D/sysroot -fuse-ld=lld -O1 -shared -fPIC -o jitdump.so jitdump.c
# test ROMs
python3 $D/rom/build.py [name] [NO_SMC] [NO_MCR]    # -> rom/out/jittest*.nds (copy into drastic/roms/)
python3 $D/diff/build.py                           # -> diff/out/diff.nds + diff/out/tests.txt (expected values)
# run (Xvfb :97 is started if needed); outputs in $OUT: drastic.log, jit_raw.txt, jit_log.txt, result.txt
OUT=$D/out_jt JITDUMP_MAX=120 $D/run.sh drastic/roms/jittest.nds 15 --recompiler
python3 -I $D/jitdis.py $D/out_jt --cpu 9 > $D/out_jt/merged.txt       # guest + host, LLVM disassembly, symbols
# differential
OUT=$D/out_diff_jit $D/run.sh drastic/roms/diff.nds 60 --recompiler
OUT=$D/out_diff_int JITDUMP_LOG= $D/run.sh drastic/roms/diff.nds 60 --interpreter
python3 -I $D/diff/compare.py $D/diff/out/tests.txt $D/out_diff_int/result.txt $D/out_diff_jit/result.txt
```
Saved outputs: `out_jt/merged.txt` (jittest, first translation of every block), `out_diff_jit/merged.txt` (34k lines,
every diff-ROM block), `out_diff_compare.txt` (all mismatches and the per-class table), `out_{nomcr,nosmc,none}/jit_raw.txt`.

## Findings from the dynamic runs

### SMC / cache invalidation (jittest variants, 30 s each; count = translations per guest PC)
| variant | str to code (smc_insn) | mcr p15,c7,c5,0 (ICache inval) | translations per main-RAM block |
|---|---|---|---|
| jittest_none | no | no | 1 |
| jittest_nosmc | no | yes | 1 |
| jittest_nomcr | yes | no | ~8000 (every block, every loop iteration) |
- **A guest store into translated code invalidates every ARM9 main-RAM block in the test (25 blocks, ~600 B of code),
  not only the block that was written.** All test code sits in one 1 KB page, so the granularity (page, region, or whole
  main-RAM cache) is not established. Next step: put a block >=64 KB away. [confirmed: invalidation of the
  neighbours; granularity unknown]
- After the SMC store, execution continues in a new block that starts at the instruction after the store (0x020001d4)
  instead of finishing the current translated block. [confirmed: block 020001d4 exists only in the SMC variants]
- `mcr p15,0,rX,c7,c5,0` produces **no host code** (guest 0x020001d8 has an empty host range) and causes no
  retranslation. [confirmed]
- The translator constant-folds PC-relative loads from guest code at translation time. `ldr r10,[pc,#-4]` becomes
  `mov w23,#0xe12fff1e` (the literal is the next guest instruction), and `ldr r0,=buf` becomes movz/movk. This is safe only
  because a store to that code retranslates it. [confirmed, example 3]

### Differential test: interpreter vs recompiler (ARM9, ARM mode, diff.nds, 1311 result words)
The expected values come from the ARMv5TE model in diff/build.py. I cross-checked the SMLALBB and QDADD expectations with
qemu-arm `-cpu arm946` ($D/chk): it gave ffffffe6/00000000, 0x21 and 3ffffff0/00000001, the same as the model.
Flags are preset with `mrs/bic/orr/msr cpsr_fc` (see the MSR finding). Words are result + NZCVQ (CPSR>>27).

| class | words | int != exp | jit != exp | int != jit |
|---|---|---|---|---|
| movs Rd,Rm,ASR Rs | 224 | 0 | 24 | 24 |
| movs LSL/LSR/ROR Rs | 3x224 | 0 | 0 | 0 |
| qadd | 22 | 0 | 10 | 10 |
| qsub | 22 | 0 | 0 | 0 |
| qdadd | 22 | 10 | 16 | 21 |
| qdsub | 22 | 10 | 12 | 17 |
| smlaBB/BT/TB/TT | 4x24 | 0 | 4 each (Q flag only) | 4 each |
| smlalBB/BT/TB/TT | 4x36 | 17-19 | 19-21 | 9-10 |
| smulxy, smulwy, smlawy | 6x8, 2x16 | 0 | 0 | 0 |
| MSR probes | 7 | 0 | 3 | 3 |

1. **MOVS r0,r1,ASR r2: JIT carry bug confirmed.** For 1<=n<=31 the JIT gives C = bit n of Rm. It should be bit n-1.
   The interpreter is correct. Examples (NZCVQ, V preset to 1):
   `r1=2,n=1`: exp/int 00010 (C=0), jit 00110 (C=1). `r1=1,n=1`: exp/int 01110 (Z,C=1), jit 01010 (C=0).
   `r1=0x80000001,n=1`: exp/int 10110, jit 10010. `r1=0x80000000,n=31`: exp/int 10010, jit 10110. Shift amounts 0, >=32
   and 0x101 (low byte 1, same as 1) agree apart from that. Cause: the helper arm64_shift_reg_flags_asr (VA 0x8f8f4)
   does compute "amount - 1" into a temporary (at 0x8f900), but the very next instruction (0x8f904) overwrites that
   temporary with the value shifted by the full amount, so the carry is extracted from bit n instead of bit n-1. The
   generated code reaches the helper with the old NZCV in w2, Rm in w1 and Rs&0xff in w3, then sets N/Z with
   `ands Rd,w1,w1` and merges the helper's C back with bfxil (example 9). [confirmed]
2. **QADD: JIT bug confirmed. Rn is zero-extended.** The emitted code is `sxtw x14,w14; add x13,x14,w15,uxtw; sub x0,x13,w13,sxtw; ...`.
   `qadd 5,-1`: exp/int 00000004 Q=0, jit 7fffffff Q=1. `qadd 0x80000000,-1`: exp/int 80000000 Q=1, jit 7fffffff Q=0.
   `qadd -1,-1`: exp fffffffe, jit 7fffffff. Any negative Rn gives a wrong result. QSUB (`sub x13,x14,w15,sxtw`) is correct. [confirmed]
3. **QDADD/QDSUB: JIT bug confirmed. The doubled value is computed and then never used.** The emitted code is `add w1,w15,w15` followed by
   the QADD/QSUB sequence on the original w15 (example 9). So the JIT computes QDADD as `sat(Rm + zext(Rn))` and QDSUB as `sat(Rm - Rn)`:
   `qdadd 1,0x10`: exp 00000021, **jit 00000011**. `qdsub 1,0x10`: exp ffffffe1, jit fffffff1.
   **The interpreter is also wrong for positive Rn.** `qdadd 1,0x10` gives 00000000 Q=1, and `qdadd 0x12345678,0x01000000` gives
   12345677 Q=1. It saturates the doubled value to 0xffffffff (-1) for any positive Rn. Negative Rn is correct.
   (QDSUB the same: `qdsub 1,0x10` -> int 00000002.) [confirmed, from the values; the interpreter code was not inspected]
4. **SMLAxy Q flag (JIT): wrong when the accumulator is negative.** The emitted code is
   `sxth w0,Rm; sxth w1,Rs; asr w16,Racc,#0; smaddl x13,w0,w1,x16; sub x0,x13,w13,sxtw; cbz x0,...; set Q`.
   `asr w16,w16,#0` zero-extends the accumulator into x16, so the 64-bit overflow check sees a large positive accumulator. As a result
   `smlabb (-2*5)+0x80000000`: exp/int Q=1, jit Q=0, and `smlatb (3*5)+0x80000000`: exp/int Q=0, jit Q=1. The results
   themselves are correct. SMULxy, SMULWy and SMLAWy match exactly. [confirmed]
5. **SMLALxy: both interpreter and JIT are wrong. Their low words agree with each other, the high words differ.** Example:
   `smlalbb r3,r4,r1,r2` with r1=0003fffe, r2=7fff0005, acc=0:fffffff0. exp lo/hi = ffffffe6/00000000, int and jit = 0003ffee/00000001.
   BT and TT give the same wrong low word as each other (7ffdbff1), so the half selection is broken too. The JIT emits
   `asr w0,w14,#1; sbfx w1,w15,#1,#15; bfi x16,x17,#32,#32; smaddl x16,w0,w1,x16; lsr x17,x16,#32`. The operand extraction
   uses lsb=1 instead of a 16-bit sign-extend of the selected half: 0x3fffe>>1 = 0x1ffff, bits[15:1] of 5 = 2, and the
   product 0x3fffe matches what was observed. On the high word the JIT gives e.g. 00001000 / 00000ffe / 80000fff where the interpreter gives
   00000000 / fffffffe / 7fffffff. [confirmed values; the cause of the extraction is likely]
6. **MSR CPSR_f (flags field only) cannot clear Q in the JIT.** Probes: after a saturating QADD sets Q,
   `msr cpsr_f,#0` and `msr cpsr_f,r0(=0)` leave Q=1 in the JIT. The interpreter clears it. `msr cpsr_fc,r0` clears it in both.
   The JIT translates `msr cpsr_f,#imm` as just `movz w0,#imm>>16,lsl#16; msr nzcv,x0`. Q lives in the CPSR word at
   [x28,#0x23c0], and MRS reads `mrs nzcv` plus `bfxil` of bits 0..27 from [x28,#0x23c0], so Q (bit 27) stays sticky. [confirmed]

### Other things the examples confirm (details in dynamic_examples.md)
- x28 = CPU state, x9 = x28+0x23d0, x10 = memory-map base (fastmem: `ldr wT,[x10,wAddr,uxtw]`), w12 = cycle countdown.
  Every block starts with `tbz w12,#31,+8; bl recompiler_cpu_next_action_arm9_to_arm7`. Guest-register mapping observed in ARM9 blocks:
  r0..r10 -> w13..w23, r11 -> w24, r12 -> w25, sp(r13) -> w26, lr(r14) -> w27. w0..w3 are scratch, x16/x17 are scratch for 64-bit work
  (corrected in the errata of dynamic_examples.md: x16/x17 are guest r3/r4). [confirmed]
- Loads are inlined with a fastmem check: `and w1,addr,#0xfc000003` (word), `#0xfc000001` (half) or `#0xfc000000` (byte),
  then `cbz w1,fast` and otherwise a branch to an out-of-line slow-path stub placed far away in the cache. **Stores are never inlined**
  (corrected in the errata of dynamic_examples.md: SP-relative stores are inlined on the ARM9). Otherwise they always
  call arm64_store_memory{8,16,32}_arm9, which presumably is where SMC detection lives. LDRD calls arm64_load_memory64.
- STM/push to SP is inlined: registers are spilled to [x9-0x120..] and then `bl arm64_store_blockN`. LDM to a non-SP base is inlined as fastmem `ldp`/`ldr`.
- Conditional instructions become a branch over the code using the inverted condition (`b.ne +8` for EQ). Flags stay in host NZCV.
  CMP/CMN/TST/TEQ write w3 as a dummy destination.
- Liveness-based flag elision: S-suffixed logical ops whose C/V outputs are dead become `op; ands Rd,Rd,Rd`. When C/V are live,
  the code is wrapped in `mrs x2,nzcv ... mrs x1,nzcv; bfxil w1,w2,#0,#30; msr nzcv,x1`, which keeps the old C and V.
- Direct branches and BL to an already-translated block link directly (`b <host block>`). BL loads lr (w27) with the return
  address. Indirect branches (bx lr, pop pc, ldr pc) do `mov w0,target; b block_indirect_branch`. A not-yet-translated
  target goes through a stub (e.g. `b 0x...` outside the cache's block area, at 0x02000188 / bne).
- Thumb is translated straight to the same host idioms (example 7): `movs` becomes `mov` + `ands Rd,Rd,Rd`, and push becomes an
  arm64_store_block2 call.

## What did not work or was not done
- Docker Hub `multiarch/qemu-user-static` was not tried: tonistiigi/binfmt worked first time. download.qemu.org and
  snapshot.debian.org are blocked (403). github.com git clones work.
- llvm-mc's armv5te triple rejects MUL/MLA/UMULL/SMLAL, so jitdis uses the armv6 triple for guest ARM code.
- Not done: SMC invalidation granularity, ARM7-side blocks (only BIOS ARM7 blocks appear: the test ARM7 is a halt
  loop), Thumb differential tests, and a check of the interpreter source for the QDADD/SMLALxy bugs.
