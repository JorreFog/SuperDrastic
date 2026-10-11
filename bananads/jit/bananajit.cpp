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

bool Compiler::NativeARM(u32 instr, u16 kind) {
    if (!IsDataProcessing(kind)) return false;
    bool imm = instr & (1 << 25);
    if (!imm && (instr & (1 << 4))) return false;  // shift by a register: later
    u32 op = (instr >> 21) & 0xF;
    bool test = op >= 8 && op <= 11;
    u32 rd = (instr >> 12) & 0xF;
    return rd != 15;  // writes to PC branch (and with S restore CPSR): the interpreter's
    (void)test;
}

// The framework asks per kind, for its flag analysis. Shifts by a register are never native; a write to the
// PC can't be told from the kind, and goes to the interpreter, which reads only the flags the kind reads.
static bool IsShiftByReg(u16 kind) {
    u32 idx = kind < 12 * 18 ? kind % 18 : (kind - 12 * 18) % 9;  // ak_ALU groups of 18, ak_Test of 9
    return (idx >= 4 && idx <= 7) || (kind < 12 * 18 && idx >= 13 && idx <= 16);
}

bool Compiler::CanCompile(bool thumb, u16 kind) {
    return !thumb && IsDataProcessing(kind) && !IsShiftByReg(kind);
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
        A.movi(W0, (u32)ConstantCycles);
        A.add(RCycles, RCycles, W0);
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
        const u32 rm = in & 0xF, st = (in >> 5) & 3, amt = (in >> 7) & 31;
        Reg src;
        if (rm == 15) { A.movi(W3, R15); src = W3; }
        else { Need(rm); src = host(rm); }
        auto carry_bit = [&](u32 bit) { if (wantC) { A.ubfx(W2, src, bit, 1); carry_changes = true; } };
        switch (st) {
        case 0:  // LSL
            if (amt == 0) op2 = src;
            else { carry_bit(32 - amt); A.lsli(W1, src, amt); op2 = W1; }
            break;
        case 1:  // LSR (#0 means #32)
            if (amt == 0) { carry_bit(31); op2 = ZR; }
            else { carry_bit(amt - 1); A.lsri(W1, src, amt); op2 = W1; }
            break;
        case 2:  // ASR (#0 means #32)
            carry_bit(amt == 0 ? 31 : amt - 1);
            A.asri(W1, src, amt == 0 ? 31 : amt);
            op2 = W1;
            break;
        default:  // ROR (#0 means RRX)
            if (amt == 0) {
                carry_bit(0);
                A.lsri(W1, src, 1);
                A.ubfx(W5, RCPSR, 29, 1);
                A.orr(W1, W1, W5, LSL, 31);
            } else {
                carry_bit(amt - 1);
                A.rori(W1, src, amt);
            }
            op2 = W1;
            break;
        }
    }

    Reg n = ZR;
    if (op != 13 && op != 15) {  // MOV and MVN have no Rn
        if (rn == 15) { A.movi(W4, R15); n = W4; }
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

// --- a block ---

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
                          instrs[i].Info.Kind, !thumb && NativeARM(instrs[i].Instr, instrs[i].Info.Kind) ? "native" : "interp",
                          instrs[i].SetFlags, instrs[i].BranchFlags);

    for (int i = 0; i < count; i++) {
        CurInstr = instrs[i];
        R15 = CurInstr.Addr + (thumb ? 4 : 8);
        const bool last = i == count - 1;
        const u16 kind = CurInstr.Info.Kind;
        const bool native = !thumb && NativeARM(CurInstr.Instr, kind);

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
            } else {
                FlushRegs(false);
            }
            FlushCycles();
            LoadFlags();
            skip = A.bcond_fwd((Cond)(cond ^ 1));
        }

        if (native) {
            CompileALU();
            AddCycles(CyclesC());  // the same taken or skipped
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
            if (!native) {  // skipped: the interpreter would have charged its C cycles
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
