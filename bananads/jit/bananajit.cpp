// BananaJIT's code generator. See bananajit.h for the design and docs/JIT.md for what is native.
#include "bananajit.h"

#if defined(JIT_ENABLED) && defined(__aarch64__)

#include <cassert>
#include <cstdlib>

#include "ARMInterpreter.h"
#include "ARMJIT.h"
#include "ARMJIT_Global.h"
#include "NDS.h"
#include "Platform.h"

extern "C" void ARM_Ret();

namespace melonDS {

using namespace bananajit;

static constexpr Reg RCPU = (Reg)29, RCycles = (Reg)28, RCPSR = (Reg)27;
static constexpr Reg W0 = (Reg)0, W1 = (Reg)1, W2 = (Reg)2, W3 = (Reg)3, W4 = (Reg)4, W5 = (Reg)5, W6 = (Reg)6;

static size_t off_R(int i) { return offsetof(ARM, R) + 4 * i; }

Compiler::Compiler(melonDS::NDS& nds) : NDS(nds) {
    ARMJIT_Global::Init();
    CodeMemBase = ARMJIT_Global::AllocateCodeMem();
    CodeBase = (u8*)CodeMemBase;
    CodeSize = ARMJIT_Global::CodeMemorySliceSize;
    // Every offset BananaJIT stores to must fit an unsigned scaled 12-bit load/store offset.
    assert(offsetof(ARM, CurInstr) < 16380 && offsetof(ARM, CodeCycles) < 16380);
    Reset();
    Platform::Log(Platform::LogLevel::Info, "[bananajit] BananaDS's own JIT, %zu MB of code memory\n",
                  CodeSize >> 20);
}

Compiler::~Compiler() { ARMJIT_Global::FreeCodeMem(CodeMemBase); }

void Compiler::Reset() { A = A64((uint32_t*)CodeBase); }

bool Compiler::IsJITFault(const u8* pc) { return pc >= CodeBase && pc < CodeBase + CodeSize; }

u8* Compiler::RewriteMemAccess(u8* pc) {
    Platform::Log(Platform::LogLevel::Error, "[bananajit] a memory fault in JIT code: fastmem must be off\n");
    return pc;
}

// --- what is native ---

static_assert(ARMInstrInfo::ak_TST_REG_LSL_IMM == 12 * 18 && ARMInstrInfo::ak_CMN_IMM == 12 * 18 + 4 * 9 - 1,
              "melonDS's instruction kinds moved: check IsDataProcessing and IsShiftByReg");
static bool IsDataProcessing(u16 kind) { return kind <= ARMInstrInfo::ak_CMN_IMM; }

static bool IsWordByte(u16 k) { return k >= ARMInstrInfo::ak_STR_REG_LSL && k <= ARMInstrInfo::ak_LDRB_POST_IMM; }
static bool IsHalf(u16 k) {
    using namespace ARMInstrInfo;
    return (k >= ak_STRH_REG && k <= ak_STRH_POST_IMM) || (k >= ak_LDRH_REG && k <= ak_LDRSH_POST_IMM);
}

int Compiler::NativeKindARM(u32 instr, u16 kind) {
    const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
    if (IsDataProcessing(kind)) {
        bool imm = instr & (1 << 25);
        if (!imm && (instr & (1 << 4))) return 0;  // shift by a register: later
        return rd != 15 ? 1 : 0;                   // writes to the PC branch: the interpreter's
    }
    if (IsWordByte(kind) || IsHalf(kind)) {
        const bool load = instr & (1 << 20), post = !(instr & (1 << 24)), wb = post || (instr & (1 << 21));
        const bool regoff = IsHalf(kind) ? !(instr & (1 << 22)) : (instr & (1 << 25));
        if (load && rd == 15) return 0;              // a load into the PC is a jump
        if (rn == 15 && wb) return 0;
        if (regoff && (instr & 0xF) == 15) return 0;
        return 2;
    }
    return 0;
}

bool Compiler::NativeARM(u32 instr, u16 kind) { return NativeKindARM(instr, kind) != 0; }

// An ARM data-processing immediate for v (v < 1024 and, from 256 on, a multiple of 4)
static u32 ArmImm(u32 v) { return v < 256 ? v : (15u << 8) | (v >> 2); }

// Thumb instructions that mean exactly what an ARM instruction means (as melonDS's interpreter runs both),
// as that ARM instruction: BananaJIT then compiles them as ARM. Returns 0 if it isn't one (the interpreter
// runs it), 1 data processing, 2 load/store. pc: what a PC read gives, when it isn't R15 (word-aligned for
// ADD/LDR PC-relative). noRotate: a word load that doesn't rotate an unaligned word (LDR SP/PC-relative).
int Compiler::ThumbToARM(u32 t, u16 kind, u32 r15, u32& arm, u16& armKind, u32& pc, bool& noRotate) {
    using namespace ARMInstrInfo;
    const u32 rd = t & 7, rs = (t >> 3) & 7, rn = (t >> 6) & 7, rd8 = (t >> 8) & 7, imm8 = t & 0xFF;
    const u32 imm5 = (t >> 6) & 31;
    pc = r15;
    noRotate = false;
    armKind = 0;
    switch (kind) {
    case tk_LSL_IMM: case tk_LSR_IMM: case tk_ASR_IMM:
        arm = 0xE1B00000 | rd << 12 | imm5 << 7 | ((t >> 11) & 3) << 5 | rs; return 1;    // MOVS rd, rs, sh #i
    case tk_ADD_REG_: arm = 0xE0900000 | rs << 16 | rd << 12 | rn; return 1;           // ADDS rd, rs, rn
    case tk_SUB_REG_: arm = 0xE0500000 | rs << 16 | rd << 12 | rn; return 1;
    case tk_ADD_IMM_: arm = 0xE2900000 | rs << 16 | rd << 12 | rn; return 1;           // ADDS rd, rs, #i3
    case tk_SUB_IMM_: arm = 0xE2500000 | rs << 16 | rd << 12 | rn; return 1;
    case tk_MOV_IMM: arm = 0xE3B00000 | rd8 << 12 | imm8; return 1;                    // MOVS rd, #i8
    case tk_CMP_IMM: arm = 0xE3500000 | rd8 << 16 | imm8; return 1;
    case tk_ADD_IMM: arm = 0xE2900000 | rd8 << 16 | rd8 << 12 | imm8; return 1;
    case tk_SUB_IMM: arm = 0xE2500000 | rd8 << 16 | rd8 << 12 | imm8; return 1;
    case tk_AND_REG: arm = 0xE0100000 | rd << 16 | rd << 12 | rs; return 1;            // ANDS rd, rd, rs
    case tk_EOR_REG: arm = 0xE0300000 | rd << 16 | rd << 12 | rs; return 1;
    case tk_ADC_REG: arm = 0xE0B00000 | rd << 16 | rd << 12 | rs; return 1;
    case tk_SBC_REG: arm = 0xE0D00000 | rd << 16 | rd << 12 | rs; return 1;
    case tk_TST_REG: arm = 0xE1100000 | rd << 16 | rs; return 1;
    case tk_NEG_REG: arm = 0xE2700000 | rs << 16 | rd << 12; return 1;                 // RSBS rd, rs, #0
    case tk_CMP_REG: arm = 0xE1500000 | rd << 16 | rs; return 1;
    case tk_CMN_REG: arm = 0xE1700000 | rd << 16 | rs; return 1;
    case tk_ORR_REG: arm = 0xE1900000 | rd << 16 | rd << 12 | rs; return 1;
    case tk_BIC_REG: arm = 0xE1D00000 | rd << 16 | rd << 12 | rs; return 1;
    case tk_MVN_REG: arm = 0xE1F00000 | rd << 12 | rs; return 1;                       // MVNS rd, rs
    case tk_ADD_HIREG: case tk_CMP_HIREG: case tk_MOV_HIREG: {
        const u32 hd = (t & 7) | ((t >> 4) & 8), hs = (t >> 3) & 0xF;
        if (kind == tk_CMP_HIREG) { arm = 0xE1500000 | hd << 16 | hs; return 1; }     // CMP hd, hs
        if (hd == 15) return 0;                                                       // a jump
        arm = (kind == tk_ADD_HIREG ? 0xE0800000 | hd << 16 : 0xE1A00000) | hd << 12 | hs;  // ADD / MOV, no S
        return 1;
    }
    case tk_ADD_PCREL: pc = r15 & ~2u; arm = 0xE2800000 | 15 << 16 | rd8 << 12 | ArmImm(imm8 << 2); return 1;
    case tk_ADD_SPREL: arm = 0xE2800000 | 13 << 16 | rd8 << 12 | ArmImm(imm8 << 2); return 1;
    case tk_ADD_SP:
        arm = ((t & 0x80) ? 0xE2400000 : 0xE2800000) | 13 << 16 | 13 << 12 | ArmImm((t & 0x7F) << 2);
        return 1;
    case tk_LDR_PCREL:
        pc = r15 & ~2u; noRotate = true;
        arm = 0xE5900000 | 15 << 16 | rd8 << 12 | imm8 << 2; armKind = ak_LDR_IMM; return 2;
    case tk_STR_REG: arm = 0xE7800000 | rs << 16 | rd << 12 | rn; armKind = ak_STR_REG_LSL; return 2;
    case tk_STRB_REG: arm = 0xE7C00000 | rs << 16 | rd << 12 | rn; armKind = ak_STRB_REG_LSL; return 2;
    case tk_LDR_REG: arm = 0xE7900000 | rs << 16 | rd << 12 | rn; armKind = ak_LDR_REG_LSL; return 2;
    case tk_LDRB_REG: arm = 0xE7D00000 | rs << 16 | rd << 12 | rn; armKind = ak_LDRB_REG_LSL; return 2;
    case tk_STRH_REG: arm = 0xE18000B0 | rs << 16 | rd << 12 | rn; armKind = ak_STRH_REG; return 2;
    case tk_LDRSB_REG: arm = 0xE19000D0 | rs << 16 | rd << 12 | rn; armKind = ak_LDRSB_REG; return 2;
    case tk_LDRH_REG: arm = 0xE19000B0 | rs << 16 | rd << 12 | rn; armKind = ak_LDRH_REG; return 2;
    case tk_LDRSH_REG: arm = 0xE19000F0 | rs << 16 | rd << 12 | rn; armKind = ak_LDRSH_REG; return 2;
    case tk_STR_IMM: arm = 0xE5800000 | rs << 16 | rd << 12 | imm5 << 2; armKind = ak_STR_IMM; return 2;
    case tk_LDR_IMM: arm = 0xE5900000 | rs << 16 | rd << 12 | imm5 << 2; armKind = ak_LDR_IMM; return 2;
    case tk_STRB_IMM: arm = 0xE5C00000 | rs << 16 | rd << 12 | imm5; armKind = ak_STRB_IMM; return 2;
    case tk_LDRB_IMM: arm = 0xE5D00000 | rs << 16 | rd << 12 | imm5; armKind = ak_LDRB_IMM; return 2;
    case tk_STRH_IMM: case tk_LDRH_IMM: {
        const u32 o = imm5 << 1;
        arm = (kind == tk_STRH_IMM ? 0xE1C000B0 : 0xE1D000B0) | rs << 16 | rd << 12 | (o >> 4) << 8 | (o & 0xF);
        armKind = kind == tk_STRH_IMM ? ak_STRH_IMM : ak_LDRH_IMM;
        return 2;
    }
    case tk_STR_SPREL: arm = 0xE5800000 | 13 << 16 | rd8 << 12 | imm8 << 2; armKind = ak_STR_IMM; return 2;
    case tk_LDR_SPREL:
        noRotate = true;
        arm = 0xE5900000 | 13 << 16 | rd8 << 12 | imm8 << 2; armKind = ak_LDR_IMM; return 2;
    default: return 0;
    }
}

static bool ThumbKindNative(u16 kind) {
    using namespace ARMInstrInfo;
    if (kind > tk_LDR_SPREL) return false;
    return kind != tk_LSL_REG && kind != tk_LSR_REG && kind != tk_ASR_REG && kind != tk_ROR_REG && kind != tk_MUL_REG;
}

// The framework asks per kind, for its flag analysis. Shifts by a register are never native; a write to the
// PC can't be told from the kind, and goes to the interpreter, which reads only the flags the kind reads.
static bool IsShiftByReg(u16 kind) {
    u32 idx = kind < 12 * 18 ? kind % 18 : (kind - 12 * 18) % 9;  // ak_ALU groups of 18, ak_Test of 9
    return (idx >= 4 && idx <= 7) || (kind < 12 * 18 && idx >= 13 && idx <= 16);
}

bool Compiler::CanCompile(bool thumb, u16 kind) {
    if (thumb) return ThumbKindNative(kind);
    return ((IsDataProcessing(kind) && !IsShiftByReg(kind)) || IsWordByte(kind) || IsHalf(kind));
}

// --- registers, flags, cycles ---

Compiler::Reg Compiler::host(int g) const { return (Reg)(g < 8 ? 19 + g : 9 + (g - 8)); }

void Compiler::Need(int g) {
    if (Loaded & (1 << g)) return;
    A.ldr(host(g), RCPU, off_R(g));
    Loaded |= 1 << g;
}

void Compiler::Dirty(int g) {
    Loaded |= 1 << g;
    Dirt |= 1 << g;
}

void Compiler::FlushRegs(bool forget) {
    for (int g = 0; g < 15; g++)
        if (Dirt & (1 << g)) A.str(host(g), RCPU, off_R(g));
    Dirt = 0;
    if (forget) Loaded = 0;
}

void Compiler::FlushCycles() {
    if (!ConstantCycles) return;
    if (ConstantCycles > 0 && ConstantCycles < 4096) A.addi(RCycles, RCycles, ConstantCycles);
    else {
        A.movi((Reg)16, (u32)ConstantCycles);
        A.add(RCycles, RCycles, (Reg)16);
    }
    ConstantCycles = 0;
}

void Compiler::LoadFlags() {
    A.lsri(W5, RCPSR, 28);
    A.lsli(W5, W5, 28);
    A.msr_nzcv(W5);
}

s32 Compiler::CyclesC() const {
    if (CurCPU->Num) return NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 1 : 3];
    return (R15 & 2) ? 0 : CurInstr.CodeCycles;
}

void Compiler::ExitBlock() {
    FlushCycles();
    FlushRegs(false);
    A.str(RCycles, RCPU, offsetof(ARM, Cycles));
    A.str(RCPSR, RCPU, offsetof(ARM, CPSR));
    A.jump((const void*)&ARM_Ret, false);
}

// --- one instruction through melonDS's interpreter ---

void Compiler::Fallback(bool thumb) {
    FlushCycles();
    FlushRegs(true);
    A.str(RCycles, RCPU, offsetof(ARM, Cycles));
    A.str(RCPSR, RCPU, offsetof(ARM, CPSR));
    A.movi(W0, CurInstr.Instr);  // R15 is already stored (CompileBlock)
    A.str(W0, RCPU, offsetof(ARM, CurInstr));
    if (CurCPU->Num == 0) {
        A.movi(W0, (u32)(s32)CurInstr.CodeCycles);
        A.str(W0, RCPU, offsetof(ARM, CodeCycles));
    }
    A.movx(W0, RCPU);
    const void* fn;
    if (thumb) fn = (const void*)InterpretTHUMB[CurInstr.Info.Kind];
    else if (CurInstr.Info.Kind == ARMInstrInfo::ak_BLX_IMM) fn = (const void*)&ARMInterpreter::A_BLX_IMM;
    else fn = (const void*)InterpretARM[CurInstr.Info.Kind];
    A.jump(fn, true);
    A.ldr(RCycles, RCPU, offsetof(ARM, Cycles));
    A.ldr(RCPSR, RCPU, offsetof(ARM, CPSR));
}

// --- ARM data processing, native ---

// A register operand shifted by an immediate (bits 0-11 of a data-processing or LDR/STR instruction), into
// dst (or the register itself when unshifted). With wantC, the shifter's carry goes to W2 when it changes C.
Compiler::Reg Compiler::Shifter(u32 in, bool wantC, bool& carry_changes, Reg dst) {
    const u32 rm = in & 0xF, st = (in >> 5) & 3, amt = (in >> 7) & 31;
    Reg src;
    if (rm == 15) { A.movi(W3, PCValue); src = W3; }
    else { Need(rm); src = host(rm); }
    auto carry_bit = [&](u32 bit) { if (wantC) { A.ubfx(W2, src, bit, 1); carry_changes = true; } };
    switch (st) {
    case 0:  // LSL
        if (amt == 0) return src;
        carry_bit(32 - amt);
        A.lsli(dst, src, amt);
        return dst;
    case 1:  // LSR (#0 means #32)
        if (amt == 0) { carry_bit(31); return ZR; }
        carry_bit(amt - 1);
        A.lsri(dst, src, amt);
        return dst;
    case 2:  // ASR (#0 means #32)
        carry_bit(amt == 0 ? 31 : amt - 1);
        A.asri(dst, src, amt == 0 ? 31 : amt);
        return dst;
    default:  // ROR (#0 means RRX)
        if (amt == 0) {
            carry_bit(0);
            A.lsri(dst, src, 1);
            A.ubfx(W5, RCPSR, 29, 1);
            A.orr(dst, dst, W5, LSL, 31);
        } else {
            carry_bit(amt - 1);
            A.rori(dst, src, amt);
        }
        return dst;
    }
}

void Compiler::CompileALU() {
    const u32 in = CurInstr.Instr;
    const u32 op = (in >> 21) & 0xF, rn = (in >> 16) & 0xF, rd = (in >> 12) & 0xF;
    const bool S = in & (1 << 20), I = in & (1 << 25);
    const bool test = op >= 8 && op <= 11;
    const bool logical = op <= 1 || op == 8 || op == 9 || op >= 12;
    const u8 flags = S ? CurInstr.SetFlags : 0;
    const bool wantC = logical && (flags & ARMInstrInfo::flag_C);

    // Operand 2 -> op2 (a register), the shifter's carry -> W2 when it changes C (carry_changes)
    Reg op2;
    bool carry_changes = false;
    if (I) {
        u32 rot = ((in >> 8) & 0xF) * 2, imm = in & 0xFF;
        imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        if (imm == 0) op2 = ZR;
        else { A.movi(W1, imm); op2 = W1; }
        if (wantC && rot) {
            A.movi(W2, imm >> 31);
            carry_changes = true;
        }
    } else {
        op2 = Shifter(in, wantC, carry_changes, W1);
    }

    Reg n = ZR;
    if (op != 13 && op != 15) {  // MOV and MVN have no Rn
        if (rn == 15) { A.movi(W4, PCValue); n = W4; }
        else { Need(rn); n = host(rn); }
    }
    Reg d = test ? W0 : host(rd);
    if (test && !flags) return;  // a compare nobody reads

    if (logical) {
        switch (op) {
        case 0: case 8: A.and_(d, n, op2); break;   // AND, TST
        case 1: case 9: A.eor(d, n, op2); break;    // EOR, TEQ
        case 12: A.orr(d, n, op2); break;           // ORR
        case 13: A.mov(d, op2); break;              // MOV
        case 14: A.bic(d, n, op2); break;           // BIC
        case 15: A.orn(d, ZR, op2); break;          // MVN
        }
        if (flags & (ARMInstrInfo::flag_N | ARMInstrInfo::flag_Z)) {
            A.tst(d, d);
            A.mrs_nzcv(W6);
            A.ubfx(W6, W6, 30, 2);
            A.bfi(RCPSR, W6, 30, 2);
        }
        if (carry_changes) A.bfi(RCPSR, W2, 29, 1);
    } else {
        if (op == 5 || op == 6 || op == 7) LoadFlags();  // ADC, SBC, RSC read C
        const bool f = flags != 0;
        switch (op) {
        case 2: f ? A.subs(d, n, op2) : A.sub(d, n, op2); break;     // SUB
        case 3: f ? A.subs(d, op2, n) : A.sub(d, op2, n); break;     // RSB
        case 4: f ? A.adds(d, n, op2) : A.add(d, n, op2); break;     // ADD
        case 5: f ? A.adcs(d, n, op2) : A.adc(d, n, op2); break;     // ADC
        case 6: f ? A.sbcs(d, n, op2) : A.sbc(d, n, op2); break;     // SBC
        case 7: f ? A.sbcs(d, op2, n) : A.sbc(d, op2, n); break;     // RSC
        case 10: A.subs(ZR, n, op2); break;                          // CMP
        case 11: A.adds(ZR, n, op2); break;                          // CMN
        }
        if (f) {
            A.mrs_nzcv(W6);
            A.ubfx(W6, W6, 28, 4);
            A.bfi(RCPSR, W6, 28, 4);
        }
    }
    if (!test) Dirty(rd);
}

// --- ARM loads and stores, native: the address here, the access through the interpreter's own functions ---

// What the interpreter's A_LDR/A_STR... do for the access itself, cycles included (docs/JIT.md).
static u32 bj_ldr(ARM* c, u32 a) { u32 v; c->DataRead32(a, &v); c->AddCycles_CDI(); u32 r = (a & 3) << 3; return r ? (v >> r) | (v << (32 - r)) : v; }
static u32 bj_ldr_raw(ARM* c, u32 a) { u32 v; c->DataRead32(a, &v); c->AddCycles_CDI(); return v; }
static u32 bj_ldrb(ARM* c, u32 a) { u32 v; c->DataRead8(a, &v); c->AddCycles_CDI(); return v; }
static u32 bj_ldrh(ARM* c, u32 a) { u32 v; c->DataRead16(a, &v); c->AddCycles_CDI(); return v; }
static u32 bj_ldrsb(ARM* c, u32 a) { u32 v; c->DataRead8(a, &v); c->AddCycles_CDI(); return (u32)(s32)(s8)v; }
static u32 bj_ldrsh(ARM* c, u32 a) { u32 v; c->DataRead16(a, &v); c->AddCycles_CDI(); return (u32)(s32)(s16)v; }
static void bj_str(ARM* c, u32 a, u32 v) { c->DataWrite32(a, v); c->AddCycles_CD(); }
static void bj_strb(ARM* c, u32 a, u32 v) { c->DataWrite8(a, (u8)v); c->AddCycles_CD(); }
static void bj_strh(ARM* c, u32 a, u32 v) { c->DataWrite16(a, (u16)v); c->AddCycles_CD(); }

void Compiler::StoreHigh() {
    for (int g = 8; g < 15; g++)
        if (Dirt & (1 << g)) A.str(host(g), RCPU, off_R(g));
    Dirt &= 0x00FF;
}

// Calls fn(cpu, w1, w2) with the state the interpreter's functions read in memory: cycles, CPSR, CodeCycles.
// Guest r0-r7 are in callee-saved registers and survive; r8-r14 are stored first and loaded again after.
void Compiler::CallOut(const void* fn) {
    FlushCycles();
    StoreHigh();
    A.str(RCycles, RCPU, offsetof(ARM, Cycles));
    A.str(RCPSR, RCPU, offsetof(ARM, CPSR));
    if (CurCPU->Num == 0) {
        A.movi((Reg)16, (u32)(s32)CurInstr.CodeCycles);
        A.str((Reg)16, RCPU, offsetof(ARM, CodeCycles));
        if (Thumb) {
            A.movi((Reg)16, R15);
            A.str((Reg)16, RCPU, off_R(15));
        }
    }
    A.movx(W0, RCPU);
    A.jump(fn, true);
    A.ldr(RCycles, RCPU, offsetof(ARM, Cycles));
    A.ldr(RCPSR, RCPU, offsetof(ARM, CPSR));
    for (int g = 8; g < 15; g++)
        if (Loaded & (1 << g)) A.ldr(host(g), RCPU, off_R(g));
}

void Compiler::CompileMem() {
    const u32 in = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    const u32 rn = (in >> 16) & 0xF, rd = (in >> 12) & 0xF;
    const bool load = in & (1 << 20), up = in & (1 << 23), pre = in & (1 << 24);
    const bool wb = !pre || (in & (1 << 21));
    const bool half = IsHalf(kind);

    // the offset -> off (a register, or an immediate)
    Reg off = ZR;
    u32 imm = 0;
    bool regoff;
    if (half) {
        regoff = !(in & (1 << 22));
        if (regoff) { Need(in & 0xF); off = host(in & 0xF); }
        else imm = (in & 0xF) | ((in >> 4) & 0xF0);
    } else {
        regoff = in & (1 << 25);
        if (regoff) { bool c = false; off = Shifter(in, false, c, W3); }
        else imm = in & 0xFFF;
    }

    // the base, and the address -> W1
    Reg base;
    if (rn == 15) { A.movi(W4, PCValue); base = W4; }
    else { Need(rn); base = host(rn); }
    auto apply = [&](Reg dst, Reg from) {
        if (regoff) up ? A.add(dst, from, off) : A.sub(dst, from, off);
        else if (imm == 0) { if (dst != from) A.mov(dst, from); }
        else up ? A.addi(dst, from, imm) : A.subi(dst, from, imm);
    };
    if (pre) apply(W1, base);
    else A.mov(W1, base);

    // a store's value -> W2, before the base is written back (STR r0, [r0], #4 stores the old r0)
    if (!load) {
        if (rd == 15) A.movi(W2, R15 + 4);
        else { Need(rd); A.mov(W2, host(rd)); }
    }

    // write-back, then the call, then the loaded register (if Rd is Rn, the load wins, as in the interpreter)
    if (wb) {
        if (pre) A.mov(host(rn), W1);
        else apply(host(rn), host(rn));
        Dirty(rn);
    }

    const void* fn;
    using namespace ARMInstrInfo;
    if (half) {
        if (kind <= ak_STRH_POST_IMM) fn = (const void*)&bj_strh;
        else if (kind <= ak_LDRH_POST_IMM) fn = (const void*)&bj_ldrh;
        else if (kind <= ak_LDRSB_POST_IMM) fn = (const void*)&bj_ldrsb;
        else fn = (const void*)&bj_ldrsh;
    } else {
        const bool byte = in & (1 << 22);
        fn = load ? (byte ? (const void*)&bj_ldrb : NoRotate ? (const void*)&bj_ldr_raw : (const void*)&bj_ldr)
                  : (byte ? (const void*)&bj_strb : (const void*)&bj_str);
    }
    CallOut(fn);
    if (load) {
        A.mov(host(rd), W0);
        Dirty(rd);
    }
}

// --- a block ---

static const char* TraceName(bool thumb, const FetchedInstr& f) {
    static const char* names[3] = {"interp", "native-alu", "native-mem"};
    u32 a, pc;
    u16 k;
    bool nr;
    return names[thumb ? Compiler::ThumbToARM(f.Instr, f.Info.Kind, f.Addr + 4, a, k, pc, nr)
                       : Compiler::NativeKindARM(f.Instr, f.Info.Kind)];
}

JitBlockEntry Compiler::CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int count, bool) {
    if ((size_t)((u8*)A.here() - CodeBase) > CodeSize - 64 * 1024) {
        Platform::Log(Platform::LogLevel::Debug, "[bananajit] code memory full, starting over\n");
        NDS.JIT.ResetBlockCache();
    }
    u32* start = A.here();
    CurCPU = cpu;
    Thumb = thumb;
    ConstantCycles = 0;
    Loaded = Dirt = 0;

    A.ldr(RCycles, RCPU, offsetof(ARM, Cycles));
    A.ldr(RCPSR, RCPU, offsetof(ARM, CPSR));

    static const bool trace = getenv("BANANAJIT_TRACE") != nullptr;
    if (trace)
        for (int i = 0; i < count; i++)
            Platform::Log(Platform::LogLevel::Info, "[bananajit] ARM%d %s %08x: %08x kind %u %s flags %x branch %x\n",
                          cpu->Num ? 7 : 9, i ? "  " : "block", instrs[i].Addr, instrs[i].Instr,
                          instrs[i].Info.Kind, TraceName(thumb, instrs[i]),
                          instrs[i].SetFlags, instrs[i].BranchFlags);

    for (int i = 0; i < count; i++) {
        CurInstr = instrs[i];
        R15 = CurInstr.Addr + (thumb ? 4 : 8);
        const bool last = i == count - 1;
        const u16 kind = CurInstr.Info.Kind;
        u32 armInstr = 0;
        u16 armKind = 0;
        PCValue = R15;
        NoRotate = false;
        const int nk = thumb ? ThumbToARM(CurInstr.Instr, kind, R15, armInstr, armKind, PCValue, NoRotate)
                             : NativeKindARM(CurInstr.Instr, kind);
        const bool native = nk != 0;
        const bool selfCycles = nk != 1;  // the interpreter or a memory helper charges this one's cycles

        u32 cond = 0xE;
        if (!thumb) cond = CurInstr.Cond();
        else if (kind == ARMInstrInfo::tk_BCOND) cond = (CurInstr.Instr >> 8) & 0xF;
        const bool nop = !thumb && cond == 0xF && kind != ARMInstrInfo::ak_BLX_IMM;

        // The framework's rule: the PC is stored for the interpreter, for a branch it followed, and at the end
        // of a block that doesn't end in an unconditional branch. Before the condition check: a conditional
        // instruction that is skipped must leave the PC right too (a loop's untaken branch at a block's end).
        if ((!native && !nop) || (CurInstr.BranchFlags & branch_FollowCondTaken) ||
            (last && (!CurInstr.Info.Branches() || cond < 0xE))) {
            A.movi(W0, R15);
            A.str(W0, RCPU, off_R(15));
        }

        if (nop) {
            AddCycles(CyclesC());  // the unconditional space: nothing to do on the DS's CPUs
            continue;
        }

        A64::Fixup skip{};
        bool conditional = cond < 0xE && !(!thumb && kind == ARMInstrInfo::ak_BLX_IMM);
        if (conditional) {
            if (native) {  // whatever it touches is loaded on both paths
                for (int g = 0; g < 15; g++)
                    if ((CurInstr.Info.SrcRegs | CurInstr.Info.DstRegs) & (1 << g)) Need(g);
                if (nk == 2) StoreHigh();  // its call stores them only on the taken path
            } else {
                FlushRegs(false);
            }
            FlushCycles();
            LoadFlags();
            skip = A.bcond_fwd((Cond)(cond ^ 1));
        }

        if (thumb && nk) {  // compiled as the ARM instruction it means; flags, registers and cycles stay Thumb's
            CurInstr.Instr = armInstr;
            if (nk == 2) CurInstr.Info.Kind = armKind;
        }
        if (nk == 1) {
            CompileALU();
            AddCycles(CyclesC());  // the same taken or skipped
        } else if (nk == 2) {
            CompileMem();
        } else {
            Fallback(thumb);
        }

        // after it ran: idle-loop marking, and leaving a block that followed the other way
        auto special = [&](bool taken) {
            if (taken && (CurInstr.BranchFlags & branch_IdleBranch)) {
                A.movz(W0, 1);
                A.strb(W0, RCPU, offsetof(ARM, IdleLoop));
            }
            if ((taken && (CurInstr.BranchFlags & branch_FollowCondNotTaken)) ||
                (!taken && (CurInstr.BranchFlags & branch_FollowCondTaken))) {
                s32 keep = ConstantCycles;
                u16 keepDirt = Dirt;
                ExitBlock();
                ConstantCycles = keep;  // the other path goes on with the same compile-time state
                Dirt = keepDirt;
            }
        };

        if (conditional) {
            u16 dirt = Dirt;
            s32 cyc = ConstantCycles;
            special(true);
            auto join = A.b_fwd();
            A.land(skip);
            if (selfCycles) {  // skipped: the interpreter would have charged its C cycles
                s32 c = CyclesC();
                if (c > 0 && c < 4096) A.addi(RCycles, RCycles, c);
                else if (c) { A.movi(W0, (u32)c); A.add(RCycles, RCycles, W0); }
            }
            Dirt = dirt;
            ConstantCycles = cyc;
            special(false);
            A.land(join);
        } else {
            special(true);
        }
    }

    ExitBlock();
    __builtin___clear_cache((char*)start, (char*)A.here());
    return (JitBlockEntry)start;
}

}  // namespace melonDS

#endif
