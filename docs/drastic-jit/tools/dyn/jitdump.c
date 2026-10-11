/* jitdump.so: LD_PRELOAD shim for DraStic r2.5.2.2 (ROCKNIX drastic_aarch64; identity checked by SHA-256, see
 * fixes.c) running under qemu-aarch64 user mode. Includes simshim.c (SuperDrastic/tools/sim) for SDL_JoystickName(NULL),
 * remap_file_pages and the crash handler, and adds an inline hook on cpu_translate_block (file VA 0x95e20):
 * after each translation it writes
 *   - JITDUMP_RAW  (default jit_raw.txt):  machine-readable: B <cpu> <thumb> <guest pc> <ninsn> <host addr> <nbytes>
 *                                          G <addr> <opcode> <host offset of its code>
 *                                          H <hex words of host code> (emitted AArch64, little-endian words)
 *   - JITDUMP_LOG  (default jit_log.txt):  DraStic's own cpu_translate_log_block() output (guest disasm with its
 *                                          ARM/Thumb disassembler + host disasm with disasm_arm64_instruction)
 *   - with JITDUMP_FLUSH=1 (merged from the dyn2 shim): also hooks cpu_block_flush and cpu_block_modify_itcm and logs
 *                                          F <cpu> <guest addr> ph=<phase>   (block flush request)
 *                                          I <cpu> <guest addr> ret=<result> ph=<phase>   (ITCM write check)
 *                                          and appends ph=<phase> to B lines (phase = guest word 0x02200008)
 * Hooks are installed by jf_inline_hook() (fixes.c), only when the executable's SHA-256 is the known build: the first
 * 4 instructions of each function are replaced by "ldr x16,#8; br x16; .quad hook", and a trampoline runs the
 * displaced instructions (copied at run time, ADRP re-materialised) and continues at +16. */
#define _GNU_SOURCE
#include "simshim.c"
#include <stdint.h>
#include <stdlib.h>
#include "fixes.c"   /* JITFIX runtime patches */

#define VA_TRANSLATE_BLOCK 0x95e20
#define VA_LOG_BLOCK       0x96570
#define VA_LOAD_MEMORY32   0x196c0
#define VA_LOAD_MEMORY16   0x194f0
#define VA_BLOCK_FLUSH     0x2e9a0   /* cpu_block_flush */
#define VA_MODIFY_ITCM     0x2e220   /* cpu_block_modify_itcm */
/* FNV-1a of the 16-byte entry windows we overwrite (guards against an already-modified entry) */
#define FNV_TRANSLATE_BLOCK 0xb248f673u
#define FNV_BLOCK_FLUSH     0xf26bb55fu
#define FNV_MODIFY_ITCM     0xf38ed664u

typedef uint32_t *(*translate_fn)(void *block, void *cpu, uint32_t pc, uint32_t thumb);
typedef void (*logblock_fn)(void *base, FILE *f, uint32_t *code);
typedef uint32_t (*load32_fn)(void *mem, uint32_t addr);
typedef uint16_t (*load16_fn)(void *mem, uint32_t addr);

static translate_fn orig_translate;
static FILE *fraw, *flog;
static long nblocks, maxblocks = 200000;
static int log_flush;                                   /* JITDUMP_FLUSH */

#define VA_NDS_SYSTEM 0x3fc000
#define NDS_MAPBASE_OFF 0x36d1e20
/* phase marker: guest word 0x02200008 (test ROMs write their phase number there) */
static unsigned phase(void) {
    uint64_t m = *(volatile uint64_t *)(base + VA_NDS_SYSTEM + NDS_MAPBASE_OFF);
    if (m == 0 || m == ~0ULL) return 0xffffffff;
    return *(volatile uint32_t *)(m + 0x02200008);
}
typedef void (*flush_fn)(void *cpu, uint32_t addr);
typedef int (*itcm_fn)(void *cpu, uint32_t addr);
static flush_fn orig_flush;
static itcm_fn orig_itcm;
static void hook_flush(void *cpu, uint32_t addr) {
    if (fraw) { fprintf(fraw, "F %d %08x ph=%u\n", *(int *)((uint8_t *)cpu + 0x210c), addr, phase()); fflush(fraw); }
    orig_flush(cpu, addr);
}
static int hook_itcm(void *cpu, uint32_t addr) {
    int r = orig_itcm(cpu, addr);
    if (fraw) { fprintf(fraw, "I %d %08x ret=%d ph=%u\n", *(int *)((uint8_t *)cpu + 0x210c), addr, r, phase()); fflush(fraw); }
    return r;
}

static uint32_t *hook_translate(void *block, void *cpu, uint32_t pc, uint32_t thumb) {
    uint32_t *code = orig_translate(block, cpu, pc, thumb);
    if (code && jitfix_applied) {
        uint8_t *cb = *(uint8_t **)((uint8_t *)cpu + 0x2088);
        jitfix_post(block, cpu, thumb, code, *(uint16_t *)(cb + code[-1] + 0x12));
    }
    if (!code || nblocks >= maxblocks) return code;
    nblocks++;
    /* code[-1] = offset of the block's metadata from *(cpu+0x2088); metadata: +4 guest pc|thumb,
     * +0x10 u16 (insn count & 0xfff, flags << 12), +0x12 u16 bytes of host code */
    uint8_t *cbase = *(uint8_t **)((uint8_t *)cpu + 0x2088);
    uint8_t *meta = cbase + code[-1];
    uint16_t f10 = *(uint16_t *)(meta + 0x10), nbytes = *(uint16_t *)(meta + 0x12);
    uint32_t mpc = *(uint32_t *)(meta + 4);
    int cputype = *(int *)((uint8_t *)cpu + 0x210c);
    int ninsn = *(uint16_t *)((uint8_t *)block + 0x20);   /* instruction records in the block descriptor */
    if (fraw) {
        fprintf(fraw, "B %d %u %08x %d %p %u meta_pc=%08x f10=%04x", cputype, thumb, pc, ninsn, (void *)code,
                nbytes, mpc, f10);
        if (log_flush) fprintf(fraw, " ph=%u", phase());
        fputc('\n', fraw);
        void *mem = (uint8_t *)cpu + 0x23d0;
        load32_fn l32 = (load32_fn)(base + VA_LOAD_MEMORY32);
        load16_fn l16 = (load16_fn)(base + VA_LOAD_MEMORY16);
        uint32_t a = pc & ~1u;
        /* instruction records: *(block+8) + 0x20*i; record+8 = host address of the instruction's code
         * (cpu_translate_block stores the emit pointer there before calling cpu_translate_instruction) */
        uint8_t *recs = *(uint8_t **)((uint8_t *)block + 8);
        for (int i = 0; i < ninsn; i++) {
            long off = (long)(*(uint8_t **)(recs + 0x20 * i + 8) - (uint8_t *)code);
            if (thumb) { fprintf(fraw, "G %08x %04x %ld\n", a, l16(mem, a), off); a += 2; }
            else       { fprintf(fraw, "G %08x %08x %ld\n", a, l32(mem, a), off); a += 4; }
        }
        fputs("H", fraw);
        for (unsigned i = 0; i < nbytes / 4u; i++) fprintf(fraw, " %08x", code[i]);
        fputs("\n", fraw);
        fflush(fraw);
    }
    if (flog) {
        /* cpu_translate_log_block(base, f, code): metadata = base + 0x8c000 + code[-1], so base = cbase - 0x8c000 */
        ((logblock_fn)(base + VA_LOG_BLOCK))(cbase - 0x8c000, flog, code);
        fflush(flog);
    }
    return code;
}

/* Result watcher (for the differential-test ROM): polls guest main RAM through DraStic's direct memory map
 * (host = map_base + guest address; map_base = nds_system+0x36d1e20, the "Memory map offset" main() prints).
 * When guest 0x02200000 holds 'DONE', writes the n words at 0x02200100 (n at 0x02200004) to $JITDUMP_RESULT
 * as hex lines and exits the process. */
static const char *result_path;
static void *result_watch(void *a) {
    volatile uint64_t *mb = (uint64_t *)(base + VA_NDS_SYSTEM + NDS_MAPBASE_OFF);
    int seen = 0;
    for (;;) {
        usleep(200000);
        uint64_t m = *mb;
        if (m == 0 || m == ~0ULL) continue;
        if (seen++ < 15) continue;                           /* give DraStic 3 s to map main RAM */
        volatile uint32_t *r = (uint32_t *)(m + 0x02200000);
        if (r[0] != 0x454E4F44) {
            /* JITDUMP_PARTIAL=<n> (from work/dyn2): keep $JITDUMP_RESULT.partial updated with words 0x02200000.. */
            const char *pn = getenv("JITDUMP_PARTIAL");
            if (pn) {
                char path[4096]; snprintf(path, sizeof path, "%s.partial", result_path);
                FILE *f = fopen(path, "w");
                if (f) { for (long i = 0; i < atol(pn) && i < 4096; i++) fprintf(f, "%08x\n", r[i]); fclose(f); }
            }
            continue;
        }
        uint32_t n = r[1];
        FILE *f = fopen(result_path, "w");
        if (f) { for (uint32_t i = 0; i < n && i < 65536; i++) fprintf(f, "%08x\n", r[64 + i]); fclose(f); }
        fprintf(stderr, "[jitdump] map base %#lx: DONE, %u result words -> %s\n", (unsigned long)m, n, result_path);
        if (fraw) fflush(fraw);
        if (flog) fflush(flog);
        _exit(0);
    }
    return 0;
}

__attribute__((constructor(200))) static void jitdump_init(void) {
    if (!base) dl_iterate_phdr(cb, 0);
    const char *e = getenv("JITDUMP_MAX"); if (e) maxblocks = atol(e);
    const char *rp = getenv("JITDUMP_RAW"), *lp = getenv("JITDUMP_LOG");
    fraw = fopen(rp ? rp : "jit_raw.txt", "w");
    if (!(lp && !*lp)) flog = fopen(lp ? lp : "jit_log.txt", "w");
    result_path = getenv("JITDUMP_RESULT");
    if (result_path && *result_path) { pthread_t t; pthread_create(&t, 0, result_watch, 0); }
    jitfix_init();
    if (!jf_identity_ok()) { fprintf(stderr, "[jitdump] not the known DraStic build, not hooking\n"); return; }
    uint32_t *tr = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tr == MAP_FAILED) { perror("[jitdump] mmap"); return; }
    uint32_t *fn = (uint32_t *)(base + VA_TRANSLATE_BLOCK);
    orig_translate = (translate_fn)jf_inline_hook("cpu_translate_block", fn, FNV_TRANSLATE_BLOCK, (void *)hook_translate, tr);
    if (!orig_translate) return;
    fprintf(stderr, "[jitdump] hooked cpu_translate_block at %p (base %#lx)\n", (void *)fn, base);
    const char *fl = getenv("JITDUMP_FLUSH");
    if (!fl || !*fl || !strcmp(fl, "0")) return;
    log_flush = 1;
    orig_flush = (flush_fn)jf_inline_hook("cpu_block_flush", (uint32_t *)(base + VA_BLOCK_FLUSH), FNV_BLOCK_FLUSH,
                                          (void *)hook_flush, tr + 32);
    orig_itcm = (itcm_fn)jf_inline_hook("cpu_block_modify_itcm", (uint32_t *)(base + VA_MODIFY_ITCM), FNV_MODIFY_ITCM,
                                        (void *)hook_itcm, tr + 64);
    if (orig_flush && orig_itcm) fprintf(stderr, "[jitdump] hooked cpu_block_flush and cpu_block_modify_itcm\n");
}
