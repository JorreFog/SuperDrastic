@ Round-3 ARM9 tests. Select with -defsym: MOVPC MOVPCEQ MOVPCNE THMOVPC BKPT THUND QD STRDSMC SWPSMC MSRC
@ Exception record at 0x02200080: +0 und lr, +4 und count, +8 abt lr, +12 abt count, +16 und spsr, +20 abt spsr
@ Results at 0x02200100 (r11), phase at 0x02200008, DONE at 0x02200000.
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
    ldr r0, =0x02200080
    mov r1, #0
    mov r2, #0
1:  str r1, [r0, r2]
    add r2, r2, #4
    cmp r2, #32
    blt 1b
    @ low vectors in ITCM: 0x04 / 0x0c = ldr pc,[pc,#0x18] -> handler address at 0x24 / 0x2c
    mov r0, #0
    ldr r1, =0xE59FF018
    str r1, [r0, #4]
    str r1, [r0, #0xc]
    ldr r1, =und_h
    str r1, [r0, #0x24]
    ldr r1, =abt_h
    str r1, [r0, #0x2c]
    str r1, [r0, #0x8]               @ placeholder, SWIV overwrites the SWI vector below
    ldr r1, =0xE59FF018
    str r1, [r0, #0x8]
    ldr r1, =swi_h
    str r1, [r0, #0x28]
    mrc p15, 0, r0, c1, c0, 0
    bic r0, r0, #0x2000
    mcr p15, 0, r0, c1, c0, 0
    mrc p15, 0, r1, c1, c0, 0       @ read back: bit 13 should now be 0
    str r1, [r11], #4
    .macro PH n
    mov r0, #\n
    str r0, [r10]
    .endm
    .macro ST r
    str \r, [r11], #4
    .endm
    .macro EXC                     @ store und count, abt count
    ldr r12, =0x02200080
    ldr r9, [r12, #4]
    ST r9
    ldr r9, [r12, #12]
    ST r9
    .endm
.ifdef BASE
    @ sanity: ARM permanently-undefined instruction -> UND handler, returns after it
    PH 1
    mov r0, #0
    .word 0xE7F000F0
    add r0, r0, #1
    ST r0
    EXC
.endif
.ifdef MOVPC
    PH 10
    mov r0, #0
    mov pc, pc
    add r0, r0, #1
    add r0, r0, #2
    ST r0                           @ spec 2
.endif
.ifdef MOVPCEQ
    PH 11
    movs r0, #0                     @ Z=1
    moveq pc, pc
    add r0, r0, #1
    add r0, r0, #2
    ST r0                           @ spec 2 (taken)
.endif
.ifdef MOVPCNE
    PH 12
    movs r0, #0                     @ Z=1
    movne pc, pc
    add r0, r0, #1
    add r0, r0, #2
    ST r0                           @ spec 3 (not taken)
.endif
.ifdef THMOVPC
    PH 13
    mov r0, #0
    ldr r12, =th_movpc
    mov lr, pc
    bx r12
    ST r0                           @ spec 2
.endif
.ifdef BKPT
    PH 20
    @ ARM bkpt: prefetch abort, lr_abt = insn+4
    mov r0, #0
bk_a: bkpt #0
    add r0, r0, #1
    ST r0
    EXC
    ldr r12, =0x02200080
    ldr r1, [r12, #8]
    ldr r2, =bk_a
    sub r1, r1, r2
    ST r1                           @ spec 4
    ldr r1, [r12, #20]
    ST r1                           @ abt spsr
    @ Thumb bkpt 0xbe03 with two words pushed: spec abort, regs/sp untouched; static: pop {r0,r1}
    mov r6, sp
    ldr r1, =0x1111
    ldr r2, =0x2222
    push {r1, r2}
    mov r0, #0
    mov r1, #0
    ldr r12, =th_bkpt
    mov lr, pc
    bx r12
    ST r0
    ST r1
    sub r2, r6, sp
    ST r2                           @ spec 8 (still pushed)
    mov sp, r6
    EXC
    ldr r12, =0x02200080
    ldr r1, [r12, #8]
    ldr r2, =th_bkpt
    bic r2, r2, #1
    sub r1, r1, r2
    ST r1                           @ abt lr - bkpt addr (spec 4)
.endif
.ifdef THBKPT
    PH 21
    mov r6, sp
    ldr r1, =0x1111
    ldr r2, =0x2222
    push {r1, r2}
    mov r0, #0
    mov r1, #0
    ldr r12, =th_bkpt
    mov lr, pc
    bx r12
    ST r0
    ST r1
    sub r2, r6, sp
    ST r2                           @ spec 8 (still pushed)
    mov sp, r6
    EXC
.endif
.ifdef STRD2
    PH 52
    bl fA
    mov r5, #0
    ldr r4, =fA - 4
    ldr r2, =0x5555AAAA
    ldr r3, =0xE3A00043
    add r5, r5, #1
    strd r2, r3, [r4]               @ only the second store hits code
    add r5, r5, #0x10
    ST r5
    bl fA
    ST r0                           @ spec 0x43
.endif
.ifdef THUND
    PH 30
    mov r0, #0
    ldr r12, =th_und
    mov lr, pc
    bx r12
    ST r0                           @ spec 7 (UND, return to next); static: 4 (branch)
    EXC
.endif
.ifdef QD
    PH 40
    .macro QT op, a, b
    mrs r10, cpsr
    bic r10, r10, #0xF8000000
    msr cpsr_fc, r10
    ldr r1, =\a
    ldr r2, =\b
    \op r0, r1, r2
    ST r0
    mrs r9, cpsr
    lsr r9, r9, #27
    ST r9
    .endm
    QT qdadd, 0, 0x80000000
    QT qdadd, 0x10, 0xA0000000
    QT qdadd, 0, 0xC0000000
    QT qdadd, 5, 0xBFFFFFFF
    QT qdsub, 0, 0x80000000
    QT qdsub, 0x10, 0xA0000000
    ldr r10, =0x02200008
.endif
.ifdef STRDSMC
    PH 50
    bl fA
    mov r5, #0
    ldr r4, =fA
    ldr r2, =0xE3A00042
    ldr r3, =0xE12FFF1E
    add r5, r5, #1
    strd r2, r3, [r4]               @ first store hits translated code
    add r5, r5, #0x10
    ST r5                           @ spec 0x11
    bl fA
    ST r0                           @ spec 0x42
    PH 51
    bl fA
    mov r5, #0
    ldr r4, =fA - 4
    ldr r2, =0x5555AAAA
    ldr r3, =0xE3A00043
    add r5, r5, #1
    strd r2, r3, [r4]               @ only the second store hits code
    add r5, r5, #0x10
    ST r5
    bl fA
    ST r0                           @ spec 0x43
.endif
.ifdef SWPSMC
    PH 60
    bl fA
    ldr r4, =fA
    ldr r1, =0xE3A00044
    swp r4, r1, [r4]                @ Rd == Rn, store hits code
    ST r4                           @ spec old word of fA (e3a00001)
    bl fA
    ST r0                           @ spec 0x44
    PH 61
    bl fA
    ldr r4, =fA
    ldr r1, =0xE3A00045
    swp r1, r1, [r4]                @ Rd == Rm
    ST r1                           @ spec e3a00044
    bl fA
    ST r0                           @ spec 0x45
.endif
.ifdef SWIV
    @ (dyn_patch) SWI after clearing CP15 control V: must vector to 0x00000008 (ITCM) -> swi_h, not 0xffff0008 (BIOS)
    PH 80
    mov r0, #1
sw_a: swi #0x30000
    ldr r12, =0x02200080
    ldr r1, [r12, #24]
    ST r1                           @ spec 1 (handler ran once)
    ldr r1, [r12, #28]
    ldr r2, =sw_a
    sub r1, r1, r2
    ST r1                           @ spec 4 (lr_svc = swi+4)
.endif
.ifdef MSRC
    PH 70
    mrs r0, cpsr
    bic r0, r0, #0xF8000000
    msr cpsr_fc, r0
    orr r0, r0, #0x08000000
    msr cpsr_c, r0                  @ bit 27 set in Rm
    mrs r1, cpsr
    lsr r1, r1, #27
    ST r1                           @ spec 0 (Q unchanged)
    ldr r1, =0x7FFFFFFF
    mov r2, #1
    qadd r0, r1, r2                 @ sets Q
    mrs r0, cpsr
    bic r0, r0, #0xF8000000
    msr cpsr_c, r0                  @ bit 27 clear in Rm
    mrs r1, cpsr
    lsr r1, r1, #27
    ST r1                           @ spec 1 (Q kept)
    mrs r0, cpsr
    bic r0, r0, #0xF8000000
    msr cpsr_fc, r0
    orr r0, r0, #0xF0000000
    msr cpsr_c, r0                  @ NZCV bits set in Rm
    mrs r1, cpsr
    lsr r1, r1, #27
    ST r1                           @ spec 0
    mrs r0, cpsr
    bic r0, r0, #0xF8000000
    msr cpsr_fc, r0
    orr r0, r0, #0x08000000
    msr cpsr_c, r0
    mrs r0, cpsr                    @ second read after an unrelated flag op
    adds r1, r0, #0
    mrs r1, cpsr
    lsr r1, r1, #27
    ST r1
.endif
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
und_h:
    ldr sp, =0x02200080
    str lr, [sp]
    ldr lr, [sp, #4]
    add lr, lr, #1
    str lr, [sp, #4]
    mrs lr, spsr
    str lr, [sp, #16]
    ldr lr, [sp]
    movs pc, lr
swi_h:
    ldr sp, =0x02200080
    str lr, [sp, #28]
    ldr lr, [sp, #24]
    add lr, lr, #1
    str lr, [sp, #24]
    ldr lr, [sp, #28]
    movs pc, lr
abt_h:
    ldr sp, =0x02200080
    str lr, [sp, #8]
    ldr lr, [sp, #12]
    add lr, lr, #1
    str lr, [sp, #12]
    mrs lr, spsr
    str lr, [sp, #20]
    tst lr, #0x20
    ldr lr, [sp, #8]
    subne lr, lr, #2               @ Thumb: lr_abt = bkpt+4 -> resume at bkpt+2
    movs pc, lr
    .ltorg
    .thumb
    .balign 4
    .thumb_func
th_movpc:
    .hword 0x46ff                   @ mov pc, pc  (A: pc = A+4)
    adds r0, #1
    adds r0, #2
    bx lr
    .balign 4
    .thumb_func
th_bkpt:
    .hword 0xbe03                   @ bkpt #3
    bx lr
    .balign 4
    .thumb_func
th_und:
    .hword 0xde01                   @ undefined-cond B (spec: UND)
    adds r0, #1
    adds r0, #2
    adds r0, #4
    bx lr
    .arm
    .balign 0x800
    .word 0
fA: mov r0, #1
    bx lr
