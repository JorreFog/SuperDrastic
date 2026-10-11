/* ipctest's ARM9 (build.py): FIFO, mailbox and IPCSYNC round trips with the ARM7, counted in main RAM. */
#include "common.h"
static u32 work[256];
void main(void) {
    FIFOCNT = 0x8008;
    u32 i = 1, frame = 0, inframe = 0, n; u16 lastv = 0;
    for (;;) {
        /* FIFO ping-pong: the ARM7 halts until the FIFO wakes it, answers 3v+1 */
        FIFOSEND = i;
        while (FIFOCNT & 0x0100) ;
        if (FIFORECV != i * 3 + 1) RES[1]++;
        /* main-RAM mailbox: the ARM7 copies MB[0] to MB[1] (no IPC register touched here) */
        MB[0] = i;
        n = 0;
        while (MB[1] != i) if (++n > 1000000) { RES[3]++; break; }
        /* IPCSYNC: ours out with an IRQ to the ARM7, which mirrors it back */
        IPCSYNC = ((i & 15) << 8) | 0x2000;
        n = 0;
        while ((IPCSYNC & 15) != (i & 15)) if (++n > 1000000) { RES[2]++; break; }
        RES[0]++; inframe++;
        /* quiet work: no IPC for a while */
        for (u32 k = 0; k < 40; k++) work[k & 255] = work[(k * 7) & 255] * 3 + k + i;
        u16 v = VCOUNT;
        if (v < lastv) { RES[16 + (frame & 127)] = inframe; inframe = 0; frame++; RES[4] = frame; }
        lastv = v; i++;
    }
}
