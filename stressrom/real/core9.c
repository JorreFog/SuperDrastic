/* dsreal ARM9 core: IRQs, timers, IPC, DMA, card reads, EEPROM writes, hashes, maths units, display lists, text */
#include "game.h"

volatile u32 vbl_count, hbl_count, stream_req, touch_word, arm7_hash, arm7_hash_ok;
static volatile u32 hash_next;
void (*volatile hblank_hook)(void);

/* the IRQ handler lives in ITCM like a NitroSDK game's (the BIOS jumps to [DTCM + 0x3FFC]) */
ITCM void irq9(void) {
    u32 f = IE & IF;
    IF = f;
    if (f & IRQ_VBLANK) vbl_count++;
    if (f & IRQ_HBLANK) { hbl_count++; void (*h)(void) = hblank_hook; if (h) h(); }
    if (f & IRQ_IPCRECV)
        while (!(IPCFIFOCNT & 0x100)) {
            u32 w = IPCFIFORECV;
            if (hash_next) { arm7_hash = w; arm7_hash_ok = 1; hash_next = 0; continue; }
            switch (w >> 28) {
            case 1: stream_req |= 1u << (w & 1); break;
            case 2: touch_word = w; break;
            case 5: hash_next = 1; break;
            }
        }
}

void core_init(void) {
    EXMEMCNT &= ~(1u << 11);                                 /* slot 1 (card, AUXSPI) to the ARM9 */
    IPCFIFOCNT = 0x8000 | (1 << 3) | (1 << 10) | (1 << 14);
    R32(0x0B003FFC) = (u32)irq9;
    DISPSTAT = (1 << 3);
    TMCNT_H(0) = 0; TMCNT_H(1) = 0; TMCNT_L(0) = 0; TMCNT_L(1) = 0;
    TMCNT_H(1) = 0x84; TMCNT_H(0) = 0x80;                    /* 33.5 MHz, cascaded into 32 bits */
    IF = ~0u;
    IE = IRQ_VBLANK | IRQ_IPCRECV;
    IME = 1;
    irq_on();
    while ((IPCSYNC & 0xF) != 7) ;                           /* the ARM7 is up */
}

ARM void wait_vblank(void) { u32 v = vbl_count; while (vbl_count == v) cpu_halt(); }

u32 timer_now(void) {
    u32 h, l, h2;
    do { h = TMCNT_L(1); l = TMCNT_L(0); h2 = TMCNT_L(1); } while (h != h2);
    return (h << 16) | l;
}

void fifo_send(u32 w) { while (IPCFIFOCNT & 2) ; IPCFIFOSEND = w; }

ARM void dma_copy32(void *dst, const void *src, u32 bytes) {
    DMASAD(3) = (u32)src; DMADAD(3) = (u32)dst; DMACNT(3) = DMA_EN | DMA_32 | (bytes >> 2);
    while (DMACNT(3) & DMA_EN) continue;
}
ARM void dma_fill32(void *dst, u32 val, u32 bytes) {
    DMAFILL(3) = val;
    DMASAD(3) = (u32)&DMAFILL(3); DMADAD(3) = (u32)dst; DMACNT(3) = DMA_EN | DMA_32 | DMA_SRC_FIX | (bytes >> 2);
    while (DMACNT(3) & DMA_EN) continue;
}
void copy16(volatile void *dst, const void *src, u32 bytes) {
    volatile u16 *d = dst; const u16 *s = src;
    for (u32 i = 0; i < bytes / 2; i++) d[i] = s[i];
}

/* the card's main-data read (command B7, 0x200-byte blocks), polled like libnds's cardReadData */
ARM void card_read(void *dst, u32 addr, u32 bytes) {
    u32 *d = dst;
    for (u32 o = 0; o < bytes; o += 0x200) {
        u32 a = addr + o;
        AUXSPICNT = 0x8000;
        CARDCMD[0] = 0xB7; CARDCMD[1] = (u8)(a >> 24); CARDCMD[2] = (u8)(a >> 16); CARDCMD[3] = (u8)(a >> 8);
        CARDCMD[4] = (u8)a; CARDCMD[5] = 0; CARDCMD[6] = 0; CARDCMD[7] = 0;
        ROMCTRL = 0xA1586000;
        u32 n = 0;
        do {
            if (ROMCTRL & (1u << 23)) { u32 w = CARDDATA; if (n < (bytes - o) >> 2) d[n] = w; n++; }
        } while (ROMCTRL & (1u << 31));
        d += 0x80;
    }
}

/* EEPROM (the 64 KB type melonDS gives an unknown retail game): WREN, then page programs of up to 32 bytes */
static void spi_x(u32 b) { AUXSPIDATA = (u16)b; while (AUXSPICNT & 0x80) continue; }
void eeprom_write(u32 addr, const void *src, u32 n) {
    const u8 *s = src;
    while (n) {
        AUXSPICNT = 0xA040; spi_x(0x06); AUXSPICNT = 0x40;
        AUXSPICNT = 0xA040; spi_x(0x02); spi_x(addr >> 8); spi_x(addr & 0xFF);
        u32 k = 0;
        do { spi_x(*s++); addr++; n--; k++; } while (n && k < 32 && (addr & 31));
        AUXSPICNT = 0x40;
        AUXSPICNT = 0xA040; spi_x(0x05);
        do spi_x(0); while (AUXSPIDATA & 1);
        AUXSPICNT = 0x40;
    }
}

/* a hash whose time doesn't depend on the data (no MUL: its cycles depend on the operand) */
ARM u32 hash_words(u32 h, const volatile u32 *p, u32 words) {
    for (u32 i = 0; i < words; i++) { h = ((h << 5) | (h >> 27)) ^ p[i]; h += 0x9E3779B9u; }
    return h;
}

s32 hw_div(s32 num, s32 den) {
    DIVCNT = 0; DIV_NUMER = (u32)num; DIV_DENOM = (u32)den; DIV_DENOM_H = 0;
    while (DIVCNT & 0x8000) ;
    return (s32)DIV_RESULT;
}
s32 hw_div_fx(s32 num, s32 den) {
    s64 n = (s64)num << 12;
    DIVCNT = 1; DIV_NUMER = (u32)n; DIV_NUMER_H = (u32)(n >> 32); DIV_DENOM = (u32)den; DIV_DENOM_H = 0;
    while (DIVCNT & 0x8000) ;
    return (s32)DIV_RESULT;
}
u32 hw_sqrt(u32 v) { SQRTCNT = 0; SQRT_PARAM = v; while (SQRTCNT & 0x8000) ; return SQRT_RESULT; }

s32 atan2i(s32 y, s32 x) {                                  /* 1024 = a turn; atantab[257]: 0..128 */
    if (x == 0 && y == 0) return 0;
    s32 ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
    if (ax >= ay) a = atantab[hw_div(ay << 8, ax)];
    else a = 256 - atantab[hw_div(ax << 8, ay)];
    if (x < 0) a = 512 - a;
    if (y < 0) a = -a;
    return a & 1023;
}

static u32 seed = 1;
u32 rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
void rnd_seed(u32 s) { seed = s; }

/* ---- display lists ---- */
static u32 *dlw, *dlc, dl_slot, *dl0;
void dl_start(u32 *buf) { dl0 = dlw = buf; dl_slot = 4; }
void dl_cmd(u32 id, u32 np, ...) {
    if (dl_slot == 4) { dlc = dlw++; *dlc = 0; dl_slot = 0; }
    *dlc |= id << (8 * dl_slot++);
    __builtin_va_list ap; __builtin_va_start(ap, np);
    for (u32 i = 0; i < np; i++) *dlw++ = __builtin_va_arg(ap, u32);
    __builtin_va_end(ap);
}
dl_t dl_finish(void) { dl_slot = 4; dl_t d = { dl0, (u32)(dlw - dl0) }; return d; }

ARM void gx_send(dl_t d) {                                  /* GX FIFO DMA (mode 7), 112 words a burst */
    DMASAD(1) = (u32)d.w; DMADAD(1) = 0x04000400;
    DMACNT(1) = DMA_EN | DMA_GXFIFO | DMA_32 | DMA_DST_FIX | d.n;
    while (DMACNT(1) & DMA_EN) ;
}
ARM void gx_rot_y(s32 a) {
    s32 c = cs(a), s = sn(a);
    MTX_MULT33 = c; MTX_MULT33 = 0; MTX_MULT33 = -s;
    MTX_MULT33 = 0; MTX_MULT33 = 4096; MTX_MULT33 = 0;
    MTX_MULT33 = s; MTX_MULT33 = 0; MTX_MULT33 = c;
}
ARM void gx_rot_x(s32 a) {
    s32 c = cs(a), s = sn(a);
    MTX_MULT33 = 4096; MTX_MULT33 = 0; MTX_MULT33 = 0;
    MTX_MULT33 = 0; MTX_MULT33 = c; MTX_MULT33 = s;
    MTX_MULT33 = 0; MTX_MULT33 = -s; MTX_MULT33 = c;
}
ARM void gx_trans(s32 x, s32 y, s32 z) { MTX_TRANS = (u32)x; MTX_TRANS = (u32)y; MTX_TRANS = (u32)z; }

/* ---- text ---- */
void font_upload(volatile u16 *chars) {
    for (int g = 0; g < 64; g++)
        for (int y = 0; y < 8; y++) {
            u32 row = 0;
            if (y >= 1 && y <= 5)
                for (int x = 0; x < 3; x++)
                    if (font3x5[g] >> ((5 - y) * 3 + (2 - x)) & 1) row |= 1u << ((x + 2) * 4);
            chars[g * 16 + y * 2] = (u16)row; chars[g * 16 + y * 2 + 1] = (u16)(row >> 16);
        }
}
void text(volatile u16 *map, int x, int y, const char *s) {
    for (; *s; s++, x++) { int c = *s; if (c >= 'a' && c <= 'z') c -= 32; c -= 32; if (c < 0 || c > 63) c = 0; map[y * 32 + x] = (u16)c; }
}
void text_hex(volatile u16 *map, int x, int y, u32 v, int digits) {
    for (int i = digits - 1; i >= 0; i--, v >>= 4) map[y * 32 + x + i] = (u16)("0123456789ABCDEF"[v & 15] - 32);
}
void text_dec(volatile u16 *map, int x, int y, u32 v, int digits) {
    for (int i = digits - 1; i >= 0; i--) { u32 q = (u32)hw_div((s32)v, 10); map[y * 32 + x + i] = (u16)('0' + v - q * 10 - 32); v = q; }
}

/* ---- textures and overlays from the card ---- */
static u32 stage[0x7000 / 4];
u32 texparam(u32 ofs, u32 fmt, u32 s, u32 t, u32 extra) { return (ofs >> 3) | (s << 20) | (t << 23) | (fmt << 26) | extra; }
void texset_load(int set) {
    card_read(stage, ROM_TEXSET(set), 0x7000);
    VRAMCNT(0) = 0x80; VRAMCNT(1) = 0x80; VRAMCNT(5) = 0x80;   /* A, B, F to the LCDC */
    dma_copy32((void *)VRAM_LCDC(0), stage, TEX_SET_BYTES);
    dma_copy32((void *)(VRAM_LCDC(1) + TEX_C4X4 / 2), (u8 *)stage + 0x6000, 0x800);
    dma_copy32((void *)LCDC_F, (u8 *)stage + 0x6800, PAL_SET_BYTES);
    VRAMCNT(0) = 0x83; VRAMCNT(1) = 0x8B; VRAMCNT(5) = 0x83;   /* texture slots 0, 1; texture palette slot 0 */
}
void overlay_load(int which) { card_read((void *)OVL_ADDR, which ? ROM_OVL_B : ROM_OVL_A, 0x4000); }

/* what clang may call */
void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }
