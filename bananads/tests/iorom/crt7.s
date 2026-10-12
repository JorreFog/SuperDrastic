@ ARM7 entry for the STRESS_ARM7 workload: a stack in ARM7 WRAM, then main7 (never returns).
    .section .text.start, "ax"
    .arm
    .global _start7
_start7:
    ldr     sp, =0x0380FF00
    ldr     r0, =main7
    mov     lr, pc
    bx      r0
1:  b       1b
    .pool
