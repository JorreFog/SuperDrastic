@ jitloops ARM9 entry (cpubench's): system mode, stack in main RAM, .bss cleared, the protection unit and both caches on (as a
@ game's startup code does; melonDS's direct boot leaves them off, and uncached code in main RAM runs several times
@ slower), then main (Thumb or ARM). The regions are direct boot's (main RAM cacheable and write-buffered), plus
@ region 2 for ITCM (0x00000000, 32 MB with its mirrors), read/write, as games set it.
    .section .text.start, "ax"
    .arm
    .global _start
_start:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]    @ IME off
    msr     cpsr_c, #0xDF       @ system mode, IRQ/FIQ masked
    mov     r1, #0
    mcr     p15, 0, r1, c7, c5, 0   @ invalidate the instruction cache
    mcr     p15, 0, r1, c7, c6, 0   @ invalidate the data cache
    ldr     r1, =0x00000031
    mcr     p15, 0, r1, c6, c2, 0   @ region 2: ITCM
    mrc     p15, 0, r1, c5, c0, 2
    bic     r1, r1, #0xF00
    orr     r1, r1, #0x100
    mcr     p15, 0, r1, c5, c0, 2   @ its data access: read/write (privileged)
    mrc     p15, 0, r1, c5, c0, 3
    bic     r1, r1, #0xF00
    orr     r1, r1, #0x100
    mcr     p15, 0, r1, c5, c0, 3   @ its instruction access
    mrc     p15, 0, r1, c1, c0, 0
    orr     r1, r1, #0x1000         @ instruction cache
    orr     r1, r1, #0x5            @ protection unit, data cache
    mcr     p15, 0, r1, c1, c0, 0
    ldr     sp, =0x023F0000
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
2:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b
    ldr     r0, =main
    blx     r0
3:  b       3b
    .pool
