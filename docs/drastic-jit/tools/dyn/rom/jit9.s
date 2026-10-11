@ jittest ARM9 program: one labelled block per instruction class, run in an endless loop, so DraStic's JIT
@ translates each of them (and re-translates the self-modified one). Load/run address 0x02000000.
    .syntax unified
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]        @ IME off
    msr     cpsr_c, #0xDF           @ system mode, IRQ/FIQ masked
    ldr     sp, =0x023F0000
    mov     r11, #0                 @ iteration counter
mainloop:
    bl      t_alu_flags
    bl      t_cond
    bl      t_mem
    bl      t_ldm_stm
    bl      t_branch
    ldr     r0, =t_thumb            @ .thumb_func symbol already has bit 0 set
    blx     r0                      @ ARM -> Thumb interworking
    bl      t_smc
    bl      t_shift_mul
    add     r11, r11, #1
    b       mainloop
    .pool

@ ---- ALU with flags: S-suffixed ops, carry in/out, compare/test ----
    .balign 32
t_alu_flags:
    mov     r0, #0x7F000000
    mov     r1, #0x81000000
    adds    r2, r0, r1              @ sets C, V
    adcs    r3, r0, #1              @ uses C
    subs    r4, r2, r3
    sbcs    r5, r4, r0
    rsbs    r6, r5, #0
    eors    r7, r6, r0
    ands    r8, r7, #0xFF
    orrs    r9, r8, r1, lsr #4
    bics    r10, r9, #0xF
    cmp     r10, r9
    cmn     r10, #1
    tst     r10, #0x80
    teq     r10, r0
    mvns    r12, r10
    bx      lr

@ ---- conditional execution ----
    .balign 32
t_cond:
    mov     r0, #5
    cmp     r0, #5
    moveq   r1, #1
    movne   r1, #2
    addgt   r2, r1, #3
    suble   r2, r1, #3
    ldreq   r3, =cvar
    streq   r0, [r3]
    cmp     r0, #10
    addlo   r4, r0, r0, lsl #1
    bxhs    lr                      @ not taken
    bx      lr

@ ---- single loads/stores: imm/reg offsets, pre/post index, writeback, byte/half/signed/double ----
    .balign 32
t_mem:
    ldr     r0, =buf
    mov     r1, #8
    ldr     r2, [r0]
    ldr     r3, [r0, #4]
    ldr     r4, [r0, r1]
    ldr     r5, [r0, r1, lsl #1]
    str     r2, [r0, #0x20]!
    str     r3, [r0], #-0x1C
    ldrb    r6, [r0, #1]
    strb    r6, [r0, #0x30]
    ldrh    r7, [r0, #2]
    strh    r7, [r0, #0x32]
    ldrsb   r8, [r0, #3]
    ldrsh   r9, [r0, #2]
    ldrd    r2, r3, [r0, #8]
    strd    r2, r3, [r0, #0x38]
    ldr     r10, [pc, #-4]          @ PC-relative load (literal)
    bx      lr

@ ---- block transfers ----
    .balign 32
t_ldm_stm:
    push    {r4-r11, lr}            @ stmdb sp!, {...}
    ldr     r0, =buf
    ldmia   r0, {r1-r3}
    ldmib   r0!, {r4, r5}
    stmda   r0, {r1, r2}
    stmia   r0, {r1-r10}
    ldmdb   r0, {r6-r8}
    pop     {r4-r11, pc}            @ ldmia sp!, {...,pc}: return through LDM

@ ---- branches: bl/bx/blx, loop with backward branch ----
    .balign 32
t_branch:
    push    {lr}
    mov     r0, #4
1:  subs    r0, r0, #1
    bne     1b
    bl      leaf
    ldr     r1, =leaf
    blx     r1                      @ BLX register (ARM target)
    pop     {pc}
leaf:
    add     r2, r2, #1
    bx      lr

@ ---- self-modifying code: rewrite the immediate of a mov each iteration ----
    .balign 32
t_smc:
    ldr     r0, =smc_insn
    and     r1, r11, #0xFF
    orr     r1, r1, #0xE3000000
    orr     r1, r1, #0x00A00000     @ mov r0, #imm8  (E3A000xx)
    .ifndef NO_SMC
    str     r1, [r0]
    .endif
    mov     r2, #0
    .ifndef NO_MCR
    mcr     p15, 0, r2, c7, c5, 0   @ invalidate ICache
    .endif
    b       smc_insn
    .balign 32
smc_insn:
    mov     r0, #0
    bx      lr

@ ---- shifts by register, multiplies, clz, saturating ----
    .balign 32
t_shift_mul:
    mov     r0, #3
    mov     r1, #0x12
    mov     r2, r1, lsl r0
    movs    r3, r1, ror r0
    mul     r4, r1, r0
    mla     r5, r1, r0, r4
    umull   r6, r7, r1, r2
    smlal   r6, r7, r1, r2
    clz     r8, r1
    qadd    r9, r1, r2
    smulbb  r10, r1, r2
    bx      lr

@ ---- Thumb ----
    .balign 32
    .thumb
    .thumb_func
t_thumb:
    push    {r4, lr}
    movs    r0, #10
    movs    r1, #3
    adds    r2, r0, r1
    subs    r3, r2, #1
    lsls    r4, r3, #2
    ands    r4, r1
    ldr     r0, =buf
    ldr     r1, [r0, #4]
    str     r1, [r0, #8]
    ldrh    r2, [r0, #2]
    strb    r2, [r0, #1]
    cmp     r1, #0
    beq     2f
    adds    r1, #1
2:  bl      thumb_leaf
    pop     {r4, pc}                @ pop pc: returns to ARM (ARMv5 interworking)
    .thumb_func
thumb_leaf:
    movs    r3, #7
    bx      lr
    .pool

    .arm
    .data
    .balign 4
cvar: .word 0
buf:  .space 16
      .space 128
