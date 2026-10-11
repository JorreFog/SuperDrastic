@ ARM9 watchdog: waits ~40 frames (VCOUNT 192 edges), then writes count=4 and DONE.
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    ldr r4, =0x04000006
    mov r5, #40
1:  ldrh r0, [r4]
    cmp r0, #192
    bne 1b
2:  ldrh r0, [r4]
    cmp r0, #192
    beq 2b
    subs r5, r5, #1
    bne 1b
    ldr r0, =0x02200000
    mov r1, #4
    str r1, [r0, #4]
    ldr r1, =0x454E4F44
    str r1, [r0]
3:  b 3b
    .ltorg
