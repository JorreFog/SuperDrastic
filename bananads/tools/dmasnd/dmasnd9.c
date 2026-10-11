/* dmasnd's ARM9 (build.py): every frame a list of triangles, built in main RAM, goes to the geometry engine by
 * DMA: half in GXFIFO mode (start mode 7), half as an immediate DMA to 0x04000400. */
typedef unsigned int u32; typedef unsigned short u16; typedef int s32;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define VCOUNT R16(0x04000006)
#define DMA_SAD(n) R32(0x040000B0 + (n) * 12)
#define DMA_DAD(n) R32(0x040000B4 + (n) * 12)
#define DMA_CNT(n) R32(0x040000B8 + (n) * 12)
#define FRAMES ((volatile u32 *)0x02320000)   /* [0] frames, [1] words sent */

static u32 list[2][4096];

static void wait_vblank(void) { while (VCOUNT >= 192) {} while (VCOUNT < 192) {} }

static u32 rnd = 12345;
static u32 next(void) { rnd = rnd * 1103515245u + 12345u; return rnd >> 8; }

/* n triangles, packed: header BEGIN_VTXS COLOR VTX_16 VTX_16, then COLOR VTX_16 */
static u32 build(u32 *l, u32 n, u32 frame) {
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        s32 cx = (s32)(next() & 0x1FFF) - 0x1000, cy = (s32)(next() & 0x1FFF) - 0x1000;
        s32 d = 0x100 + (s32)((frame * 7 + i * 13) & 0x3FF);
        u32 z = (u32)(-(s32)(i * 4)) & 0xFFFF;
        l[k++] = 0x40 | 0x20 << 8 | 0x23 << 16 | 0x23 << 24;
        l[k++] = 0;                                           /* separate triangles */
        l[k++] = next() & 0x7FFF;
        l[k++] = ((u32)(cx - d) & 0xFFFF) | (u32)(cy - d) << 16; l[k++] = z;
        l[k++] = ((u32)(cx + d) & 0xFFFF) | (u32)(cy - d) << 16; l[k++] = z;
        l[k++] = 0x20 | 0x23 << 8;
        l[k++] = next() & 0x7FFF;
        l[k++] = ((u32)cx & 0xFFFF) | (u32)(cy + d) << 16; l[k++] = z;
    }
    return k;
}

int main(void) {
    R32(0x04000304) = 0x820F;                                 /* LCDs, 2D A+B, 3D, A on top */
    R32(0x04000000) = 0x10000 | (1 << 8) | (1 << 3);          /* BG0 = 3D */
    R32(0x04000060) = 0;
    R32(0x04000350) = 0x0421 | (31u << 16);
    R32(0x04000354) = 0x7FFF;
    R32(0x04000580) = 255u << 16 | 191u << 24;
    R32(0x04000440) = 0; R32(0x04000454) = 0;                 /* projection: identity */
    R32(0x04000440) = 2; R32(0x04000454) = 0;                 /* position and vector: identity */
    R32(0x040004A4) = 0xC0 | 31u << 16;                       /* both sides, opaque */
    for (u32 frame = 0;; frame++) {
        u32 n0 = build(list[0], 120 + (frame & 63), frame);
        u32 n1 = build(list[1], 100, frame + 1);
        DMA_SAD(0) = (u32)list[0]; DMA_DAD(0) = 0x04000400;
        DMA_CNT(0) = n0 | 2u << 21 | 1u << 26 | 7u << 27 | 1u << 31;   /* dest fixed, 32-bit, GXFIFO mode */
        while (DMA_CNT(0) & (1u << 31)) {}
        DMA_SAD(1) = (u32)list[1]; DMA_DAD(1) = 0x04000400;
        DMA_CNT(1) = n1 | 2u << 21 | 1u << 26 | 1u << 31;              /* immediate */
        while (DMA_CNT(1) & (1u << 31)) {}
        R32(0x04000540) = 1;                                  /* SWAP_BUFFERS, manual sort */
        FRAMES[0] = frame; FRAMES[1] += n0 + n1;
        wait_vblank();
    }
}
