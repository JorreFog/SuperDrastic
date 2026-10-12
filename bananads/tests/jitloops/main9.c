/* jitloops ARM9: loop shapes for the JIT's loop blocks (patch 0007's LOOPREGS), with IME on and VBlank and
   timer 0 IRQs whose handler (irq.s) writes memory the loops poll. Each pass runs every loop and folds its result
   into a checksum; the top screen shows them every pass, so two JIT runs whose timing must be the same (LOOPREGS on
   and off) can be compared frame by frame, and the timing-free ones (row 0-7) against the interpreter at pass 8.
   Written for this test. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef int s32;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define ARM __attribute__((target("arm"), noinline))
#define NOINLINE __attribute__((noinline))
volatile u32 irq_counts[4];
extern void irq_handler(void);
static u32 buf[512];
static u8 bytes[1024];

/* 0. mid-loop exits, a table walk with byte and word loads */
static NOINLINE u32 l_midexit(u32 seed)
{
    u32 acc = seed, i = 0;
    for (;;) {
        u32 v = buf[i & 511];
        if (v == 0x12345) break;
        acc = acc * 33 + v;
        if ((acc & 0xFF) == 0x7F) break;
        i++;
        if (i > 300) break;
        acc ^= bytes[(acc >> 3) & 1023];
    }
    return acc + i;
}
/* 1. memcpy-like with stores, register pressure (8+ registers live) */
static NOINLINE u32 l_pressure(u32 seed)
{
    u32 a = seed, b = seed * 3, c = seed ^ 0x55, d = 7, e = 11, f = 13, g = 17, h = 19, k = 0;
    for (u32 i = 0; i < 200; i++) {
        a += b; b ^= c; c += d; d = d * 5 + e; e ^= f; f += g; g ^= h; h += a;
        buf[i & 511] = a ^ h; k += buf[(i * 7) & 511];
    }
    return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h ^ k;
}
/* 2. polls an IO register (VCOUNT): the access faults to the slow path the first time */
static NOINLINE u32 l_vcount(void)
{
    u32 n = 0, v0 = R16(0x04000006);
    while (R16(0x04000006) == v0 && n < 100000) n++;
    return 1 + (n != 0);
}
/* 3. polls memory the IRQ handler writes, while counting */
static NOINLINE u32 l_pollirq(void)
{
    u32 n = 0, c0 = irq_counts[1];
    while (irq_counts[1] == c0 && n < 2000000) n++;
    return irq_counts[1] - c0;
}
/* 4. stores into VRAM (slow path) and reads back */
static NOINLINE u32 l_vram(u32 seed)
{
    volatile u16 *p = (volatile u16 *)0x06890000;   /* VRAM E..? mapped as LCDC below */
    u32 s = 0;
    for (u32 i = 0; i < 256; i++) { p[i] = (u16)(seed + i * 3); s += p[(i * 5) & 255]; }
    return s;
}
/* 5. stores into its own code page: copied to main RAM and run there */
typedef u32 (*fn2)(u32 *, u32);
ARM u32 selfpage(u32 *data, u32 n)
{
    u32 s = 0;
    for (u32 i = 0; i < n; i++) { s += data[i]; data[i + 64] = s; }
    return s;
}
/* 6. IME-guarded loop: IRQs off inside, on outside, polling IF */
static NOINLINE u32 l_ime(void)
{
    u32 n = 0;
    R32(0x04000208) = 0;
    while (!(R32(0x04000214) & 8) && n < 1000000) n++;
    R32(0x04000208) = 1;
    return (n != 0) + 1;
}
ARM void intrwait(void) { __asm__ volatile("mov r0, #1\n mov r1, #1\n swi #0x40000" : : : "r0", "r1", "r2", "r3", "r12", "lr", "memory"); }
ARM u32 dtcm_base(void) { u32 d; __asm__ volatile("mrc p15, 0, %0, c9, c1, 0" : "=r"(d)); return d & ~0xFFFu; }
ARM void icache_flush(void) { __asm__ volatile("mov r0, #0\n mcr p15, 0, r0, c7, c5, 0\n mcr p15, 0, r0, c7, c14, 0" : : : "r0", "memory"); }
ARM void irqs_on(void) { __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" : : : "r0"); }
/* 7. a halt (IntrWait through the BIOS) inside a loop that also counts */
static NOINLINE u32 l_halt(void)
{
    u32 c0 = irq_counts[0], n = 0;
    while (irq_counts[0] == c0) {
        n++;
        intrwait();
    }
    return n != 0;
}
/* 9. geometry commands by single stores into the GX FIFO and command ports, polling GXSTAT */
static NOINLINE u32 l_gx(u32 seed)
{
    u32 n = 0;
    for (u32 i = 0; i < 64; i++) {
        R32(0x04000440) = 0;                            /* MTX_MODE */
        R32(0x04000454) = 0;                            /* MTX_IDENTITY */
        R32(0x04000400) = 0x15;                         /* FIFO: packed MTX_IDENTITY */
        R32(0x04000400) = 0;
        while ((R32(0x04000600) & (1u << 27)) && n < 100000) n++;   /* GXSTAT busy */
        n += R32(0x04000600) & 0x1FF;
    }
    return (n != 0) + (seed & 0);
}
/* 8. ARM loops: conditional execution, flags kept across a store */
ARM u32 l_armcond(u32 seed)
{
    u32 a = seed, b = 0, i = 0;
    do { if (a & 1) a = a * 3 + 1; else a >>= 1; b += a; bytes[i & 1023] = (u8)a; i++; } while (a != 1 && i < 500);
    return b + i;
}
static void draw(const u32 *v, int n)
{
    volatile u16 *fb = (volatile u16 *)0x06800000;
    for (int r = 0; r < n; r++)
        for (int b = 0; b < 32; b++) {
            u16 col = (v[r] >> b) & 1 ? 0x7FFF : (u16)(0x0010 + r * 0x0C00);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) fb[(8 + r * 6 + y) * 256 + 8 + b * 7 + x] = col;
        }
}
static u32 code[256];
int main(void)
{
    R32(0x04000304) = 0x820F;                      /* POWCNT1: LCDs, 2D A, 3D render and geometry, top screen */
    R8(0x04000240) = 0x80;                          /* VRAM A: LCDC */
    R8(0x04000244) = 0x80;                          /* VRAM E: LCDC (0x06880000) */
    R32(0x04000000) = 0x00020000;
    for (int i = 0; i < 512; i++) buf[i] = i * 2654435761u;
    for (int i = 0; i < 1024; i++) bytes[i] = (u8)(i * 7);
    for (int i = 0; i < 256; i++) code[i] = ((u32 *)selfpage)[i];
    icache_flush();
    fn2 volatile sp2 = (fn2)(u32)code;
    /* the IRQ handler for the BIOS, VBlank and timer 0 IRQs, IME on, IRQs on in the CPSR */
    u32 dtcm = dtcm_base();
    R32(dtcm + 0x3FFC) = (u32)irq_handler;
    R16(0x04000004) = 0x0008;                       /* DISPSTAT: VBlank IRQ */
    R16(0x04000100) = 0x8000; R16(0x04000102) = 0x00C1;   /* timer 0: /64, IRQ */
    R32(0x04000210) = 0x9;                          /* IE: VBlank, timer 0 */
    R32(0x04000208) = 1;
    irqs_on();
    static u32 sums[12], frozen[13];
    u32 pass = 0;
    for (;;) {
        pass++;
        sums[0] += l_midexit(pass);
        sums[1] += l_pressure(pass);
        sums[2] += l_vram(pass);
        sums[3] += sp2((u32 *)bytes, 40 + (pass & 15));
        sums[4] += l_armcond(pass * 7 + 27);
        sums[5] += l_vcount();
        sums[6] += l_ime();
        sums[7] += l_halt();
        sums[8] += l_pollirq();
        sums[2] += l_gx(pass);
        { u32 s0 = R16(0x04000180) >> 8 & 0xF; R16(0x04000180) = (u16)((pass & 0xF) << 8);   /* IPCSYNC to the ARM7 */
          sums[8] ^= s0 + R32(0x02300000) * 0 + R32(0x02300008) + R32(0x0230000C); }
        sums[9] = irq_counts[0]; sums[10] = irq_counts[1]; sums[11] = irq_counts[2];
        if (pass == 8) for (int i = 0; i < 8; i++) frozen[i + 1] = sums[i];
        frozen[0] = pass;
        for (int i = 8; i < 12; i++) frozen[i + 1] = sums[i];
        draw(frozen, 13);
    }
}
