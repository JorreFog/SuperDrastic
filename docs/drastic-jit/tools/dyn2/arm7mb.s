@ ARM7 mailbox servant: waits for cmd==1 at 0x02200040, stores value [0x02200048] at address [0x02200044], sets cmd=2
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
    ldr r3, [r0, #8]
    str r3, [r2]
    mov r1, #2
    str r1, [r0]
    b 1b
    .ltorg
