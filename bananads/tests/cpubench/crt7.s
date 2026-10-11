@ cpubench ARM7 entry: system mode, stack in ARM7 WRAM, then main7 (busy variant) or a halt loop.
    .section .text.start, "ax"
    .arm
    .global _start7
_start7:
    msr     cpsr_c, #0xDF
    ldr     sp, =0x0380FF00
    ldr     r0, =main7
    mov     lr, pc
    bx      r0
1:  b       1b
    .pool
