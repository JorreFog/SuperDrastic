/* jitloops ARM7: loops polling IPCSYNC (written by the ARM9) and memory its timer IRQ handler writes, with IME
   on, a loop inside the IRQ handler (IRQ mode, banked SP/LR), and a mailbox in main RAM the ARM9 reads. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define ARM __attribute__((target("arm"), noinline))
volatile u32 tick7, hsum;
static u32 tab[64];
static volatile u32 *const box = (volatile u32 *)0x02300000;
ARM void irq7(void)
{
    u32 f = R32(0x04000214) & R32(0x04000210);
    R32(0x04000214) = f;
    u32 s = hsum;
    for (int i = 0; i < 16; i++) { s = s * 31 + tab[(s + i) & 63]; if (s == 0x55) break; }   /* a loop in IRQ mode */
    hsum = s; tick7++;
    *(volatile u32 *)0x0380FFF8 |= f;
}
ARM void irqs7_on(void) { __asm__ volatile("mrs r0, cpsr\n bic r0, r0, #0x80\n msr cpsr_c, r0" : : : "r0"); }
int main7(void)
{
    for (int i = 0; i < 64; i++) tab[i] = i * 0x9E3779B9u;
    *(volatile u32 *)0x0380FFFC = (u32)irq7;
    R16(0x04000104) = 0x8000; R16(0x04000106) = 0x00C1;      /* timer 1: /64, IRQ */
    R32(0x04000210) = 0x10; R32(0x04000208) = 1; irqs7_on();
    u32 n = 0, acc = 0;
    for (;;) {
        u32 s0 = R16(0x04000180) & 0xF, t0 = tick7;
        while ((R16(0x04000180) & 0xF) == s0) { n++; acc += tab[n & 63] ^ tick7; }     /* IPCSYNC from the ARM9 */
        R16(0x04000180) = (u16)(((s0 + 1) & 0xF) << 8);
        while (tick7 == t0) n++;                                                       /* the IRQ handler's */
        box[0] = n; box[1] = acc; box[2] = tick7; box[3] = hsum;
    }
}
