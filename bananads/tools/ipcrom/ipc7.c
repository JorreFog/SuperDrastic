/* ipctest's ARM7 (build.py): halts until the ARM9 calls through the FIFO or IPCSYNC, answers. */
#include "common.h"
void main7(void) {
    FIFOCNT = 0x8408;                 /* enable, clear send, IRQ on receive not empty */
    IPCSYNC = 0x4000;                 /* IRQ from the ARM9's sync */
    IE = (1 << 16) | (1 << 18);
    u32 last = 0;
    for (;;) {
        while (!(IE & IF)) REG8(0x04000301) = 0x80;   /* halt until the ARM9 calls */
        IF = IF;
        while (!(FIFOCNT & 0x0100)) {
            u32 v = FIFORECV;
            FIFOSEND = v * 3 + 1;
            /* then follow the main-RAM mailbox for this round */
            for (u32 n = 0; MB[0] == last && n < 2000000; n++) {}
            last = MB[0];
            MB[1] = last;
        }
        IPCSYNC = 0x4000 | ((IPCSYNC & 15) << 8);
    }
}
