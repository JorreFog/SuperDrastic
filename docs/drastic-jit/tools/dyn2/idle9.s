@ timing / idle-loop test (ARM9). Timer0+Timer1 cascade (33.5 MHz ticks). Results at 0x02200100.
    .syntax unified
    .arm
    .section .text.start, "ax"
    .global _start
_start:
    mov r0, #0x04000000
    mov r1, #0
    str r1, [r0, #0x208]
    msr cpsr_c, #0xDF
    ldr sp, =0x023F0000
    ldr r11, =0x02200100
    ldr r10, =0x02200008
    ldr r9, =0x02200040            @ ARM7 mailbox {cmd, delay, -, -, flag@0x50}
    ldr r4, =0x02200050            @ flag
    ldr r5, =0x04000006            @ VCOUNT
    ldr r8, =0x04000100
    mov r0, #0
    str r0, [r9]
    str r0, [r4]
    str r0, [r8]
    str r0, [r8, #4]
    ldr r0, =0x00840000
    str r0, [r8, #4]               @ TM1: cascade, enable
    ldr r0, =0x00800000
    str r0, [r8]                   @ TM0: F/1, enable
    .macro TICKS rd
    ldrh \rd, [r8, #4]
    ldrh r3, [r8]
    orr \rd, r3, \rd, lsl #16
    .endm
    .macro PH n
    mov r0, #\n
    str r0, [r10]
    .endm
    .macro A7DELAY n               @ ask ARM7 to set flag after n loop iterations
    mov r0, #0
    str r0, [r4]
    ldr r0, =\n
    str r0, [r9, #4]
    mov r0, #1
    str r0, [r9]
    .endm
    .macro A7WAIT
2:  ldr r0, [r9]
    cmp r0, #2
    bne 2b
    .endm

    @ 1: tight loop, 1,000,000 iterations
    PH 1
    TICKS r6
    ldr r0, =1000000
    bl tight
    TICKS r7
    sub r7, r7, r6
    str r7, [r11], #4
    @ 2: count iterations of a VCOUNT-polling counter loop over 100 scanlines
    PH 2
    bl vwait0
    TICKS r7
    mov r6, #0
    bl vcount_loop
    TICKS r1
    sub r1, r1, r7
    str r6, [r11], #4
    str r1, [r11], #4
    @ 3: classic idle loop on a flag set by the ARM7 after a delay
    PH 3
    TICKS r7
    A7DELAY 200000
    bl idleA
    TICKS r1
    sub r1, r1, r7
    str r1, [r11], #4
    A7WAIT
    @ 4: same, but the loop counts (not idle)
    PH 4
    TICKS r7
    A7DELAY 200000
    mov r6, #0
    bl idleC
    TICKS r1
    sub r1, r1, r7
    str r1, [r11], #4
    str r6, [r11], #4
    A7WAIT
    @ 5: loop with a store (not idle)
    PH 5
    TICKS r7
    A7DELAY 200000
    bl idleD
    TICKS r1
    sub r1, r1, r7
    str r1, [r11], #4
    A7WAIT
    @ 6: pure VCOUNT idle loop (wait for line 150)
    PH 6
    bl vwait0
    TICKS r7
    bl vwait150
    TICKS r1
    sub r1, r1, r7
    str r1, [r11], #4
    PH 200
    ldr r0, =0x02200000
    ldr r1, =0x02200100
    sub r1, r11, r1
    lsr r1, r1, #2
    str r1, [r0, #4]
    ldr r1, =0x454E4F44
    str r1, [r0]
3:  b 3b
    .ltorg
    .balign 64
tight:
1:  subs r0, r0, #1
    bne 1b
    bx lr
    .balign 64
vwait0:
1:  ldrh r0, [r5]
    cmp r0, #0
    bne 1b
    bx lr
    .balign 64
vwait150:
1:  ldrh r0, [r5]
    cmp r0, #150
    bne 1b
    bx lr
    .balign 64
vcount_loop:
1:  add r6, r6, #1
    ldrh r0, [r5]
    cmp r0, #100
    bne 1b
    bx lr
    .balign 64
idleA:
1:  ldr r0, [r4]
    cmp r0, #0
    beq 1b
    bx lr
    .balign 64
idleC:
1:  add r6, r6, #1
    ldr r0, [r4]
    cmp r0, #0
    beq 1b
    bx lr
    .balign 64
idleD:
1:  ldr r0, [r4]
    str r0, [r4, #4]
    cmp r0, #0
    beq 1b
    bx lr
