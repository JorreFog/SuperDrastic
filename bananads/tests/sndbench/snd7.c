/* sndbench's ARM7 (build.py): a game's sound driver, roughly. All 16 channels: PCM8, PCM16 and IMA-ADPCM samples
 * (looping and one-shot, one streamed from ARM7 WRAM, one from the BIOS area that reads as zeros), PSG at every
 * duty, both noise channels; notes keyed on and off, volume envelopes written as bytes every frame, pans, dividers
 * and hold; SOUNDCNT's output selectors and "don't mix ch1/ch3" bits changed now and then; both capture units
 * (PCM16 looping from the left mixer, PCM8 one-shot restarted from the right mixer) writing to main RAM, which the
 * ARM9 folds into checksums on the screen. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef signed short s16;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define VCOUNT R16(0x04000006)
#define CH_CNT(n) R32(0x04000400 + (n) * 16)
#define CH_VOL(n) R8(0x04000400 + (n) * 16)
#define CH_PAN(n) R8(0x04000402 + (n) * 16)
#define CH_SAD(n) R32(0x04000404 + (n) * 16)
#define CH_TMR(n) R16(0x04000408 + (n) * 16)
#define CH_PNT(n) R16(0x0400040A + (n) * 16)
#define CH_LEN(n) R32(0x0400040C + (n) * 16)
#define SAMPLES 0x02340000                     /* 16 x 4 KB of sample data */
#define WRAMSMP 0x03808000                     /* 4 KB in ARM7 WRAM, for channel 7 */
#define CAPTURE 0x02300000                     /* capture 0 at +0 (16 KB), capture 1 at +0x8000 (4 KB) */

static u32 rnd = 4321;
static u32 next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }
static void wait_vblank(void) { while (VCOUNT >= 192) {} while (VCOUNT < 192) {} }
static u8 env[16];

/* format by channel: 0,3,6 PCM8; 1,4,7 PCM16; 2,5 ADPCM; 8-13 PSG; 14-15 noise */
static void note(u32 ch, u32 seed) {
    u32 fmt = ch < 8 ? (ch == 7 ? 1 : ch % 3) : 3;
    u32 vol = (seed * 29 + ch * 11) % 128, pan = (ch * 23 + seed * 7) & 127;
    u32 rep = (seed + ch) % 3 == 0 ? 2 : 1;                   /* one-shot now and then */
    u32 div = (seed + ch) & 3, hold = ((seed >> 2) + ch) % 5 == 0;
    if (ch == 12) pan = 127;
    if (ch == 13) vol = 127;
    CH_CNT(ch) = 0;
    CH_SAD(ch) = ch == 7 ? WRAMSMP : ch == 6 && (seed & 1) ? 0x00000100 : SAMPLES + ch * 0x1000;
    /* 11-44 kHz for samples, PSG and noise higher */
    CH_TMR(ch) = (u16)(0x10000 - (ch < 8 ? 380 + ((seed * 3 + ch * 41) % 1100) : 200 + ch * 23 + (seed & 127)));
    CH_PNT(ch) = ch == 2 || ch == 5 ? 1 + (seed & 3) : (seed + ch) & 7;
    CH_LEN(ch) = 0x80 + ((seed * 5 + ch * 13) & 0x1FF);
    env[ch] = (u8)vol;
    CH_CNT(ch) = vol | div << 8 | hold << 15 | pan << 16 | ((ch + seed) & 7) << 24 | rep << 27 | fmt << 29 | 1u << 31;
}

void main7(void) {
    for (u32 i = 0; i < 0x10000; i += 4) {                    /* PCM8/16: a few sines and noise, as bytes */
        u32 v = next();
        R32(SAMPLES + i) = v ^ (i * 0x01030507u);
    }
    for (u32 ch = 0; ch < 16; ch++)                           /* ADPCM headers: value, index */
        R32(SAMPLES + ch * 0x1000) = (next() & 0xFFFF) | (next() % 89) << 16;
    for (u32 i = 0; i < 0x1000; i += 4) R32(WRAMSMP + i) = next() * 0x9E3779B1u;
    R16(0x04000304) = 1;                                      /* POWCNT2: speakers on */
    R16(0x04000500) = 0x807F;                                 /* SOUNDCNT: on, master volume 127 */
    R16(0x04000504) = 0x200;
    for (u32 ch = 0; ch < 16; ch++) note(ch, ch);
    R32(0x04000510) = CAPTURE; R16(0x04000514) = 0x1000;   /* lengths in words */
    R32(0x04000518) = CAPTURE + 0x8000; R16(0x0400051C) = 0x400;
    R8(0x04000508) = 0x80;                                    /* capture 0: PCM16, looping */
    R8(0x04000509) = 0x80 | 0x08 | 0x04;                      /* capture 1: PCM8, one-shot */
    for (u32 frame = 1;; frame++) {
        wait_vblank();
        for (u32 ch = 0; ch < 16; ch++) {                     /* envelopes: decay, written as bytes */
            if (env[ch] > 2) env[ch] -= 1 + (ch & 1);
            CH_VOL(ch) = env[ch];
            if ((frame + ch) % 9 == 0) CH_PAN(ch) = (u8)((frame * 3 + ch * 19) & 127);
        }
        if (frame % 3 == 0) note(next() & 15, frame);         /* key a note */
        if (frame % 5 == 0) note(8 + (next() & 7), frame + 1);
        if (frame % 11 == 0) CH_CNT(next() & 15) &= 0x7FFFFFFF;  /* key one off */
        if (frame % 17 == 0) R16(0x04000500) = 0x8000 | (0x60 + (frame & 31)) | ((frame >> 4) & 0xF) << 8 | ((frame >> 3) & 3) << 12;
        if (frame % 40 == 0) R16(0x04000500) = 0x807F;
        if (!(R8(0x04000509) & 0x80)) R8(0x04000509) = 0x80 | 0x08 | 0x04;  /* restart the one-shot capture */
        if (frame % 23 == 0) R8(0x04000508) = 0;              /* capture 0 stopped and restarted */
        if (frame % 23 == 1) R8(0x04000508) = 0x80;
    }
}
