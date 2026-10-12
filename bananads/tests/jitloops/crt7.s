    .section .text.start, "ax"
    .arm
    .global _start7
_start7:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]
    msr     cpsr_c, #0xD2           @ IRQ mode: its stack
    ldr     sp, =0x0380FD80
    msr     cpsr_c, #0xDF           @ system mode
    ldr     sp, =0x0380FC00
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b
    ldr     r0, =main7
    bx      r0
    .pool
