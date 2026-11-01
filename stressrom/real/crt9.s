@ dsreal ARM9 entry. The config block at offset 4 is patched by build.py.
    .section .text.start, "ax"
    .arm
    .global _start, cfg
_start:
    b       1f
cfg:
    .word   0x4C414552          @ magic "REAL"
    .word   0                   @ mode: 0 = every scene in turn (loops), 1 = one scene (cfg.scene) forever
    .word   0                   @ scene (mode 1)
    .word   5                   @ level 1..10 (5 = realistic)
    .word   0                   @ frames per scene override (0 = the phase table's)
    .word   0                   @ flags
1:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]    @ IME off
    @ CP15: DTCM 16 KB at 0x0B000000, ITCM at 0 (32 MB mirror), both on, high vectors kept
    ldr     r0, =0x0B00000A
    mcr     p15, 0, r0, c9, c1, 0
    mov     r0, #0x20
    mcr     p15, 0, r0, c9, c1, 1
    @ protection regions beside the direct boot's (as libnds's crt0): 2 = ITCM (0, 32 MB), 5 = DTCM (16 KB)
    ldr     r0, =0x00000031
    mcr     p15, 0, r0, c6, c2, 0
    ldr     r0, =0x0B00001B
    mcr     p15, 0, r0, c6, c5, 0
    ldr     r0, =0x15111111     @ data access: regions 0-5, 7 privileged RW, 6 (BIOS) read-only
    mcr     p15, 0, r0, c5, c0, 2
    ldr     r0, =0x05100111     @ code: IO, main RAM, ITCM, DTCM, BIOS
    mcr     p15, 0, r0, c5, c0, 3
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x50000    @ DTCM enable (16), ITCM enable (18)
    orr     r0, r0, #0x1000     @ instruction cache (12), as a game's crt0 (the direct boot's regions: main RAM cachable)
    orr     r0, r0, #0x5        @ protection unit (0), data cache (2)
    mcr     p15, 0, r0, c1, c0, 0
    @ stacks in DTCM: IRQ mode, then system mode
    msr     cpsr_c, #0xD2
    ldr     sp, =0x0B003F00
    msr     cpsr_c, #0xDF
    ldr     sp, =0x0B003D00
    @ .itcm: copy from its load address
    ldr     r0, =__itcm_lma
    ldr     r1, =__itcm_start
    ldr     r2, =__itcm_end
2:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     2b
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
3:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     3b
    ldr     r0, =main
    blx     r0
4:  b       4b
    .pool

    .global cpu_halt, irq_on, irq_off
    .type cpu_halt, %function
    .type irq_on, %function
    .type irq_off, %function
cpu_halt:                       @ wait for interrupt (CP15), as the BIOS's IntrWait does
    mov     r0, #0
    mcr     p15, 0, r0, c7, c0, 4
    bx      lr
irq_on:
    mrs     r0, cpsr
    bic     r0, r0, #0x80
    msr     cpsr_c, r0
    bx      lr
irq_off:
    mrs     r0, cpsr
    orr     r0, r0, #0x80
    msr     cpsr_c, r0
    bx      lr
