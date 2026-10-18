/* geomtest ARM9: the geometry engine's readable results, for comparing GPU3D builds and settings (render.geom_fast)
   with bananads-difftest, which compares main RAM every frame. Each frame runs 24 rounds of random matrix commands
   (load, multiply 4x4/4x3/3x3, scale, translate, push/pop/store/restore, in each matrix mode, with values from small
   to full 32-bit range), then reads CLIPMTX_RESULT and VECMTX_RESULT, runs POS_TEST, VEC_TEST and BOX_TEST, draws
   random triangles and triangle strips (lit, partly outside the view volume) and reads RAM_COUNT; every result goes
   to a log in main RAM at 0x02200000 (and into a checksum). Frames alternate Z- and W-buffering. Written for this
   test.
   The FIFO IRQ phase (session 3, geomlazy): GXSTAT's FIFO IRQ ("less than half" and "empty") enabled in IE (IME on,
   the CPU's IRQs masked, so IF is polled rather than taken), the FIFO filled by the CPU's writes and by a GX FIFO DMA
   (mode 7) that ends, IF acknowledged while the FIFO is full enough, then IF polled until the geometry engine sets
   it: the poll count, timer 0 (33 MHz) and the FIFO level at that point are logged, so an IRQ set a run late (or
   never: the polls stop at a limit) changes the log. Random lengths, so the last entries drain in every phase of
   the engine's runs. */
typedef unsigned int u32; typedef unsigned short u16; typedef int s32; typedef short s16;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))

#define GXSTAT 0x04000600
static volatile u32 *const LOG = (volatile u32 *)0x02200000;   /* [0] frame, [1] checksum, [2] write index, log */
#define LOGWORDS 0x8000

static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) | (seed << 16); }
/* a random matrix value: mostly 4.12 values near 1, sometimes anything */
static u32 rval(void)
{
    u32 r = rnd(), k = r & 7;
    if (k == 0) return rnd();                              /* full range: products overflow 32 bits */
    if (k == 1) return (u32)((s32)(rnd() << 8) >> 8);      /* 24-bit */
    return (u32)((s32)(r & 0x3FFF) - 0x2000);              /* +-2.0 */
}
static void put(u32 v)
{
    u32 i = LOG[2];
    LOG[3 + (i & (LOGWORDS - 1))] = v;
    LOG[2] = i + 1;
    LOG[1] = (LOG[1] ^ v) * 16777619u + i;
}
static void gx(u32 reg, u32 v) { R32(0x04000400 + reg) = v; }
#define IF_GXFIFO (1u << 21)
#define IFREG 0x04000214
static void wait_idle(void) { while (R32(GXSTAT) & (1u << 27)) ; }
static void wait_test(void) { while (R32(GXSTAT) & 1u) ; }

static void matrix_round(void)
{
    u32 mode = rnd() & 3, op = ((rnd() & 0xFFFF) * 10) >> 16;
    gx(0x40, mode);
    switch (op) {
    case 0: for (int k = 0; k < 16; k++) gx(0x58, rval()); break;           /* load 4x4 */
    case 1: for (int k = 0; k < 12; k++) gx(0x5C, rval()); break;           /* load 4x3 */
    case 2: for (int k = 0; k < 16; k++) gx(0x60, rval()); break;           /* mult 4x4 */
    case 3: for (int k = 0; k < 12; k++) gx(0x64, rval()); break;           /* mult 4x3 */
    case 4: for (int k = 0; k < 9; k++) gx(0x68, rval()); break;            /* mult 3x3 */
    case 5: for (int k = 0; k < 3; k++) gx(0x6C, rval()); break;            /* scale */
    case 6: for (int k = 0; k < 3; k++) gx(0x70, rval()); break;            /* translate */
    case 7: gx(0x44, 0); break;                                              /* push */
    case 8: gx(0x48, rnd() & 1 ? 1 : 0x3F); break;                           /* pop 1 or -1 */
    default: gx(rnd() & 1 ? 0x4C : 0x50, rnd() & 31); break;                 /* store / restore */
    }
    if ((rnd() & 15) == 0) { gx(0x40, rnd() & 3); gx(0x54, 0); }            /* identity */
}

static void read_results(void)
{
    wait_idle();
    for (int k = 0; k < 16; k++) put(R32(0x04000640 + k * 4));            /* CLIPMTX_RESULT */
    for (int k = 0; k < 9; k++) put(R32(0x04000680 + k * 4));             /* VECMTX_RESULT */
    put(R32(GXSTAT) & 0xFFFF);                                              /* stack levels, overflow */
    gx(0x1C4, rnd()); gx(0x1C4, rnd() & 0xFFFF);                            /* POS_TEST */
    wait_test();
    for (int k = 0; k < 4; k++) put(R32(0x04000620 + k * 4));
    gx(0x1C8, rnd());                                                       /* VEC_TEST */
    wait_test();
    put(R16(0x04000630)); put(R16(0x04000632)); put(R16(0x04000634));
}

/* BOX_TEST with a projection and a rotation/scale/translation (melonDS's clipping overflows its 10-vertex buffers on
   faces a random matrix folds into a non-convex shape, as on a self-intersecting quad: neither is drawn here) */
static void box_tests(void)
{
    for (int k = 0; k < 8; k++) {
        gx(0x40, 1); gx(0x54, 0);
        s32 c = (s32)(rnd() & 0xFFF), sn = (s32)(rnd() & 0xFFF) - 0x800;
        gx(0x68, (u32)c); gx(0x68, (u32)sn); gx(0x68, 0); gx(0x68, (u32)-sn); gx(0x68, (u32)c); gx(0x68, 0);
        gx(0x68, 0); gx(0x68, 0); gx(0x68, 0x1000);
        gx(0x70, (rnd() & 0x3FFF) - 0x2000); gx(0x70, (rnd() & 0x3FFF) - 0x2000); gx(0x70, (u32)-(0x2000 + (rnd() & 0x7FFF)));
        gx(0x1C0, (rnd() & 0x1FFF1FFF) - 0x10001000); gx(0x1C0, (rnd() & 0x0FFF1FFF) - 0x1000); gx(0x1C0, rnd() & 0x0FFF0FFF);
        wait_test();
        put(R32(GXSTAT) & 3);
        wait_idle();
        for (int j = 0; j < 16; j++) put(R32(0x04000640 + j * 4));
    }
}

static void draw(void)
{
    gx(0x40, 2); gx(0x54, 0);
    for (int k = 0; k < 9; k++) gx(0x68, rval() & 0x80000007 ? (u32)((s32)(rnd() & 0x1FFF) - 0x1000) : 0x1000);
    gx(0x70, 0); gx(0x70, 0); gx(0x70, (u32)-(0x4000 + (rnd() & 0x3FFF)));
    gx(0xC8, (rnd() & 0x3FFFFFFF)); gx(0xCC, 0x7FFF);                       /* light 0 */
    gx(0xC8, (rnd() & 0x3FFFFFFF) | 1u << 30); gx(0xCC, 0x03E0 | 1u << 30); /* light 1 */
    gx(0xC0, 0x4210 | (0x2108u << 16)); gx(0xC4, 0x1084 | (0x0421u << 16) | (rnd() & 0x8000));
    for (int p = 0; p < 6; p++) {
        u32 kind = rnd() & 2, n = kind == 0 ? 3 : 3 + (rnd() & 7);          /* triangles, triangle strips */
        gx(0xA4, (rnd() & 0x30F0) | 0xC0 | (31u << 16) | ((p & 1) << 12) | (p << 24));   /* lights, both faces */
        gx(0x100, kind);
        for (u32 v = 0; v < n; v++) {
            gx(0x84, rnd() & 0x3FFFFFFF);
            gx(0x80, rnd() & 0x7FFF);
            u32 w = rnd(), range = (w & 3) == 0 ? 0xFFFF : 0x3FFF;
            s32 x = (s32)(rnd() & range) - (s32)(range >> 1), y = (s32)(rnd() & range) - (s32)(range >> 1);
            s32 z = (s32)(rnd() & range) - (s32)(range >> 1);
            if (w & 4) { gx(0x8C, ((u32)x & 0xFFFF) | (u32)y << 16); gx(0x8C, (u32)z & 0xFFFF); }
            else if (w & 8) gx(0xA0, rnd() & 0x3FFFFFFF);                  /* VTX_DIFF */
            else gx(0x90, (((u32)x >> 6) & 0x3FF) | (((u32)y >> 6) & 0x3FF) << 10 | (((u32)z >> 6) & 0x3FF) << 20);
        }
        gx(0x104, 0);
    }
    wait_idle();
    put(R32(0x04000604));                                                    /* RAM_COUNT */
}

/* the GX FIFO DMA's display list: BOX_TESTs, each a packed word and 3 parameters (~103 cycles for 3 entries: the
   engine is slower than the DMA and the CPU, so the FIFO fills) */
static u32 dlist[4 * 170];
static __attribute__((target("arm"), noinline)) void dcache_flush(void)
{
    /* clean and invalidate the ARM946's 4 KB data cache by segment and index (so the DMA reads the list from RAM) */
    for (u32 seg = 0; seg < 4; seg++)
        for (u32 idx = 0; idx < 0x400; idx += 32)
            __asm__ volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"((seg << 30) | idx));
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0));   /* drain the write buffer */
}
static void timer_restart(void) { R16(0x04000102) = 0; R16(0x04000100) = 0; R16(0x04000102) = 0x80; }
/* poll IF until the FIFO IRQ's bit is set (or a limit), then log the polls, timer 0 and the FIFO level */
static void wait_fifo_irq(void)
{
    u32 c = 0;
    while (!(R32(IFREG) & IF_GXFIFO) && c < 20000) c++;
    put(c); put(R16(0x04000100)); put((R32(GXSTAT) >> 16) & 0x1FF);
}
static void fifo_irq_phase(void)
{
    put(0x1F1F0000);                                                        /* the phase's marker in the log */
    R32(0x04000210) |= IF_GXFIFO;                                           /* IE */
    R32(0x04000208) = 1;                                                    /* IME (CPSR.I masks them) */
    for (int t = 0; t < 4; t++) {
        /* the CPU's writes: random BOX_TESTs (past a full FIFO: the CPU stalls) and a random tail of 1-entry
           commands, then until "empty" */
        R32(GXSTAT) = 2u << 30;
        u32 n = 8 + (rnd() & 127), tail = rnd() & 7;
        for (u32 k = 0; k < n; k++) { gx(0x1C0, rnd()); gx(0x1C0, rnd()); gx(0x1C0, rnd()); }
        for (u32 k = 0; k < tail; k++) gx(0x40, 3);
        R32(IFREG) = IF_GXFIFO;                                             /* acknowledge (set again if empty) */
        timer_restart();
        wait_fifo_irq();
        /* a GX FIFO DMA of a random length: "less than half" after it filled the FIFO past half, then "empty" */
        u32 cmds = 40 + (rnd() & 127);
        dcache_flush();
        R32(GXSTAT) = 1u << 30;
        R32(0x040000D4) = (u32)dlist; R32(0x040000D8) = 0x04000400;
        R32(0x040000DC) = (4 * cmds) | (2u << 21) | (1u << 26) | (7u << 27) | (1u << 31);
        u32 c = 0;
        while (((R32(GXSTAT) >> 16) & 0x1FF) < 128 && (R32(0x040000DC) & (1u << 31)) && c < 20000) c++;
        put(c);
        R32(IFREG) = IF_GXFIFO;
        timer_restart();
        wait_fifo_irq();
        R32(GXSTAT) = 2u << 30;
        R32(IFREG) = IF_GXFIFO;
        wait_fifo_irq();
        c = 0;
        while ((R32(0x040000DC) & (1u << 31)) && c < 20000) c++;
        put(c);
        put(R32(IFREG) & IF_GXFIFO);
    }
    R32(GXSTAT) = 0;
    R32(0x04000208) = 0;
    R32(0x04000210) &= ~IF_GXFIFO;
    R32(IFREG) = IF_GXFIFO;
    wait_idle();
}

int main(void)
{
    R32(0x04000304) = 0x820F;
    R32(0x04000000) = 0x10000 | (1 << 8) | (1 << 3);
    R32(0x04000060) = 1 << 4;                                               /* 3D, alpha blending */
    R32(0x04000350) = 0x2108 | (31u << 16); R32(0x04000354) = 0x7FFF;
    gx(0x180, 0 | (0 << 8) | (255u << 16) | (191u << 24));
    gx(0x40, 0); gx(0x54, 0);
    static const s32 proj[16] = { 0x1800, 0, 0, 0,  0, 0x2000, 0, 0,  0, 0, -0x1010, -0x1000,  0, 0, -0x200, 0 };
    for (int k = 0; k < 16; k++) gx(0x58, (u32)proj[k]);
    gx(0x40, 2); gx(0x54, 0);
    LOG[0] = 0; LOG[1] = 0; LOG[2] = 0;
    for (int k = 0; k < 170; k++) {
        dlist[4 * k] = 0x70;                                                /* BOX_TEST */
        for (int j = 0; j < 3; j++) dlist[4 * k + 1 + j] = rnd();
    }
    for (u32 frame = 0;; frame++) {
        for (int r = 0; r < 24; r++) { matrix_round(); if ((r & 3) == 3) read_results(); }
        gx(0x40, 0); gx(0x54, 0);                                           /* back to a sane projection */
        for (int k = 0; k < 16; k++) gx(0x58, (u32)proj[k]);
        box_tests();
        fifo_irq_phase();                                                   /* its BOX_TESTs with box_tests' matrices */
        draw();
        gx(0x140, (frame & 1) << 1);                                        /* SWAP_BUFFERS: Z / W-buffering */
        LOG[0] = frame;
        while (R16(0x04000006) != 192) ;
        while (R16(0x04000006) == 192) ;
    }
}
