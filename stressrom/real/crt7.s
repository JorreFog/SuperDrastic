@ dsreal ARM7 entry (main RAM at 0x02380000, like dsstress's)
    .section .text.start, "ax"
    .arm
    .global _start7, halt7
    .type halt7, %function
_start7:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]
    msr     cpsr_c, #0xD2
    ldr     sp, =0x0380FF00
    msr     cpsr_c, #0xDF
    ldr     sp, =0x0380FD00
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    ldr     r0, =main7
    mov     lr, pc
    bx      r0
2:  b       2b
    .pool
halt7:                          @ HALTCNT: halt until an interrupt
    mov     r0, #0x04000000
    mov     r1, #0x80
    strb    r1, [r0, #0x301]
    bx      lr
    .global irq_on7
    .type irq_on7, %function
irq_on7:
    mrs     r0, cpsr
    bic     r0, r0, #0x80
    msr     cpsr_c, r0
    bx      lr
