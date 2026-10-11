@ SMC scope test 2 (ARM9): STM into code, same-value STM, Thumb STRH into code, ARM9 store into ARM7 code,
@ main-RAM store vs ITCM blocks. Phase marker 0x02200008, ARM7 mailbox 0x02200040 {cmd, -, -, result}
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
    ldr r9, =0x02200040
    mov r0, #0
    str r0, [r9]
    ldr r0, =fI
    ldr r1, =0x01FF8000
    ldr r2, [r0]
    str r2, [r1]
    ldr r2, [r0, #4]
    str r2, [r1, #4]
    ldr r4, =0x01FF8001 - 1
    .macro PH n
    mov r0, #\n
    str r0, [r10]
    .endm
    .macro CALLI
    mov lr, pc
    bx r4
    str r0, [r11], #4
    .endm
    .macro CALLT
    ldr r0, =fT
    mov lr, pc
    bx r0
    str r0, [r11], #4
    .endm
    .macro ASK7
    mov r0, #1
    str r0, [r9]
2:  ldr r0, [r9]
    cmp r0, #2
    bne 2b
    ldr r0, [r9, #12]
    str r0, [r11], #4
    .endm
    PH 1
    bl fA
    str r0, [r11], #4
    CALLI
    CALLT
    ASK7
    @ 2: main-RAM store into fA: is the ITCM block retranslated? (static: main flush includes ITCM)
    PH 2
    ldr r0, =fA
    ldr r1, =0xE3A00011
    str r1, [r0]
    PH 3
    bl fA
    str r0, [r11], #4
    CALLI
    @ 4: stmia over fA with new values
    PH 4
    ldr r0, =fA
    ldr r1, =0xE3A00021
    ldr r2, =0xE12FFF1E
    stmia r0, {r1, r2}
    PH 5
    bl fA
    str r0, [r11], #4
    @ 6: stmia over fA with the same values (static: block stores have no same-value test)
    PH 6
    ldr r0, =fA
    stmia r0, {r1, r2}
    PH 7
    bl fA
    str r0, [r11], #4
    @ 8: strh into Thumb code fT (movs r0,#5 -> movs r0,#0x55)
    PH 8
    CALLT
    PH 81
    ldr r0, =fT
    bic r0, r0, #1
    ldr r1, =0x2055
    strh r1, [r0]
    PH 9
    CALLT
    @ 10: ARM9 stores into ARM7 code f7 (mov r0,#0x70 -> #0x71), then ARM7 calls it
    PH 10
    ldr r0, =0x02390000
    ldr r1, =0xE3A00071
    str r1, [r0]
    PH 11
    ASK7
    @ 12: unrelated ARM9 SMC store (forces a main flush), ARM7 calls f7 again
    PH 12
    bl fA
    PH 121
    ldr r0, =fA
    ldr r1, =0xE3A00031
    str r1, [r0]
    PH 13
    ASK7
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
    .org 0x1000
    .thumb
    .thumb_func
fT: movs r0, #5
    bx lr
    .arm
