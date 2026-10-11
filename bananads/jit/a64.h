// BananaJIT's AArch64 encoder: one function per instruction form BananaJIT emits, written from the Arm
// Architecture Reference Manual (A64 base instructions). tests/jit_encodings.cpp checks every form against
// LLVM's assembler (llvm-mc).
#pragma once
#include <cstdint>
#include <cstring>

namespace bananajit {

// Register numbers 0-30; 31 is WZR/XZR in data-processing forms and SP in load/store base forms.
enum Reg : uint32_t { ZR = 31, SP = 31 };
enum Shift : uint32_t { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
// Condition codes: the same numbers as ARM's (EQ 0 ... LE 13, AL 14).
enum Cond : uint32_t { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };

struct A64 {
    uint32_t* code;  // where the next word goes
    uint32_t* start;

    explicit A64(uint32_t* at = nullptr) : code(at), start(at) {}
    void put(uint32_t w) { *code++ = w; }
    uint32_t* here() const { return code; }

    // --- data processing, 32-bit (W) unless the name ends in X ---
    // ADD/SUB (shifted register): op 0 ADD, 1 ADDS, 2 SUB, 3 SUBS
    static uint32_t addsub_reg(bool x, int op, Reg d, Reg n, Reg m, Shift sh, uint32_t amt) {
        static const uint32_t base[4] = {0x0B000000, 0x2B000000, 0x4B000000, 0x6B000000};
        return (x ? 0x80000000u : 0) | base[op] | sh << 22 | m << 16 | (amt & 63) << 10 | n << 5 | d;
    }
    void add(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(addsub_reg(false, 0, d, n, m, sh, a)); }
    void adds(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(addsub_reg(false, 1, d, n, m, sh, a)); }
    void sub(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(addsub_reg(false, 2, d, n, m, sh, a)); }
    void subs(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(addsub_reg(false, 3, d, n, m, sh, a)); }
    void addx(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(addsub_reg(true, 0, d, n, m, sh, a)); }

    // ADD/SUB (immediate, 0-4095, optionally << 12): op as above; n may be SP for ADD/SUB
    static uint32_t addsub_imm(bool x, int op, Reg d, Reg n, uint32_t imm12, bool lsl12) {
        static const uint32_t base[4] = {0x11000000, 0x31000000, 0x51000000, 0x71000000};
        return (x ? 0x80000000u : 0) | base[op] | (lsl12 ? 1u << 22 : 0) | (imm12 & 0xFFF) << 10 | n << 5 | d;
    }
    void addi(Reg d, Reg n, uint32_t imm) { put(addsub_imm(false, 0, d, n, imm, false)); }
    void subi(Reg d, Reg n, uint32_t imm) { put(addsub_imm(false, 2, d, n, imm, false)); }
    void cmpi(Reg n, uint32_t imm) { put(addsub_imm(false, 3, ZR, n, imm, false)); }
    void cmpx(Reg n, Reg m) { put(addsub_reg(true, 3, ZR, n, m, LSL, 0)); }
    void addx_sxtw(Reg d, Reg n, Reg m) { put(0x8B20C000 | m << 16 | n << 5 | d); }  // d = n + (s64)(s32)m
    void addxi(Reg d, Reg n, uint32_t imm) { put(addsub_imm(true, 0, d, n, imm, false)); }

    // Logical (shifted register): op 0 AND, 1 BIC, 2 ORR, 3 ORN, 4 EOR, 5 EON, 6 ANDS, 7 BICS
    static uint32_t logic_reg(bool x, int op, Reg d, Reg n, Reg m, Shift sh, uint32_t amt) {
        static const uint32_t base[8] = {0x0A000000, 0x0A200000, 0x2A000000, 0x2A200000,
                                         0x4A000000, 0x4A200000, 0x6A000000, 0x6A200000};
        return (x ? 0x80000000u : 0) | base[op] | sh << 22 | m << 16 | (amt & 63) << 10 | n << 5 | d;
    }
    void and_(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 0, d, n, m, sh, a)); }
    void bic(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 1, d, n, m, sh, a)); }
    void orr(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 2, d, n, m, sh, a)); }
    void orn(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 3, d, n, m, sh, a)); }
    void eor(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 4, d, n, m, sh, a)); }
    void ands(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(logic_reg(false, 6, d, n, m, sh, a)); }
    void mov(Reg d, Reg m) { orr(d, ZR, m); }
    void movx(Reg d, Reg m) { put(logic_reg(true, 2, d, ZR, m, LSL, 0)); }
    void tst(Reg n, Reg m) { ands(ZR, n, m); }

    // ADC/SBC (flags in, optionally out)
    static uint32_t adcsbc(int op, Reg d, Reg n, Reg m) {  // 0 ADC, 1 ADCS, 2 SBC, 3 SBCS
        static const uint32_t base[4] = {0x1A000000, 0x3A000000, 0x5A000000, 0x7A000000};
        return base[op] | m << 16 | n << 5 | d;
    }
    void adc(Reg d, Reg n, Reg m) { put(adcsbc(0, d, n, m)); }
    void adcs(Reg d, Reg n, Reg m) { put(adcsbc(1, d, n, m)); }
    void sbc(Reg d, Reg n, Reg m) { put(adcsbc(2, d, n, m)); }
    void sbcs(Reg d, Reg n, Reg m) { put(adcsbc(3, d, n, m)); }

    // Shifts by register (the amount is taken mod 32, unlike ARM's: callers handle >= 32)
    static uint32_t shiftv(Shift sh, Reg d, Reg n, Reg m) { return 0x1AC02000 | m << 16 | sh << 10 | n << 5 | d; }
    void lslv(Reg d, Reg n, Reg m) { put(shiftv(LSL, d, n, m)); }
    void lsrv(Reg d, Reg n, Reg m) { put(shiftv(LSR, d, n, m)); }
    void asrv(Reg d, Reg n, Reg m) { put(shiftv(ASR, d, n, m)); }
    void rorv(Reg d, Reg n, Reg m) { put(shiftv(ROR, d, n, m)); }

    // Bitfield moves
    static uint32_t bfm(uint32_t base, Reg d, Reg n, uint32_t immr, uint32_t imms) {
        return base | (immr & 31) << 16 | (imms & 31) << 10 | n << 5 | d;
    }
    void ubfx(Reg d, Reg n, uint32_t lsb, uint32_t width) { put(bfm(0x53000000, d, n, lsb, lsb + width - 1)); }
    void bfi(Reg d, Reg n, uint32_t lsb, uint32_t width) { put(bfm(0x33000000, d, n, (32 - lsb) & 31, width - 1)); }
    void lsli(Reg d, Reg n, uint32_t s) { put(bfm(0x53000000, d, n, (32 - s) & 31, 31 - s)); }  // 1-31
    void lsri(Reg d, Reg n, uint32_t s) { put(bfm(0x53000000, d, n, s, 31)); }                  // 1-31
    void asri(Reg d, Reg n, uint32_t s) { put(bfm(0x13000000, d, n, s, 31)); }                  // 1-31
    void sxtb(Reg d, Reg n) { put(bfm(0x13000000, d, n, 0, 7)); }
    void sxth(Reg d, Reg n) { put(bfm(0x13000000, d, n, 0, 15)); }
    void lsrxi(Reg d, Reg n, uint32_t s) { put(0xD3400000 | (s & 63) << 16 | 63 << 10 | n << 5 | d); }
    void rori(Reg d, Reg n, uint32_t s) { put(0x13800000 | n << 16 | (s & 31) << 10 | n << 5 | d); }  // EXTR

    // Wide moves
    void movz(Reg d, uint32_t imm16, uint32_t hw = 0) { put(0x52800000 | hw << 21 | (imm16 & 0xFFFF) << 5 | d); }
    void movk(Reg d, uint32_t imm16, uint32_t hw = 0) { put(0x72800000 | hw << 21 | (imm16 & 0xFFFF) << 5 | d); }
    void movn(Reg d, uint32_t imm16, uint32_t hw = 0) { put(0x12800000 | hw << 21 | (imm16 & 0xFFFF) << 5 | d); }
    void movzx(Reg d, uint32_t imm16, uint32_t hw) { put(0xD2800000 | hw << 21 | (imm16 & 0xFFFF) << 5 | d); }
    void movkx(Reg d, uint32_t imm16, uint32_t hw) { put(0xF2800000 | hw << 21 | (imm16 & 0xFFFF) << 5 | d); }
    void movi(Reg d, uint32_t v) {  // any 32-bit value in one or two instructions
        if ((v & 0xFFFF0000) == 0) movz(d, v);
        else if ((v & 0xFFFF) == 0) movz(d, v >> 16, 1);
        else if ((~v & 0xFFFF0000) == 0) movn(d, ~v & 0xFFFF);
        else if ((~v & 0xFFFF) == 0) movn(d, (~v >> 16) & 0xFFFF, 1);
        else { movz(d, v & 0xFFFF); movk(d, v >> 16, 1); }
    }
    void movix(Reg d, uint64_t v) {  // a 64-bit address
        movzx(d, v & 0xFFFF, 0);
        for (uint32_t hw = 1; hw < 4; hw++)
            if ((v >> (16 * hw)) & 0xFFFF) movkx(d, (v >> (16 * hw)) & 0xFFFF, hw);
    }

    // Flags
    void mrs_nzcv(Reg t) { put(0xD53B4200 | t); }
    void msr_nzcv(Reg t) { put(0xD51B4200 | t); }
    void cset(Reg d, Cond c) { put(0x1A9F07E0 | (c ^ 1) << 12 | d); }  // CSINC d, wzr, wzr, !c
    void csel(Reg d, Reg n, Reg m, Cond c) { put(0x1A800000 | m << 16 | c << 12 | n << 5 | d); }

    // Loads and stores, unsigned scaled offset from a 64-bit base
    void ldr(Reg t, Reg n, uint32_t off) { put(0xB9400000 | (off / 4) << 10 | n << 5 | t); }
    void str(Reg t, Reg n, uint32_t off) { put(0xB9000000 | (off / 4) << 10 | n << 5 | t); }
    void ldrx(Reg t, Reg n, uint32_t off) { put(0xF9400000 | (off / 8) << 10 | n << 5 | t); }
    void strx(Reg t, Reg n, uint32_t off) { put(0xF9000000 | (off / 8) << 10 | n << 5 | t); }
    void strb(Reg t, Reg n, uint32_t off) { put(0x39000000 | off << 10 | n << 5 | t); }
    void ldrb(Reg t, Reg n, uint32_t off) { put(0x39400000 | off << 10 | n << 5 | t); }
    void strh(Reg t, Reg n, uint32_t off) { put(0x79000000 | (off / 2) << 10 | n << 5 | t); }
    // Register offset: [Xn, Xm] (Xm unscaled, its upper half must be zero)
    void ldr_reg(Reg t, Reg n, Reg m) { put(0xB8606800 | m << 16 | n << 5 | t); }
    void ldrh_reg(Reg t, Reg n, Reg m) { put(0x78606800 | m << 16 | n << 5 | t); }
    void ldrb_reg(Reg t, Reg n, Reg m) { put(0x38606800 | m << 16 | n << 5 | t); }
    void ldrx_reg_lsl3(Reg t, Reg n, Reg m) { put(0xF8607800 | m << 16 | n << 5 | t); }  // [Xn, Xm, LSL #3]

    // Branches. Offsets are in words from the branch itself; patch() fills one in later.
    static uint32_t b_(int32_t words) { return 0x14000000 | (words & 0x3FFFFFF); }
    static uint32_t bl_(int32_t words) { return 0x94000000 | (words & 0x3FFFFFF); }
    static uint32_t bcond_(Cond c, int32_t words) { return 0x54000000 | (words & 0x7FFFF) << 5 | c; }
    static uint32_t cbz_(Reg t, int32_t words) { return 0x34000000 | (words & 0x7FFFF) << 5 | t; }
    static uint32_t cbnz_(Reg t, int32_t words) { return 0x35000000 | (words & 0x7FFFF) << 5 | t; }
    void br(Reg n) { put(0xD61F0000 | n << 5); }
    void blr(Reg n) { put(0xD63F0000 | n << 5); }
    void ret() { put(0xD65F03C0); }
    void nop() { put(0xD503201F); }
    void brk() { put(0xD4200000); }

    // A forward branch: emit it now, point it at here() later.
    struct Fixup { uint32_t* at; uint32_t kind; };  // kind: 0 B, 1 B.cond, 2 CBZ/CBNZ
    Fixup b_fwd() { Fixup f{code, 0}; put(b_(0)); return f; }
    Fixup bcond_fwd(Cond c) { Fixup f{code, 1}; put(bcond_(c, 0)); return f; }
    Fixup cbz_fwd(Reg t) { Fixup f{code, 2}; put(cbz_(t, 0)); return f; }
    Fixup cbnz_fwd(Reg t) { Fixup f{code, 2}; put(cbnz_(t, 0)); return f; }
    void land(Fixup f) {
        int32_t words = (int32_t)(code - f.at);
        if (f.kind == 0) *f.at |= words & 0x3FFFFFF;
        else *f.at |= (words & 0x7FFFF) << 5;
    }
    // A direct branch or call to an absolute address, when it's within +-128 MB; else through x16.
    void jump(const void* target, bool link) {
        int64_t d = ((const uint8_t*)target - (const uint8_t*)code) / 4;
        if (d >= -(1 << 25) && d < (1 << 25)) put(link ? bl_((int32_t)d) : b_((int32_t)d));
        else { movix((Reg)16, (uint64_t)(uintptr_t)target); link ? blr((Reg)16) : br((Reg)16); }
    }
};

}  // namespace bananajit
