@ SMC granularity/scope test (ARM9). Results at 0x02200100 (r11 post-inc), phase marker at 0x02200008,
@ ARM7 mailbox at 0x02200040 {cmd, addr, value}: cmd=1 -> ARM7 stores value at addr, sets cmd=2.
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
    ldr r10, =0x02200008            @ phase marker
    ldr r9, =0x02200040             @ ARM7 mailbox
    mov r0, #0
    str r0, [r9]
    @ copy fI to ITCM mirror 0x01ff8000
    ldr r0, =fI
    ldr r1, =0x01FF8000
    ldr r2, [r0]
    str r2, [r1]
    ldr r2, [r0, #4]
    str r2, [r1, #4]

    .macro PH n
    mov r0, #\n
    str r0, [r10]
    .endm
    .macro CALLALL
    bl fA
    str r0, [r11], #4
    bl fB
    str r0, [r11], #4
    bl fC
    str r0, [r11], #4
    bl fL
    str r0, [r11], #4
    .endm
    .macro STEP n
    PH \n*10+1
    CALLALL
    PH \n*10+2
    .endm
    .macro AFTER n
    PH \n*10+3
    mov r0, #\n
    str r0, [r11], #4
    CALLALL
    .endm

    @ phase 0: warm-up
    PH 1
    CALLALL
    CALLALL
    @ phase 1: str new value into fA (fB shares its 2 KB page; fC is 128 KB away)
    STEP 1
    ldr r0, =fA
    ldr r1, =0xE3A00011
    str r1, [r0]
    AFTER 1
    @ phase 2: str new value into fC (other page, 128 KB away)
    STEP 2
    ldr r0, =fC
    ldr r1, =0xE3A00033
    str r1, [r0]
    AFTER 2
    @ phase 3: str into a data word in fC's 2 KB page that is not inside any block
    STEP 3
    ldr r0, =dataD
    ldr r1, =0x12345678
    str r1, [r0]
    AFTER 3
    @ phase 4: str of the unchanged value into fA
    STEP 4
    ldr r0, =fA
    ldr r1, =0xE3A00011
    str r1, [r0]
    AFTER 4
    @ phase 5: strb into fB's immediate byte
    STEP 5
    ldr r0, =fB
    mov r1, #0x5B
    strb r1, [r0]
    AFTER 5
    @ phase 6: str into fL's literal (constant-folded at translation time)
    STEP 6
    ldr r0, =litL
    mov r1, #0x66
    str r1, [r0]
    AFTER 6
    @ phase 7: second store into the literal (now in the modified bitmap: no more folding?)
    STEP 7
    ldr r0, =litL
    mov r1, #0x77
    str r1, [r0]
    AFTER 7
    @ phase 8: ARM7 stores into fA (mov r0,#0x7a)
    STEP 8
    ldr r0, =fA
    str r0, [r9, #4]
    ldr r0, =0xE3A0007A
    str r0, [r9, #8]
    mov r0, #1
    str r0, [r9]
1:  ldr r0, [r9]
    cmp r0, #2
    bne 1b
    AFTER 8
    @ phase 9: ARM9 str into fC again (forces a flush): does fA now see the ARM7 write?
    STEP 9
    ldr r0, =fC
    ldr r1, =0xE3A00039
    str r1, [r0]
    AFTER 9
    @ phase 10: DMA3 copies one word over fB (mov r0,#0x8b)
    STEP 10
    ldr r0, =dmasrc
    ldr r1, =0xE3A0008B
    str r1, [r0]
    ldr r2, =0x040000D4
    str r0, [r2]
    ldr r1, =fB
    str r1, [r2, #4]
    ldr r1, =0x84000001
    str r1, [r2, #8]
1:  ldr r1, [r2, #8]
    tst r1, #0x80000000
    bne 1b
    AFTER 10
    @ phase 11: ITCM: call 0x01ff8000, rewrite its mov r0,#imm 24 times alternating a0/a1 (and a2 once)
    PH 111
    ldr r4, =0x01FF8000
    mov lr, pc
    bx r4
    str r0, [r11], #4
    PH 112
    mov r5, #0
2:  and r1, r5, #1
    ldr r2, =0xE3A000A0
    orr r1, r2, r1
    cmp r5, #20
    orreq r1, r2, #2
    str r1, [r4]
    mov lr, pc
    bx r4
    str r0, [r11], #4
    add r5, r5, #1
    cmp r5, #24
    blt 2b
    PH 113
    CALLALL
    @ done
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
fI: mov r0, #0xA0
    bx lr

    .org 0x800
fA: mov r0, #1
    bx lr
    .org 0x900
fB: mov r0, #2
    bx lr
    .org 0xa00
fL: ldr r0, litL
    bx lr
litL: .word 0x55
    .org 0x20000
fC: mov r0, #3
    bx lr
    .org 0x20400
dataD: .word 0
    .org 0x20800
dmasrc: .word 0
