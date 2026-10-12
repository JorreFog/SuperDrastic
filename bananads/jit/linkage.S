// BananaJIT's way in and out of generated code, in place of melonDS's ARMJIT_Linkage.S.
// ARM_Dispatch(ARM* cpu, JitBlockEntry block): saves the callee-saved registers, puts the CPU in x29, its cycles
// in w28, its CPSR in w27 and (with resident registers, the default) the guest's r0-r14 in their host registers:
// the state blocks keep in registers, linked block to linked block. It jumps to the block. A block leaves through
// BJ_RetState, which stores that state back, and ARM_Ret, which restores the callee-saved registers.
// bj_offsets (bananajit.cpp): offsetof(ARM, Cycles), offsetof(ARM, CPSR), offsetof(ARM, R), resident.
    .text
    .p2align 4
    .global ARM_Dispatch
ARM_Dispatch:
    stp x19, x20, [sp, #-112]!  // (sp + 96: the cycle budget, bananajit.cpp's BudgetOff)
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    adrp x2, bj_link            // an exit asked to be linked to the block it leaves for: bj_link_pending(cpu, block)
    ldr x3, [x2, :lo12:bj_link]
    cbz x3, 2f
    mov x19, x0
    mov x20, x1
    bl bj_link_pending
    mov x0, x19
    mov x1, x20
2:  mov x29, x0
    adrp x2, bj_offsets
    add x2, x2, :lo12:bj_offsets
    ldp w3, w4, [x2]
    ldr w28, [x29, x3]
    ldr w27, [x29, x4]
    ldr w5, [x2, #24]           // offsetof(ARM, Num)
    ldr w6, [x29, x5]
    add x7, x2, #16             // the fast-memory base's offset from this CPU object -> x8
    ldrsw x7, [x7, x6, lsl #2]
    ldr x8, [x29, x7]
    add x7, x2, #32             // B0 = Target - Timestamp; w28 = Cycles - B0 (bananajit.cpp, BudgetOff)
    ldrsw x7, [x7, x6, lsl #2]
    add x7, x29, x7
    ldp x9, x10, [x7]
    sub x10, x10, x9
    sub w28, w28, w10
    ldr w5, [x2, #28]           // StopExecution set: forced (B0 += w28, w28 = 0), the first exit leaves
    ldr w9, [x29, x5]
    cbz w9, 3f
    add w10, w10, w28
    mov w28, #0
3:  str w10, [sp, #96]
    ldp w5, w6, [x2, #8]        // offsetof(ARM, R), resident
    cbz w6, 1f
    add x7, x29, x5             // the guest's r0-r7 in w19-w26, r8-r14 in w9-w15
    ldp w19, w20, [x7]
    ldp w21, w22, [x7, #8]
    ldp w23, w24, [x7, #16]
    ldp w25, w26, [x7, #24]
    ldp w9, w10, [x7, #32]
    ldp w11, w12, [x7, #40]
    ldp w13, w14, [x7, #48]
    ldr w15, [x7, #56]
1:  br x1

    .p2align 4
    .global BJ_RetState
BJ_RetState:
    adrp x2, bj_offsets
    add x2, x2, :lo12:bj_offsets
    ldp w3, w4, [x2]
    ldr w5, [sp, #96]           // the CPU's Cycles: w28 + B0
    add w28, w28, w5
    str w28, [x29, x3]
    str w27, [x29, x4]
    ldp w5, w6, [x2, #8]
    cbz w6, ARM_Ret
    add x7, x29, x5
    stp w19, w20, [x7]
    stp w21, w22, [x7, #8]
    stp w23, w24, [x7, #16]
    stp w25, w26, [x7, #24]
    stp w9, w10, [x7, #32]
    stp w11, w12, [x7, #40]
    stp w13, w14, [x7, #48]
    str w15, [x7, #56]

    .global ARM_Ret
ARM_Ret:
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #112
    ret

// melonDS's fastmem fault handler returns through this. BananaJIT runs with fastmem off, so it is never
// reached; it exists so the framework links.
    .p2align 4
    .global ARM_RestoreContext
ARM_RestoreContext:
    brk #0

    .section .note.GNU-stack,"",%progbits
