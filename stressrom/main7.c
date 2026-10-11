/* ARM7 workload for BananaJIT's tests (build.py with STRESS_ARM7=arm or thumb): the ARM7 never halts and
 * keeps moving bytes, halfwords and words through its WRAM and main RAM. How far it gets in a frame shows in
 * RAM, so any difference in the ARM7's cycle count between two runs shows as different RAM. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define WRAM8  ((volatile u8*)0x03801000)
#define WRAM16 ((volatile u16*)0x03802000)
#define WRAM32 ((volatile u32*)0x03803000)
#define SHARED ((volatile u32*)0x02300000)   /* main RAM, clear of the ARM9's code, data and stack */

void main7(void) {
    u32 n = 0;
    for (;;) {
        u32 sum = 0;
        for (u32 i = 0; i < 256; i++) {
            WRAM8[i] = (u8)(i * 7 + n);
            WRAM16[i] = (u16)(WRAM16[i] + WRAM8[i]);
            WRAM32[i] = WRAM32[i] ^ ((u32)WRAM16[i] << (i & 15));
            sum += WRAM32[i] + (u32)(signed char)WRAM8[i];
            /* An ARM7 multiply so the jit.mul difftest gate exercises the ARM7 MUL path: WRAM32[i] spans the
             * full 32-bit range, so its leading sign/zero bits vary and all of the ARM7's per-high-byte MUL
             * cycle buckets fire across a frame. */
            sum += WRAM32[i] * (i | 0x10101);
        }
        SHARED[n & 255] += sum;
        SHARED[256] = ++n;
    }
}
