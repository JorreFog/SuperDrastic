/* sndbench's ARM9 (build.py): folds the capture buffers (the SPU's mixed output, written by both capture units)
 * into checksums every frame and draws them on the top screen as 4x4 blocks, so two pictures compare the audio. */
typedef unsigned int u32; typedef unsigned short u16; typedef unsigned char u8;
#define R32(a) (*(volatile u32 *)(a))
#define R16(a) (*(volatile u16 *)(a))
#define R8(a) (*(volatile u8 *)(a))
#define VCOUNT R16(0x04000006)
static void wait_vblank(void) { while (VCOUNT >= 192) {} while (VCOUNT < 192) {} }
static void draw(const u32 *v, int n) {
    volatile u16 *fb = (volatile u16 *)0x06800000;
    for (int r = 0; r < n; r++)
        for (int b = 0; b < 32; b++) {
            u16 col = (v[r] >> b) & 1 ? 0x7FFF : (u16)(0x0010 + r * 0x0C00);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) fb[(8 + r * 6 + y) * 256 + 8 + b * 7 + x] = col;
        }
}
/* immediate DMA 3: count units, 32- or 16-bit, source going up (or down) */
static void dma3(u32 src, u32 dst, u32 count, u32 bits32, u32 srcdown) {
    R32(0x040000D4) = src; R32(0x040000D8) = dst;
    R32(0x040000DC) = count | (srcdown ? 1u : 0u) << 23 | bits32 << 26 | 1u << 31;
    while (R32(0x040000DC) & 0x80000000u) {}
}
static u32 fold(u32 h, u32 a, u32 n) {
    for (u32 i = 0; i < n; i += 4) h = (h ^ R32(a + i)) * 0x01000193u;
    return h;
}
int main(void) {
    R32(0x04000304) = 0x8003;
    R8(0x04000240) = 0x80;
    R32(0x04000000) = 0x00020000;
    u32 v[6] = {0};
    for (u32 frame = 1;; frame++) {
        wait_vblank();
        v[0] = fold(0x811C9DC5u, 0x02300000, 0x2000);
        v[1] = fold(0x811C9DC5u, 0x02308000, 0x400);
        v[2] = fold(v[2], 0x02300000, 0x2000);               /* running: every frame's capture so far */
        v[3] = fold(v[3], 0x02308000, 0x400);
        v[4] = R32(0x04000400) ^ R32(0x04000450) ^ R32(0x040004F0);  /* channel status as the CPU sees it */
        v[5] = frame;
        draw(v, 6);
        /* DMA as a game's engine does: the captured audio into VRAM (rows 100-131 of the top screen, 32-bit), into
         * the palettes and OAM (16-bit), main RAM to main RAM both ways, and VRAM, palette and OAM back to main
         * RAM (0x02310000-), so the pictures and the RAM show every copy */
        u32 off = (frame * 0x40) & 0x1FFF;
        dma3(0x02300000 + off, 0x06800000 + 100 * 512, 0x1000, 1, 0);
        dma3(0x02308000 + (off & 0x3FF), 0x06800000 + 116 * 512, 0x1000, 0, 0);
        dma3(0x02300000 + off, 0x05000000, 0x200, 0, 0);
        dma3(0x02302000 + off, 0x07000000, 0x200, 0, 0);
        dma3(0x02304000 + off, 0x05000400, 0x100, 1, 1);
        dma3(0x02300000 + off, 0x02310000, 0x400, 1, 0);
        dma3(0x02306000 - 4, 0x02311000, 0x400, 0, 1);
        dma3(0x05000000, 0x02312000, 0x200, 0, 0);
        dma3(0x07000000, 0x02312400, 0x100, 1, 0);
        dma3(0x06800000 + 100 * 512, 0x02313000, 0x800, 0, 0);
        dma3(0x05000400, 0x02314000, 0x100, 1, 0);
    }
}
