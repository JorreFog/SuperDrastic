/* dmasnd's ARM7 (build.py): all 16 sound channels in every format (two left off), keyed on and off now and then,
 * and both capture units writing the mixers' output to main RAM, so a difference in the mixing shows as different
 * RAM (bananads-difftest). */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define VCOUNT R16(0x04000006)
#define CH_CNT(n) R32(0x04000400 + (n) * 16)
#define CH_SAD(n) R32(0x04000404 + (n) * 16)
#define CH_TMR(n) R16(0x04000408 + (n) * 16)
#define CH_PNT(n) R16(0x0400040A + (n) * 16)
#define CH_LEN(n) R32(0x0400040C + (n) * 16)
#define SAMPLES ((volatile u8 *)0x02340000)   /* 64 KB of generated sample data */
#define CAPTURE 0x02300000                     /* capture 0 at +0, capture 1 at +0x8000 */

static u32 rnd = 777;
static u32 next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
static void wait_vblank(void) { while (VCOUNT >= 192) {} while (VCOUNT < 192) {} }

static void start(u32 ch, u32 seed) {
    /* format by channel: PCM8, PCM16, ADPCM, ...; 8-13 PSG, 14-15 noise; 5 and 9 stay off */
    if (ch == 5 || ch == 9) return;
    u32 fmt = ch < 8 ? ch % 3 : 3;
    u32 vol = 40 + (seed * 13 + ch * 7) % 88, pan = (ch * 17 + seed * 5) & 127;
    u32 rep = (seed + ch) % 4 == 0 ? 2 : 1;                   /* sometimes one-shot */
    CH_CNT(ch) = 0;
    CH_SAD(ch) = 0x02340000 + ch * 0x1000;
    CH_TMR(ch) = (u16)(0x10000 - (ch < 8 ? 380 + ((seed + ch) & 255) : 600 + ch * 37 + (seed & 63)));
    CH_PNT(ch) = 4;
    CH_LEN(ch) = 0x200 + ((seed + ch) & 0xFF);
    CH_CNT(ch) = vol | ((ch & 3) << 8) | pan << 16 | ((ch + seed) & 7) << 24 | rep << 27 | fmt << 29 | 1u << 31;
}

void main7(void) {
    for (u32 i = 0; i < 0x10000; i++) SAMPLES[i] = (u8)next();
    for (u32 ch = 0; ch < 16; ch++) {                         /* ADPCM headers: value, index */
        R32(0x02340000 + ch * 0x1000) = (next() & 0x7FFF) | (next() % 89) << 16;
    }
    R16(0x04000304) = 1;                                      /* POWCNT2: speakers on */
    R16(0x04000500) = 0x807F;                                 /* SOUNDCNT: on, master volume 127 */
    R16(0x04000504) = 0x200;
    for (u32 ch = 0; ch < 16; ch++) start(ch, 0);
    /* capture 0: left mixer, capture 1: right mixer, PCM16, looping, timers of channels 1 and 3 */
    R32(0x04000510) = CAPTURE; R16(0x04000514) = 0x1000;
    R32(0x04000518) = CAPTURE + 0x8000; R16(0x0400051C) = 0x1000;
    R8(0x04000508) = 0x80; R8(0x04000509) = 0x80;
    for (u32 frame = 1;; frame++) {
        wait_vblank();
        if (frame % 7 == 0) start(next() & 15, frame);       /* key a channel on again */
        if (frame % 11 == 0) CH_CNT(next() & 7) &= 0x7FFFFFFF;    /* or off */
        if (frame % 13 == 0) R16(0x04000500) = 0x8000 | (frame & 127);
    }
}
