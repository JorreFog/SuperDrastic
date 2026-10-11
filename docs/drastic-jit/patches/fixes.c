/* fixes.c: runtime patches for twelve JIT bugs in DraStic r2.5.2.2 (ROCKNIX drastic_aarch64).
 * #included by jitdump.c, which provides `base` (DraStic's load address, from simshim.c).
 *
 * Enable with env JITFIX:  JITFIX=1 or JITFIX=all -> all fixes;  JITFIX=0x<hex mask> -> selected fixes:
 *   0x01 MOVS Rd,Rm,ASR Rs carry        static: shift amount register in arm64_shift_reg_flags_asr
 *   0x02 QADD sign-extension of Rn      static: extend option of an emitter template (cpu_translate_instruction)
 *   0x04 QDADD/QDSUB doubling           post-pass on emitted code + helper (needs the cpu_translate_block hook)
 *   0x08 SMLAxy Q flag (accumulator)    static: accumulator-prep template becomes SXTW
 *   0x10 SMLALxy operand halves         static: operand templates rebuilt (SXTH / ASR #16) plus an x-bit select
 *   0x20 MSR CPSR_f does not write Q    post-pass on emitted code + helper (needs the cpu_translate_block hook)
 *   0x40 TST Rn,Rm{,shift}: C/V clobbered  static: logical-op test mask 0xe -> 0x6
 *   0x80 rotated-imm shifter carry      static: carry taken from bit 31 of the rotated immediate
 *   0x100 lone Thumb BL/BLX suffix      post-pass on emitted Thumb code (target computed before LR is overwritten)
 *   0x200 MCR CP15 c1 control lost      static: call in arm64_update_coprocessor_control -> helper (store, tail call)
 *   0x400 mov pc,pc dropped as no-op    static: MOV Rd,Rd elimination compares host registers
 *   0x800 MSR CPSR_c writes bits 31:8   post-pass: call to arm64_msr_cpsr_low -> helper that merges by field mask
 *
 * Safety: nothing is written unless the running executable is exactly the known build (SHA-256 of the file, see
 * jf_identity_ok), and each patch site is additionally checked with a 32-bit FNV-1a hash of its window so that a
 * site that was already modified in memory is refused. No original DraStic code is embedded here: every word we
 * write is built from the AArch64 ISA field encoders below.
 * Helpers live in the zero padding at the end of DraStic's R-X LOAD segment (VA 0x14c800..), which is mapped
 * (page-rounded) and within +-128 MB `bl` range of the translation cache (inside nds_system .bss).
 * Call jitfix_init() from a constructor before any translation, and jitfix_post() on every block (ARM and Thumb,
 * both CPUs) that cpu_translate_block returns. jf_inline_hook() installs such a hook. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------------------------------------------
 * AArch64 instruction encoders (field layouts from the Arm ARM, A64 base instructions).
 * sf: 0 = 32-bit (W registers), 1 = 64-bit (X registers). Register 31 is WZR/XZR or SP depending on the class.
 * ------------------------------------------------------------------------------------------------------------- */
enum { A64_ZR = 31, A64_LR = 30 };
enum { A64_EQ = 0, A64_NE = 1 };                                   /* condition codes used here */
enum { A64_UXTW = 2, A64_SXTH = 5, A64_SXTW = 6 };                 /* extend options (add/sub extended register) */

static inline uint32_t a64_rd_rn(uint32_t rd, uint32_t rn) { return (rn & 31) << 5 | (rd & 31); }

/* Move wide immediate: opc 00 MOVN, 10 MOVZ, 11 MOVK; hw = 16-bit chunk index */
static inline uint32_t a64_movw(uint32_t opc, int sf, uint32_t rd, uint32_t imm16, uint32_t hw) {
    return (uint32_t)sf << 31 | opc << 29 | 0x25u << 23 | (hw & 3) << 21 | (imm16 & 0xffff) << 5 | (rd & 31);
}
static inline uint32_t a64_movn(int sf, uint32_t rd, uint32_t imm16, uint32_t hw) { return a64_movw(0, sf, rd, imm16, hw); }
static inline uint32_t a64_movz(int sf, uint32_t rd, uint32_t imm16, uint32_t hw) { return a64_movw(2, sf, rd, imm16, hw); }
static inline uint32_t a64_movk(int sf, uint32_t rd, uint32_t imm16, uint32_t hw) { return a64_movw(3, sf, rd, imm16, hw); }

/* Add/subtract immediate: op 0 ADD, 1 SUB; S sets flags; sh = LSL #12 */
static inline uint32_t a64_addsub_imm(int sf, int op, int S, uint32_t rd, uint32_t rn, uint32_t imm12, int sh) {
    return (uint32_t)sf << 31 | (uint32_t)op << 30 | (uint32_t)S << 29 | 0x22u << 23 | (uint32_t)sh << 22 |
           (imm12 & 0xfff) << 10 | a64_rd_rn(rd, rn);
}
static inline uint32_t a64_add_imm(int sf, uint32_t rd, uint32_t rn, uint32_t imm12, int sh) { return a64_addsub_imm(sf, 0, 0, rd, rn, imm12, sh); }
static inline uint32_t a64_sub_imm(int sf, uint32_t rd, uint32_t rn, uint32_t imm12, int sh) { return a64_addsub_imm(sf, 1, 0, rd, rn, imm12, sh); }

/* Add/subtract shifted register (LSL #0 only here) */
static inline uint32_t a64_addsub_reg(int sf, int op, uint32_t rd, uint32_t rn, uint32_t rm) {
    return (uint32_t)sf << 31 | (uint32_t)op << 30 | 0x0bu << 24 | (rm & 31) << 16 | a64_rd_rn(rd, rn);
}
static inline uint32_t a64_add_reg(int sf, uint32_t rd, uint32_t rn, uint32_t rm) { return a64_addsub_reg(sf, 0, rd, rn, rm); }

/* Add/subtract extended register: Xd = Xn +/- extend(Wm/Xm) << imm3 */
static inline uint32_t a64_addsub_ext(int sf, int op, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t option, uint32_t imm3) {
    return (uint32_t)sf << 31 | (uint32_t)op << 30 | 0x59u << 21 | (rm & 31) << 16 | (option & 7) << 13 |
           (imm3 & 7) << 10 | a64_rd_rn(rd, rn);
}
static inline uint32_t a64_add_ext(int sf, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t opt) { return a64_addsub_ext(sf, 0, rd, rn, rm, opt, 0); }
static inline uint32_t a64_sub_ext(int sf, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t opt) { return a64_addsub_ext(sf, 1, rd, rn, rm, opt, 0); }
#define A64_ADDSUB_EXT_RM_MASK  (31u << 16)
#define A64_ADDSUB_EXT_OPT_MASK (7u << 13)

/* Bitfield moves: opc 00 SBFM, 01 BFM, 10 UBFM (N must equal sf) */
static inline uint32_t a64_bitfield(uint32_t opc, int sf, uint32_t rd, uint32_t rn, uint32_t immr, uint32_t imms) {
    return (uint32_t)sf << 31 | opc << 29 | 0x26u << 23 | (uint32_t)sf << 22 | (immr & 63) << 16 | (imms & 63) << 10 |
           a64_rd_rn(rd, rn);
}
static inline uint32_t a64_sbfm(int sf, uint32_t rd, uint32_t rn, uint32_t immr, uint32_t imms) { return a64_bitfield(0, sf, rd, rn, immr, imms); }
static inline uint32_t a64_bfm(int sf, uint32_t rd, uint32_t rn, uint32_t immr, uint32_t imms) { return a64_bitfield(1, sf, rd, rn, immr, imms); }
static inline uint32_t a64_ubfm(int sf, uint32_t rd, uint32_t rn, uint32_t immr, uint32_t imms) { return a64_bitfield(2, sf, rd, rn, immr, imms); }
static inline uint32_t a64_sxtw(uint32_t xd, uint32_t wn) { return a64_sbfm(1, xd, wn, 0, 31); }
static inline uint32_t a64_sxth(int sf, uint32_t rd, uint32_t rn) { return a64_sbfm(sf, rd, rn, 0, 15); }
static inline uint32_t a64_asr_imm(int sf, uint32_t rd, uint32_t rn, uint32_t sh) { return a64_sbfm(sf, rd, rn, sh, sf ? 63 : 31); }
static inline uint32_t a64_lsr_imm(int sf, uint32_t rd, uint32_t rn, uint32_t sh) { return a64_ubfm(sf, rd, rn, sh, sf ? 63 : 31); }
static inline uint32_t a64_sbfiz(int sf, uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) {
    uint32_t bits = sf ? 64 : 32;
    return a64_sbfm(sf, rd, rn, (bits - lsb) & (bits - 1), width - 1);
}
static inline uint32_t a64_bfi(int sf, uint32_t rd, uint32_t rn, uint32_t lsb, uint32_t width) {
    uint32_t bits = sf ? 64 : 32;
    return a64_bfm(sf, rd, rn, (bits - lsb) & (bits - 1), width - 1);
}

/* Data processing (2 source): variable shift */
static inline uint32_t a64_lsr_reg(int sf, uint32_t rd, uint32_t rn, uint32_t rm) {
    return (uint32_t)sf << 31 | 0xd6u << 21 | (rm & 31) << 16 | 0x9u << 10 | a64_rd_rn(rd, rn);
}

/* Logical immediate. a64_bitmask() turns a value into N:immr:imms (returns -1 if the value is not encodable). */
static int a64_bitmask(uint64_t v, int sf) {
    if (!sf) { v &= 0xffffffffu; v |= v << 32; }
    if (v == 0 || v == ~0ULL) return -1;
    unsigned e = 64;                                   /* smallest element size the value repeats with */
    while (e > 2) {
        unsigned h = e / 2; uint64_t m = (1ULL << h) - 1;
        if ((v & m) != ((v >> h) & m)) break;
        e = h;
    }
    uint64_t emask = e == 64 ? ~0ULL : (1ULL << e) - 1, x = v & emask;
    for (unsigned r = 0; r < e; r++) {                 /* rotate right by r until the ones sit at the bottom */
        uint64_t y = r ? ((x >> r) | (x << (e - r))) & emask : x;
        if ((y & (y + 1)) == 0) {
            unsigned ones = (unsigned)__builtin_popcountll(y);
            unsigned immr = (e - r) & (e - 1), imms = ((~(e * 2 - 1)) & 0x3f) | (ones - 1), n = e == 64;
            return (int)(n << 12 | immr << 6 | imms);
        }
    }
    return -1;
}
/* opc 00 AND, 01 ORR, 10 EOR, 11 ANDS. An unencodable value yields 0 (UDF), which never matches emitted code. */
static inline uint32_t a64_logic_imm(uint32_t opc, int sf, uint32_t rd, uint32_t rn, uint64_t value) {
    int f = a64_bitmask(value, sf);
    if (f < 0) return 0;
    return (uint32_t)sf << 31 | opc << 29 | 0x24u << 23 | (uint32_t)f << 10 | a64_rd_rn(rd, rn);
}
static inline uint32_t a64_and_imm(int sf, uint32_t rd, uint32_t rn, uint64_t v) { return a64_logic_imm(0, sf, rd, rn, v); }
static inline uint32_t a64_orr_imm(int sf, uint32_t rd, uint32_t rn, uint64_t v) { return a64_logic_imm(1, sf, rd, rn, v); }
static inline uint32_t a64_tst_imm(int sf, uint32_t rn, uint64_t v) { return a64_logic_imm(3, sf, A64_ZR, rn, v); }

/* Decode a logical-immediate instruction's value (DecodeBitMasks). Emitters may use any valid immr spelling (e.g.
 * bits above the element size set), so emitted logical immediates are matched by value, not by word. */
static int a64_logic_imm_value(uint32_t w, uint64_t *val) {
    int sf = (int)(w >> 31);
    uint32_t n = w >> 22 & 1, immr = w >> 16 & 63, imms = w >> 10 & 63, comb = n << 6 | (~imms & 63);
    if ((w >> 23 & 0x3f) != 0x24 || (!sf && n) || comb < 2) return 0;
    unsigned len = 31 - (unsigned)__builtin_clz(comb), e = 1u << len, levels = e - 1, S = imms & levels, R = immr & levels;
    if (S == levels) return 0;
    uint64_t emask = e == 64 ? ~0ULL : (1ULL << e) - 1, x = (S + 1 == 64) ? ~0ULL : (1ULL << (S + 1)) - 1;
    if (R) x = ((x >> R) | (x << (e - R))) & emask;
    for (unsigned k = e; k < 64; k *= 2) x |= x << k;
    *val = sf ? x : (x & 0xffffffffu);
    return 1;
}
static inline int a64_is_logic_imm(uint32_t w, uint32_t opc, int sf, uint32_t rd, uint32_t rn, uint64_t value) {
    uint64_t v;
    return (w >> 31) == (uint32_t)sf && (w >> 29 & 3) == opc && (w & 31) == rd && (w >> 5 & 31) == rn &&
           a64_logic_imm_value(w, &v) && v == value;
}

/* Logical shifted register (LSL #0): ORR */
static inline uint32_t a64_orr_reg(int sf, uint32_t rd, uint32_t rn, uint32_t rm) {
    return (uint32_t)sf << 31 | 1u << 29 | 0x0au << 24 | (rm & 31) << 16 | a64_rd_rn(rd, rn);
}

/* Conditional select / conditional compare (register) */
static inline uint32_t a64_csel(int sf, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t cond) {
    return (uint32_t)sf << 31 | 0xd4u << 21 | (rm & 31) << 16 | (cond & 15) << 12 | a64_rd_rn(rd, rn);
}
static inline uint32_t a64_ccmp_reg(int sf, uint32_t rn, uint32_t rm, uint32_t nzcv, uint32_t cond) {
    return (uint32_t)sf << 31 | 1u << 30 | 1u << 29 | 0xd2u << 21 | (rm & 31) << 16 | (cond & 15) << 12 |
           (rn & 31) << 5 | (nzcv & 15);
}

/* Loads/stores: 32-bit register, unsigned scaled offset; 64-bit literal load */
static inline uint32_t a64_ldr_w(uint32_t rt, uint32_t rn, uint32_t byte_off) { return 0xb9400000u | (byte_off / 4 & 0xfff) << 10 | a64_rd_rn(rt, rn); }
static inline uint32_t a64_str_w(uint32_t rt, uint32_t rn, uint32_t byte_off) { return 0xb9000000u | (byte_off / 4 & 0xfff) << 10 | a64_rd_rn(rt, rn); }
static inline uint32_t a64_ldr_lit_x(uint32_t rt, int32_t words) { return 0x58000000u | ((uint32_t)words & 0x7ffff) << 5 | (rt & 31); }

/* Branches. Offsets are in instructions (words). */
#define A64_B_OP   0x14000000u
#define A64_BL_OP  0x94000000u
#define A64_B_MASK 0xfc000000u
static inline uint32_t a64_b(int32_t words)  { return A64_B_OP  | ((uint32_t)words & 0x3ffffff); }
static inline uint32_t a64_bl(int32_t words) { return A64_BL_OP | ((uint32_t)words & 0x3ffffff); }
static inline int a64_is_bl(uint32_t w) { return (w & A64_B_MASK) == A64_BL_OP; }
static inline int32_t a64_b_offset(uint32_t w) { return (int32_t)(w << 6) >> 6; }
static inline uint32_t a64_cbnz(int sf, uint32_t rt, int32_t words) { return (uint32_t)sf << 31 | 0x35u << 24 | ((uint32_t)words & 0x7ffff) << 5 | (rt & 31); }
static inline uint32_t a64_tbnz(uint32_t rt, uint32_t bit, int32_t words) {
    return (bit >> 5 & 1) << 31 | 0x37u << 24 | (bit & 31) << 19 | ((uint32_t)words & 0x3fff) << 5 | (rt & 31);
}
static inline uint32_t a64_br(uint32_t rn)  { return 0xd61f0000u | (rn & 31) << 5; }
static inline uint32_t a64_ret(void)        { return 0xd65f0000u | A64_LR << 5; }
static inline uint32_t a64_nop(void)        { return 0xd503201fu; }          /* HINT #0 */

/* System register moves for NZCV (op0=3, op1=3, CRn=4, CRm=2, op2=0); L=0 MSR, L=1 MRS */
static inline uint32_t a64_sysreg(int L, uint32_t op0, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint32_t rt) {
    return 0xd5000000u | (uint32_t)L << 21 | (op0 & 3) << 19 | (op1 & 7) << 16 | (crn & 15) << 12 | (crm & 15) << 8 |
           (op2 & 7) << 5 | (rt & 31);
}
static inline uint32_t a64_msr_nzcv(uint32_t rt) { return a64_sysreg(0, 3, 3, 4, 2, 0, rt); }

/* Classes used when matching emitted code */
static inline int a64_is_movw32_to(uint32_t w, uint32_t rd) {      /* MOVN/MOVZ/MOVK Wd, #imm */
    uint32_t opc = w >> 29 & 3;
    return (w >> 31) == 0 && (w >> 23 & 0x3f) == 0x25 && opc != 1 && (w & 31) == rd;
}
static inline int a64_is_orr_wzr_imm_to(uint32_t w, uint32_t rd) { /* ORR Wd, WZR, #bitmask (MOV Wd, #imm) */
    return (w >> 23) == ((1u << 6) | 0x24u) && (w >> 5 & 31) == A64_ZR && (w & 31) == rd;
}
static inline int a64_is_pc_relative(uint32_t w) {
    return (w & 0x1f000000u) == 0x10000000u ||   /* ADR / ADRP */
           (w & 0x7c000000u) == 0x14000000u ||   /* B / BL */
           (w & 0xff000010u) == 0x54000000u ||   /* B.cond */
           (w & 0x7e000000u) == 0x34000000u ||   /* CBZ / CBNZ */
           (w & 0x7e000000u) == 0x36000000u ||   /* TBZ / TBNZ */
           (w & 0x3b000000u) == 0x18000000u;     /* LDR (literal), LDRSW (literal), PRFM (literal) */
}
static inline int a64_is_adrp(uint32_t w) { return (w & 0x9f000000u) == 0x90000000u; }
static inline int64_t a64_adrp_target(uint32_t w, uint64_t pc) {
    int64_t imm = (int64_t)((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) << 43 >> 43;   /* immhi:immlo, signed 21 */
    return (int64_t)(pc & ~0xfffULL) + imm * 4096;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Build identity: SHA-256 (FIPS 180-4) of the running executable, plus FNV-1a for individual patch windows.
 * ------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; unsigned n; } jf_sha256;
static const uint32_t jf_sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
static inline uint32_t jf_ror(uint32_t x, int r) { return x >> r | x << (32 - r); }
static void jf_sha_block(jf_sha256 *s, const uint8_t *p) {
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = jf_ror(w[i - 15], 7) ^ jf_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = jf_ror(w[i - 2], 17) ^ jf_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, s->h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (jf_ror(v[4], 6) ^ jf_ror(v[4], 11) ^ jf_ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
                      jf_sha_k[i] + w[i];
        uint32_t t2 = (jf_ror(v[0], 2) ^ jf_ror(v[0], 13) ^ jf_ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += v[i];
}
static void jf_sha_init(jf_sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void jf_sha_update(jf_sha256 *s, const uint8_t *p, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, k); s->n += (unsigned)k; p += k; n -= k;
        if (s->n == 64) { jf_sha_block(s, s->buf); s->n = 0; }
    }
}
static void jf_sha_final(jf_sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0, l[8];
    jf_sha_update(s, &pad, 1);
    while (s->n != 56) jf_sha_update(s, &z, 1);
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    jf_sha_update(s, l, 8);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

static uint32_t jf_fnv1a(const void *p, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x01000193u;
    return h;
}

/* The one build these patches were written for: DraStic r2.5.2.2, ROCKNIX Linux aarch64 (1581200 bytes,
 * md5 17550db727f3b59d36b57746ad1944be). */
#define JF_EXE_SHA256 "dab3275ef772451b9235b78e64b2d0d041a28ac22937668231088c76b709e218"
#ifndef JF_EXE_PATH
#define JF_EXE_PATH "/proc/self/exe"
#endif

static int jf_hash_file(const char *path, char hex[65]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    jf_sha256 s; jf_sha_init(&s);
    uint8_t buf[65536], d[32];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) jf_sha_update(&s, buf, (size_t)r);
    close(fd);
    if (r < 0) return 0;
    jf_sha_final(&s, d);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return 1;
}
/* 1 if the running executable is the known build. Computed once. */
static int jf_identity_ok(void) {
    static int state = -1;
    if (state >= 0) return state;
    char hex[65] = "";
    const char *alt = (const char *)getauxval(AT_EXECFN);
    if (!jf_hash_file(JF_EXE_PATH, hex) && !(alt && jf_hash_file(alt, hex))) {
        fprintf(stderr, "[jitfix] cannot read the executable to check its identity: nothing patched\n");
        return state = 0;
    }
    state = strcmp(hex, JF_EXE_SHA256) == 0;
    fprintf(stderr, "[jitfix] executable sha256 %s: %s\n", hex,
            state ? "DraStic r2.5.2.2 aarch64" : "unknown build, nothing patched");
    return state;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Patching primitives
 * ------------------------------------------------------------------------------------------------------------- */
#define JF_ASR   0x01
#define JF_QADD  0x02
#define JF_QDBL  0x04
#define JF_SMLA  0x08
#define JF_SMLAL 0x10
#define JF_MSRF  0x20
#define JF_TST   0x40
#define JF_ROTC  0x80
#define JF_BLSUF 0x100
#define JF_CP15  0x200
#define JF_MOVPC 0x400
#define JF_MSRC  0x800
#define JF_ALL   0xfff
#define JF_NEEDHELP (JF_QDBL | JF_MSRF | JF_CP15 | JF_MSRC)

#define VA_HELPERS     0x14c800   /* end of R-X segment is 0x14c7ef; page end 0x14d000 */
#define HELPERS_BYTES  0x300      /* used: 0x2c4 */
#define VA_UPDATE_CP15_CONTROL 0x295b0
#define VA_MSR_CPSR_LOW        0x8f43c
#define VA_CP15_STUB_CALL      0x8f6b0   /* the call to update_coprocessor_control in arm64_update_coprocessor_control */

/* Guest state the helpers touch: CPSR word in DraStic's per-CPU state, addressed from x28 */
#define CPU_CPSR_OFF   0x23c0
#define CPSR_Q_BIT     27
#define CP15_CONTROL_OFF 0x14

static unsigned jitfix_mask, jitfix_applied;
static uint32_t *jf_dbl;          /* 32 entries x 4 words: w1 = sat(2*wR), Q on saturation */
static uint32_t *jf_msrf;         /* msr nzcv,x0 + copy bit 27 of w0 into CPSR */
static uint32_t *jf_cp15c;        /* store control word, tail-call update_coprocessor_control */
static uint32_t *jf_msrc, *jf_msrfc; /* merge w0 into CPSR by field mask (c / f+c), tail-call arm64_msr_cpsr_low */
static long jf_cnt_qdbl, jf_cnt_msrf, jf_cnt_blsuf, jf_cnt_msrc, jf_cnt_skip;

static int jf_mprotect(uintptr_t a, size_t n, int prot) {
    uintptr_t pg = a & ~4095UL, end = (a + n + 4095) & ~4095UL;
    return mprotect((void *)pg, end - pg, prot);
}
static int32_t jf_words(const uint32_t *from, const uint32_t *to) { return (int32_t)(to - from); }
static int jf_in_bl_range(const uint32_t *from, const uint32_t *to) {
    long d = (long)(to - from);
    return d >= -(1L << 25) && d < (1L << 25);
}

/* Write n words at p (in DraStic's R-X image) and restore R-X. */
static int jf_write_code(uint32_t *p, const uint32_t *words, int n) {
    if (jf_mprotect((uintptr_t)p, 4 * n, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("[jitfix] mprotect"); return 0; }
    memcpy(p, words, 4 * n);
    __builtin___clear_cache((char *)p, (char *)(p + n));
    jf_mprotect((uintptr_t)p, 4 * n, PROT_READ | PROT_EXEC);
    return 1;
}

/* Static patch: the window [win_va, win_va+win_len) must hash to win_fnv (i.e. be unmodified); then the n words at
 * va are replaced. Returns 1 on success. */
static int jf_patch(const char *name, uintptr_t win_va, size_t win_len, uint32_t win_fnv,
                    uintptr_t va, const uint32_t *neu, int n) {
    uint32_t h = jf_fnv1a((const void *)(base + win_va), win_len);
    if (h != win_fnv) {
        fprintf(stderr, "[jitfix] %s: REFUSED, site %#lx is not as expected (fnv %08x)\n", name, (unsigned long)win_va, h);
        return 0;
    }
    if (!jf_write_code((uint32_t *)(base + va), neu, n)) return 0;
    fprintf(stderr, "[jitfix] %s: patched %d word(s) at %#lx\n", name, n, (unsigned long)va);
    return 1;
}

/* Inline hook: replaces the first 4 instructions at fn with "ldr x16,#8 ; br x16 ; .quad hook" and builds a
 * trampoline (in tramp, >= 32 words) that runs the displaced instructions and continues at fn+16. The displaced
 * instructions are copied from memory at run time; an ADRP among them is re-materialised as MOVZ/MOVK of its
 * target, and any other PC-relative instruction makes the hook refuse. The 16-byte window must hash to win_fnv.
 * Returns the trampoline (call it as the original function) or 0. */
static void *jf_inline_hook(const char *name, uint32_t *fn, uint32_t win_fnv, void *hook, uint32_t *tramp) {
    if (jf_fnv1a(fn, 16) != win_fnv) { fprintf(stderr, "[jitfix] hook %s: REFUSED, entry not as expected\n", name); return 0; }
    int k = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t w = fn[i];
        if (a64_is_adrp(w)) {
            uint64_t t = (uint64_t)a64_adrp_target(w, (uint64_t)(uintptr_t)(fn + i));
            uint32_t rd = w & 31;
            tramp[k++] = a64_movz(1, rd, (uint32_t)t, 0);
            for (uint32_t hw = 1; hw < 4; hw++) tramp[k++] = a64_movk(1, rd, (uint32_t)(t >> (16 * hw)), hw);
        } else if (a64_is_pc_relative(w)) {
            fprintf(stderr, "[jitfix] hook %s: REFUSED, PC-relative instruction in the entry\n", name); return 0;
        } else tramp[k++] = w;
    }
    if (k & 1) tramp[k++] = a64_nop();                  /* keep the 64-bit literal 8-byte aligned */
    tramp[k] = a64_ldr_lit_x(16, 2); tramp[k + 1] = a64_br(16);
    *(uint64_t *)&tramp[k + 2] = (uint64_t)(uintptr_t)(fn + 4);
    __builtin___clear_cache((char *)tramp, (char *)(tramp + k + 4));
    uint32_t patch[4] = { a64_ldr_lit_x(16, 2), a64_br(16), 0, 0 };
    *(uint64_t *)&patch[2] = (uint64_t)(uintptr_t)hook;
    if (!jf_write_code(fn, patch, 4)) return 0;
    return tramp;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Helpers (written into the R-X segment's zero tail)
 * ------------------------------------------------------------------------------------------------------------- */
static int jf_build_helpers(void) {
    uint32_t *h = (uint32_t *)(base + VA_HELPERS);
    for (int i = 0; i < HELPERS_BYTES / 4; i++)
        if (h[i]) { fprintf(stderr, "[jitfix] helper area %#x not zero (%08x at +%#x), helper-based fixes disabled\n",
                            VA_HELPERS, h[i], 4 * i); return 0; }
    if (jf_mprotect((uintptr_t)h, HELPERS_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC)) { perror("[jitfix] mprotect"); return 0; }
    uint32_t *dbl = h, *sat = h + 128, *msrf = h + 144;
    /* dbl[R]: w1 = 2*wR exactly if it fits in s32, else saturate and set Q (shared tail `sat`). Clobbers x0, x1. */
    for (int r = 0; r < 32; r++) {
        uint32_t *e = dbl + 4 * r;
        e[0] = a64_sbfiz(1, 1, r, 1, 32);                     /* x1 = sext(wR) << 1 */
        e[1] = a64_sub_ext(1, 0, 1, 1, A64_SXTW);             /* x0 = x1 - sext(w1): nonzero iff overflow */
        e[2] = a64_cbnz(1, 0, jf_words(e + 2, sat));
        e[3] = a64_ret();
    }
    sat[0] = a64_tbnz(1, 63, 3);                              /* negative -> 0x80000000 */
    sat[1] = a64_movn(0, 1, 0x8000, 1);                       /* w1 = 0x7fffffff */
    sat[2] = a64_b(2);
    sat[3] = a64_movz(0, 1, 0x8000, 1);                       /* w1 = 0x80000000 */
    sat[4] = a64_ldr_w(0, 28, CPU_CPSR_OFF);
    sat[5] = a64_orr_imm(0, 0, 0, 1u << CPSR_Q_BIT);
    sat[6] = a64_str_w(0, 28, CPU_CPSR_OFF);
    sat[7] = a64_ret();
    /* msrf: the emitted NZCV write, plus CPSR.Q = bit 27 of the new value. Clobbers w0, w1. */
    msrf[0] = a64_msr_nzcv(0);
    msrf[1] = a64_ldr_w(1, 28, CPU_CPSR_OFF);
    msrf[2] = a64_lsr_imm(0, 0, 0, CPSR_Q_BIT);
    msrf[3] = a64_bfi(0, 1, 0, CPSR_Q_BIT, 1);
    msrf[4] = a64_str_w(1, 28, CPU_CPSR_OFF);
    msrf[5] = a64_ret();
    uint32_t *cp = h + 160, *mc = h + 164, *mfc = h + 172;
    uint32_t *ucc = (uint32_t *)(base + VA_UPDATE_CP15_CONTROL), *mcl = (uint32_t *)(base + VA_MSR_CPSR_LOW);
    /* cp15c: store the merged control word (w1) to cp15 state (x0), then tail-call update_coprocessor_control */
    cp[0] = a64_str_w(1, 0, CP15_CONTROL_OFF);
    cp[1] = a64_b(jf_words(cp + 1, ucc));
    /* msrc / msrfc: keep the CPSR bits outside the written fields, then tail-call arm64_msr_cpsr_low */
    const uint32_t keep_c = 0xffffff00u, keep_fc = 0x00ffff00u;
    mc[0] = a64_ldr_w(1, 28, CPU_CPSR_OFF);
    mc[1] = a64_and_imm(0, 1, 1, keep_c);
    mc[2] = a64_and_imm(0, 0, 0, ~keep_c & 0xffffffffu);
    mc[3] = a64_orr_reg(0, 0, 0, 1);
    mc[4] = a64_b(jf_words(mc + 4, mcl));
    mfc[0] = a64_ldr_w(1, 28, CPU_CPSR_OFF);
    mfc[1] = a64_and_imm(0, 1, 1, keep_fc);
    mfc[2] = a64_and_imm(0, 0, 0, ~keep_fc & 0xffffffffu);
    mfc[3] = a64_orr_reg(0, 0, 0, 1);
    mfc[4] = a64_b(jf_words(mfc + 4, mcl));
    __builtin___clear_cache((char *)h, (char *)h + HELPERS_BYTES);
    jf_mprotect((uintptr_t)h, HELPERS_BYTES, PROT_READ | PROT_EXEC);
    jf_dbl = dbl; jf_msrf = msrf; jf_cp15c = cp; jf_msrc = mc; jf_msrfc = mfc;
    fprintf(stderr, "[jitfix] helpers at %p (VA %#x)\n", (void *)h, VA_HELPERS);
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Static patches
 * ------------------------------------------------------------------------------------------------------------- */
static void jitfix_init(void) {
    const char *e = getenv("JITFIX");
    if (!e || !*e || !strcmp(e, "0")) return;
    if (!strcmp(e, "1") || !strcmp(e, "all")) jitfix_mask = JF_ALL;
    else jitfix_mask = (unsigned)strtoul(e, 0, 16) & JF_ALL;
    fprintf(stderr, "[jitfix] mask %#x\n", jitfix_mask);
    if (!jf_identity_ok()) { jitfix_mask = 0; return; }

    if (jitfix_mask & JF_ASR) {
        /* ASR-by-register flag stub: the carry shift must use the amount-1 register (w4), not the amount (w3). */
        uint32_t n[] = { a64_lsr_reg(0, 4, 1, 4) };
        if (jf_patch("asr-carry", 0x8f900, 8, 0x5ecd08b3u, 0x8f904, n, 1)) jitfix_applied |= JF_ASR;
    }
    if (jitfix_mask & JF_QADD) {
        /* QADD/QDADD template: low half of "add x,x,w,SXTW" (the high half, already loaded by the next word, is the
         * same for UXTW and SXTW). */
        uint32_t tmpl = a64_add_ext(1, 0, 0, 0, A64_SXTW);
        uint32_t n[] = { a64_movz(0, 2, tmpl & 0xffff, 0) };
        if (jf_patch("qadd-sxtw", 0x957b8, 8, 0xcac7911cu, 0x957b8, n, 1)) jitfix_applied |= JF_QADD;
    }
    if (jitfix_mask & JF_SMLA) {
        /* SMLAxy accumulator prep template: high half of "sxtw x,w" (the low half, set by the previous word, is the
         * same as for the 32-bit shift it replaces). */
        uint32_t tmpl = a64_sxtw(0, 0);
        uint32_t n[] = { a64_movk(0, 7, tmpl >> 16, 1) };
        if (jf_patch("smla-acc-sxtw", 0x937ac, 8, 0x4be98544u, 0x937b0, n, 1)) jitfix_applied |= JF_SMLA;
    }
    if (jitfix_mask & JF_SMLAL) {
        /* SMLALxy operand templates, 7 slots. Live-outs: w2 = Rs template for y=0, w4 = Rs template for y=1,
         * w6 = Rm template chosen by the x bit (opcode bit 5), flags = y bit (opcode bit 6) for the later select.
         * Templates are "dest w1, source w0" with the source register OR-ed in by the emitter; the Rm one has
         * destination w0, i.e. the Rs one minus 1. */
        uint32_t t_bot = a64_sxth(0, 1, 0), t_top = a64_asr_imm(0, 1, 0, 16);
        uint32_t diff = t_top - t_bot;                    /* only the immr field differs: fits ADD #imm, LSL #12 */
        uint32_t n[] = {
            a64_movz(0, 2, t_bot & 0xffff, 0),
            a64_movk(0, 2, t_bot >> 16, 1),               /* w2 = sxth w1,Rs */
            a64_add_imm(0, 4, 2, diff >> 12, 1),          /* w4 = asr w1,Rs,#16 */
            a64_tst_imm(1, 20, 1u << 5),                  /* x bit */
            a64_csel(0, 6, 4, 2, A64_NE),
            a64_sub_imm(0, 6, 6, 1, 0),                   /* w6: destination w0 */
            a64_tst_imm(1, 20, 1u << 6),                  /* y bit, consumed by the select that follows */
        };
        if ((diff & 0xfff) == 0 && jf_patch("smlal-halves", 0x95068, 28, 0x2319f013u, 0x95068, n, 7))
            jitfix_applied |= JF_SMLAL;
    }
    if (jitfix_mask & JF_TST) {
        /* Logical-op test in the ALU-register path: mask 0xe -> 0x6, so op 8 (TST) groups with 0, 1, 9. */
        uint32_t n[] = { a64_tst_imm(0, 24, 0x6) };
        if (jf_patch("tst-reg-flags", 0x93894, 12, 0x49e381d0u, 0x93894, n, 1)) jitfix_applied |= JF_TST;
    }
    if (jitfix_mask & JF_ROTC) {
        /* Rotated-immediate carry: bit 31 of the rotated value (w28). */
        uint32_t n[] = { a64_lsr_imm(0, 0, 28, 31) };
        if (jf_patch("rotimm-carry", 0x9209c, 16, 0xe4d12b82u, 0x920a4, n, 1)) jitfix_applied |= JF_ROTC;
    }
    if (jitfix_mask & JF_BLSUF) jitfix_applied |= JF_BLSUF;     /* post-pass only, no helper needed */
    if (jitfix_mask & JF_MOVPC) {
        /* MOV Rd,Rd elimination: compare host register numbers w26 (Rd, 0 for PC) and w23 (Rm, 28 for PC). */
        uint32_t n[] = { a64_ccmp_reg(0, 26, 23, 0, A64_EQ) };
        if (jf_patch("movpcpc", 0x93c9c, 12, 0x356cc079u, 0x93ca0, n, 1)) jitfix_applied |= JF_MOVPC;
    }
    if ((jitfix_mask & JF_NEEDHELP) && jf_build_helpers()) {
        jitfix_applied |= jitfix_mask & (JF_QDBL | JF_MSRF | JF_MSRC);
        if (jitfix_mask & JF_CP15) {
            /* CP15 control stub: call the store-then-tail-call helper instead of update_coprocessor_control. */
            uint32_t *call = (uint32_t *)(base + VA_CP15_STUB_CALL);
            uint32_t n[] = { a64_bl(jf_words(call, jf_cp15c)) };
            if (jf_in_bl_range(call, jf_cp15c) &&
                jf_patch("cp15-control-store", VA_CP15_STUB_CALL - 12, 16, 0xb254ea31u, VA_CP15_STUB_CALL, n, 1))
                jitfix_applied |= JF_CP15;
        }
    }
    fprintf(stderr, "[jitfix] applied %#x\n", jitfix_applied);
}

/* ---------------------------------------------------------------------------------------------------------------
 * Post-pass over freshly translated blocks (lengths never change).
 * Block descriptor: +8 -> instruction records (0x20 bytes each: +0 guest opcode, +8 host address of its code),
 * +0x20 u16 record count. CPU state +0x210c: 1 = ARM9.
 * ------------------------------------------------------------------------------------------------------------- */
#define BLK_RECS       8
#define BLK_NINSN      0x20
#define REC_SIZE       0x20
#define REC_HOST       8
#define CPU_TYPE_OFF   0x210c

/* Guest ARM encodings (ARMv5TE) */
#define ARM_COND(op)          ((op) >> 28)
#define ARM_QDADD_MASKED      0x01400050u          /* (op & 0x0ff00ff0) */
#define ARM_QDSUB_MASKED      0x01600050u
#define ARM_IS_MSR_CPSR(op)   ((((op) & 0x0fb0f000u) == 0x0320f000u) || (((op) & 0x0fb0fff0u) == 0x0120f000u))
#define ARM_MSR_FIELD_C       (1u << 16)
#define ARM_MSR_FIELD_F       (1u << 19)
/* DraStic's ARM pseudo-op for a lone Thumb BL/BLX suffix: 0xe61f0010 | off11<<5 (BL), 0xe61d0010 | off11<<5 (BLX) */
#define THUMB_SUFFIX_MASK     0xfffd001fu
#define THUMB_SUFFIX_VALUE    0xe61d0010u

static inline int jf_rec_range(uint8_t *recs, int i, int ninsn, uint32_t *code, uint32_t *bend,
                               uint32_t **s, uint32_t **e) {
    *s = *(uint32_t **)(recs + REC_SIZE * i + REC_HOST);
    *e = (i + 1 < ninsn) ? *(uint32_t **)(recs + REC_SIZE * (i + 1) + REC_HOST) : bend;
    return !(*s < code || *e > bend || *e < *s);
}

/* Lone Thumb BL/BLX suffix: the emitted code writes the new LR (2-4 immediate writes to w27) before computing the
 * target from w27 (`add w0,w27,#off*2` then ORR #1 or AND #~3). Move the add in front of the LR writes. */
static void jitfix_post_thumb(void *block, uint32_t *code, unsigned nbytes) {
    int ninsn = *(uint16_t *)((uint8_t *)block + BLK_NINSN);
    uint8_t *recs = *(uint8_t **)((uint8_t *)block + BLK_RECS);
    uint32_t *bend = code + nbytes / 4;
    for (int i = 0; i < ninsn; i++) {
        uint32_t op = *(uint32_t *)(recs + REC_SIZE * i), *s, *e;
        if ((op & THUMB_SUFFIX_MASK) != THUMB_SUFFIX_VALUE) continue;
        if (!jf_rec_range(recs, i, ninsn, code, bend, &s, &e)) continue;
        uint32_t add = a64_add_imm(0, 0, 27, ((op >> 5) & 0x7ff) * 2, 0), *a = 0;
        for (uint32_t *q = s; q + 1 < e; q++)
            if (q[0] == add && (a64_is_logic_imm(q[1], 1, 0, 0, 0, 1) ||              /* orr w0,w0,#1 */
                                a64_is_logic_imm(q[1], 0, 0, 0, 0, 0xfffffffcu))) {    /* and w0,w0,#~3 */
                a = q; break;
            }
        uint32_t *f = a;
        while (a && f > s && (a64_is_movw32_to(f[-1], 27) || a64_is_orr_wzr_imm_to(f[-1], 27))) f--;
        int k = a ? (int)(a - f) : 0;
        if (k < 2 || k > 4) {
            if (jf_cnt_skip++ < 10) fprintf(stderr, "[jitfix] thumb bl suffix %08x: pattern not found, left as is\n", op);
            continue;
        }
        memmove(f + 1, f, 4 * k);                       /* LR writes move down one slot */
        f[0] = add;                                      /* target from the old LR */
        __builtin___clear_cache((char *)f, (char *)(a + 1));
        if (jf_cnt_blsuf++ < 4) fprintf(stderr, "[jitfix] thumb bl suffix %08x rewritten at %p\n", op, (void *)f);
    }
}

static void jitfix_post(void *block, void *cpu, uint32_t thumb, uint32_t *code, unsigned nbytes) {
    if (!code) return;
    if (thumb) { if (jitfix_applied & JF_BLSUF) jitfix_post_thumb(block, code, nbytes); return; }
    if (!(jitfix_applied & (JF_QDBL | JF_MSRF | JF_MSRC))) return;
    int arm9 = *(int *)((uint8_t *)cpu + CPU_TYPE_OFF) == 1;   /* QDADD/QDSUB/Q fixes: ARMv5TE (ARM9) only; MSR c: both */
    int ninsn = *(uint16_t *)((uint8_t *)block + BLK_NINSN);
    uint8_t *recs = *(uint8_t **)((uint8_t *)block + BLK_RECS);
    uint32_t *bend = code + nbytes / 4;
    const uint32_t msr_nzcv_x0 = a64_msr_nzcv(0);
    for (int i = 0; i < ninsn; i++) {
        uint32_t op = *(uint32_t *)(recs + REC_SIZE * i), *s, *e;
        if (ARM_COND(op) == 0xf) continue;
        if (!jf_rec_range(recs, i, ninsn, code, bend, &s, &e)) continue;
        uint32_t m = op & 0x0ff00ff0u;
        if (arm9 && (jitfix_applied & JF_QDBL) && (m == ARM_QDADD_MASKED || m == ARM_QDSUB_MASKED)) {
            /* Expected host shape (host reg = guest reg + 13):
             *   add w1,wN,wN ; sxtw xM,wM ; add|sub xD,xM,wN,{uxtw|sxtw} ; sub x0,xD,wD,sxtw
             * Rewrite: word 0 -> bl dbl[N] (w1 = saturated 2*Rn), word 2 -> add|sub xD,xM,w1,sxtw. */
            int is_sub = m == ARM_QDSUB_MASKED;
            uint32_t rn = (op >> 16) & 15, rm = op & 15, rd = (op >> 12) & 15;
            if (rn == 15 || rm == 15 || rd == 15) { jf_cnt_skip++; continue; }
            uint32_t hn = rn + 13, hm = rm + 13, hd = rd + 13;
            uint32_t w0 = a64_add_reg(0, 1, hn, hn), w1 = a64_sxtw(hm, hm), w3 = a64_sub_ext(1, 0, hd, hd, A64_SXTW);
            uint32_t *p = 0;
            for (uint32_t *q = s; q + 4 <= e; q++)
                if (q[0] == w0 && q[1] == w1 && q[3] == w3) { p = q; break; }
            int okop = 0;
            if (p) {
                uint32_t a = p[2];
                okop = is_sub ? a == a64_sub_ext(1, hd, hm, hn, A64_SXTW)
                              : (a == a64_add_ext(1, hd, hm, hn, A64_UXTW) || a == a64_add_ext(1, hd, hm, hn, A64_SXTW));
            }
            if (!okop || !jf_in_bl_range(p, jf_dbl + 4 * hn)) {
                if (jf_cnt_skip++ < 10) fprintf(stderr, "[jitfix] qdadd/qdsub %08x: emitted pattern not found, left as is\n", op);
                continue;
            }
            p[0] = a64_bl(jf_words(p, jf_dbl + 4 * hn));
            p[2] = is_sub ? a64_sub_ext(1, hd, hm, 1, A64_SXTW) : a64_add_ext(1, hd, hm, 1, A64_SXTW);
            __builtin___clear_cache((char *)p, (char *)(p + 3));
            if (jf_cnt_qdbl++ < 4) fprintf(stderr, "[jitfix] qdadd/qdsub %08x rewritten at %p\n", op, (void *)p);
        } else if ((jitfix_applied & JF_MSRC) && ARM_IS_MSR_CPSR(op) && (op & ARM_MSR_FIELD_C)) {
            /* MSR CPSR with the c field: the call to arm64_msr_cpsr_low stores the whole value; merge first */
            uint32_t *p = 0, *tgt = (uint32_t *)(base + VA_MSR_CPSR_LOW);
            for (uint32_t *q = s; q < e; q++)
                if (a64_is_bl(*q) && q + a64_b_offset(*q) == tgt) { p = q; break; }
            uint32_t *h = (op & ARM_MSR_FIELD_F) ? jf_msrfc : jf_msrc;
            if (!p || !jf_in_bl_range(p, h)) {
                if (jf_cnt_skip++ < 10) fprintf(stderr, "[jitfix] msr cpsr_c %08x: call to arm64_msr_cpsr_low not found, left as is\n", op);
                continue;
            }
            *p = a64_bl(jf_words(p, h));
            __builtin___clear_cache((char *)p, (char *)(p + 1));
            if (jf_cnt_msrc++ < 4) fprintf(stderr, "[jitfix] msr cpsr_c %08x rewritten at %p\n", op, (void *)p);
        } else if (arm9 && (jitfix_applied & JF_MSRF) && ARM_IS_MSR_CPSR(op) &&
                   (op & ARM_MSR_FIELD_F) && !(op & ARM_MSR_FIELD_C)) {
            /* MSR CPSR_f (no c field): the emitted NZCV write -> call msrf, which also writes Q */
            uint32_t *p = 0;
            for (uint32_t *q = s; q < e; q++) if (*q == msr_nzcv_x0) { p = q; break; }
            if (!p || !jf_in_bl_range(p, jf_msrf)) {
                if (jf_cnt_skip++ < 10) fprintf(stderr, "[jitfix] msr %08x: msr nzcv,x0 not found, left as is\n", op);
                continue;
            }
            *p = a64_bl(jf_words(p, jf_msrf));
            __builtin___clear_cache((char *)p, (char *)(p + 1));
            if (jf_cnt_msrf++ < 4) fprintf(stderr, "[jitfix] msr cpsr_f %08x rewritten at %p\n", op, (void *)p);
        }
    }
}
