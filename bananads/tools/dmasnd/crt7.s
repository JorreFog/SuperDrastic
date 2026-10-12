    .section .text.start, "ax"
    .arm
    .global _start7
_start7:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]
    msr     cpsr_c, #0xDF
    ldr     sp, =0x0380FF00
    bl      main7
1:  b       1b
    .pool
