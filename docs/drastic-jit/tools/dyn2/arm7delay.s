@ ARM7: waits for cmd==1 at 0x02200040, spins [0x02200044] iterations, sets flag 0x02200050 = 1, cmd = 2
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    ldr r0, =0x02200040
1:  ldr r1, [r0]
    cmp r1, #1
    bne 1b
    ldr r2, [r0, #4]
2:  subs r2, r2, #1
    bne 2b
    mov r1, #1
    str r1, [r0, #0x10]
    mov r1, #2
    str r1, [r0]
    b 1b
    .ltorg
