// BananaJIT's way in and out of generated code, in place of melonDS's ARMJIT_Linkage.S.
// ARM_Dispatch(ARM* cpu, JitBlockEntry block): saves the callee-saved registers, puts the CPU in x29, its cycles
// in w28 and its CPSR in w27 (the state blocks keep in registers, linked block to linked block) and jumps to the
// block. A block leaves through BJ_RetState (it stores w28 and w27 back) or ARM_Ret, which restore the registers.
// bj_offsets (bananajit.cpp): offsetof(ARM, Cycles), offsetof(ARM, CPSR).
    .text
    .p2align 4
    .global ARM_Dispatch
ARM_Dispatch:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x29, x0
    adrp x2, bj_offsets
    add x2, x2, :lo12:bj_offsets
    ldp w3, w4, [x2]
    ldr w28, [x29, x3]
    ldr w27, [x29, x4]
    br x1

    .p2align 4
    .global BJ_RetState
BJ_RetState:
    adrp x2, bj_offsets
    add x2, x2, :lo12:bj_offsets
    ldp w3, w4, [x2]
    str w28, [x29, x3]
    str w27, [x29, x4]

    .global ARM_Ret
ARM_Ret:
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #96
    ret

// melonDS's fastmem fault handler returns through this. BananaJIT runs with fastmem off, so it is never
// reached; it exists so the framework links.
    .p2align 4
    .global ARM_RestoreContext
ARM_RestoreContext:
    brk #0

    .section .note.GNU-stack,"",%progbits
