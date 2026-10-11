// ds2dtest ARM9: exercises the 2D engines for render.2d_thread's exactness checks. Both units draw text, affine
// and 8-bit ext-palette BGs, sprites (normal, rotscale, semi-transparent, OBJ window, mosaic), both windows (edges in
// every order) and colour effects, all animated; mid-frame (by VCOUNT) it rewrites palettes, BG and OBJ VRAM, OAM,
// scroll and affine registers, DISPCNT and a VRAM bank's mapping; every other 32 frames it captures unit A into
// bank D, and shows bank D through VRAM display mode during some of them.
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef int s32;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a)  (*(volatile u8 *)(a))
#define VCOUNT R16(0x04000006)
#define POWCNT1 R32(0x04000304)
#define CAPCNT R32(0x04000064)

static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }
static void fill16(u32 addr, u32 bytes) { for (u32 i = 0; i < bytes; i += 2) R16(addr + i) = (u16)rnd(); }
static void wait_line(u32 l) { while (VCOUNT != l) ; }

static void setup_unit(u32 io, u32 bgvram, u32 objvram, u32 pal, u32 oam) {
    // BG0 4bpp char 0 map 31; BG1 8bpp ext-pal (slot 1) char 2 map 30 mosaic; BG2 4bpp char 0 map 29;
    // BG3 affine char 4 map 28 (128x128, wrap)
    R16(io + 0x08) = 0 | (0 << 2) | (31 << 8);
    R16(io + 0x0A) = 1 | (2 << 2) | (1 << 6) | (1 << 7) | (30 << 8) | (1 << 13);
    R16(io + 0x0C) = 2 | (0 << 2) | (29 << 8);
    R16(io + 0x0E) = 3 | (4 << 2) | (28 << 8) | (1 << 13);
    fill16(bgvram + 0x0000, 0x8000);   // 4bpp tiles (char 0..1)
    fill16(bgvram + 0x8000, 0x8000);   // 8bpp tiles (char 2..3)
    fill16(bgvram + 0x10000, 0x4000);  // affine tiles
    for (u32 i = 0; i < 1024; i++) {
        u32 r = rnd();
        R16(bgvram + 0xF800 + i * 2) = (r & 0x3FF) | ((r >> 10) & 0xC00) | (((r >> 12) & 0xF) << 12);
        r = rnd();
        R16(bgvram + 0xF000 + i * 2) = (r & 0x1FF) | (r & 0xC00) | ((r >> 12) & 0xF) << 12;
        r = rnd();
        R16(bgvram + 0xE800 + i * 2) = ((r & 1) ? 0 : (r & 0x3FF)) | (((r >> 12) & 0xF) << 12);
    }
    for (u32 i = 0; i < 256; i += 2) R16(bgvram + 0xE000 + i) = (u16)rnd();
    fill16(objvram, 0x4000);
    fill16(pal, 0x400);
    for (u32 i = 0; i < 128; i++) {
        u32 r = rnd(), s = rnd();
        u32 a0 = (r & 0xFF) | (((s & 7) == 0 ? 1 : (s & 7) == 1 ? 3 : 0) << 8) | (((s >> 3) & 3) % 3 << 10)
               | (((s >> 5) & 7) == 0 ? (1 << 12) : 0) | ((s >> 8) & 1 ? (1 << 13) : 0) | (((s >> 9) % 3) << 14);
        u32 a1 = ((r >> 8) & 0x1FF) | (((r >> 17) & 0x1F) << 9) | (((s >> 12) & 3) << 14);
        u32 a2 = ((r >> 22) & 0x3FF) | (((s >> 14) & 3) << 10) | (((s >> 16) & 0xF) << 12);
        R16(oam + i * 8) = a0; R16(oam + i * 8 + 2) = a1; R16(oam + i * 8 + 4) = a2;
        R16(oam + i * 8 + 6) = (u16)(rnd() & 0x1FF) - 0x100 + 0x100;
    }
    R16(io + 0x48) = 0x3F1F; R16(io + 0x4A) = 0x2B37;  // WININ, WINOUT (OBJ window 0x2B)
    R16(io + 0x50) = 0x2F42;                           // BLDCNT: alpha, BG1 on top of everything
    R16(io + 0x52) = 0x0A06;
}

static void animate(u32 io, u32 f, u32 k) {
    u32 x1 = (f * 3 + k) & 0xFF, x2 = (f * 5 + 40 * k) & 0xFF;
    if ((f & 15) == 7) x2 = x1;
    R16(io + 0x40) = (x1 << 8) | x2;
    R16(io + 0x42) = (x2 << 8) | ((x1 + 60) & 0xFF);
    R16(io + 0x44) = (((f + 20) & 0xFF) << 8) | ((f * 2 + 100) & 0xFF);
    R16(io + 0x46) = (10 << 8) | 180;
    R16(io + 0x4C) = ((f >> 2) & 0xF) | (((f >> 3) & 0xF) << 4) | (((f >> 4) & 3) << 8) | (((f >> 5) & 3) << 12);
    R16(io + 0x10) = f * 2; R16(io + 0x12) = f;
    R16(io + 0x14) = -f; R16(io + 0x16) = f * 3;
    R16(io + 0x18) = f * 7; R16(io + 0x1A) = f >> 1;
    s32 c = 256 - (s32)(f & 63), s = (s32)(f & 31) - 16;
    R16(io + 0x30) = c; R16(io + 0x32) = s; R16(io + 0x34) = -s; R16(io + 0x36) = c;
    R32(io + 0x38) = (f * 64) << 4; R32(io + 0x3C) = (f * 32) << 4;
    R16(io + 0x52) = ((f >> 2) & 0x1F) | (((31 - (f >> 2)) & 0x1F) << 8);
    R16(io + 0x54) = (f >> 3) & 0x1F;
    R16(io + 0x50) = ((f >> 6) & 1) ? 0x2FC1 : ((f >> 7) & 1) ? 0x0081 : 0x2F42;
}

int main(void) {
    POWCNT1 = 0x8203;
    R8(0x04000240) = 0x81;  // A: ABG 0x06000000
    R8(0x04000241) = 0x82;  // B: AOBJ 0x06400000
    R8(0x04000242) = 0x84;  // C: BBG 0x06200000
    R8(0x04000243) = 0x80;  // D: LCDC (capture)
    R8(0x04000244) = 0x80;  // E: LCDC first, filled, then ABG ext pal
    R8(0x04000245) = 0x80;  // F: LCDC, then AOBJ ext pal
    R8(0x04000248) = 0x80;  // H: LCDC, then BBG ext pal
    R8(0x04000249) = 0x82;  // I: BOBJ 0x06600000
    fill16(0x06880000, 0x10000);   // E
    fill16(0x06890000, 0x4000);    // F
    fill16(0x06898000, 0x8000);    // H
    R8(0x04000244) = 0x84; R8(0x04000245) = 0x85; R8(0x04000248) = 0x82;
    setup_unit(0x04000000, 0x06000000, 0x06400000, 0x05000000, 0x07000000);
    setup_unit(0x04001000, 0x06200000, 0x06600000, 0x05000400, 0x07000400);

    for (u32 f = 0;; f++) {
        wait_line(192);
        u32 dispA = 1 | (1 << 4) | (1 << 16) | (1u << 30) | (1u << 31) | (0x1F << 8)
                  | ((f & 32) ? 0x2000 : 0) | ((f & 64) ? 0x4000 : 0) | ((f & 16) ? 0x8000 : 0);
        u32 dispB = ((f >> 6) & 1 ? 1 : 0) | (1 << 4) | (1 << 16) | (1u << 30) | (0x1F << 8)
                  | ((f & 16) ? 0x2000 : 0) | ((f & 32) ? 0x4000 : 0) | ((f & 64) ? 0x8000 : 0);
        if ((f >> 5) & 1) {
            CAPCNT = (1u << 31) | (3 << 16) | (3 << 20) | ((f & 8) ? (1 << 29) | (8 << 0) | (8 << 8) : 0);
            if ((f >> 6) & 1) dispA = (dispA & ~(3 << 16)) | (2 << 16) | (3 << 18);
        }
        R32(0x04000000) = dispA; R32(0x04001000) = dispB;
        R16(0x0400106C) = ((f >> 5) & 1 ? 0x4000 : 0x8000) | ((f >> 1) & 0x1F);
        animate(0x04000000, f, 0); animate(0x04001000, f, 1);
        for (u32 i = 0; i < 128; i++) {
            u32 o = 0x07000000 + i * 8;
            R16(o + 2) = (R16(o + 2) & ~0x1FF) | ((R16(o + 2) + (i & 3) + 1) & 0x1FF);
            o += 0x400;
            R16(o) = (R16(o) & ~0xFF) | ((R16(o) + 1) & 0xFF);
        }
        R16(0x05000000) = (u16)(f * 33);   // backdrop
        wait_line(40);
        for (u32 i = 0; i < 16; i++) R16(0x05000020 + i * 2) = (u16)rnd();
        R16(0x05000600 + (f & 0xFF) * 2) = (u16)rnd();
        wait_line(80);
        fill16(0x06000000 + ((f * 512) & 0x7FFF), 512);
        wait_line(100);
        R16(0x04000010) = f * 5; R16(0x04001030) = 300;
        wait_line(120);
        for (u32 i = 0; i < 16; i++) R16(0x07000002 + i * 8) ^= 0x1000;
        wait_line(140);
        R32(0x04000000) = dispA ^ 0x0200;
        wait_line(160);
        fill16(0x06400000 + ((f * 256) & 0x3FFF), 256);
        if ((f & 3) == 1) {
            wait_line(170);
            R8(0x04000244) = 0x80;
            fill16(0x06880000 + ((f * 512) & 0x7FFF), 512);
            R8(0x04000244) = 0x84;
        }
    }
}
