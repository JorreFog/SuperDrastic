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
#include <functional>
#include <unordered_map>
#include <vector>

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
    u8* RewriteMemAccess(u8* pc);  // a fast-memory access faulted: send it to its slow path from now on
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
    void LoadFlags();          // host NZCV <- CPSR, unless the host's NZCV still holds them
    u32 FlagsEpoch = ~0u;      // host NZCV == CPSR's NZCV while this equals A.flagEpoch
    // knownPC: the R15 it leaves, when constant; thumbAfter: the Thumb state then (-1: the block's)
    // addrInW0: the target's instruction address is in W0 (EmitJumpTo), R15 and the Thumb bit already set
    void ExitBlock(bool known = false, u32 knownPC = 0, int thumbAfter = -1, bool addrInW0 = false);
    void EmitJumpTo();
    static int DynJump(bool thumb, u32 instr, u16 kind, int num);  // 0 no, 1 BX/BLX reg, 2 LDM/POP with the PC
    bool CompileJump(int form);
    bool DoJumps = true;  // jit.jumps=off: jumps to a register through the interpreter
    // block timing: B, BL, BLX imm and Thumb B, B<cond>, BL (merged) with their target known when compiled, as
    // melonDS's JIT does them (fetch cycles counted at compile time); the taken path leaves the block itself unless
    // the framework follows the branch. Returns whether it did (the rest of the taken path is then dead).
    bool CompileBranch();
    static bool StaticBranch(bool thumb, u16 kind, int num);
    void StoreCPU(Reg r, size_t off);  // a u32 field of the CPU at any offset
    void Fallback(bool thumb);
    void CompileALU();
    void CompileMul();         // MUL/MLA/UMULL/UMLAL/SMULL/SMLAL, and Thumb MUL
    void CompileMem();
    void CompileBlockTransfer();  // LDM/STM and Thumb PUSH/POP/LDMIA/STMIA
    Reg Shifter(u32 instr, bool wantC, bool& carry_changes, Reg dst);
    Reg RegShifter(u32 instr, bool wantC);  // operand 2 shifted by a register; writes C itself when wantC
    void StoreHigh();             // store dirty r8-r14 (they live in caller-saved registers)
    void CallOut(const void* fn);  // fn(cpu, w1, w2) with the interpreter's state in memory
    void FastLoad9(int size, bool sign, bool rotate, const void* slowfn);  // ARM9 TCM/main-RAM loads inline
    bool FastLoads = false;
    bool FastStores = false;
    bool FastMem7On = false;
    void FastMem7(bool load, int size, bool sign, bool rotate, const void* slowfn);  // ARM7 main RAM/WRAM
    void FastStore9(int size, const void* slowfn);  // ARM9 DTCM/main-RAM stores inline
    bool LinkBlocks = false;
    bool DoMul = true;  // jit.mul=off sends multiply and register-shift to the interpreter (difftest gate)
    bool DoLDM = true;  // jit.ldm=off sends block transfers (LDM/STM/PUSH/POP) to the interpreter (difftest gate)
    void AddCycles(s32 n) { ConstantCycles += n; }
    void AddCyclesCI(s32 internal);  // emit AddCycles_CI(internal) to RCycles now (taken-path cycles)
    void FlushCycles();
    s32 CyclesC() const;       // what the interpreter's AddCycles_C charges for CurInstr

    // Block timing (jit.timing=block, the default): melonDS's JIT's level of timing, for speed. Loads and stores
    // go straight to the framework's fast memory (one host load or store; a fault sends that access to a slow
    // path for good) and their cycles are counted when the block is compiled, from the access the framework saw
    // when it decoded the block. Literal loads become constants. jit.timing=exact keeps the interpreter's timing.
    bool Exact = false;        // jit.timing=exact (BANANAJIT_EXACT)
    bool FastMem = false;      // this block: block timing with the framework's fast memory on
    bool MemBaseOK = false;    // x8 holds this CPU's fast-memory base (calls clobber it)
    bool CurConditional = false;
    void EnsureMemBase();
    s32 BlockMemCycles(bool store) const;  // AddCycles_CDI/CD at compile time (melonDS's JIT's formulas)
    void ChargeTaken(s32 n);   // cycles that only the executed path pays
    bool LoadLiteral(int size, int rd, u32 addr);
    void FastAccess(bool load, int size, bool sign, bool rotate, int rd, Reg val);
    void SlowAccess(bool load, int size, bool sign, bool rotate, Reg dst, Reg val);
    bool FastBlockTransfer(bool load, int rn, const int* regs, int n);
    void SlowBlockTransfer(bool load, const int* regs, int n);
    void SaveCallerSaved(u32 extra);
    void RestoreCallerSaved(u32 extra);
    bool StaticSlow() const;   // the access goes to memory fast memory never maps (I/O...): straight to the call
    struct Deferred { u32* seq; std::vector<u32*> faulting; u32* back; std::function<void()> emit; };
    std::vector<Deferred> SlowPaths;  // this block's slow paths, emitted after its code
    struct Patch { u32 seq, slow; };
    std::unordered_map<u32, Patch> Patches;  // a faulting load/store's code offset -> its sequence and slow path

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
