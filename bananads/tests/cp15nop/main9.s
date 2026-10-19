@ cp15nop's ARM9 main (ARM): the CP15 cache and write buffer MCRs as NitroSDK's DC_* loops issue them, with every
@ register live, conditional ones, and timer 0 read after each loop so the cycles they count land in main RAM; then,
@ after 64 rounds, one MCR in user mode (A_MCR's undefined instruction exception), its registers and CPSR stored.
@ For JIT patch 0086 (jit.cp15nop): compare jit.cp15nop=off against on with bananads-difftest --every --regs.
    .arm
    .text
    .global main
main:
    ldr     r0, =0x04000100
    ldr     r1, =0x00800000         @ TM0: enabled, F/1 (33 MHz), reload 0
    str     r1, [r0]
    mrc     p15, 0, r1, c1, c0, 0
    bic     r1, r1, #0x2000         @ low vectors: the undefined instruction's at 0x00000004 (ITCM)
    mcr     p15, 0, r1, c1, c0, 0
    mov     r0, #0
    ldr     r1, =0xE51FF004         @ ldr pc, [pc, #-4]
    str     r1, [r0, #4]
    ldr     r1, =und_handler
    str     r1, [r0, #8]
    ldr     r12, =results
    mov     r11, #0                 @ round
round:
    @ 1: DC_FlushRange's loop: drain, clean and invalidate per 32-byte line, plus work on every register
    ldr     r0, =buf
    add     r1, r0, #2048
    mov     r2, #0
    mov     r3, r11
    mov     r4, #0x55
    ldr     r5, =0x9E3779B9
    mov     r6, #7
    mov     r7, #3
    mov     r8, #1
    mov     r9, #2
    mov     r10, #5
    mov     lr, #9
1:  mcr     p15, 0, r2, c7, c10, 4
    mcr     p15, 0, r0, c7, c14, 1
    add     r3, r3, r0
    eor     r4, r4, r3, ror #3
    mla     r6, r5, r6, r7
    add     r7, r7, r8
    eor     r8, r8, r9, lsl #1
    add     r9, r9, r10
    sub     r10, r10, lr
    add     lr, lr, r4
    str     r3, [r0]
    add     r0, r0, #32
    cmp     r0, r1
    blt     1b
    ldr     r2, =0x04000100
    ldrh    r2, [r2]
    stmia   r12!, {r2-r10, lr}
    @ 2: conditional ones (DC_StoreRange's c7,c10,1, DC_InvalidateRange's c7,c6,1, by set/index c7,c10,2/c7,c14,2)
    ldr     r0, =buf
    add     r1, r0, #1024
3:  tst     r0, #64
    mcrne   p15, 0, r0, c7, c10, 1
    mcreq   p15, 0, r0, c7, c6, 1
    tst     r0, #32
    mcrne   p15, 0, r3, c7, c10, 2
    mcreq   p15, 1, r3, c7, c14, 2
    addne   r3, r3, #1
    eoreq   r4, r4, r0
    add     r0, r0, #32
    cmp     r0, r1
    blo     3b
    ldr     r2, =0x04000100
    ldrh    r2, [r2]
    stmia   r12!, {r2-r4}
    ldr     r0, =results_end
    cmp     r12, r0
    ldrhs   r12, =results
    add     r11, r11, #1
    cmp     r11, #64
    blo     round
    @ 3: user mode, an MCR, everything stored (the JIT continues the block after A_MCR's exception, as it always did)
    ldr     r12, =umode
    mov     r0, #0x11
    mov     r1, #0x22
    mov     r13, #0x33
    mov     r14, #0x44
    msr     cpsr_c, #0x10
    mcr     p15, 0, r0, c7, c10, 4
    add     r0, r0, r1
    mrs     r2, cpsr
    b       5f                      @ (a new block: the timer read below sees the cycles counted up to here)
5:  stmia   r12, {r0-r2, r13, r14}
    ldr     r3, =0x04000100
    ldrh    r3, [r3]
    str     r3, [r12, #20]
    @ again, in user mode with R13/R14 not known constants (the patch's far region; with constants above the JIT
    @ keeps the interpreter's call)
    add     r12, r12, #32
    add     r13, r12, #0x100
    add     r14, r12, #0x200
    mov     r0, #0x11
    mov     r5, #1000               @ (1000 times: a few cycles' mistake each adds up past a slice)
7:  msr     cpsr_c, #0x10
    mcr     p15, 0, r0, c7, c14, 1
    add     r0, r0, r14
    mrs     r2, cpsr
    subs    r5, r5, #1
    bne     7b
    b       6f
6:  stmia   r12, {r0-r2, r13, r14}
    ldr     r3, =0x04000100
    ldrh    r3, [r3]
    str     r3, [r12, #20]
4:  b       4b
und_handler:
    ldr     r12, =und_seen
    str     lr, [r12]
    mrs     r0, spsr
    str     r0, [r12, #4]
    movs    pc, lr
    .pool
    .data
    .align 5
buf:    .space 2048
results: .space 1600
results_end:
umode:  .space 64
und_seen: .space 8
