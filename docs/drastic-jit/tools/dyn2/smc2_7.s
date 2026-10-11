@ ARM7: waits for cmd==1 at 0x02200040, calls f7 at 0x02390000, stores r0 at 0x0220004c, sets cmd=2
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    ldr sp, =0x0380FE00
    ldr r4, =0x02200040
1:  ldr r1, [r4]
    cmp r1, #1
    bne 1b
    bl f7
    str r0, [r4, #12]
    mov r1, #2
    str r1, [r4]
    b 1b
    .ltorg
    .org 0x10000
f7: mov r0, #0x70
    bx lr
