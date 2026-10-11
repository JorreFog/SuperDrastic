// BananaJIT: BananaDS's own ARM-to-AArch64 code generator, for melonDS's JIT framework (block decoding, the
// block cache, invalidation and idle-loop detection stay melonDS's). It takes the place of melonDS's
// ARMJIT_A64 compiler when BananaDS is built with -DBANANADS_JIT=banana (docs/JIT.md).
//
// Design:
//  - Registers: x29 = the ARM CPU, w28 = cycles, w27 = CPSR. Guest r0-r7 live in w19-w26, r8-r14 in w9-w15
//    while a block runs. Each is loaded the first time the block needs it and stored back if it was written,
//    at the block's exits and before any call out.
//  - Flags: ARM's condition codes and NZCV positions match AArch64's, so conditions are a B.cond on the host
//    flags, and S instructions take their flags straight from the host's arithmetic (MRS NZCV).
//  - Cycles: each instruction costs what melonDS's interpreter charges for it, taken or skipped.
//  - Coverage: what isn't compiled natively runs through melonDS's interpreter, one instruction at a time,
//    with the framework's protocol (R15, CurInstr, CodeCycles set first). So every instruction works from the
//    start, and native forms are added one class at a time. docs/JIT.md lists what is native.
#ifndef BANANAJIT_H
#define BANANAJIT_H

#if defined(JIT_ENABLED) && defined(__aarch64__)

#include <cstddef>

#include "ARM.h"
#include "ARMJIT_Internal.h"
#include "a64.h"

namespace melonDS {

class Compiler {
public:
    explicit Compiler(melonDS::NDS& nds);
    ~Compiler();

    // What melonDS's JIT framework calls.
    JitBlockEntry CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int instrsCount, bool hasMemInstr);
    bool CanCompile(bool thumb, u16 kind);
    void Reset();
    bool IsJITFault(const u8* pc);
    u8* RewriteMemAccess(u8* pc);  // BananaJIT doesn't use fastmem yet: never reached
    JitBlockEntry AddEntryOffset(u32 offset) { return (JitBlockEntry)(CodeBase + offset); }
    u32 SubEntryOffset(JitBlockEntry entry) { return (u32)((u8*)entry - CodeBase); }

    void* CodeMemBase = nullptr;

    // Native coverage (for docs and tests): true if this ARM instruction compiles without the interpreter.
    static bool NativeARM(u32 instr, u16 kind);
    static int NativeKindARM(u32 instr, u16 kind);  // 0 interpreter, 1 data processing, 2 load/store
    static int ThumbToARM(u32 thumb, u16 kind, u32 r15, u32& arm, u16& armKind, u32& pc, bool& noRotate);

private:
    using Reg = bananajit::Reg;
    Reg host(int guest) const;
    void Need(int guest);      // load it if this block hasn't yet
    void Dirty(int guest);     // it now holds a new value
    void FlushRegs(bool forget);  // store dirty registers; forget = they must be reloaded (after a call)
    void LoadFlags();          // host NZCV <- CPSR
    void ExitBlock();
    void Fallback(bool thumb);
    void CompileALU();
    void CompileMem();
    Reg Shifter(u32 instr, bool wantC, bool& carry_changes, Reg dst);
    void StoreHigh();             // store dirty r8-r14 (they live in caller-saved registers)
    void CallOut(const void* fn);  // fn(cpu, w1, w2) with the interpreter's state in memory
    void AddCycles(s32 n) { ConstantCycles += n; }
    void FlushCycles();
    s32 CyclesC() const;       // what the interpreter's AddCycles_C charges for CurInstr

    melonDS::NDS& NDS;
    u8* CodeBase = nullptr;
    bananajit::A64 A;
    size_t CodeSize = 0;

    // per block
    ARM* CurCPU = nullptr;
    FetchedInstr CurInstr{};
    bool Thumb = false;
    u32 R15 = 0;
    s32 ConstantCycles = 0;
    u16 Loaded = 0, Dirt = 0;
    u32 PCValue = 0;     // what reading the PC gives in this instruction
    bool NoRotate = false;
};

}  // namespace melonDS

#endif
#endif
