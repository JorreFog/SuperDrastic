@ jitloops: the IRQ handler the BIOS calls (its address at DTCM+0x3FFC): counts VBlank and timer 0 IRQs in
@ memory, flips a register-visible flag word, acknowledges IF and the BIOS's IntrWait flags
    .arm
    .global irq_handler
irq_handler:
    ldr     r0, =0x04000000
    ldr     r1, [r0, #0x214]        @ IF
    ldr     r2, [r0, #0x210]        @ IE
    and     r1, r1, r2
    str     r1, [r0, #0x214]        @ acknowledge
    ldr     r2, =irq_counts
    tst     r1, #1
    ldrne   r3, [r2]
    addne   r3, r3, #1
    strne   r3, [r2]                @ VBlank count
    tst     r1, #8
    ldrne   r3, [r2, #4]
    addne   r3, r3, #1
    strne   r3, [r2, #4]            @ timer 0 count
    ldr     r3, [r2, #8]
    eor     r3, r3, r1
    str     r3, [r2, #8]            @ a flag word the loops poll
    mrc     p15, 0, r0, c9, c1, 0
    bic     r0, r0, #0xFF
    bic     r0, r0, #0xF00
    add     r0, r0, #0x3F00
    ldr     r2, [r0, #0xF8]         @ the BIOS's IRQ flags (IntrWait)
    orr     r2, r2, r1
    str     r2, [r0, #0xF8]
    bx      lr
    .pool
