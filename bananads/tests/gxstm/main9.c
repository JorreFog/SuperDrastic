/* gxstm ARM9: packed display lists sent to the GX FIFO the ways games send them, for comparing the JIT's and the
   geometry engine's GX FIFO paths (render.gx_batch) with bananads-difftest (main RAM every frame) and pictures.
   Each frame builds a list of NNS-style model draws in main RAM (matrix push, 4x3 load or multiply, scale, material,
   polygon attributes, strips/triangles/quads of lit, coloured, textured-coordinate vertices in every vertex format,
   pop; now and then a 4x4 load, identity, store/restore, POS/VEC/BOX tests and all-NOP headers), packed 1-4
   commands per header, and sends it in pieces: ARM STMIA bursts of 1-8 words (as GX_SendFifo and MI_CpuSend32 do),
   STMDB bursts ending at 0x0400043C, Thumb STMIA, single STRs, immediate DMA to 0x04000400 and GX-FIFO-mode DMA.
   The bursts don't wait, so the 256-entry FIFO fills and the system stalls. Between pieces it logs GXSTAT,
   RAM_COUNT, test results and the clip matrix, and also does STMs that straddle the FIFO's end (into MTX_MODE
   and PUSH). Written for this test. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8; typedef int s32; typedef short s16;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define ARM __attribute__((target("arm"), noinline))
#define NOINLINE __attribute__((noinline))

#define GXSTAT 0x04000600
#define FIFO 0x04000400
static volatile u32 *const LOG = (volatile u32 *)0x02200000;   /* [0] frame, [1] checksum, [2] write index, log */
#define LOGWORDS 0x8000

static u32 seed = 777;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) | (seed << 16); }
static void put(u32 v)
{
    u32 i = LOG[2];
    LOG[3 + (i & (LOGWORDS - 1))] = v;
    LOG[2] = i + 1;
    LOG[1] = (LOG[1] ^ v) * 16777619u + i;
}
static void gx(u32 reg, u32 v) { R32(0x04000400 + reg) = v; }
static void wait_idle(void) { while (R32(GXSTAT) & (1u << 27)) ; }

/* the display list: commands queued with their parameters, then packed */
static u32 dl[0x6000];
static u32 dln;
static u8 qop[8]; static u32 qpar[8][16]; static u32 qn;
static const u8 nparams[256] = {
    [0x10] = 1, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x16] = 16, [0x17] = 12, [0x18] = 16, [0x19] = 12, [0x1A] = 9,
    [0x1B] = 3, [0x1C] = 3, [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1, [0x25] = 1, [0x26] = 1,
    [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1, [0x30] = 1, [0x31] = 1, [0x32] = 1, [0x33] = 1,
    [0x40] = 1, [0x70] = 3, [0x71] = 2, [0x72] = 1,
};
static void flushq(void)
{
    if (!qn) return;
    u32 h = 0;
    for (u32 k = 0; k < qn; k++) h |= (u32)qop[k] << (8 * k);
    dl[dln++] = h;
    for (u32 k = 0; k < qn; k++)
        for (u32 j = 0; j < nparams[qop[k]]; j++) dl[dln++] = qpar[k][j];
    qn = 0;
}
/* queue a command; a header holds 1-4 (random), sometimes with a NOP between */
static u32 qmax = 4;
static u32 *cmd(u8 op)
{
    if (qn >= qmax) flushq();
    if (!qn) qmax = 1 + (rnd() & 3);
    if (qn && qn < 3 && (rnd() & 15) == 0) qop[qn++] = 0;   /* a packed NOP */
    if (qn >= 4) flushq();
    if (!qn) qmax = 1 + (rnd() & 3);
    qop[qn] = op;
    return qpar[qn++];   /* (filled by the caller; the header is packed at the next command or flushq) */
}
static void c1(u8 op, u32 v) { *cmd(op) = v; }
static s32 sr(u32 range) { return (s32)(rnd() & range) - (s32)(range >> 1); }

static void model(u32 m)
{
    c1(0x10, 2);
    cmd(0x11);                                                              /* push */
    u32 *p;
    u32 k = rnd() & 15;
    if (k == 0) { p = cmd(0x16); for (int j = 0; j < 16; j++) p[j] = (j == 0 || j == 5 || j == 10) ? 0x1000 : (u32)sr(0x7FF);
                  p[12] = (u32)sr(0x3FFF); p[13] = (u32)sr(0x3FFF); p[14] = (u32)-(0x3000 + (rnd() & 0x3FFF)); p[15] = 0x1000; }
    else {
        s32 c = 0x1000 - (s32)(rnd() & 0x3FF), sn = sr(0xFFF);
        p = cmd(k < 8 ? 0x17 : 0x19);
        p[0] = (u32)c; p[1] = (u32)sn; p[2] = 0; p[3] = (u32)-sn; p[4] = (u32)c; p[5] = 0; p[6] = 0; p[7] = 0; p[8] = 0x1000;
        p[9] = (u32)sr(0x3FFF); p[10] = (u32)sr(0x3FFF); p[11] = k < 8 ? (u32)-(0x3000 + (rnd() & 0x3FFF)) : (u32)sr(0x7FF);
    }
    if (rnd() & 1) { p = cmd(0x1B); p[0] = 0x800 + (rnd() & 0xFFF); p[1] = 0x800 + (rnd() & 0xFFF); p[2] = 0x1000; }
    if ((rnd() & 7) == 0) { p = cmd(0x1A); for (int j = 0; j < 9; j++) p[j] = (j == 0 || j == 4 || j == 8) ? 0x1000 : (u32)sr(0x3FF); }
    if ((rnd() & 7) == 0) { p = cmd(0x1C); p[0] = (u32)sr(0x7FF); p[1] = (u32)sr(0x7FF); p[2] = (u32)sr(0x7FF); }
    c1(0x30, 0x4210 | (0x2108u << 16) | (rnd() & 0x8000));                 /* diffuse/ambient */
    c1(0x31, 0x1084 | (0x0421u << 16));
    c1(0x2A, 0); c1(0x2B, 0);
    c1(0x29, (rnd() & 3) | 0xC0 | (31u << 16) | ((m & 63) << 24));          /* lights, both faces */
    u32 prim = rnd() & 3, nv = prim == 0 ? 3 * (1 + (rnd() & 3)) : prim == 1 ? 4 * (1 + (rnd() & 1)) : 3 + (rnd() & 7);
    c1(0x40, prim);
    s32 x = sr(0x7FF), y = sr(0x7FF), z = sr(0x7FF);
    for (u32 v = 0; v < nv; v++) {
        if (rnd() & 1) c1(0x21, rnd() & 0x3FFFFFFF);
        else c1(0x20, rnd() & 0x7FFF);
        c1(0x22, rnd());
        x = sr(0x1FFF); y = sr(0x1FFF); z = sr(0x7FF);
        switch (rnd() & 7) {
        case 0: case 1: case 2: p = cmd(0x23); p[0] = ((u32)x & 0xFFFF) | (u32)y << 16; p[1] = (u32)z & 0xFFFF; break;
        case 3: c1(0x24, (((u32)x >> 6) & 0x3FF) | (((u32)y >> 6) & 0x3FF) << 10 | (((u32)z >> 6) & 0x3FF) << 20); break;
        case 4: c1(0x25, ((u32)x & 0xFFFF) | (u32)y << 16); break;
        case 5: c1(0x26, ((u32)x & 0xFFFF) | (u32)z << 16); break;
        case 6: c1(0x27, ((u32)y & 0xFFFF) | (u32)z << 16); break;
        default: c1(0x28, rnd() & 0x3FFFFFFF); break;
        }
    }
    cmd(0x41);                                                              /* end */
    if ((rnd() & 7) == 0) { c1(0x13, rnd() & 31); c1(0x14, rnd() & 31); }   /* store / restore */
    if ((rnd() & 7) == 0) { c1(0x10, rnd() & 1 ? 2 : 3); cmd(0x15); c1(0x10, 2); }
    if ((rnd() & 7) == 0) { p = cmd(0x71); p[0] = rnd(); p[1] = rnd() & 0xFFFF; }
    if ((rnd() & 7) == 0) c1(0x72, rnd() & 0x3FFFFFFF);
    if ((rnd() & 7) == 0) { p = cmd(0x70); p[0] = rnd() & 0x0FFF0FFF; p[1] = rnd() & 0x0FFF0FFF; p[2] = rnd() & 0x0FFF0FFF; }
    c1(0x12, 1);                                                            /* pop */
    if ((rnd() & 15) == 0) { flushq(); dl[dln++] = 0; }                     /* an all-NOP header */
}

/* bursts of n words with LDMIA/STMIA, as GX_SendFifo* does */
ARM const u32 *stm_burst(const u32 *p, u32 n)
{
    register const u32 *rp __asm__("r0") = p;
    register u32 rf __asm__("r1") = FIFO;
    switch (n) {
    case 1: __asm__ volatile("ldmia %0!, {r2}\n\tstmia %1, {r2}" : "+r"(rp) : "r"(rf) : "r2", "memory"); break;
    case 2: __asm__ volatile("ldmia %0!, {r2-r3}\n\tstmia %1, {r2-r3}" : "+r"(rp) : "r"(rf) : "r2", "r3", "memory"); break;
    case 3: __asm__ volatile("ldmia %0!, {r2-r4}\n\tstmia %1, {r2-r4}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "memory"); break;
    case 4: __asm__ volatile("ldmia %0!, {r2-r5}\n\tstmia %1, {r2-r5}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "r5", "memory"); break;
    case 5: __asm__ volatile("ldmia %0!, {r2-r6}\n\tstmia %1, {r2-r6}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "r5", "r6", "memory"); break;
    case 6: __asm__ volatile("ldmia %0!, {r2-r7}\n\tstmia %1, {r2-r7}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "r5", "r6", "r7", "memory"); break;
    case 7: __asm__ volatile("ldmia %0!, {r2-r8}\n\tstmia %1, {r2-r8}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "memory"); break;
    default: __asm__ volatile("ldmia %0!, {r2-r8, r12}\n\tstmia %1, {r2-r8, r12}" : "+r"(rp) : "r"(rf)
                              : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r12", "memory"); break;
    }
    return rp;
}
/* 4 words with STMDB from the FIFO's end (0x04000430-0x0400043C) */
ARM const u32 *stmdb4(const u32 *p)
{
    register const u32 *rp __asm__("r0") = p;
    register u32 rf __asm__("r1") = FIFO + 0x40;
    __asm__ volatile("ldmia %0!, {r2-r5}\n\tstmdb %1, {r2-r5}" : "+r"(rp) : "r"(rf) : "r2", "r3", "r4", "r5", "memory");
    return rp;
}
/* Thumb: 4 words with STMIA (writeback on a scratch copy of the FIFO's address) */
const u32 *thumb4(const u32 *p)
{
    register const u32 *rp __asm__("r0") = p;
    register u32 rf __asm__("r1") = FIFO + 0x20;
    __asm__ volatile("ldmia %0!, {r2-r5}\n\tstmia %1!, {r2-r5}" : "+l"(rp), "+l"(rf) : : "r2", "r3", "r4", "r5", "memory");
    return rp;
}
/* an STM across the FIFO's end: two FIFO words (two IDENTITY headers), then MTX_MODE 2 and PUSH */
ARM void straddle(void)
{
    register u32 rf __asm__("r1") = FIFO + 0x38;
    __asm__ volatile("mov r2, #0x15\n\tmov r3, #0x15\n\tmov r4, #2\n\tmov r5, #0\n\tstmia %0, {r2-r5}" : : "r"(rf)
                     : "r2", "r3", "r4", "r5", "memory");
}

static void dma3(const u32 *p, u32 n, u32 mode)
{
    R32(0x040000D4) = (u32)p;
    R32(0x040000D8) = FIFO;
    R32(0x040000DC) = n | (2u << 21) | (1u << 26) | (mode << 27) | (1u << 31);   /* dest fixed, 32-bit */
    while (R32(0x040000DC) & (1u << 31)) ;
}

static void log_state(void)
{
    put(R32(GXSTAT));
    put(R32(0x04000604));                                                    /* RAM_COUNT */
}

static void send(const u32 *p, u32 n, u32 frame)
{
    const u32 *end = p + n;
    u32 piece = 0;
    while (p < end) {
        u32 left = (u32)(end - p), way = (((rnd() & 0xFFFF) * 10) >> 16) ^ (frame & 1), len = 16 + (rnd() & 255);
        if (len > left) len = left;
        const u32 *stop = p + len;
        switch (way) {
        case 0: case 1: case 2: case 3: case 4:                             /* STMIA bursts, 1-8 words */
            while (p < stop) { u32 b = 1 + (rnd() & 7); if (b > (u32)(stop - p)) b = (u32)(stop - p); p = stm_burst(p, b); }
            break;
        case 5:                                                              /* 8-word bursts, as GX_SendFifo64B */
            while (stop - p >= 8) p = stm_burst(p, 8);
            while (p < stop) p = stm_burst(p, 1);
            break;
        case 6:                                                              /* STMDB and Thumb STMIA */
            while (stop - p >= 4) p = (rnd() & 1) ? stmdb4(p) : thumb4(p);
            while (p < stop) R32(FIFO) = *p++;
            break;
        case 7:                                                              /* single STRs */
            while (p < stop) R32(FIFO + (rnd() & 0x3C)) = *p++;
            break;
        case 8: dma3(p, len, 0); p = stop; break;                            /* immediate DMA */
        default: dma3(p, len, 7); p = stop; break;                           /* GX FIFO DMA */
        }
        if ((++piece & 3) == 0) log_state();
    }
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
    gx(0xC8, 0x1FF003FF); gx(0xCC, 0x7FFF);                                 /* lights 0 and 1 */
    gx(0xC8, 0x0FF00200 | 1u << 30); gx(0xCC, 0x03E0 | 1u << 30);
    LOG[0] = 0; LOG[1] = 0; LOG[2] = 0;
    for (u32 frame = 0;; frame++) {
        dln = 0;
        u32 models = 40 + (rnd() & 31);
        for (u32 m = 0; m < models; m++) model(m);
        flushq();
        put(dln);
        send(dl, dln, frame);
        straddle();
        gx(0x48, 1);                                                         /* the straddle's push, popped */
        wait_idle();
        log_state();
        for (int k = 0; k < 16; k++) put(R32(0x04000640 + k * 4));           /* CLIPMTX_RESULT */
        for (int k = 0; k < 4; k++) put(R32(0x04000620 + k * 4));            /* POS_TEST */
        put(R16(0x04000630)); put(R16(0x04000632)); put(R16(0x04000634));    /* VEC_TEST */
        gx(0x140, (frame & 1) << 1);                                        /* SWAP_BUFFERS: Z / W-buffering */
        LOG[0] = frame;
        while (R16(0x04000006) != 192) ;
        while (R16(0x04000006) == 192) ;
    }
}
