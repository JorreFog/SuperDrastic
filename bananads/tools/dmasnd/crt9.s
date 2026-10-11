    .section .text.start, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]
    msr     cpsr_c, #0xDF
    ldr     sp, =0x023F0000
    bl      main
1:  b       1b
    .pool
