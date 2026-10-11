/* membench ARM7: the IPC receiver, as a sound or touch driver's: halts until the ARM9's words wake it (the
   receive FIFO's not-empty IRQ, IRQs masked in the CPSR), drains the FIFO, and replies to every batch of 16 with
   a hash of the words and the ARM9's sync nibble. Written for this benchmark. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define IRQ_IPCRECV (1u << 18)

int main7(void)
{
    R16(0x04000184) = 0x8408;                       /* IPCFIFOCNT: on, receive-not-empty IRQ, send FIFO cleared */
    R32(0x04000210) = IRQ_IPCRECV;                  /* IE */
    R32(0x04000214) = ~0u;                          /* IF */
    R32(0x04000208) = 1;                            /* IME */
    u32 sum = 0x811C9DC5, n = 0;
    for (;;) {
        while (R16(0x04000184) & 0x100) {          /* receive FIFO empty: halt until it isn't */
            R8(0x04000301) = 0x80;
            R32(0x04000214) = IRQ_IPCRECV;
        }
        sum = (sum ^ R32(0x04100000)) * 0x01000193;
        if (++n == 16) {
            R32(0x04000188) = sum + (R16(0x04000180) & 15);
            sum = 0x811C9DC5; n = 0;
        }
    }
}
