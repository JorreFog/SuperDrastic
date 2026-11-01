/* dsreal ARM7: a game-like sound driver and input poll, IRQ driven, halted between IRQs.
 *  - sequencer on timer 0 at ~192 Hz (NitroSDK's sound driver ticks at ~5.2 ms): 12 channels (2-13) of PCM16
 *    instruments and PSG squares with volume envelopes, 2 noise drum channels (14, 15), notes from patterns
 *  - a PCM16 stereo stream on channels 0/1 looping over the ARM9's ring (2 halves); timer 1 fires at each half's
 *    end and asks the ARM9 (IPC FIFO) to refill it, as games' strm players do
 *  - vblank: touchscreen through SPI (3 transfers, like libnds's touchRead), KEYXY, sent to the ARM9 over the FIFO
 *  - the ARM9's commands over the FIFO: sample bank, stream start/stop, sound effects, song reset, state hash */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef int s32;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a)  (*(volatile u8 *)(a))
#define IME R32(0x04000208)
#define IE  R32(0x04000210)
#define IF  R32(0x04000214)
#define DISPSTAT R16(0x04000004)
#define IPCSYNC R16(0x04000180)
#define IPCFIFOCNT R16(0x04000184)
#define IPCFIFOSEND R32(0x04000188)
#define IPCFIFORECV R32(0x04100000)
#define SPICNT R16(0x040001C0)
#define SPIDATA R16(0x040001C2)
#define KEYXY R16(0x04000136)
#define POWCNT2 R16(0x04000304)
#define SOUNDCNT R16(0x04000500)
#define SOUNDBIAS R16(0x04000504)
#define SCNT(c) R32(0x04000400 + (c) * 16)
#define SVOL(c) R8(0x04000400 + (c) * 16)
#define SPAN(c) R8(0x04000402 + (c) * 16)
#define SSAD(c) R32(0x04000404 + (c) * 16)
#define STMR(c) R16(0x04000408 + (c) * 16)
#define SPNT(c) R16(0x0400040A + (c) * 16)
#define SLEN(c) R32(0x0400040C + (c) * 16)
#define TMCNT_L(t) R16(0x04000100 + (t) * 4)
#define TMCNT_H(t) R16(0x04000102 + (t) * 4)
#define IRQHANDLER R32(0x0380FFFC)

void halt7(void); void irq_on7(void);

static volatile u32 pend;              /* IRQ -> main loop: bit 0 vblank, 1 seq tick, 2 stream half, 3 fifo */
static volatile u32 ticks_pending;
static u32 bank;                       /* sample bank address (ARM9 main RAM) */
static u32 stream_buf, stream_half;
static u32 seq_rng = 1, seq_tick, song, notes;
static u8 vol[16], rel[16], hold[16];

/* sample bank layout (built by the ARM9, see main9.c snd_bank): */
#define B_SINE   0x0000     /* 32 samples PCM16, one cycle */
#define B_SAW    0x0040     /* 32 samples PCM16 */
#define B_PLUCK  0x0080     /* 8192 samples PCM16, one-shot */
#define B_PAD    0x4080     /* 64 samples PCM8, one cycle */

static const u16 scale_hz[16] = { 131, 147, 165, 175, 196, 220, 247, 262, 294, 330, 349, 392, 440, 494, 523, 587 };

static u32 rnd(void) { seq_rng = seq_rng * 1103515245u + 12345u; return seq_rng >> 16; }

static void ch_start(int c, u32 sad, u32 tmr, u32 pnt, u32 len, u32 fmt_rep, u32 v, u32 pan) {
    SCNT(c) = 0;
    SSAD(c) = sad; STMR(c) = (u16)tmr; SPNT(c) = (u16)pnt; SLEN(c) = len;
    SCNT(c) = v | (pan << 16) | fmt_rep | 0x80000000u;
    vol[c] = (u8)v;
}

#define F_PCM8 (0u << 29)
#define F_PCM16 (1u << 29)
#define F_PSG (3u << 29)
#define LOOP (1u << 27)
#define ONESHOT (2u << 27)

static void note(int c, u32 hz, u32 v, u32 pan, u32 r) {
    rel[c] = (u8)r; notes++;
    if (c >= 8 && c <= 13)                               /* PSG square, duty from the channel */
        ch_start(c, 0, 0x10000 - 16756991 / (8 * hz), 0, 0, F_PSG | ((u32)(c & 7) << 24), v, pan);
    else if (c == 14 || c == 15)                         /* noise drum */
        ch_start(c, 0, 0x10000 - 16756991 / (hz * 64), 0, 0, F_PSG, v, pan);
    else if (c == 2 || c == 3)                           /* bass: saw cycle */
        ch_start(c, bank + B_SAW, 0x10000 - 16756991 / (32 * hz / 2), 0, 16, F_PCM16 | LOOP, v, pan);
    else if (c == 4 || c == 5)                           /* lead: sine cycle */
        ch_start(c, bank + B_SINE, 0x10000 - 16756991 / (32 * hz), 0, 16, F_PCM16 | LOOP, v, pan);
    else                                                 /* 6, 7: pluck one-shot, pitch from the rate */
        ch_start(c, bank + B_PLUCK, 0x10000 - 16756991 / (hz * 40), 0, 4096, F_PCM16 | ONESHOT, v, pan);
}

static void seq_step(void) {
    /* envelopes every tick: games write the volume byte */
    for (int c = 2; c < 16; c++)
        if (vol[c]) { u32 v = vol[c] > rel[c] ? vol[c] - rel[c] : 0; vol[c] = (u8)v; SVOL(c) = (u8)v; }
    u32 t = seq_tick++;
    u32 sub = t % 24;                                    /* 16th notes at 120 bpm (96 ticks a beat) */
    if (sub) return;
    u32 step = t / 24;
    /* drums */
    if ((step & 3) == 0) note(14, 60, 70, 64, 6);
    if ((step & 3) == 2) note(15, 220, 40, 64, 10);
    /* bass on 8ths */
    if ((step & 1) == 0) note(2 + (step >> 1 & 1), scale_hz[(rnd() & 3) + (song & 1) * 2] / 2, 60, 48 + (step & 1) * 32, 2);
    /* lead and PSG harmony */
    if ((rnd() & 3) != 0) note(4 + (step & 1), scale_hz[8 + (rnd() & 7)], 50, 40 + (rnd() & 47), 1);
    if ((step & 3) == 1) note(8 + (rnd() % 6), scale_hz[4 + (rnd() & 7)], 28, rnd() & 127, 1);
    if (song == 2 && (step & 7) == 4) note(9 + (step & 3), scale_hz[rnd() & 15], 24, 64, 1);   /* town: more PSG */
}

static void spi_wait(void) { while (SPICNT & 0x80) ; }
static u32 touch_read(u32 cmd) {
    SPICNT = 0x8A01; SPIDATA = (u16)cmd; spi_wait();
    SPIDATA = 0; spi_wait(); u32 hi = SPIDATA & 0xFF;
    SPICNT = 0x8201; SPIDATA = 0; spi_wait(); u32 lo = SPIDATA & 0xFF;
    return ((hi << 8) | lo) >> 3;
}

static void fifo_send(u32 w) { while (IPCFIFOCNT & 2) ; IPCFIFOSEND = w; }

__attribute__((target("arm"))) static void irq7(void) {
    u32 f = IE & IF;
    IF = f;
    if (f & 1) pend |= 1;
    if (f & (1 << 3)) { ticks_pending++; pend |= 2; }
    if (f & (1 << 4)) pend |= 4;
    if (f & (1 << 18)) pend |= 8;
}

static u32 hash7(void) {
    u32 h = 2166136261u;
    u32 v[6] = { seq_rng, seq_tick, notes, song, stream_half, bank };
    for (int i = 0; i < 6; i++) h = (h ^ v[i]) * 16777619u;
    return h;
}

static void fifo_cmd(u32 w) {
    u32 op = w >> 28, arg = w & 0x0FFFFFFF;
    switch (op) {
    case 1: bank = arg; break;
    case 2:                                                /* stream start: L at arg, R at arg + 8 KB */
        stream_buf = arg; stream_half = 0;
        TMCNT_H(1) = 0;
        ch_start(0, arg, 0x10000 - 511, 0, 2048, F_PCM16 | LOOP, 90, 0);
        ch_start(1, arg + 8192, 0x10000 - 511, 0, 2048, F_PCM16 | LOOP, 90, 127);
        TMCNT_L(1) = (u16)(65536 - 8176); TMCNT_H(1) = 0x00C2;   /* F/256, IRQ, on */
        break;
    case 3: {                                              /* sound effect: pluck on 6/7 */
        u32 id = arg & 15, pan = (arg >> 8) & 127, v = (arg >> 16) & 127;
        note(6 + (id & 1), scale_hz[id], v ? v : 80, pan, 1);
        break; }
    case 4:                                                /* song reset (scene start) */
        song = arg & 3; seq_rng = 1 + song * 977; seq_tick = 0; notes = 0;
        break;
    case 5: fifo_send(5u << 28); fifo_send(hash7()); break;
    case 6: TMCNT_H(1) = 0; SCNT(0) = 0; SCNT(1) = 0; break;
    }
}

void main7(void) {
    POWCNT2 = 1;
    SOUNDCNT = 0x8000 | 80; SOUNDBIAS = 0x200;
    IPCFIFOCNT = 0x8000 | (1 << 3) | (1 << 10) | (1 << 14);   /* enable, clear send, recv IRQ, ack error */
    IRQHANDLER = (u32)irq7;
    DISPSTAT = 1 << 3;                                     /* vblank IRQ */
    TMCNT_L(0) = (u16)(65536 - 2727); TMCNT_H(0) = 0x00C1;  /* F/64: ~192 Hz, IRQ, on */
    IE = 1 | (1 << 3) | (1 << 4) | (1 << 18);
    IF = ~0u;
    IME = 1;
    irq_on7();
    IPCSYNC = 7 << 8;                                      /* tell the ARM9 we are up */
    u32 frame = 0;
    for (;;) {
        while (!pend) halt7();
        IME = 0; u32 p = pend; pend = 0; u32 tk = ticks_pending; ticks_pending = 0; IME = 1;
        if (p & 8) while (!(IPCFIFOCNT & 0x100)) fifo_cmd(IPCFIFORECV);
        if (p & 2) while (tk--) seq_step();
        if (p & 4) { fifo_send((1u << 28) | stream_half); stream_half ^= 1; }
        if (p & 1) {
            frame++;
            u32 x = touch_read(0xD0), y = touch_read(0x90), z = touch_read(0xB0);
            u32 keys = KEYXY;
            fifo_send((2u << 28) | ((x & 0xFF) << 16) | ((y & 0xFF) << 8) | ((z > 100) << 7) | (keys & 0x7F));
        }
    }
}
