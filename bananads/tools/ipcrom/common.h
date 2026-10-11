/* ipctest (build.py): registers and the result block. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define REG32(a) (*(volatile u32*)(a))
#define REG16(a) (*(volatile u16*)(a))
#define REG8(a) (*(volatile u8*)(a))
#define IPCSYNC REG16(0x04000180)
#define FIFOCNT REG16(0x04000184)
#define FIFOSEND REG32(0x04000188)
#define FIFORECV REG32(0x04100000)
#define IE REG32(0x04000210)
#define IF REG32(0x04000214)
#define VCOUNT REG16(0x04000006)
#define RES ((volatile u32*)0x02300000)   /* results: [0] round trips, [1] FIFO errors, [2] sync errors, [3] mailbox errors, [4] frames, [16+f] round trips in frame f */
#define MB ((volatile u32*)0x02310000)    /* main-RAM mailbox */
