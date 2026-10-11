@ Round-3 ARM7 tests (ARMv5-only instructions on the ARMv4T ARM7). Select: CLZ BLXI LDRD STRD THBLX.
@ Writes progress at 0x02200100 (1 = before, 2 = after), values at 0x02200104.. ; the ARM9 watchdog writes DONE.
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    msr cpsr_c, #0xDF
    ldr sp, =0x0380FE00
    ldr r11, =0x02200100
    mov r0, #1
    str r0, [r11]
.ifdef CLZ
    ldr r1, =0x00F00000
    mov r0, #0x55
    clz r0, r1                      @ executed: 8, NOP: 0x55, UND: hang
    str r0, [r11, #4]
.endif
.ifdef BLXI
    mov r0, #0x55
    blx tf                          @ executed: 0x77
    str r0, [r11, #4]
.endif
.ifdef LDRD
    ldr r4, =0x02210000
    ldr r2, =0xAAAA0001
    str r2, [r4]
    ldr r2, =0xBBBB0002
    str r2, [r4, #4]
    mov r2, #0
    mov r3, #0
    ldrd r2, r3, [r4]
    str r2, [r11, #4]
    str r3, [r11, #8]
.endif
.ifdef STRD
    ldr r4, =0x02210000
    mov r2, #0
    str r2, [r4]
    str r2, [r4, #4]
    ldr r2, =0x12345678
    ldr r3, =0x9ABCDEF0
    strd r2, r3, [r4]
    ldr r2, [r4]
    ldr r3, [r4, #4]
    str r2, [r11, #4]
    str r3, [r11, #8]
.endif
.ifdef THBLX
    mov r0, #0x55
    ldr r12, =th_blx
    mov lr, pc
    bx r12
    str r0, [r11, #4]               @ executed BLX: 0x66+1
.endif
    mov r0, #2
    str r0, [r11]
1:  b 1b
    .ltorg
af: mov r0, #0x66
    bx lr
    .thumb
    .balign 4
    .thumb_func
th_blx:
    mov r7, lr
    blx af
    adds r0, #1
    bx r7
    .thumb_func
tf: movs r0, #0x77
    bx lr
    .arm
