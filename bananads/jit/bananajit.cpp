// BananaJIT's code generator. See bananajit.h for the design and docs/JIT.md for what is native.
#include "bananajit.h"

#if defined(JIT_ENABLED) && defined(__aarch64__)

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "ARMInterpreter.h"
#include "ARMJIT.h"
#include "ARMJIT_Global.h"
#include "NDS.h"
#include "Platform.h"

extern "C" void ARM_Ret();
extern "C" void BJ_RetState();  // linkage.S: stores w28 (cycles) and w27 (CPSR), then ARM_Ret
// linkage.S's ARM_Dispatch and BJ_RetState read these offsets
extern "C" const melonDS::u32 bj_offsets[2];
extern "C" const melonDS::u32 bj_offsets[2] = { (melonDS::u32)offsetof(melonDS::ARM, Cycles), (melonDS::u32)offsetof(melonDS::ARM, CPSR) };

namespace melonDS {

using namespace bananajit;

static constexpr Reg RCPU = (Reg)29, RCycles = (Reg)28, RCPSR = (Reg)27;
static constexpr Reg W0 = (Reg)0, W1 = (Reg)1, W2 = (Reg)2, W3 = (Reg)3, W4 = (Reg)4, W5 = (Reg)5, W6 = (Reg)6;

static size_t off_R(int i) { return offsetof(ARM, R) + 4 * i; }
static bool FastLoads9Ok();

Compiler::Compiler(melonDS::NDS& nds) : NDS(nds) {
    ARMJIT_Global::Init();
    CodeMemBase = ARMJIT_Global::AllocateCodeMem();
    CodeBase = (u8*)CodeMemBase;
    CodeSize = ARMJIT_Global::CodeMemorySliceSize;
    // Every offset BananaJIT stores to must fit an unsigned scaled 12-bit load/store offset.
    assert(offsetof(ARM, CurInstr) < 16380 && offsetof(ARM, CodeCycles) < 16380);
    Reset();
    FastLoads = FastLoads9Ok() && !getenv("BANANAJIT_NO_FASTLOADS");
    FastStores = FastLoads && !getenv("BANANAJIT_NO_FASTSTORES") && offsetof(AddressRange, Code) % 4 == 0;
    FastMem7On = !getenv("BANANAJIT_NO_FASTMEM7") && offsetof(AddressRange, Code) % 4 == 0 &&
                 offsetof(ARM, CodeCycles) < 16384;
    LinkBlocks = !getenv("BANANAJIT_NO_LINK") && offsetof(ARM, FastBlockLookup) % 8 == 0 &&
                 offsetof(ARM, FastBlockLookup) < 32768 && offsetof(ARM, StopExecution) % 4 == 0;
    DoMul = !getenv("BANANAJIT_NO_MUL");
    DoLDM = !getenv("BANANAJIT_NO_LDM");
    DoJumps = !getenv("BANANAJIT_NO_JUMPS");
    Exact = getenv("BANANAJIT_EXACT") != nullptr;
    Platform::Log(Platform::LogLevel::Info, "[bananajit] timing %s\n",
                  Exact ? "exact (the interpreter's, cycle for cycle)" : "per block, with fast memory");
    Platform::Log(Platform::LogLevel::Info, "[bananajit] BananaDS's own JIT, %zu MB of code memory, ARM9 loads %s\n",
                  CodeSize >> 20, FastLoads ? "inline" : "through calls");
    Platform::Log(Platform::LogLevel::Info, "[bananajit] blocks %s, ARM9 stores %s\n",
                  LinkBlocks ? "linked" : "return to the loop", FastStores ? "inline" : "through calls");
    Platform::Log(Platform::LogLevel::Info, "[bananajit] ARM7 loads/stores %s\n", FastMem7On ? "inline" : "through calls");
}

// BANANAJIT_PERFMAP=1: name every block for perf (/tmp/perf-<pid>.map, perf's JIT map format) and write the
// code memory out at exit (/tmp/bananajit-<pid>-<base>.bin), so a profile's hot addresses can be disassembled.
static FILE* PerfMap() {
    static FILE* f = getenv("BANANAJIT_PERFMAP") ? fopen(("/tmp/perf-" + std::to_string(getpid()) + ".map").c_str(), "w")
                                                 : nullptr;
    return f;
}

// ... and each block's code as it is compiled (/tmp/bananajit-<pid>.code: u64 address, u32 bytes, the code), so a
// profile's hot blocks can be disassembled (the code memory is reset before the compiler is destroyed)
static FILE* CodeDump() {
    static FILE* f = getenv("BANANAJIT_PERFMAP") ? fopen(("/tmp/bananajit-" + std::to_string(getpid()) + ".code").c_str(), "wb")
                                                 : nullptr;
    return f;
}

Compiler::~Compiler() {
    if (PerfMap()) fflush(PerfMap());
    if (CodeDump()) fflush(CodeDump());
    ARMJIT_Global::FreeCodeMem(CodeMemBase);
}

void Compiler::Reset() {
    A = A64((uint32_t*)CodeBase);
    Patches.clear();
    SlowPaths.clear();
}

bool Compiler::IsJITFault(const u8* pc) { return pc >= CodeBase && pc < CodeBase + CodeSize; }

// A fast-memory access faulted (its address isn't mapped: I/O, VRAM, code the framework write-protects...). Its
// sequence's first instruction becomes a branch to the slow path compiled for it, which does the access through
// melonDS's functions and comes back after the sequence. Execution resumes at that branch. It stays slow.
u8* Compiler::RewriteMemAccess(u8* pc) {
    auto it = Patches.find((u32)(pc - CodeBase));
    if (it == Patches.end()) {
        Platform::Log(Platform::LogLevel::Error, "[bananajit] a fault at %p that isn't a fast-memory access\n", pc);
        abort();
    }
    u32* seq = (u32*)(CodeBase + it->second.seq);
    u32* slow = (u32*)(CodeBase + it->second.slow);
    *seq = A64::b_((int32_t)(slow - seq));
    __builtin___clear_cache((char*)seq, (char*)(seq + 1));
    return (u8*)seq;
}

// --- what is native ---

static_assert(ARMInstrInfo::ak_TST_REG_LSL_IMM == 12 * 18 && ARMInstrInfo::ak_CMN_IMM == 12 * 18 + 4 * 9 - 1,
              "melonDS's instruction kinds moved: check IsDataProcessing");
static bool IsDataProcessing(u16 kind) { return kind <= ARMInstrInfo::ak_CMN_IMM; }

static bool IsWordByte(u16 k) { return k >= ARMInstrInfo::ak_STR_REG_LSL && k <= ARMInstrInfo::ak_LDRB_POST_IMM; }
static bool IsHalf(u16 k) {
    using namespace ARMInstrInfo;
    return (k >= ak_STRH_REG && k <= ak_STRH_POST_IMM) || (k >= ak_LDRH_REG && k <= ak_LDRSH_POST_IMM);
}

int Compiler::NativeKindARM(u32 instr, u16 kind) {
    using namespace ARMInstrInfo;
    const u32 rd = (instr >> 12) & 0xF, rn = (instr >> 16) & 0xF;
    if (IsDataProcessing(kind)) {
        bool imm = instr & (1 << 25);
        if (!imm && (instr & (1 << 4))) {  // operand 2 shifted by a register
            const u32 rm = instr & 0xF, rs = (instr >> 8) & 0xF;
            if (rd == 15 || rn == 15 || rm == 15 || rs == 15) return 0;  // a PC operand: the interpreter's
            return 4;
        }
        return rd != 15 ? 1 : 0;                   // writes to the PC branch: the interpreter's
    }
    if (kind >= ak_MUL && kind <= ak_SMLAL) {  // MUL/MLA/UMULL/UMLAL/SMULL/SMLAL (never a PC register)
        if (((instr >> 16) & 0xF) == 15 || ((instr >> 12) & 0xF) == 15 ||
            ((instr >> 8) & 0xF) == 15 || (instr & 0xF) == 15) return 0;
        return 3;
    }
    if (IsWordByte(kind) || IsHalf(kind)) {
        const bool load = instr & (1 << 20), post = !(instr & (1 << 24)), wb = post || (instr & (1 << 21));
        const bool regoff = IsHalf(kind) ? !(instr & (1 << 22)) : (instr & (1 << 25));
        if (load && rd == 15) return 0;              // a load into the PC is a jump
        if (rn == 15 && wb) return 0;
        if (regoff && (instr & 0xF) == 15) return 0;
        return 2;
    }
    if (kind == ak_LDM || kind == ak_STM) {
        if (instr & (1 << 22)) return 0;   // the S bit (user-bank / CPSR-restore form): the interpreter's
        if (instr & (1 << 15)) return 0;   // the PC in the list: LDM is a jump, STM a rare constant: interpreter
        if ((instr & 0xFFFF) == 0) return 0;  // an empty register list is unpredictable: the interpreter's
        if (rn == 15) return 0;            // a PC base is unpredictable: the interpreter's
        return 5;
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
    case tk_LSL_REG: case tk_LSR_REG: case tk_ASR_REG: case tk_ROR_REG: {              // MOVS rd, rd, sh rs
        const u32 sh = kind == tk_LSL_REG ? 0 : kind == tk_LSR_REG ? 1 : kind == tk_ASR_REG ? 2 : 3;
        arm = 0xE1B00000 | rd << 12 | rs << 8 | sh << 5 | 1 << 4 | rd; return 4;
    }
    case tk_MUL_REG: arm = 0xE0100090 | rd << 16 | rs << 8 | rd; armKind = ak_MUL; return 3;  // MULS rd, rd, rs
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
    // Block transfers (not an ARM instruction form: CompileBlockTransfer reads the Thumb encoding directly).
    case tk_PUSH:  return ((t & 0xFF) | ((t & (1 << 8)) ? (1 << 14) : 0)) ? 5 : 0;  // STMDB sp! {low regs, LR}
    case tk_POP:   return (t & (1 << 8)) ? 0 : ((t & 0xFF) ? 5 : 0);  // POP with PC is a jump: the interpreter's
    case tk_LDMIA: case tk_STMIA: return (t & 0xFF) ? 5 : 0;          // LDMIA/STMIA rb! {low regs}
    default: return 0;
    }
}

static bool ThumbKindNative(u16 kind) {
    using namespace ARMInstrInfo;
    // everything ThumbToARM has a case for: 0..tk_LDR_SPREL, plus the four block-transfer kinds
    return kind <= tk_LDR_SPREL || kind == tk_PUSH || kind == tk_POP || kind == tk_LDMIA || kind == tk_STMIA;
}

bool Compiler::CanCompile(bool thumb, u16 kind) {
    using namespace ARMInstrInfo;
    if (thumb) return ThumbKindNative(kind);
    return IsDataProcessing(kind) || IsWordByte(kind) || IsHalf(kind) || (kind >= ak_MUL && kind <= ak_SMLAL) ||
           kind == ak_LDM || kind == ak_STM;
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

// cond: the condition the flags are for (-1: all four needed). EQ, NE, MI and PL read only N and Z, which a
// logical S instruction's TST leaves right in the host's NZCV (its C and V are not the guest's)
void Compiler::LoadFlags(int cond) {
    if (FlagsEpoch == A.flagEpoch) return;  // nothing has written the host's NZCV since it held the guest's
    if ((cond == 0 || cond == 1 || cond == 4 || cond == 5) && NZEpoch == A.flagEpoch) return;
    A.lsri(W5, RCPSR, 28);
    A.lsli(W5, W5, 28);
    A.msr_nzcv(W5);
    FlagsEpoch = NZEpoch = A.flagEpoch;
}

// AddCycles_CI(internal): a nonsequential code fetch (index 0/2, not AddCycles_C's 1/3) plus the internal
// cycles. Charged straight to RCycles here, so it lands only on the taken path of a conditional instruction
// (the skip path charges AddCycles_C through CompileBlock); multiply adds its data-dependent part after.
void Compiler::AddCyclesCI(s32 internal) {
    s32 n = (CurCPU->Num ? NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 0 : 2]
                         : ((R15 & 2) ? 0 : (s32)CurInstr.CodeCycles)) +
            internal;
    if (n > 0 && n < 4096) A.addi(RCycles, RCycles, n);
    else if (n) { A.movi((Reg)16, (u32)n); A.add(RCycles, RCycles, (Reg)16); }
}

s32 Compiler::CyclesC() const {
    if (CurCPU->Num) return NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 1 : 3];
    return (R15 & 2) ? 0 : CurInstr.CodeCycles;
}

// Leaving a block. With block linking on (the default; jit.link=off turns it off), the block does what
// ARMv5/ARMv4::Execute's JIT loop does between two blocks, in the same order, and jumps straight into the
// next one instead of returning to it:
//   if (StopExecution) -> back to the loop (it handles IRQs, halts, idle loops);
//   Timestamp += Cycles; Cycles = 0; if (Timestamp >= Target) -> back (the loop ends);
//   instrAddr = R[15] - (Thumb ? 2 : 4); outside FastBlockLookupStart/Size -> back (it sets the region up);
//   entry = FastBlockLookup[(instrAddr - Start) / 2]; not (instrAddr | Num) -> back (it compiles the block);
//   else jump to the entry: what ARM_Dispatch would have done.
// The lookup reads the framework's table every time, so invalidated blocks are never jumped to.
// The cycles and CPSR stay in w28/w27 into the next block: a linked jump enters past the block's prologue (the
// two loads that ARM_Dispatch's entry needs), and only the way back to the loop stores them. knownPC: the R15
// this exit leaves, when the compiler knows it (a block that runs on into the next instruction); it is then
// stored on the way back only, and instrAddr is a constant.
// x16 <- the address p, from the CPU pointer when it is within 16 MB of it (two adds), else as a constant
void Compiler::AddrNearCPU(Reg d, const void* p) {
    const intptr_t delta = (const u8*)p - (const u8*)CurCPU;
    const uintptr_t mag = delta < 0 ? (uintptr_t)-delta : (uintptr_t)delta;
    if (mag < (1u << 24)) {
        const u32 hi = (u32)(mag >> 12), lo = (u32)(mag & 0xFFF);
        Reg src = RCPU;
        if (hi) { delta < 0 ? A.subxi12(d, RCPU, hi) : A.addxi12(d, RCPU, hi); src = d; }
        if (lo || !hi) delta < 0 ? A.subxi(d, src, lo) : A.addxi(d, src, lo);
        return;
    }
    A.movix(d, (uint64_t)(uintptr_t)p);
}

void Compiler::ExitBlock(bool known, u32 knownPC, int thumbAfter, bool addrInW0) {
    const bool thumbExit = thumbAfter < 0 ? Thumb : thumbAfter != 0;
    FlushCycles();
    FlushRegs(false);
    auto back = [&]() {  // to the loop, with the state it reads in memory (BJ_RetState stores cycles and CPSR)
        if (known) { A.movi(W0, knownPC); A.str(W0, RCPU, off_R(15)); }
        A.jump((const void*)&BJ_RetState, false);
    };
    if (!LinkBlocks) { back(); return; }
    const Reg X0r = W0, X16 = (Reg)16, X17 = (Reg)17;
    if (addrInW0) A.mov(W2, W0);  // (the Timestamp/Target pair is loaded into x17/x0 below)
    u64* ts = CurCPU->Num ? &NDS.ARM7Timestamp : &NDS.ARM9Timestamp;  // its Target is the next u64

    std::vector<A64::Fixup> toBack;
    A.ldr(X16, RCPU, offsetof(ARM, StopExecution));
    toBack.push_back(A.cbnz_fwd(X16));

    AddrNearCPU(X16, ts);
    A.ldpx(X17, X0r, X16, 0);     // Timestamp, Target
    A.addx_sxtw(X17, X17, RCycles);
    A.strx(X17, X16, 0);
    A.movz(RCycles, 0);
    A.cmpx(X17, X0r);
    toBack.push_back(A.bcond_fwd(CS));  // Timestamp >= Target (unsigned)

    if (known) {
        // The loop set the lookup region up for this block's address before compiling it, and a block only runs
        // while its region is the loop's: a target in the same region has its table entry at a known address
        const u32 instrAddr = knownPC - (thumbExit ? 2 : 4);
        const u32 off = instrAddr - CurCPU->FastBlockLookupStart;
        if (!CurCPU->FastBlockLookup || off >= CurCPU->FastBlockLookupSize) {
            for (auto f : toBack) A.land(f);
            back();  // another region: the loop sets it up
            return;
        }
        A.movix(X16, (uint64_t)(uintptr_t)&CurCPU->FastBlockLookup[off >> 1]);
        A.ldrx(X16, X16, 0);
        A.lsrxi(X17, X16, 32);
        A.movi(X0r, instrAddr | (CurCPU->Num ? 1 : 0));
        A.subs(ZR, X17, X0r);
        toBack.push_back(A.bcond_fwd(NE));
    } else {
        if (addrInW0) {
            A.mov(X0r, W2);  // EmitJumpTo's instrAddr (R15 and the Thumb bit are in place)
        } else {
            A.ldr(X0r, RCPU, off_R(15));
            A.ubfx(X17, RCPSR, 5, 1);     // Thumb: 2, ARM: 4
            A.lsli(X17, X17, 1);
            A.movz(X16, 4);
            A.sub(X17, X16, X17);
            A.sub(X0r, X0r, X17);         // instrAddr
        }
        A.ldr(X16, RCPU, offsetof(ARM, FastBlockLookupStart));
        A.sub(X17, X0r, X16);         // offset (huge when below Start)
        A.ldr(X16, RCPU, offsetof(ARM, FastBlockLookupSize));
        A.subs(ZR, X17, X16);
        toBack.push_back(A.bcond_fwd(CS));
        A.ldrx(X16, RCPU, offsetof(ARM, FastBlockLookup));
        A.lsri(X17, X17, 1);
        A.ldrx_reg_lsl3(X16, X16, X17);
        if (CurCPU->Num) A.addi(X0r, X0r, 1);  // instrAddr | 1 (it is even)
        A.lsrxi(X17, X16, 32);
        A.subs(ZR, X17, X0r);
        toBack.push_back(A.bcond_fwd(NE));
    }
    A.adrp(X17, CodeBase);        // the code memory is page-aligned; blocks have no prologue (ARM_Dispatch's)
    A.add_uxtw(X16, X17, X16);    // the low 32 bits: the entry's offset in code memory
    A.br(X16);

    for (auto f : toBack) A.land(f);
    back();
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
    MemBaseOK = false;
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

// Operand 2 shifted by the bottom byte of a register (ARM's reg-specified shift). The host's shifts take the
// amount mod 32, so the >= 32 cases (result 0 or the sign, and the carry) are handled explicitly, matching the
// interpreter's LSL/LSR/ASR/ROR_REG(_S) macros. With wantC the shifter's carry is written to CPSR here (and
// left unchanged when the amount is 0); the result is returned in W0.
Compiler::Reg Compiler::RegShifter(u32 in, bool wantC) {
    const u32 rm = in & 0xF, st = (in >> 5) & 3, rs = (in >> 8) & 0xF;
    Need(rm);
    Need(rs);
    const Reg Rm = host(rm);
    A.ubfx(W1, host(rs), 0, 8);  // amount = Rs & 0xFF
    if (!wantC) {
        if (st == 3) {  // ROR (RORV is mod 32: an amount of 0 or a multiple of 32 leaves Rm)
            A.rorv(W0, Rm, W1);
        } else {
            A.cmpi(W1, 32);
            if (st == 2) {  // ASR: amount >= 32 fills with the sign (saturate the amount to 31)
                A.movi(W2, 31);
                A.csel(W1, W2, W1, GE);
                A.asrv(W0, Rm, W1);
            } else {  // LSL / LSR: amount >= 32 gives 0
                st == 0 ? A.lslv(W0, Rm, W1) : A.lsrv(W0, Rm, W1);
                A.csel(W0, ZR, W0, GE);
            }
        }
    } else {
        A.mov(W0, Rm);
        auto zero = A.cbz_fwd(W1);  // amount 0: result is Rm, carry unchanged
        A.subi(W1, W1, 1);
        if (st == 3) {  // ROR by amount: carry is Rm[(amount-1) & 31]; one more rotate completes it
            A.rorv(W0, Rm, W1);
            A.bfi(RCPSR, W0, 29, 1);
        } else {
            A.cmpi(W1, 31);  // amount-1 > 31 -> amount > 32; amount-1 >= 31 -> amount >= 32
            if (st == 2) {  // ASR
                A.movi(W2, 31);
                A.csel(W1, W2, W1, GT);
                A.asrv(W0, Rm, W1);
                A.bfi(RCPSR, W0, 29, 1);
            } else {  // LSL / LSR
                if (st == 0) { A.lslv(W0, Rm, W1); A.ubfx(W1, W0, 31, 1); }
                else A.lsrv(W0, Rm, W1);
                A.csel(W1, ZR, st ? W0 : W1, GT);  // amount > 32: carry 0
                A.bfi(RCPSR, W1, 29, 1);
                A.csel(W0, ZR, W0, GE);            // amount >= 32: result 0
            }
        }
        A.orr(W0, ZR, W0, (Shift)st, 1);  // the last unit of shift, so the total is `amount`
        A.land(zero);
    }
    return W0;
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
    const bool regShift = !I && (in & (1 << 4));  // operand 2 shifted by a register
    if (I) {
        u32 rot = ((in >> 8) & 0xF) * 2, imm = in & 0xFF;
        imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
        if (imm == 0) op2 = ZR;
        else { A.movi(W1, imm); op2 = W1; }
        if (wantC && rot) {
            A.movi(W2, imm >> 31);
            carry_changes = true;
        }
    } else if (regShift) {
        op2 = RegShifter(in, wantC);  // writes C to CPSR itself when wantC
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
            NZEpoch = A.flagEpoch;  // (N and Z are the guest's; C and V are not)
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
            FlagsEpoch = NZEpoch = A.flagEpoch;  // the host's NZCV is the guest's now (all four go to CPSR below)
            A.mrs_nzcv(W6);
            A.ubfx(W6, W6, 28, 4);
            A.bfi(RCPSR, W6, 28, 4);
        }
    }
    if (!test) Dirty(rd);
    if (regShift) AddCyclesCI(1);  // a register-specified shift: the interpreter's AddCycles_CI(1)
}

// Multiply, native. MUL/MLA write a 32-bit result; the long forms split a 64-bit product into RdLo/RdHi. The
// cost is the interpreter's AddCycles_CI (ARMInterpreter_ALU.cpp): on ARM9 a compile-time S?3:1; on ARM7 it
// depends on the multiplier at run time (1 if Rs[31:8] is all 0s/1s, 2 at [31:16], 3 at [31:24], else 4):
//   short signed (A_MUL):        4 - min(3, (CLS(Rs)+1) >> 3), and +1 for A_MLA
//   long  signed (A_SMULL/SMLAL): 5 - min(3, (CLS(Rs)+1) >> 3)
//   long  unsigned (A_UMULL/UMLAL): 5 - min(3, CLZ(Rs) >> 3)   (only tests == 0, so CLZ, no +1 on the count)
//   Thumb T_MUL:                 4 - min(3, CLZ(Rd) >> 3)      (unsigned, on the first operand)
// S sets N/Z from the full result; on ARM7 S also clears C (SetC(0)); ARM9 leaves C alone.
void Compiler::CompileMul() {
    using namespace ARMInstrInfo;
    const u32 in = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    const bool S = in & (1 << 20);
    const u8 flags = S ? CurInstr.SetFlags : 0;
    const u32 rdHi = (in >> 16) & 0xF, rn12 = (in >> 12) & 0xF, rs = (in >> 8) & 0xF, rm = in & 0xF;

    bool isLong, isSigned, accumulate;
    if (Thumb) { isLong = isSigned = accumulate = false; }  // Thumb MUL: a plain 32-bit multiply
    else
        switch (kind) {
        case ak_MUL:   isLong = false; isSigned = true;  accumulate = false; break;
        case ak_MLA:   isLong = false; isSigned = true;  accumulate = true;  break;
        case ak_UMULL: isLong = true;  isSigned = false; accumulate = false; break;
        case ak_UMLAL: isLong = true;  isSigned = false; accumulate = true;  break;
        case ak_SMULL: isLong = true;  isSigned = true;  accumulate = false; break;
        default:       isLong = true;  isSigned = true;  accumulate = true;  break;  // ak_SMLAL
        }

    Need(rm);
    Need(rs);

    // Cycles first: on ARM7 the count reads an operand the result may overwrite (Rd == Rm/Rs is allowed).
    if (CurCPU->Num == 0) {
        AddCyclesCI(S ? 3 : 1);
    } else {
        const u32 cycleOp = Thumb ? rm : rs;  // ARM counts on Rs; Thumb T_MUL on the first operand (Rd)
        const bool cycleSigned = Thumb ? false : (isLong ? isSigned : true);
        const s32 C = (accumulate || isLong) ? 5 : 4;  // MUL/Thumb MUL give 1..4; MLA and the long forms 2..5
        if (cycleSigned) { A.cls(W0, host(cycleOp)); A.addi(W0, W0, 1); }  // K = leading sign bits (CLS + 1)
        else A.clz(W0, host(cycleOp));                                     // K = leading zero bits (CLZ)
        A.lsri(W0, W0, 3);            // q = K >> 3 (0..4)
        A.lsri(W1, W0, 2);            // 1 exactly when q == 4
        A.sub(W0, W0, W1);            // min(3, q)
        AddCyclesCI(C);              // RCycles += nonseq code + C ...
        A.sub(RCycles, RCycles, W0);  // ... - min(3, q)
    }

    if (!isLong) {
        if (accumulate) { Need(rn12); A.madd(host(rdHi), host(rm), host(rs), host(rn12)); }
        else A.mul(host(rdHi), host(rm), host(rs));
        Dirty(rdHi);
        if (flags & (flag_N | flag_Z)) {
            A.tst(host(rdHi), host(rdHi));
            NZEpoch = A.flagEpoch;
            A.mrs_nzcv(W6);
            A.ubfx(W6, W6, 30, 2);
            A.bfi(RCPSR, W6, 30, 2);
        }
    } else {
        const Reg X16 = (Reg)16, X17 = (Reg)17;  // the 64-bit product stays out of the guest registers
        if (accumulate) {
            Need(rn12);
            Need(rdHi);
            A.mov(X16, host(rn12));                 // RdLo, zero-extended
            A.orrx(X16, X16, host(rdHi), LSL, 32);  // | RdHi << 32: the 64-bit accumulator
            isSigned ? A.smaddl(X17, host(rm), host(rs), X16) : A.umaddl(X17, host(rm), host(rs), X16);
        } else {
            isSigned ? A.smull(X17, host(rm), host(rs)) : A.umull(X17, host(rm), host(rs));
        }
        if (flags & (flag_N | flag_Z)) {
            A.tstx(X17);
            NZEpoch = A.flagEpoch;
            A.mrs_nzcv(W6);
            A.ubfx(W6, W6, 30, 2);
            A.bfi(RCPSR, W6, 30, 2);
        }
        A.mov(host(rn12), X17);        // RdLo = low 32 (a W move zero-extends the guest register)
        A.lsrxi(host(rdHi), X17, 32);  // RdHi = high 32 (written last: it wins if RdHi == RdLo, as the interpreter does)
        Dirty(rn12);
        Dirty(rdHi);
    }
    if (CurCPU->Num == 1 && (flags & flag_C)) A.bfi(RCPSR, ZR, 29, 1);  // ARM7: S clears C
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
    MemBaseOK = false;
    A.ldr(RCycles, RCPU, offsetof(ARM, Cycles));
    A.ldr(RCPSR, RCPU, offsetof(ARM, CPSR));
    for (int g = 8; g < 15; g++)
        if (Loaded & (1 << g)) A.ldr(host(g), RCPU, off_R(g));
}

// ARM9 loads, inline: the interpreter's ARMv5::DataRead8/16/32 and AddCycles_CDI, step for step, for the cases
// that don't leave the CPU: ITCM, DTCM and main RAM. Anything else (no read permission, I/O, VRAM, the
// cartridge...) takes the slow path, the same call CompileMem makes otherwise. The ARM9 has no data-cache
// model in melonDS, so the cost depends on the address alone: 1 for the TCMs, else MemTimings[addr >> 12][1]
// (8/16-bit) or [2] (32-bit). The value ends in W0; W1 holds the address.
static size_t OffPUMap, OffITCMSize, OffDTCMBase, OffDTCMMask, OffDTCM, OffITCM, OffMemTimings;
static bool FastLoads9Ok() {
    OffPUMap = offsetof(ARMv5, PU_Map);
    OffITCMSize = offsetof(ARMv5, ITCMSize);
    OffDTCMBase = offsetof(ARMv5, DTCMBase);
    OffDTCMMask = offsetof(ARMv5, DTCMMask);
    OffDTCM = offsetof(ARMv5, DTCM);
    OffITCM = offsetof(ARMv5, ITCM);
    OffMemTimings = offsetof(ARMv5, MemTimings);
    auto w = [](size_t o) { return o % 4 == 0 && o < 16384; };
    auto x = [](size_t o) { return o % 8 == 0 && o < 32768; };
    (void)x;  // PU_Map, DTCM and MemTimings are far into ARMv5 (past its 4 MB timing table): reached by address
    return w(OffITCMSize) && w(OffDTCMBase) && w(OffDTCMMask) && OffITCM < 4096 &&
           w(offsetof(ARM, DataRegion)) && w(offsetof(ARM, DataCycles));
}

void Compiler::FastLoad9(int size, bool sign, bool rotate, const void* slowfn) {
    const Reg X2 = W2, X16 = (Reg)16, X17 = (Reg)17;
    FlushCycles();
    StoreHigh();  // both paths leave r8-r14 clean: the slow one calls out

    // A block belongs to one CPU object, so the far fields are at fixed addresses.
    u8* cpu = (u8*)CurCPU;

    // permission: PU_Map[addr >> 12] & 1
    A.movix(X16, (uint64_t)(uintptr_t)(cpu + OffPUMap));
    A.ldrx(X16, X16, 0);
    A.lsri(X17, W1, 12);
    A.ldrb_reg(X17, X16, X17);
    A.ubfx(X17, X17, 0, 1);
    auto noPerm = A.cbz_fwd(X17);
    A.str(W1, RCPU, offsetof(ARM, DataRegion));

    // the aligned address -> W16
    if (size == 4) { A.lsri(X16, W1, 2); A.lsli(X16, X16, 2); }
    else if (size == 2) { A.lsri(X16, W1, 1); A.lsli(X16, X16, 1); }
    else A.mov(X16, W1);

    // what the interpreter's AddCycles_CDI adds, numD in W17 -> W17
    const s32 C = Thumb && (R15 & 2) ? 0 : CurInstr.CodeCycles;
    auto charge = [&]() {
        if (C > 6) A.addi(X17, X17, C - 6);           // max(C, numD + C - 6)
        if (C > 0) {                                  // (C <= 6: max(C, numD))
            A.movi(X2, (u32)C);
            A.subs(ZR, X17, X2);
            A.csel(X17, X17, X2, HI);
        }
        A.add(RCycles, RCycles, X17);
    };
    auto load = [&](Reg base, Reg idx) {
        if (size == 4) A.ldr_reg(W0, base, idx);
        else if (size == 2) A.ldrh_reg(W0, base, idx);
        else A.ldrb_reg(W0, base, idx);
    };

    // ITCM: addr < ITCMSize
    A.ldr(X17, RCPU, OffITCMSize);
    A.subs(ZR, X16, X17);
    auto toITCM = A.bcond_fwd(CC);
    // DTCM: (addr & DTCMMask) == DTCMBase
    A.ldr(X17, RCPU, OffDTCMMask);
    A.and_(X17, X16, X17);
    A.ldr(X2, RCPU, OffDTCMBase);
    A.subs(ZR, X17, X2);
    auto toDTCM = A.bcond_fwd(EQ);
    // main RAM: 0x02xxxxxx
    A.lsri(X17, X16, 24);
    A.cmpi(X17, 2);
    auto notMain = A.bcond_fwd(NE);
    A.movix(X2, (uint64_t)(uintptr_t)&NDS.MainRAM);
    A.ldrx(X2, X2, 0);
    A.movix(X17, (uint64_t)(uintptr_t)&NDS.MainRAMMask);
    A.ldr(X17, X17, 0);
    A.and_(X17, X16, X17);
    load(X2, X17);
    // numD = MemTimings[addr >> 12][size == 4 ? 2 : 1]
    A.movix(X2, (uint64_t)(uintptr_t)(cpu + OffMemTimings));
    A.lsri(X17, X16, 12);
    A.addx(X2, X2, X17, LSL, 2);
    A.ldrb(X17, X2, size == 4 ? 2 : 1);
    A.str(X17, RCPU, offsetof(ARM, DataCycles));
    charge();
    auto done1 = A.b_fwd();

    // the TCMs: numD = 1
    auto tcm = [&](bool itcm) {
        if (itcm) {
            A.addxi(X2, RCPU, (u32)OffITCM);
            A.ubfx(X17, X16, 0, 15);  // & (ITCMPhysicalSize - 1)
        } else {
            A.movix(X2, (uint64_t)(uintptr_t)(cpu + OffDTCM));
            A.ldrx(X2, X2, 0);
            A.ubfx(X17, X16, 0, 14);  // & (DTCMPhysicalSize - 1)
        }
        load(X2, X17);
        A.movz(X17, 1);
        A.str(X17, RCPU, offsetof(ARM, DataCycles));
        charge();
    };
    A.land(toITCM);
    tcm(true);
    auto done2 = A.b_fwd();
    A.land(toDTCM);
    tcm(false);
    // shared by the fast paths: the word's rotation, the sign
    A.land(done1);
    A.land(done2);
    if (size == 4 && rotate) {
        A.ubfx(X17, W1, 0, 2);
        A.lsli(X17, X17, 3);
        A.rorv(W0, W0, X17);
    }
    if (sign) size == 2 ? A.sxth(W0, W0) : A.sxtb(W0, W0);
    auto done = A.b_fwd();

    // everything else: the interpreter's own functions
    A.land(noPerm);
    A.land(notMain);
    CallOut(slowfn);
    A.land(done);
}

// ARM9 stores, inline: ARMv5::DataWrite8/16/32, AddCycles_CD and NDS::ARM9Write* for DTCM and main RAM.
// A main-RAM store goes inline only when melonDS's code bitmap (CodeIndexMainRAM, which its
// CheckAndInvalidate reads) has no compiled code in that 16-byte chunk; then the check would have done nothing.
// Code there, ITCM (it holds code), no write permission or any other address: the interpreter's call.
// W1 holds the address, W2 the value.
void Compiler::FastStore9(int size, const void* slowfn) {
    const Reg X16 = (Reg)16, X17 = (Reg)17, X3 = W3, X4 = W4;
    u8* cpu = (u8*)CurCPU;
    FlushCycles();
    StoreHigh();

    A.movix(X16, (uint64_t)(uintptr_t)(cpu + OffPUMap));
    A.ldrx(X16, X16, 0);
    A.lsri(X17, W1, 12);
    A.ldrb_reg(X17, X16, X17);
    A.ubfx(X17, X17, 1, 1);      // write permission
    auto noPerm = A.cbz_fwd(X17);
    A.str(W1, RCPU, offsetof(ARM, DataRegion));
    if (size == 4) { A.lsri(X16, W1, 2); A.lsli(X16, X16, 2); }
    else if (size == 2) { A.lsri(X16, W1, 1); A.lsli(X16, X16, 1); }
    else A.mov(X16, W1);

    const s32 C = Thumb && (R15 & 2) ? 0 : CurInstr.CodeCycles;
    auto charge = [&]() {  // AddCycles_CD: the same max() as AddCycles_CDI on the ARM9
        if (C > 6) A.addi(X17, X17, C - 6);
        if (C > 0) {
            A.movi(X3, (u32)C);
            A.subs(ZR, X17, X3);
            A.csel(X17, X17, X3, HI);
        }
        A.add(RCycles, RCycles, X17);
    };
    auto store = [&](Reg base, Reg idx) {
        if (size == 4) A.str_reg(W2, base, idx);
        else if (size == 2) A.strh_reg(W2, base, idx);
        else A.strb_reg(W2, base, idx);
    };

    A.ldr(X17, RCPU, OffITCMSize);
    A.subs(ZR, X16, X17);
    auto itcm = A.bcond_fwd(CC);   // ITCM: the call (it may hold code)
    A.ldr(X17, RCPU, OffDTCMMask);
    A.and_(X17, X16, X17);
    A.ldr(X3, RCPU, OffDTCMBase);
    A.subs(ZR, X17, X3);
    auto toDTCM = A.bcond_fwd(EQ);
    A.lsri(X17, X16, 24);
    A.cmpi(X17, 2);
    auto notMain = A.bcond_fwd(NE);
    // main RAM: local = addr & MainRAMMask; code there if CodeIndexMainRAM[local / 512].Code bit (local & 0x1FF) / 16
    A.movix(X17, (uint64_t)(uintptr_t)&NDS.MainRAMMask);
    A.ldr(X17, X17, 0);
    A.and_(X17, X16, X17);
    A.lsri(W0, X17, 9);
    A.movi(X4, (u32)sizeof(AddressRange));
    A.mulx(W0, W0, X4);
    A.movix(X3, (uint64_t)(uintptr_t)&NDS.JIT.CodeIndexMainRAM[0] + offsetof(AddressRange, Code));
    A.addx(X3, X3, W0);
    A.ldr(W0, X3, 0);
    A.ubfx(X4, X17, 4, 5);
    A.lsrv(W0, W0, X4);
    A.ubfx(W0, W0, 0, 1);
    auto code = A.cbnz_fwd(W0);
    A.movix(X3, (uint64_t)(uintptr_t)&NDS.MainRAM);
    A.ldrx(X3, X3, 0);
    store(X3, X17);
    A.movix(X3, (uint64_t)(uintptr_t)(cpu + OffMemTimings));
    A.lsri(X17, X16, 12);
    A.addx(X3, X3, X17, LSL, 2);
    A.ldrb(X17, X3, size == 4 ? 2 : 1);
    A.str(X17, RCPU, offsetof(ARM, DataCycles));
    charge();
    auto done1 = A.b_fwd();

    A.land(toDTCM);
    A.movix(X3, (uint64_t)(uintptr_t)(cpu + OffDTCM));
    A.ldrx(X3, X3, 0);
    A.ubfx(X17, X16, 0, 14);
    store(X3, X17);
    A.movz(X17, 1);
    A.str(X17, RCPU, offsetof(ARM, DataCycles));
    charge();
    auto done2 = A.b_fwd();

    A.land(noPerm);
    A.land(itcm);
    A.land(notMain);
    A.land(code);
    CallOut(slowfn);
    A.land(done1);
    A.land(done2);
}

// ARM7 loads and stores, inline: ARMv4::DataRead/DataWrite8/16/32, NDS::ARM7Read*/ARM7Write* and
// ARMv4::AddCycles_CDI/CD for main RAM (0x02xxxxxx) and ARM7 WRAM (0x038xxxxx). Stores go inline only when
// the code bitmap of that memory (CodeIndexMainRAM / CodeIndexARM7WRAM) has no code in the 16-byte chunk.
// Anything else (BIOS, shared WRAM, I/O...) takes the call. The ARM7 has no protection unit. Unlike the
// ARM9, DataRegion is the aligned address. The cost reads CodeCycles (set by the last jump) at run time:
//   numC = ARM7MemTimings[CodeCycles][Thumb ? 0 : 2]; numD = ARM7MemTimings[addr >> 15][size == 4 ? 2 : 0]
//   loads:  data in main RAM: code there too ? C + D : max(C+1 + D - 3, max(C+1, D))
//           else:             code in main RAM ? max(C + D+1 - 3, max(C, D+1)) : C + D + 1
//   stores: data in main RAM or code in main RAM, but not both: max(C + D - 3, max(C, D)); else C + D
// CodeRegion (code in main RAM?) is the block's: set by the jump into it, like CodeCycles' region.
void Compiler::FastMem7(bool load, int size, bool sign, bool rotate, const void* slowfn) {
    const Reg X16 = (Reg)16, X17 = (Reg)17, X3 = W3, X4 = W4, X5 = W5;
    FlushCycles();
    StoreHigh();
    if (size == 4) { A.lsri(X16, W1, 2); A.lsli(X16, X16, 2); }
    else if (size == 2) { A.lsri(X16, W1, 1); A.lsli(X16, X16, 1); }
    else A.mov(X16, W1);

    const bool codeMain = (CurInstr.Addr >> 24) == 2;
    // numD -> W17 (from the aligned address), numC -> W5, then the cost -> added to the cycles
    auto charge = [&](bool dataMain) {
        A.movix(X3, (uint64_t)(uintptr_t)&NDS.ARM7MemTimings[0][0]);
        A.lsri(X17, X16, 15);
        A.addx(X4, X3, X17, LSL, 2);
        A.ldrb(X17, X4, size == 4 ? 2 : 0);
        A.str(X16, RCPU, offsetof(ARM, DataRegion));
        A.str(X17, RCPU, offsetof(ARM, DataCycles));
        A.ldr(X5, RCPU, offsetof(ARM, CodeCycles));
        A.addx(X4, X3, X5, LSL, 2);
        A.ldrb(X5, X4, Thumb ? 0 : 2);
        auto maxsum = [&]() {  // W17 = max(W5 + W17 - 3, max(W5, W17))
            A.add(X3, X5, X17);
            A.subi(X3, X3, 3);
            A.subs(ZR, X5, X17);
            A.csel(X4, X5, X17, HI);        // max(C, D), unsigned: both small
            A.subs(ZR, X3, X4);
            A.csel(X17, X3, X4, GT);        // C + D - 3 can be negative: signed compare
        };
        if (load) {
            if (dataMain) {
                if (codeMain) A.add(X17, X5, X17);
                else { A.addi(X5, X5, 1); maxsum(); }
            } else {
                if (codeMain) { A.addi(X17, X17, 1); maxsum(); }
                else { A.add(X17, X5, X17); A.addi(X17, X17, 1); }
            }
        } else {
            if (dataMain != codeMain) maxsum();
            else A.add(X17, X5, X17);
        }
        A.add(RCycles, RCycles, X17);
    };
    auto access = [&](Reg base, Reg idx) {
        if (load) {
            if (size == 4) A.ldr_reg(W0, base, idx);
            else if (size == 2) A.ldrh_reg(W0, base, idx);
            else A.ldrb_reg(W0, base, idx);
        } else {
            if (size == 4) A.str_reg(W2, base, idx);
            else if (size == 2) A.strh_reg(W2, base, idx);
            else A.strb_reg(W2, base, idx);
        }
    };
    // a store hitting compiled code goes to the call: CodeIndex[local / 512].Code bit (local & 0x1FF) / 16
    auto codeCheck = [&](AddressRange* index) {
        A.lsri(W0, X17, 9);
        A.movi(X4, (u32)sizeof(AddressRange));
        A.mulx(W0, W0, X4);
        A.movix(X3, (uint64_t)(uintptr_t)index + offsetof(AddressRange, Code));
        A.addx(X3, X3, W0);
        A.ldr(W0, X3, 0);
        A.ubfx(X4, X17, 4, 5);
        A.lsrv(W0, W0, X4);
        A.ubfx(W0, W0, 0, 1);
        return A.cbnz_fwd(W0);
    };

    std::vector<A64::Fixup> slow, done;
    // main RAM: (addr & 0xFF800000) is 0x02000000 or 0x02800000
    A.lsri(X17, X16, 24);
    A.cmpi(X17, 2);
    auto notMain = A.bcond_fwd(NE);
    A.movix(X17, (uint64_t)(uintptr_t)&NDS.MainRAMMask);
    A.ldr(X17, X17, 0);
    A.and_(X17, X16, X17);
    if (!load) slow.push_back(codeCheck(NDS.JIT.CodeIndexMainRAM));
    A.movix(X3, (uint64_t)(uintptr_t)&NDS.MainRAM);
    A.ldrx(X3, X3, 0);
    access(X3, X17);
    charge(true);
    done.push_back(A.b_fwd());

    // ARM7 WRAM: (addr & 0xFF800000) == 0x03800000
    A.land(notMain);
    A.lsri(X17, X16, 23);
    A.cmpi(X17, 7);
    slow.push_back(A.bcond_fwd(NE));
    A.ubfx(X17, X16, 0, 16);  // & (ARM7WRAMSize - 1)
    if (!load) slow.push_back(codeCheck(NDS.JIT.CodeIndexARM7WRAM));
    A.movix(X3, (uint64_t)(uintptr_t)&NDS.ARM7WRAM);
    A.ldrx(X3, X3, 0);
    access(X3, X17);
    charge(false);
    done.push_back(A.b_fwd());

    for (auto f : slow) A.land(f);
    CallOut(slowfn);
    auto end = A.b_fwd();
    for (auto f : done) A.land(f);
    if (load) {
        if (size == 4 && rotate) {
            A.ubfx(X17, W1, 0, 2);
            A.lsli(X17, X17, 3);
            A.rorv(W0, W0, X17);
        }
        if (sign) size == 2 ? A.sxth(W0, W0) : A.sxtb(W0, W0);
    }
    A.land(end);
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

    using namespace ARMInstrInfo;
    // block timing: a load from the literal pool is a constant
    if (FastMem && load && rn == 15 && pre && !wb && rd != 15 && !regoff &&
        (kind == ak_LDR_IMM || kind == ak_LDRB_IMM || kind == ak_LDRH_IMM)) {
        const u32 addr = up ? PCValue + imm : PCValue - imm;
        if (LoadLiteral(kind == ak_LDR_IMM ? 4 : kind == ak_LDRB_IMM ? 1 : 2, rd, addr)) return;
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

    if (FastMem) {  // block timing: straight to fast memory
        int size = 4;
        bool sign = false;
        if (half) {
            if (load) {
                size = kind <= ak_LDRH_POST_IMM ? 2 : kind <= ak_LDRSB_POST_IMM ? 1 : 2;
                sign = kind > ak_LDRH_POST_IMM;
            } else size = 2;
        } else if (in & (1 << 22)) size = 1;
        Reg val = W2;  // a store's value: the guest register itself unless the write-back changes it first
        if (!load) {
            if (rd == 15) A.movi(W2, R15 + 4);
            else {
                Need(rd);
                if (wb && rd == rn) A.mov(W2, host(rd));
                else val = host(rd);
            }
        }
        if (wb) {
            if (pre) A.mov(host(rn), W1);
            else apply(host(rn), host(rn));
            Dirty(rn);
        }
        FastAccess(load, size, sign, !NoRotate, rd, val);
        return;
    }

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
    if (load && CurCPU->Num == 0 && FastLoads) {
        int size = 4;
        bool sign = false;
        if (half) {
            size = kind <= ak_LDRH_POST_IMM ? 2 : kind <= ak_LDRSB_POST_IMM ? 1 : 2;
            sign = kind > ak_LDRH_POST_IMM;
        } else if (in & (1 << 22)) size = 1;
        FastLoad9(size, sign, !NoRotate, fn);
    } else if (!load && CurCPU->Num == 0 && FastStores) {
        int size = half ? 2 : (in & (1 << 22)) ? 1 : 4;
        FastStore9(size, fn);
    } else if (CurCPU->Num == 1 && FastMem7On) {
        int size = 4;
        bool sign = false;
        if (half) {
            if (!load) size = 2;
            else {
                size = kind <= ak_LDRH_POST_IMM ? 2 : kind <= ak_LDRSB_POST_IMM ? 1 : 2;
                sign = kind > ak_LDRH_POST_IMM;
            }
        } else if (in & (1 << 22)) size = 1;
        FastMem7(load, size, sign, !NoRotate, fn);
    } else {
        CallOut(fn);
    }
    if (load) {
        A.mov(host(rd), W0);
        Dirty(rd);
    }
}


// --- block timing: fast memory, compile-time cycles, literals ---

void Compiler::EnsureMemBase() {
    if (MemBaseOK) return;
    A.movix((Reg)8, (uint64_t)(uintptr_t)(CurCPU->Num ? NDS.JIT.Memory.FastMem7Start : NDS.JIT.Memory.FastMem9Start));
    MemBaseOK = true;
}

// The interpreter's AddCycles_CDI (loads) / AddCycles_CD (stores), with the data cycles and region the framework
// recorded for this instruction when it decoded the block: what melonDS's JIT charges (Comp_AddCycles_CDI/CD).
s32 Compiler::BlockMemCycles(bool store) const {
    const s32 D = CurInstr.DataCycles;
    if (CurCPU->Num == 0) {
        const s32 C = (R15 & 2) ? 0 : CurInstr.CodeCycles;
        return std::max(C + D - 6, std::max(C, D));
    }
    const s32 C = NDS.ARM7MemTimings[CurInstr.CodeCycles][Thumb ? 0 : 2];
    const bool dataMain = (CurInstr.DataRegion >> 24) == 2, codeMain = (R15 >> 24) == 2;
    auto overlap = [](s32 c, s32 d) { return std::max(c + d - 3, std::max(c, d)); };
    if (store) return dataMain != codeMain ? overlap(C, D) : C + D;
    if (dataMain) return codeMain ? C + D : overlap(C + 1, D);
    return codeMain ? overlap(C, D + 1) : C + D + 1;
}

void Compiler::ChargeTaken(s32 n) {
    if (!CurConditional) { ConstantCycles += n; return; }
    if (n > 0 && n < 4096) A.addi(RCycles, RCycles, n);
    else if (n) { A.movi((Reg)16, (u32)n); A.add(RCycles, RCycles, (Reg)16); }
}

// An access the framework saw go to memory that fast memory never maps (I/O, the wireless...): it would fault
// every time, so it goes straight to the call. A conditional one may not have run when the block was decoded,
// so it gets the fast path (as in melonDS's JIT).
bool Compiler::StaticSlow() const {
    if (CurConditional) return false;
    const int region = CurCPU->Num == 0 ? NDS.JIT.Memory.ClassifyAddress9(CurInstr.DataRegion)
                                        : NDS.JIT.Memory.ClassifyAddress7(CurInstr.DataRegion);
    return !NDS.JIT.Memory.IsFastmemCompatible(region);
}

// A load from the code's own literal pool (LDR rd, [pc, #i]; Thumb LDR rd, [pc, #i]) becomes a constant. The
// framework tracks the literal's address with the block, so a write to it recompiles the block, and a literal
// that has been written once (InvalidLiterals) is loaded normally from then on.
bool Compiler::LoadLiteral(int size, int rd, u32 addr) {
    if (!NDS.JIT.LiteralOptimizationsEnabled() || CurInstr.Info.SpecialKind != ARMInstrInfo::special_LoadLiteral)
        return false;
    const u32 local = NDS.JIT.LocaliseCodeAddress(CurCPU->Num, addr);
    if (!local || NDS.JIT.InvalidLiterals.Find(local) != -1) return false;
    u32 v;
    const u32 keep = CurCPU->R[15];
    CurCPU->R[15] = R15;  // the ARM7 BIOS can only be read from inside it
    if (size == 4) {
        CurCPU->DataRead32(addr & ~3u, &v);
        const u32 r = (addr & 3) * 8;
        if (r && !NoRotate) v = (v >> r) | (v << (32 - r));
    } else if (size == 2) CurCPU->DataRead16(addr & ~1u, &v);
    else CurCPU->DataRead8(addr, &v);
    CurCPU->R[15] = keep;
    A.movi(host(rd), v);
    Dirty(rd);
    ChargeTaken(BlockMemCycles(false));
    return true;
}

// What a slow path's call must keep, as it is where the access is compiled: x8 (the fast-memory base) when the
// block relies on it, the host registers of the guest's r8-r14 that hold their values, and the host's NZCV when it
// holds the guest's flags. (r0-r7, the CPSR and the cycles are in callee-saved registers.)
Compiler::SaveSet Compiler::LiveSet() const {
    u32 m = MemBaseOK ? 1 : 0;
    for (int g = 8; g < 15; g++)
        if (Loaded & (1 << g)) m |= 1u << (host(g) - 8);
    return {(u8)m, FlagsEpoch == A.flagEpoch || NZEpoch == A.flagEpoch};
}

// That set around a call from a slow path, plus `extra` bytes of stack for its own use at sp+80
void Compiler::SaveCallerSaved(u32 extra, SaveSet ss) {
    A.subxi(SP, SP, 80 + extra);
    for (int k = 0; k < 4; k++) {
        const bool a = (ss.regs >> (2 * k)) & 1, b = (ss.regs >> (2 * k + 1)) & 1;
        const Reg r0 = (Reg)(8 + 2 * k), r1 = (Reg)(9 + 2 * k);
        if (a && b) A.stpx(r0, r1, SP, 16 * k);
        else if (a) A.strx(r0, SP, 16 * k);
        else if (b) A.strx(r1, SP, 16 * k + 8);
    }
    if (ss.nzcv) {
        A.mrs_nzcv((Reg)16);
        A.strx((Reg)16, SP, 64);
    }
    if (CurCPU->Num == 1) {  // the ARM7's BIOS can only be read from inside it: its handlers check the PC
        A.movi((Reg)16, R15);
        A.str((Reg)16, RCPU, off_R(15));
    }
}

void Compiler::RestoreCallerSaved(u32 extra, SaveSet ss) {
    if (ss.nzcv) {
        A.ldrx((Reg)16, SP, 64);
        A.msr_nzcv((Reg)16);
    }
    for (int k = 0; k < 4; k++) {
        const bool a = (ss.regs >> (2 * k)) & 1, b = (ss.regs >> (2 * k + 1)) & 1;
        const Reg r0 = (Reg)(8 + 2 * k), r1 = (Reg)(9 + 2 * k);
        if (a && b) A.ldpx(r0, r1, SP, 16 * k);
        else if (a) A.ldrx(r0, SP, 16 * k);
        else if (b) A.ldrx(r1, SP, 16 * k + 8);
    }
    A.addxi(SP, SP, 80 + extra);
}

// The access through melonDS's own slow functions (the ones its JIT's fast memory falls back to), with W1 the
// address and `val` a store's value; a load's value ends in `dst`. Nothing else changes.
void Compiler::SlowAccess(bool load, int size, bool sign, bool rotate, Reg dst, Reg val, SaveSet ss) {
    const bool dsi = NDS.ConsoleType != 0;
    const bool flagsFull = FlagsEpoch == A.flagEpoch, flagsNZ = NZEpoch == A.flagEpoch;
    const void* fn;
    if (CurCPU->Num == 0) {
        if (load) fn = size == 4 ? (dsi ? (const void*)&SlowRead9<u32, 1> : (const void*)&SlowRead9<u32, 0>)
                     : size == 2 ? (dsi ? (const void*)&SlowRead9<u16, 1> : (const void*)&SlowRead9<u16, 0>)
                                 : (dsi ? (const void*)&SlowRead9<u8, 1> : (const void*)&SlowRead9<u8, 0>);
        else fn = size == 4 ? (dsi ? (const void*)&SlowWrite9<u32, 1> : (const void*)&SlowWrite9<u32, 0>)
                : size == 2 ? (dsi ? (const void*)&SlowWrite9<u16, 1> : (const void*)&SlowWrite9<u16, 0>)
                            : (dsi ? (const void*)&SlowWrite9<u8, 1> : (const void*)&SlowWrite9<u8, 0>);
    } else {
        if (load) fn = size == 4 ? (dsi ? (const void*)&SlowRead7<u32, 1> : (const void*)&SlowRead7<u32, 0>)
                     : size == 2 ? (dsi ? (const void*)&SlowRead7<u16, 1> : (const void*)&SlowRead7<u16, 0>)
                                 : (dsi ? (const void*)&SlowRead7<u8, 1> : (const void*)&SlowRead7<u8, 0>);
        else fn = size == 4 ? (dsi ? (const void*)&SlowWrite7<u32, 1> : (const void*)&SlowWrite7<u32, 0>)
                : size == 2 ? (dsi ? (const void*)&SlowWrite7<u16, 1> : (const void*)&SlowWrite7<u16, 0>)
                            : (dsi ? (const void*)&SlowWrite7<u8, 1> : (const void*)&SlowWrite7<u8, 0>);
    }
    SaveCallerSaved(0, ss);
    // SlowRead*<u32> rotates an unaligned word; a load that doesn't (Thumb's PC/SP-relative LDR) passes it aligned
    auto addr = [&]() { if (load && size == 4 && !rotate) A.andi(W0, W1, ~3u); else A.mov(W0, W1); };
    if (CurCPU->Num == 0) {  // SlowRead9(addr, cpu) / SlowWrite9(addr, cpu, val)
        if (!load) A.mov(W2, val);
        addr();
        A.movx(W1, RCPU);
    } else {                 // SlowRead7(addr) / SlowWrite7(addr, val)
        addr();
        if (!load) A.mov(W1, val);
    }
    A.jump(fn, true);
    RestoreCallerSaved(0, ss);
    if (load) {
        if (size == 4) A.mov(dst, W0);
        else if (size == 2) sign ? A.sxth(dst, W0) : A.uxth(dst, W0);
        else sign ? A.sxtb(dst, W0) : A.uxtb(dst, W0);
    }
    if (ss.nzcv) {  // (restored: the guest's flags are still in the host's NZCV)
        if (flagsFull) FlagsEpoch = A.flagEpoch;
        if (flagsNZ) NZEpoch = A.flagEpoch;
    }
}

// One load or store with block timing. W1 holds the address, `val` a store's value; a load goes straight into
// the guest register rd. The fast path is the host access into the framework's fast-memory window (x8 + the
// aligned address); its slow path is compiled after the block and reached only once the access has faulted.
void Compiler::FastAccess(bool load, int size, bool sign, bool rotate, int rd, Reg val) {
    const Reg dst = load ? host(rd) : val;
    if (StaticSlow()) {
        SlowAccess(load, size, sign, rotate, dst, val, LiveSet());
    } else {
        EnsureMemBase();
        u32* seq = A.here();
        Reg idx = (Reg)1;  // x1: a W write zeroed its upper half
        if (size > 1) { A.andi((Reg)16, W1, size == 4 ? ~3u : ~1u); idx = (Reg)16; }
        u32* at = A.here();
        if (load) {
            if (size == 4) A.ldr_reg(dst, (Reg)8, idx);
            else if (size == 2) sign ? A.ldrsh_reg(dst, (Reg)8, idx) : A.ldrh_reg(dst, (Reg)8, idx);
            else sign ? A.ldrsb_reg(dst, (Reg)8, idx) : A.ldrb_reg(dst, (Reg)8, idx);
            if (size == 4 && rotate) {
                A.ubfiz((Reg)17, W1, 3, 2);
                A.rorv(dst, dst, (Reg)17);
            }
        } else {
            if (size == 4) A.str_reg(val, (Reg)8, idx);
            else if (size == 2) A.strh_reg(val, (Reg)8, idx);
            else A.strb_reg(val, (Reg)8, idx);
        }
        const u32 r15 = R15;
        const SaveSet ss = LiveSet();
        SlowPaths.push_back({seq, {at}, A.here(), [=]() {
            const u32 keep = R15;
            R15 = r15;
            SlowAccess(load, size, sign, rotate, dst, val, ss);
            R15 = keep;
        }});
    }
    if (load) Dirty(rd);
    ChargeTaken(BlockMemCycles(!load));
}

// A block transfer's slow path: melonDS's SlowBlockTransfer9/7 with the words in a stack buffer (u64 slots).
// W1 holds the lowest address. A load's registers are written from the buffer after x8-x15 are restored.
void Compiler::SlowBlockTransfer(bool load, const int* regs, int n, SaveSet ss) {
    const bool dsi = NDS.ConsoleType != 0;
    const bool flagsFull = FlagsEpoch == A.flagEpoch, flagsNZ = NZEpoch == A.flagEpoch;
    const u32 extra = (u32)((n * 8 + 15) & ~15);
    SaveCallerSaved(extra, ss);
    if (!load)
        for (int i = 0; i < n; i++) A.str(host(regs[i]), SP, 80 + i * 8);
    A.mov(W0, W1);
    A.addxi(W1, SP, 80);
    A.movz(W2, (u32)n);
    const void* fn;
    if (CurCPU->Num == 0) {
        A.movx(W3, RCPU);
        fn = load ? (dsi ? (const void*)&SlowBlockTransfer9<false, 1> : (const void*)&SlowBlockTransfer9<false, 0>)
                  : (dsi ? (const void*)&SlowBlockTransfer9<true, 1> : (const void*)&SlowBlockTransfer9<true, 0>);
    } else {
        fn = load ? (dsi ? (const void*)&SlowBlockTransfer7<false, 1> : (const void*)&SlowBlockTransfer7<false, 0>)
                  : (dsi ? (const void*)&SlowBlockTransfer7<true, 1> : (const void*)&SlowBlockTransfer7<true, 0>);
    }
    A.jump(fn, true);
    // a load's words: into the registers that are not restored now, and into the stack slots of those that are
    // (the restore then puts them in place)
    if (load)
        for (int i = 0; i < n; i++) {
            const int g = regs[i];
            const Reg h = g == 15 ? W0 : host(g);
            if (g != 15 && h >= 8 && h <= 15 && ((ss.regs >> (h - 8)) & 1)) {
                A.ldr((Reg)16, SP, 80 + i * 8);
                A.str((Reg)16, SP, 8 * (h - 8));  // (the low half of its x slot)
            } else {
                A.ldr(h, SP, 80 + i * 8);
            }
        }
    RestoreCallerSaved(extra, ss);
    if (ss.nzcv) {
        if (flagsFull) FlagsEpoch = A.flagEpoch;
        if (flagsNZ) NZEpoch = A.flagEpoch;
    }
}

// A block transfer with block timing: the words go straight between the guest registers and fast memory, two at
// a time (LDP/STP). W1 holds the lowest address. Returns false for the forms it leaves to the exact path: a load
// whose list holds its base (the base is needed after a partial fault).
bool Compiler::FastBlockTransfer(bool load, int rn, const int* regs, int n) {
    if (load)
        for (int i = 0; i < n; i++) if (regs[i] == rn) return false;
    for (int i = 0; i < n; i++) if (regs[i] != 15) load ? Dirty(regs[i]) : Need(regs[i]);
    auto xr = [&](int g) { return g == 15 ? W0 : host(g); };  // a loaded PC goes to W0 (CompileJump)
    if (StaticSlow()) {
        SlowBlockTransfer(load, regs, n, LiveSet());
    } else {
        EnsureMemBase();
        u32* seq = A.here();
        A.andi((Reg)16, W1, ~3u);
        A.addx((Reg)16, (Reg)8, (Reg)16);
        std::vector<u32*> faulting;
        int i = 0;
        while (i < n) {
            faulting.push_back(A.here());
            if (i + 1 < n) {
                load ? A.ldp(xr(regs[i]), xr(regs[i + 1]), (Reg)16, i * 4)
                     : A.stp(xr(regs[i]), xr(regs[i + 1]), (Reg)16, i * 4);
                i += 2;
            } else {
                load ? A.ldr(xr(regs[i]), (Reg)16, i * 4) : A.str(xr(regs[i]), (Reg)16, i * 4);
                i++;
            }
        }
        std::vector<int> list(regs, regs + n);
        const u32 r15 = R15;
        const SaveSet ss = LiveSet();
        SlowPaths.push_back({seq, faulting, A.here(), [=]() {
            const u32 keep = R15;
            R15 = r15;
            SlowBlockTransfer(load, list.data(), (int)list.size(), ss);
            R15 = keep;
        }});
    }
    ChargeTaken(BlockMemCycles(!load));
    return true;
}

// --- ARM/Thumb block transfers (LDM/STM, PUSH/POP/LDMIA/STMIA), native ---

// A scratch buffer the generated code gathers a store's register list into, and scatters a load's out of into
// the guest registers. One block runs at a time and a transfer completes within the single helper call below,
// so one file-static buffer is safe (it is never live across two transfers, on either CPU). Slots 0.. hold the
// transferred words; a high slot carries a load's write-back value across the call.
static constexpr int BlockWBSlot = 16;
static u32 BlockXferBuf[BlockWBSlot + 1];

// The word-transfer sequence the interpreter's block-transfer functions do: the first access nonsequential
// (DataRead32/DataWrite32), the rest sequential (DataRead32S/DataWrite32S) -- which is what accumulates
// DataCycles -- then one AddCycles_CDI (loads) / AddCycles_CD (stores) at the end. `addr` is the lowest
// transfer address (the sequence ascends, low register first) and `buf` holds `count` (>= 1) words. Going
// through the interpreter's own functions in this exact order makes the side effects and the cycle count the
// interpreter's. (ARMInterpreter_LoadStore.cpp: A_LDM / A_STM, T_PUSH / T_POP / T_LDMIA / T_STMIA.)
static void bj_blockload(ARM* c, u32 addr, u32* buf, u32 count) {
    c->DataRead32(addr, &buf[0]);
    for (u32 i = 1; i < count; i++) c->DataRead32S(addr + 4 * i, &buf[i]);
    c->AddCycles_CDI();
}
static void bj_blockstore(ARM* c, u32 addr, u32* buf, u32 count) {
    c->DataWrite32(addr, buf[0]);
    for (u32 i = 1; i < count; i++) c->DataWrite32S(addr + 4 * i, buf[i]);
    c->AddCycles_CD();
}

// LDM/STM and the Thumb PUSH/POP/LDMIA/STMIA, native. The register list is known at compile time, so the walk
// is unrolled: the lowest transfer address and the write-back are computed in host registers, the list is
// gathered into / scattered out of BlockXferBuf two words at a time (STP/LDP), and one call does the actual
// word accesses and the cycle charge through the interpreter's functions (bj_blockstore / bj_blockload). The
// guest registers stay in host registers across the block; only the transferred ones are touched. NativeKind
// has already excluded the forms left to the interpreter: the S bit (user-bank / CPSR-restore), the PC in the
// list (LDM is a jump; STM a rare constant), POP with PC (a jump), an empty list, and a PC base.
void Compiler::CompileBlockTransfer() {
    using namespace ARMInstrInfo;
    const u32 in = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    const bool thumbForm = (kind == tk_PUSH || kind == tk_POP || kind == tk_LDMIA || kind == tk_STMIA);

    u32 rlist, rn;
    bool load, pre, up;  // pre = add 4 before each word (P bit); up = increment (U bit)
    if (!thumbForm) {  // ARM LDM / STM
        rlist = in & 0x7FFF;  // bits 0..14 (PC excluded by NativeKind)
        rn = (in >> 16) & 0xF;
        load = in & (1 << 20);
        pre = in & (1 << 24);
        up = in & (1 << 23);
    } else {
        rn = (kind == tk_LDMIA || kind == tk_STMIA) ? ((in >> 8) & 7) : 13;
        load = (kind == tk_POP || kind == tk_LDMIA);
        switch (kind) {
        case tk_PUSH: rlist = (in & 0xFF) | ((in & (1 << 8)) ? (1 << 14) : 0); pre = true;  up = false; break;  // STMDB sp!
        default:      rlist = in & 0xFF;                                       pre = false; up = true;  break;  // POP/LDMIA/STMIA
        }
    }

    const int n = __builtin_popcount(rlist);
    const int Num = CurCPU->Num;
    const bool baseInList = rlist & (1u << rn);

    // The lowest transfer address, matching the interpreter's loop (a decrement first does base -= 4*n, then
    // both directions transfer ascending): start = base + (up ? (pre?4:0) : -4*n + (pre?0:4)).
    const s32 startOff = up ? (pre ? 4 : 0) : (-4 * n + (pre ? 0 : 4));
    const s32 wbDelta = up ? 4 * n : -4 * n;  // the write-back value is base + wbDelta

    // Whether the base register is written back (its value is base + wbDelta):
    //  stores: PUSH/STMIA always; ARM STM iff the W bit.
    //  loads : base not in the list -> as the W bit says (always for POP/LDMIA);
    //          base in the list -> the arch differs (A_LDM / T_LDMIA):
    //            Thumb LDMIA: no write-back; ARMv4 (Num 1): no write-back;
    //            ARMv5 (Num 0): iff base is the only register, or a register above base is in the list.
    const bool wbBit = thumbForm ? true : (in & (1 << 21));
    bool doWriteback;
    if (!wbBit) doWriteback = false;
    else if (!load) doWriteback = true;
    else if (!baseInList) doWriteback = true;
    else if (thumbForm) doWriteback = false;
    else doWriteback = (Num == 0) && ((!(rlist & ~(1u << rn))) || (rlist & ~((2u << rn) - 1)));

    // A store's base-register slot holds a special value (A_STM / T_STMIA): the original base for ARMv5, Thumb,
    // or when base is the lowest register in the list; the running transfer address for ARMv4 when it is not
    // the lowest. (The user-bank form, which would bank the stored value, is excluded by NativeKind.)
    const bool storeBaseRunning = !load && baseInList && !thumbForm && Num == 1 && (rlist & ((1u << rn) - 1));

    int regs[16], ns = 0;
    for (int r = 0; r < 15; r++) if (rlist & (1 << r)) regs[ns++] = r;

    const Reg BUF = W2;  // x2 = &BlockXferBuf (also the helper's buf argument)
    Need(rn);
    // the lowest transfer address -> W1 (the helper's addr argument; also the base of a running-address slot)
    if (startOff > 0) A.addi(W1, host(rn), startOff);
    else if (startOff < 0) A.subi(W1, host(rn), -startOff);
    else A.mov(W1, host(rn));

    if (FastMem && !storeBaseRunning && FastBlockTransfer(load, (int)rn, regs, ns)) {  // block timing
        if (doWriteback) {
            wbDelta >= 0 ? A.addi(host(rn), host(rn), (u32)wbDelta) : A.subi(host(rn), host(rn), (u32)-wbDelta);
            Dirty(rn);
        }
        return;
    }
    A.movix(BUF, (uint64_t)(uintptr_t)&BlockXferBuf[0]);

    if (load) {
        // a load may overwrite the base in the list; save base + wbDelta now, across the call, for the write-back
        if (doWriteback) {
            wbDelta >= 0 ? A.addi(W4, host(rn), (u32)wbDelta) : A.subi(W4, host(rn), (u32)-wbDelta);
            A.str(W4, BUF, BlockWBSlot * 4);
        }
    } else {
        for (int r = 0; r < ns; r++) Need(regs[r]);  // every stored register in a host register (before writeback)
        int i = 0;
        while (i < ns) {
            const bool special = (regs[i] == (int)rn) && storeBaseRunning;
            if (special) { A.addi(W4, W1, 4 * i); A.str(W4, BUF, i * 4); i++; continue; }  // the running address
            if (i + 1 < ns && !((regs[i + 1] == (int)rn) && storeBaseRunning)) {
                A.stp(host(regs[i]), host(regs[i + 1]), BUF, i * 4);  // base-in-list (ARMv5/Thumb/lowest): host(rn) = old base
                i += 2;
            } else {
                A.str(host(regs[i]), BUF, i * 4);
                i++;
            }
        }
    }

    // the call: fn(cpu, startAddr = W1, buf = x2, count = W3), with the interpreter's state in memory
    FlushCycles();
    StoreHigh();
    A.str(RCycles, RCPU, offsetof(ARM, Cycles));
    A.str(RCPSR, RCPU, offsetof(ARM, CPSR));
    if (Num == 0) {  // the ARM9's AddCycles_CDI/CD read CodeCycles, and R[15]&2 for Thumb
        A.movi((Reg)16, (u32)(s32)CurInstr.CodeCycles);
        A.str((Reg)16, RCPU, offsetof(ARM, CodeCycles));
        if (Thumb) { A.movi((Reg)16, R15); A.str((Reg)16, RCPU, off_R(15)); }
    }
    A.movx(W0, RCPU);
    A.movi(W3, (u32)n);
    A.jump(load ? (const void*)&bj_blockload : (const void*)&bj_blockstore, true);
    MemBaseOK = false;
    A.ldr(RCycles, RCPU, offsetof(ARM, Cycles));
    A.ldr(RCPSR, RCPU, offsetof(ARM, CPSR));
    for (int g = 8; g < 15; g++)
        if (Loaded & (1 << g)) A.ldr(host(g), RCPU, off_R(g));  // r8-r14 live in caller-saved regs: the call may clobber them

    if (load) {
        A.movix(W3, (uint64_t)(uintptr_t)&BlockXferBuf[0]);  // w3 (count) is free now: reuse as the buffer base
        int i = 0;
        while (i < ns) {
            if (i + 1 < ns) { A.ldp(host(regs[i]), host(regs[i + 1]), W3, i * 4); Dirty(regs[i]); Dirty(regs[i + 1]); i += 2; }
            else { A.ldr(host(regs[i]), W3, i * 4); Dirty(regs[i]); i++; }
        }
        if (doWriteback) { A.ldr(host(rn), W3, BlockWBSlot * 4); Dirty(rn); }  // after the scatter: overrides an in-list load
    } else if (doWriteback) {
        wbDelta >= 0 ? A.addi(host(rn), host(rn), (u32)wbDelta) : A.subi(host(rn), host(rn), (u32)-wbDelta);
        Dirty(rn);
    }
}


// --- block timing: branches with a static target ---

bool Compiler::StaticBranch(bool thumb, u16 kind, int num) {
    using namespace ARMInstrInfo;
    if (thumb) return kind == tk_B || kind == tk_BCOND || kind == tk_BL_LONG;
    return kind == ak_B || kind == ak_BL || (kind == ak_BLX_IMM && num == 0);
}

void Compiler::StoreCPU(Reg r, size_t off) {
    if (off % 4 == 0 && off < 16384) { A.str(r, RCPU, off); return; }
    A.movix((Reg)17, (uint64_t)(uintptr_t)((u8*)CurCPU + off));
    A.str(r, (Reg)17, 0);
}

bool Compiler::CompileBranch() {
    using namespace ARMInstrInfo;
    const u32 in = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    u32 target, lr = 0;
    bool link = false;
    s32 extra = 0;  // the merged Thumb BL's first half: its own AddCycles_C
    if (!Thumb) {
        target = R15 + ((s32)(in << 8) >> 6);
        link = kind != ak_B;
        if (kind == ak_BLX_IMM) target += (((in >> 24) & 1) << 1) + 1;  // to Thumb
        lr = R15 - 4;
    } else if (kind == tk_B) {
        target = R15 + ((s32)((in & 0x7FF) << 21) >> 20) + 1;
    } else if (kind == tk_BCOND) {
        target = R15 + ((s32)(in << 24) >> 23) + 1;
    } else {  // tk_BL_LONG: the framework merged BL's two halves (Instr = first | second << 16)
        extra = CyclesC();
        const u32 r15 = R15 + 2, upper = in >> 16;
        target = (r15 - 2) + ((s32)((in & 0x7FF) << 21) >> 9) + ((upper & 0x7FF) << 1);
        if (CurCPU->Num == 1 || (upper & (1 << 12))) target |= 1;  // BL; ARM9 BLX: to ARM
        link = true;
        lr = (r15 - 2) | 1;
    }
    const bool toThumb = target & 1;
    u32 addr, newPC;
    s32 cycles = extra;
    // the fetch cycles at the target, as melonDS's Comp_JumpTo works them out (ARMv5/ARMv4::JumpTo at run time)
    if (CurCPU->Num == 0) {
        ARMv5* cpu9 = (ARMv5*)CurCPU;
        const u32 region = cpu9->MemTimings[target >> 12][0];
        const s32 keep = cpu9->RegionCodeCycles;
        cpu9->RegionCodeCycles = region;
        const bool setup = (target >> 24) != (R15 >> 24);
        if (setup) cpu9->SetupCodeMem(target);
        if (toThumb) {
            addr = target & ~1u;
            newPC = addr + 2;
            if (addr & 2) {
                cpu9->CodeRead32(addr - 2, true); cycles += cpu9->CodeCycles;
                cpu9->CodeRead32(addr + 2, false); cycles += cpu9->CodeCycles;
            } else {
                cpu9->CodeRead32(addr, true); cycles += cpu9->CodeCycles;
            }
        } else {
            addr = target & ~3u;
            newPC = addr + 4;
            cpu9->CodeRead32(addr, true); cycles += cpu9->CodeCycles;
            cpu9->CodeRead32(addr + 4, false); cycles += cpu9->CodeCycles;
        }
        cpu9->RegionCodeCycles = keep;
        if (setup) cpu9->SetupCodeMem(R15);
        A.movi((Reg)16, region);
        StoreCPU((Reg)16, offsetof(ARMv5, RegionCodeCycles));
    } else {
        const u32 codeCycles = target >> 15;
        addr = toThumb ? target & ~1u : target & ~3u;
        newPC = addr + (toThumb ? 2 : 4);
        cycles += toThumb ? NDS.ARM7MemTimings[codeCycles][0] + NDS.ARM7MemTimings[codeCycles][1]
                          : NDS.ARM7MemTimings[codeCycles][2] + NDS.ARM7MemTimings[codeCycles][3];
        A.movi((Reg)16, target >> 24);
        StoreCPU((Reg)16, offsetof(ARM, CodeRegion));
        A.movi((Reg)16, codeCycles);
        StoreCPU((Reg)16, offsetof(ARM, CodeCycles));
    }
    if (link) {
        A.movi(host(14), lr);
        Dirty(14);
    }
    if (toThumb != Thumb) toThumb ? A.orri(RCPSR, RCPSR, 0x20) : A.andi(RCPSR, RCPSR, ~0x20u);
    ChargeTaken(cycles);
    if (CurInstr.BranchFlags & branch_IdleBranch) {
        A.movz(W0, 1);
        A.strb(W0, RCPU, offsetof(ARM, IdleLoop));
    }
    if (CurInstr.BranchFlags & branch_FollowCondTaken) return false;  // the block goes on at the target
    const s32 keep = ConstantCycles;
    const u16 keepDirt = Dirt;
    ExitBlock(true, newPC, toThumb);
    ConstantCycles = keep;
    Dirt = keepDirt;
    return true;
}


// --- block timing: jumps to a register (BX, BLX, LDM/POP with the PC) ---

// What ARMv5/ARMv4::JumpTo does, at melonDS's JIT's level (its JumpTo stubs): W0 holds the address, bit 0 the state
// it goes to. Sets the Thumb bit, R15, the code region's cycles and charges the fetches; leaves the instruction's
// address in W0 (for ExitBlock).
void Compiler::EmitJumpTo() {
    const Reg X16 = (Reg)16, X17 = (Reg)17;
    if (CurCPU->Num == 0) {
        // the fetch cost w: MemTimings[addr >> 12][0] (RegionCodeCycles; 0xFF: the instruction cache, 3), 1 in ITCM
        A.movix(X16, (uint64_t)(uintptr_t)((u8*)CurCPU + offsetof(ARMv5, MemTimings)));
        A.lsri(X17, W0, 12);
        A.addx(X16, X16, X17, LSL, 2);
        A.ldrb(X17, X16, 0);
        StoreCPU(X17, offsetof(ARMv5, RegionCodeCycles));
        A.cmpi(X17, 0xFF);
        A.movz(X16, 3);
        A.csel(X17, X16, X17, EQ);
        A.ldr(X16, RCPU, offsetof(ARMv5, ITCMSize));
        A.subs(ZR, W0, X16);
        A.csinc(X17, X17, ZR, CS);  // addr < ITCMSize: 1
        auto thumb = A.tbnz_fwd(W0, 0);
        A.andi(RCPSR, RCPSR, ~0x20u);  // ARM: two fetches
        A.andi(W0, W0, ~3u);
        A.addi(X16, W0, 4);
        A.str(X16, RCPU, off_R(15));
        A.add(RCycles, RCycles, X17, LSL, 1);
        auto done = A.b_fwd();
        A.land(thumb);
        A.orri(RCPSR, RCPSR, 0x20);    // Thumb: two fetches from a halfword-aligned address, else one
        A.andi(W0, W0, ~1u);
        A.addi(X16, W0, 2);
        A.str(X16, RCPU, off_R(15));
        A.add(RCycles, RCycles, X17);
        auto one = A.tbnz_fwd(W0, 1);
        auto skip = A.b_fwd();
        A.land(one);
        A.add(RCycles, RCycles, X17);
        A.land(skip);
        A.land(done);
    } else {
        // CodeRegion, CodeCycles (addr >> 15) and ARM7MemTimings[CodeCycles]: Thumb [0] + [1], ARM [2] + [3]
        A.lsri(X16, W0, 24);
        StoreCPU(X16, offsetof(ARM, CodeRegion));
        A.lsri(X17, W0, 15);
        StoreCPU(X17, offsetof(ARM, CodeCycles));
        A.movix(X16, (uint64_t)(uintptr_t)&NDS.ARM7MemTimings[0][0]);
        A.ldr_reg_lsl2(X16, X16, X17);
        auto thumb = A.tbnz_fwd(W0, 0);
        A.andi(RCPSR, RCPSR, ~0x20u);
        A.ubfx(X17, X16, 16, 8);
        A.lsri(X16, X16, 24);
        A.add(RCycles, RCycles, X17);
        A.add(RCycles, RCycles, X16);
        A.andi(W0, W0, ~3u);
        A.addi(X16, W0, 4);
        A.str(X16, RCPU, off_R(15));
        auto done = A.b_fwd();
        A.land(thumb);
        A.orri(RCPSR, RCPSR, 0x20);
        A.ubfx(X17, X16, 0, 8);
        A.ubfx(X16, X16, 8, 8);
        A.add(RCycles, RCycles, X17);
        A.add(RCycles, RCycles, X16);
        A.andi(W0, W0, ~1u);
        A.addi(X16, W0, 2);
        A.str(X16, RCPU, off_R(15));
        A.land(done);
    }
}

int Compiler::DynJump(bool thumb, u32 in, u16 kind, int num) {
    using namespace ARMInstrInfo;
    if (thumb) {
        if (kind == tk_BX) return ((in >> 3) & 0xF) != 15 ? 1 : 0;
        if (kind == tk_BLX_REG) return num == 0 && ((in >> 3) & 0xF) != 15 ? 1 : 0;
        if (kind == tk_POP) return (in & (1 << 8)) ? 2 : 0;
        return 0;
    }
    if (kind == ak_BX || kind == ak_BLX_REG) return (in & 0xF) != 15 ? 1 : 0;
    if (kind == ak_LDM) {
        const u32 rn = (in >> 16) & 0xF;
        if (!(in & (1 << 15)) || (in & (1 << 22)) || rn == 15 || (in & (1u << rn))) return 0;
        return 2;
    }
    return 0;
}

// The jump itself: BX/BLX to a register, or the PC loaded by LDM/POP (with the other registers and the write-back),
// then the block's exit with the target in W0. Returns true (the taken path has left the block).
bool Compiler::CompileJump(int form) {
    using namespace ARMInstrInfo;
    const u32 in = CurInstr.Instr;
    const u16 kind = CurInstr.Info.Kind;
    if (form == 1) {
        const u32 rm = Thumb ? (in >> 3) & 0xF : in & 0xF;
        Need(rm);
        A.mov(W0, host(rm));   // (before BLX writes LR: BLX LR jumps to the old LR)
        if (kind == ak_BLX_REG || kind == tk_BLX_REG) {
            A.movi(host(14), Thumb ? R15 - 1 : R15 - 4);
            Dirty(14);
        }
    } else {
        u32 rlist, rn;
        bool pre, up, wb;
        if (Thumb) { rlist = (in & 0xFF) | 1u << 15; rn = 13; pre = false; up = true; wb = true; }
        else { rlist = in & 0xFFFF; rn = (in >> 16) & 0xF; pre = in & (1 << 24); up = in & (1 << 23); wb = in & (1 << 21); }
        const int n = __builtin_popcount(rlist);
        int regs[16], ns = 0;
        for (int r = 0; r < 16; r++) if (rlist & (1u << r)) regs[ns++] = r;
        const s32 startOff = up ? (pre ? 4 : 0) : (-4 * n + (pre ? 0 : 4));
        const s32 wbDelta = up ? 4 * n : -4 * n;
        Need(rn);
        if (startOff > 0) A.addi(W1, host(rn), startOff);
        else if (startOff < 0) A.subi(W1, host(rn), -startOff);
        else A.mov(W1, host(rn));
        FastBlockTransfer(true, (int)rn, regs, ns);  // the PC into W0 (DynJump excluded a base in the list)
        if (wb) {
            wbDelta >= 0 ? A.addi(host(rn), host(rn), (u32)wbDelta) : A.subi(host(rn), host(rn), (u32)-wbDelta);
            Dirty(rn);
        }
        // ARMv4: LDM stays in ARM state (bit 0 cleared), POP in Thumb state (set); ARMv5 interworks on bit 0
        if (CurCPU->Num == 1) Thumb ? A.orri(W0, W0, 1) : A.andi(W0, W0, ~1u);
    }
    EmitJumpTo();
    const s32 keep = ConstantCycles;
    const u16 keepDirt = Dirt;
    ExitBlock(false, 0, -1, true);
    ConstantCycles = keep;
    Dirt = keepDirt;
    return true;
}

// --- a block ---

static const char* TraceName(bool thumb, const FetchedInstr& f) {
    static const char* names[6] = {"interp", "native-alu", "native-mem", "native-mul", "native-shift", "native-ldm"};
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
    FastMem = !Exact && NDS.JIT.FastMemoryEnabled();
    MemBaseOK = false;
    FlagsEpoch = NZEpoch = A.flagEpoch - 1;
    SlowPaths.clear();

    // (no prologue: ARM_Dispatch loads the cycles and CPSR, and linked blocks keep them in w28/w27)

    static const bool trace = getenv("BANANAJIT_TRACE") != nullptr;
    if (trace)
        for (int i = 0; i < count; i++)
            Platform::Log(Platform::LogLevel::Info, "[bananajit] ARM%d %s %08x: %08x kind %u %s flags %x branch %x\n",
                          cpu->Num ? 7 : 9, i ? "  " : "block", instrs[i].Addr, instrs[i].Instr,
                          instrs[i].Info.Kind, TraceName(thumb, instrs[i]),
                          instrs[i].SetFlags, instrs[i].BranchFlags);

    bool exitKnown = false, lastExited = false;
    u32 exitPC = 0;
    for (int i = 0; i < count; i++) {
        CurInstr = instrs[i];
        R15 = CurInstr.Addr + (thumb ? 4 : 8);
        const bool last = i == count - 1;
        const u16 kind = CurInstr.Info.Kind;
        u32 armInstr = 0;
        u16 armKind = 0;
        PCValue = R15;
        NoRotate = false;
        int nk = thumb ? ThumbToARM(CurInstr.Instr, kind, R15, armInstr, armKind, PCValue, NoRotate)
                       : NativeKindARM(CurInstr.Instr, kind);
        if (!DoMul && (nk == 3 || nk == 4)) nk = 0;  // jit.mul=off: multiply and register-shift through the interpreter
        if (!DoLDM && nk == 5) nk = 0;               // jit.ldm=off: block transfers through the interpreter
        if (!nk && !Exact && FastMem && StaticBranch(thumb, kind, cpu->Num)) nk = 6;  // block timing: native
        int jumpForm = 0;
        if (!nk && !Exact && FastMem && DoJumps && (jumpForm = DynJump(thumb, CurInstr.Instr, kind, cpu->Num))) nk = 7;
        const bool native = nk != 0;
        bool branchExited = false;
        const bool selfCycles = nk != 1;  // the interpreter or a memory helper charges this one's cycles

        u32 cond = 0xE;
        if (!thumb) cond = CurInstr.Cond();
        else if (kind == ARMInstrInfo::tk_BCOND) cond = (CurInstr.Instr >> 8) & 0xF;
        const bool nop = !thumb && cond == 0xF && kind != ARMInstrInfo::ak_BLX_IMM;

        // The framework's rule: the PC is stored for the interpreter, for a branch it followed, and at the end
        // of a block that doesn't end in an unconditional branch. Before the condition check: a conditional
        // instruction that is skipped must leave the PC right too (a loop's untaken branch at a block's end).
        // A block that runs on past its last instruction (a native one, or the unconditional space) leaves a PC
        // the compiler knows: ExitBlock stores it on its way back to the loop only.
        const bool lastKnown = last && (native || nop) && !(CurInstr.BranchFlags & branch_FollowCondTaken);
        if (lastKnown) { exitKnown = true; exitPC = R15; }
        if ((!native && !nop) || (CurInstr.BranchFlags & branch_FollowCondTaken) ||
            (last && !lastKnown && (!CurInstr.Info.Branches() || cond < 0xE))) {
            A.movi(W0, R15);
            A.str(W0, RCPU, off_R(15));
        }

        if (nop) {
            AddCycles(CyclesC());  // the unconditional space: nothing to do on the DS's CPUs
            continue;
        }

        A64::Fixup skip{};
        bool preFull = true;
        bool conditional = cond < 0xE && !(!thumb && kind == ARMInstrInfo::ak_BLX_IMM);
        CurConditional = conditional;
        if (conditional) {
            if (native && FastMem && (nk == 2 || nk == 5)) EnsureMemBase();  // set on both paths
            if (native) {  // whatever it touches is loaded on both paths
                for (int g = 0; g < 15; g++)
                    if ((CurInstr.Info.SrcRegs | CurInstr.Info.DstRegs) & (1 << g)) Need(g);
                if (nk == 2 || nk == 5) StoreHigh();  // their call stores them only on the taken path
            } else {
                FlushRegs(false);
            }
            FlushCycles();
            LoadFlags((int)cond);
            preFull = FlagsEpoch == A.flagEpoch;  // (N and Z only, when the condition reads no more)
            skip = A.bcond_fwd((Cond)(cond ^ 1));
        }

        if (thumb && nk && nk != 5 && nk != 6 && nk != 7) {  // compiled as the ARM instruction it means; flags, registers and cycles stay Thumb's
            CurInstr.Instr = armInstr;
            if (nk == 2) CurInstr.Info.Kind = armKind;
        }
        if (nk == 1) {
            CompileALU();
            AddCycles(CyclesC());  // the same taken or skipped
        } else if (nk == 2) {
            CompileMem();
        } else if (nk == 3) {
            CompileMul();          // charges its own AddCycles_CI
        } else if (nk == 4) {
            CompileALU();          // register-shifted operand; CompileALU charges its own AddCycles_CI(1)
        } else if (nk == 5) {
            CompileBlockTransfer();  // charges its own AddCycles_CDI/CD (loads/stores) through the helper
        } else if (nk == 6) {
            branchExited = CompileBranch();
        } else if (nk == 7) {
            branchExited = CompileJump(jumpForm);
        } else {
            Fallback(thumb);
        }

        // after it ran: idle-loop marking, and leaving a block that followed the other way
        auto special = [&](bool taken) {
            if (taken && branchExited) return;  // CompileBranch's taken path has left the block
            if (!taken && nk == 6 && (CurInstr.BranchFlags & branch_FollowCondTaken)) {
                s32 keep = ConstantCycles;  // not taken, the block leaves to the next instruction
                u16 keepDirt = Dirt;
                ExitBlock(true, R15);
                ConstantCycles = keep;
                Dirt = keepDirt;
                return;
            }
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

        lastExited = branchExited && !conditional;
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
            // the skip path's host C and V are not the guest's when only N and Z were valid before the branch
            if (!preFull && FlagsEpoch == A.flagEpoch) FlagsEpoch = A.flagEpoch - 1;
        } else {
            special(true);
        }
    }

    if (!lastExited) ExitBlock(exitKnown, exitPC);

    // the fast-memory accesses' slow paths, out of the way of the block's own code
    for (auto& d : SlowPaths) {
        u32* slow = A.here();
        d.emit();
        A.put(A64::b_((int32_t)(d.back - A.here())));
        for (u32* f : d.faulting)
            Patches[(u32)((u8*)f - CodeBase)] = {(u32)((u8*)d.seq - CodeBase), (u32)((u8*)slow - CodeBase)};
    }
    SlowPaths.clear();
    CurConditional = false;

    __builtin___clear_cache((char*)start, (char*)A.here());
    if (PerfMap()) {
        fprintf(PerfMap(), "%llx %llx bj%d_%s_%08x\n", (unsigned long long)(uintptr_t)start,
                (unsigned long long)((u8*)A.here() - (u8*)start), cpu->Num ? 7 : 9, thumb ? "t" : "a", instrs[0].Addr);
        const uint64_t at = (uint64_t)(uintptr_t)start;
        const uint32_t len = (uint32_t)((u8*)A.here() - (u8*)start);
        fwrite(&at, 8, 1, CodeDump()); fwrite(&len, 4, 1, CodeDump()); fwrite(start, 1, len, CodeDump());
    }
    return (JitBlockEntry)start;
}

}  // namespace melonDS

#endif
