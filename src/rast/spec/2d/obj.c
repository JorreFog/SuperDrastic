/* obj.c: exact C ports of DraStic r2.5.2.2's 2D sprite (OBJ) path (key "obj", tools/rast/re2d/obj.md), on DraStic's
 * own engine struct layout, so the tables compare byte for byte: video_2d_obj_affine_setup_edges 0x3e390,
 * video_2d_reorder_obj 0x3e530, render_scanline_obj_c 0x36b70. Integer semantics follow the AArch64 code exactly
 * (32-bit wraps, 16-bit texture coordinates and texel offsets, 32.32 span arithmetic). Verified against the originals
 * in DraStic's process by tools/rast/ut/t_obj2d.c (the whole engine struct after every reorder and frame, the scratch
 * area with guard bytes after every line, the return values), and during the analysis in the running emulator on the
 * ds2d sprite scenes (about 27 000 reorders and 2.7 M lines, 0 differences).
 *
 * ===================================================================================================================
 * WHERE IT RUNS
 * ===================================================================================================================
 *   start_frame (line 0), per engine rendered this frame:   video_2d_reorder_obj(eng)
 *   render_scanline_2d (per DS line): if eng[0xb6] (an OAM write was replayed after the previous line): reorder, then
 *       eng[0xb6] = 0; flags = eng[0x21340 + line]; if DISPCNT.12 (OBJ on):
 *       lmask |= render_scanline_obj_c(eng, S+0xa70, S+0xc90, S+0xe20, S+0xec0, S+0xee0, line)
 *   video_2d_load_savestate: reorder.
 * Sprites are always rendered once per DS line at 256 pixels; the four 2x quarters reuse the line (the 12-sprite
 * full-screen image is the only OBJ content with 2x data, render_scanline_2d handles it). OAM is [eng+0x30] (the real
 * array: line-exact through the event replay), the palette [eng+0x18] + 0x200, the OBJ ext palette [eng+0x28], VRAM
 * through the alias [eng+8] + 0x400000 (engine A) / 0x600000 (engine B, eng[0xb7] == 1).
 *
 * ===================================================================================================================
 * video_2d_reorder_obj(eng)  (0x3e530)
 * ===================================================================================================================
 * Clears the counts (0x3c0 bytes), the line flags (0xc0) and the image pointer, then decodes OAM entries 0..127 into
 * records (obj.h; indexed by OAM number, skipped OBJs keep stale or partly written records) and per-line lists:
 *   dc = DISPCNT; tile shift = dc.4 (1D) ? 5 + dc[21:20] : 5; ext = dc.31 ? [eng+0x28] : NULL (latched here: a
 *   mid-frame DISPCNT change of bits 4-6, 20-22, 31 takes effect at the next reorder)
 *   skip: an OBJ of the full-screen image (below); shape 3; not affine with attr0.9 ("disable"); y > 191 with
 *   y + box height <= 255 (below the screen, not wrapping); x > 255 with x + box width <= 511; alpha-0 bitmap OBJs
 *   (not registered at all); an OBJ at (0, 0) whose (a0, a1, a2) equal those of the last OBJ seen at (0, 0)
 *   (output-neutral: the lower index wins anyway)
 *   affine (a0.8): PA, PB, PC, PD from OAM group (a1 >> 9) & 31 (stored for every affine OBJ). The identity matrix
 *     (PA = PD = 0x100, PB = PC = 0, full s16 values) is drawn as a plain unflipped sprite; with double size centred
 *     and unscaled (y + h/2, x + w/2, and the range tests again). Otherwise kind 8: tx0 = (w/2)*256 - PA*(bw/2),
 *     ty0 = (h/2)*256 - PC*(bw/2) (32-bit), left clip in whole tiles, right clip (263 - x) & ~7, Y = y + bh/2,
 *     setup_edges(tx0, PA, w*256-1, PB) -> X span, setup_edges(ty0, PC, h*256-1, PD) -> Y span
 *   not affine: kind bit 2 = hflip (a1.12); vflip (a1.13): Y = y + h - 1, E.vflip = 1
 *   bitmap (mode 3): attr = 2*alpha + 1; dc.6 (1D, wins over dc.5): off = tile << (7 + dc.22), pitch 2w; dc.5 (2D,
 *     256 wide): off = ((tile >> 5)*256 + (tile & 31))*16, pitch 0x200; else (128 wide) off = ((tile >> 4)*128 +
 *     (tile & 15))*16, pitch 0x100; hflip: off += (w-1)*2; line flag 2; list = priority
 *   tiled: off = tile << tile shift, kind bit 0 = attr0.13 (8bpp); mode 1 (semi-transparent): attr 0x80, line flag 1;
 *     mode 2 (window): list 4; 2D: pitch 0x400, 8bpp ignores tile bit 0; 1D: pitch = tiles * 32 (4bpp) / 64 (8bpp);
 *     hflip: off += (w/8 - 1) * tile bytes
 *   clipping of non-affine OBJs to whole tiles: x < -7: c = (-x) & ~7, x += c, width -= c, off moves c pixels (back
 *     with hflip); x + width > 256: width = (263 - x) & ~7. So x stays in -7..255 and 1..7 pixels may lie right of
 *     255: they are drawn into the line buffers' padding, which nothing reads.
 *   E.vram = [eng+8] + objbase + off; palette 4bpp: pal + 0x200 + palbank*32; 8bpp: ext ? ext + palbank*0x200 : pal +
 *     0x200 (also computed for bitmap OBJs, unused); a registration start > 191 wraps: Y -= 256, start -= 256
 *   for l = start .. start + box height - 1: y = l & 0xff; if y <= 191: append i to list[list][y], flags[y] |= flag
 * The full-screen image (only when (dc & 0x60) == 0x20, 2D 256-wide bitmap mapping): OBJs with y <= 191, x and y
 * multiples of 64, x < 256, a0 & 0xff00 == 0x0c00, a1 & 0xfe00 == 0xc000, alpha 15; base = tile - (row*256 + col*8),
 * prio = a2 bits 10-11; OBJ 0 fixes (prio, base) ONLY if it is OBJ number 0 (otherwise every candidate needs prio 0,
 * base 0); the first OBJ per cell counts. When all 12 cells are filled: bank = the LAST of A..D whose record
 * [video+0x10+16b] == 6 and [video+0x18+16b] << 14 == objbase (none: no image, the excluded list is dropped);
 * w1 = (base & 31) + (base >> 5)*256; [eng+0x21400] = [eng+8] + objbase + w1*16, [eng+0x21410] = prio,
 * [eng+0x21408] = (w1*8 <= 0x4000 and the six 16 KiB blocks from (w1*16) >> 14 valid in C+0x20+bank) ? [C+8*bank] +
 * w1*48 : NULL, C = video+0x458820. The 12 OBJs are left out of the lists.
 *
 * ===================================================================================================================
 * video_2d_obj_affine_setup_edges(t0, dA, lim, dB, &start, &step, &len)  (0x3e390)
 * ===================================================================================================================
 * The box pixels i whose texture coordinate t0 + i*dA + n*dB lies in [0, lim], as a span [start + n*step, start +
 * n*step + len] (32.32, n = line - the box centre). Exact 64-bit ceilings; numerators formed in 32 bits:
 *   dA > 0:  start = ceil(((dA-1-t0) << 32) / dA),        len = ceil(((lim-t0) << 32) / dA) - start
 *   dA < 0:  start = ceil(((dA+lim+1-t0) << 32) / dA),    len = ceil(((-t0) << 32) / dA) - start
 *   dA != 0: step = ceil(((-dB) << 32) / dA)
 *   dA = 0, dB = 0: inside = 0 <= t0 <= lim: start = inside ? 0 : -1, len = inside ? 128 << 32 : 0, step = 0
 *   dA = 0, dB != 0: q1 = trunc((dB > 0 ? lim - t0 : -t0) / dB), q0 = trunc((dB > 0 ? -t0 : lim - t0) / dB) (32-bit)
 *                    start = (-128 q1) << 32, len = ((-128 q0) << 32) - start + (128 << 32), step = 128 << 32
 * Traps (where this differs from the per-pixel rule, all on lines above the box centre): step is rounded up, so
 * n*step for n < 0 is rounded down: when an exact edge is an integer the span's left end moves one pixel left (an
 * extra pixel whose coordinate is one 8.8 unit outside the texture, fetched through the 16-bit offset wrap) and its
 * right end one pixel left; the dA = 0 span end is inclusive, so box pixel 0 is drawn on the line just before the
 * first visible line.
 *
 * ===================================================================================================================
 * render_scanline_obj_c(eng, col, alpha, vis, semi, bmp, line) -> 0 / 0x10  (0x36b70)
 * ===================================================================================================================
 *   for l = 4, 3, 2, 1, 0 (the window list first, then priority 3 .. 0):
 *       n = count[l][line]; n == 0: vis + l*32 = 32 zero bytes; continue
 *       buf[256] = 0 (a stack index line, 8 spare bytes either side); draw list[l][line][k] for k = n-1 .. 0 (the
 *       highest OAM number first, so the lowest is on top); vis + l*32 = bitmap(buf[i] != 0); return value 0x10
 *   line flags != 0: semi = bitmap(alpha[i] & 0x80), bmp = bitmap((alpha[i] & 0x3f) != 0), alpha[i] &= 0x3f (all 256)
 *   else: semi = bmp = 0 (alpha untouched)
 * draw, for every opaque texel at screen pixel p (-7 .. 263): buf[p] = index (bitmaps: colour >> 8), col[p] = the
 * colour (palette u16 as stored, or the bitmap u16), alpha[p] = E.attr. Pixels no OBJ covers keep their old col and
 * alpha bytes (never consumed: every consumer masks with this line's OBJ bitmaps).
 *   non-affine: row = E.vflip ? Y - line : line - Y (u32); whole tiles of 8 pixels:
 *     4bpp  u32 at vram + (row >> 3)*pitch + (row & 7)*4 +- t*32, nibble k (7-k with hflip); colour pal[index]
 *     8bpp  the 8 bytes at vram + (row >> 3)*pitch + (row & 7)*8 +- t*64, byte k (7-k with hflip)
 *     bitmap u16 at vram + row*pitch (u32 product) +- 2k; opaque = bit 15
 *   affine (kind 8 4bpp, 9 8bpp, 10 bitmap): dy = line - Y, += 256 if < -192 (boxes wrapping past line 255);
 *     xs = XS + dy*XSTEP, ys = YS + dy*YSTEP (s64); first = max(xs >> 32, ys >> 32, 0); last = min((xs + XLEN) >> 32,
 *     (ys + YLEN) >> 32, width - 1); nothing if last < first;
 *     tx = (s16)(TX0 + dy*PB + first*PA), ty = (s16)(TY0 + dy*PD + first*PC), then += PA / PC per pixel (16-bit)
 *     8:  off = (u16)(((tx >> 11) << 5) + (ty >> 11)*pitch + ((ty >> 8) & 7)*4 + ((tx >> 9) & 3)),
 *         index = (vram[off] >> ((tx >> 8) & 1)*4) & 15
 *     9:  off = (u16)(((tx >> 11) << 6) + (ty >> 11)*pitch + ((ty >> 8) & 7)*8 + ((tx >> 8) & 7)), index = vram[off]
 *     10: off = (u16)((tx >> 8)*2 + (ty >> 8)*pitch), colour = u16 at vram + off
 *     (>> arithmetic on the s16 coordinates; the u16 offset keeps every fetch within 64 KiB after E.vram)
 * DraStic-specific: OBJ-vs-OBJ order is priority first, then OAM index (the DS shows the lowest OAM index whatever
 * its priority); no OBJ mosaic (attr0.12 and MOSAIC bits 8-15 are never read); no per-line OBJ limit (DISPCNT.23 is
 * ignored); kinds 3, 7 and > 10 cannot occur and draw nothing. */
#include <string.h>
#include "obj.h"

#define U8(p, o)  (*(uint8_t *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define S16(p, o) (*(int16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define S64(p, o) (*(int64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) (*(uint8_t **)((uint8_t *)(p) + (o)))

static const uint8_t obj_size[12][2] = {      /* obj_size_table 0x11def0: (width, height) by shape*4 + size */
    {8, 8}, {16, 16}, {32, 32}, {64, 64}, {16, 8}, {32, 8}, {32, 16}, {64, 32}, {8, 16}, {8, 32}, {16, 32}, {32, 64}};

static int64_t sh32(int32_t v) { return (int64_t)((uint64_t)(uint32_t)v << 32); }

/* the exact ceiling of n/d (every division in setup_edges rounds up; d != 0) */
static int64_t cdiv(int64_t n, int64_t d) {
    if (d > 0) return n >= 0 ? (n + d - 1) / d : n / d;
    return n >= 0 ? n / d : (n + d + 1) / d;
}

void spec_video_2d_obj_affine_setup_edges(int32_t t0, int32_t dA, int32_t lim, int32_t dB, int64_t *start, int64_t *step,
                                          int64_t *len) {
    if (dA == 0) {
        if (dB == 0) {
            int in = t0 >= 0 && lim >= t0;
            *start = in ? 0 : -1;
            *len = in ? (int64_t)128 << 32 : 0;
            *step = 0;
        } else {
            int32_t a = (int32_t)((uint32_t)lim - (uint32_t)t0);
            int32_t nt = (int32_t)(0u - (uint32_t)t0);
            int32_t q1 = (dB > 0 ? a : nt) / dB, q0 = (dB <= 0 ? a : nt) / dB;   /* sdiv: truncation */
            int64_t s = sh32((int32_t)((uint32_t)q1 * (uint32_t)-128));
            int64_t e = sh32((int32_t)((uint32_t)q0 * (uint32_t)-128));
            *start = s;
            *len = e - s + ((int64_t)128 << 32);
            *step = (int64_t)128 << 32;
        }
        return;
    }
    int32_t n1, n0;
    if (dA > 0) {
        n1 = (int32_t)((uint32_t)dA - 1u - (uint32_t)t0);
        n0 = (int32_t)((uint32_t)lim - (uint32_t)t0);
    } else {
        n1 = (int32_t)((uint32_t)dA + (uint32_t)lim + 1u - (uint32_t)t0);
        n0 = (int32_t)(0u - (uint32_t)t0);
    }
    int64_t s = cdiv(sh32(n1), dA);
    *start = s;
    *len = cdiv(sh32(n0), dA) - s;
    *step = cdiv(sh32((int32_t)(0u - (uint32_t)dB)), dA);
}

/* the 12-sprite full-screen bitmap image (0x3ecdc..0x3ee64): the image fields when found; returns the number of OBJ
 * numbers put in excl[] (0 when there is no image) */
static int fullscreen(uint8_t *eng, const uint16_t *oam, uint32_t objbase, uint8_t *excl) {
    uint32_t found = 0, prio0 = 0, base0 = 0;
    int n = 0;
    for (int i = 0; i < 128; i++) {
        uint32_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        uint32_t y = a0 & 0xff;
        if (!(y <= 191 && ((a1 & 0x13f) | (a0 & 0x3f)) == 0)) continue;   /* x, y multiples of 64, x < 256 */
        if ((a0 & 0xff00) != 0x0c00) continue;      /* bitmap mode, not affine / disabled, no mosaic, 4bpp, square */
        if ((a1 & 0xfe00) != 0xc000) continue;      /* 64x64, no flips */
        if ((a2 & 0xf000) != 0xf000) continue;      /* alpha 15 */
        uint32_t row = y >> 6, col = (a1 >> 6) & 7;
        uint32_t base = (a2 & 0x3ff) - ((row << 8) | (col << 3));
        uint32_t prio = (a2 >> 10) & 3;
        if (i == 0) { prio0 = prio; base0 = base; }
        else if (prio != prio0 || base != base0) continue;
        uint32_t bit = (1u << col) << (row << 2);
        if (!(bit & found)) { found |= bit; excl[n++] = (uint8_t)i; }
        if (found == 0xfff) {
            uint8_t *video = PTR(eng, 0x00);
            int bank = 0xff;
            for (int b = 0; b < 4; b++)                 /* the last bank of A..D mapped as this engine's OBJ VRAM */
                if (U32(video, 0x10 + b * 16) == 6 && objbase == U32(video, 0x18 + b * 16) << 14) bank = b;
            if (bank == 0xff) return 0;
            uint32_t w1 = (base0 & 0x1f) + ((base0 >> 5) << 8);
            uint32_t off = w1 << 4;
            PTR(eng, OBJT_IMAGE2X) = NULL;
            if ((w1 << 3) <= 0x4000) {                  /* the six 16 KiB blocks hold valid hi-res capture data */
                uint32_t valid = U8(video, 0x458840 + bank);
                uint32_t mask = 0x3fu << ((off >> 14) & 31);
                if ((mask & ~valid) == 0)
                    PTR(eng, OBJT_IMAGE2X) = PTR(video, 0x458820 + bank * 8) + ((uint64_t)(uint32_t)(w1 * 24) << 1);
            }
            PTR(eng, OBJT_IMAGE) = PTR(eng, 0x08) + objbase + (uint64_t)off;
            U8(eng, OBJT_IMAGEPRIO) = (uint8_t)prio0;
            return n;
        }
    }
    return 0;
}

void spec_video_2d_reorder_obj(uint8_t *eng) {
    uint32_t dc = U32(eng, 0x90);
    const uint16_t *oam = (const uint16_t *)PTR(eng, 0x30);
    uint8_t *objpal = PTR(eng, 0x18) + 0x200;
    uint32_t tshift = (dc & 0x10) ? 5 + ((dc >> 20) & 3) : 5;
    uint32_t objbase = U8(eng, 0xb7) == 1 ? 0x600000 : 0x400000;
    uint8_t *extpal = (dc & 0x80000000u) ? PTR(eng, 0x28) : NULL;
    uint32_t bmpshift = 7 + ((dc >> 22) & 1);
    memset(eng + OBJT_COUNTS, 0, 0x3c0);       /* per-line counts, 5 lists x 192 */
    memset(eng + OBJT_LINEFLAGS, 0, 0xc0);     /* per-line flags */
    PTR(eng, OBJT_IMAGE) = NULL;
    uint8_t excl[16];
    int nex = 0;
    if ((dc & 0x60) == 0x20) nex = fullscreen(eng, oam, objbase, excl);
    excl[nex] = 0xff;
    uint32_t last0 = 0xffff, last1 = 0xffff, last2 = 0xffff;   /* the (0, 0) duplicate filter */
    int ei = 0;
    for (int i = 0; i < 128; i++) {
        uint8_t *E = eng + OBJT_RECORDS + i * OBJE_SIZE;
        uint32_t a0 = oam[i * 4], a1 = oam[i * 4 + 1], a2 = oam[i * 4 + 2];
        if (excl[ei] == i) { ei++; continue; }
        uint32_t mode = (a0 >> 10) & 3, prio = (a2 >> 10) & 3;
        if ((a0 >> 14) == 3) continue;
        if ((a0 & 0x300) == 0x200) continue;
        uint32_t idx = (((a0 >> 14) << 2) | (a1 >> 14)) & 15;
        int32_t w = obj_size[idx][0], h = obj_size[idx][1];
        uint32_t dbl = a0 & 0x200;
        int32_t bh = dbl ? 2 * h : h, bw = dbl ? 2 * w : w;
        int32_t y = a0 & 0xff, ystart = y, x;
        if ((uint32_t)y > 191) {
            if ((uint32_t)(bh + y) <= 255) continue;
            x = a1 & 0x1ff;
        } else {
            x = a1 & 0x1ff;
            if ((y | x) == 0) {
                if (a0 == last0 && a1 == last1 && a2 == last2) continue;
                last0 = a0; last1 = a1; last2 = a2;
            }
        }
        if (x > 255 && x + bw <= 0x1ff) continue;
        x = (int32_t)((uint32_t)x << 23) >> 23;                /* sign-extend 9 bits */
        S16(E, OBJE_Y) = (int16_t)y;
        U8(E, OBJE_ATTR) = 0;
        U8(E, OBJE_VFLIP) = 0;
        uint32_t fl;                                           /* the kind: bit0 8bpp, bit1 bitmap, bit2 hflip, bit3 affine */
        if (a0 & 0x100) {
            const uint8_t *g = (const uint8_t *)oam + ((a1 >> 9) & 0x1f) * 32;
            int32_t pa = *(const int16_t *)(g + 6), pb = *(const int16_t *)(g + 0xe);
            int32_t pc = *(const int16_t *)(g + 0x16), pd = *(const int16_t *)(g + 0x1e);
            S16(E, OBJE_PA) = (int16_t)pa; S16(E, OBJE_PC) = (int16_t)pc;
            S16(E, OBJE_PB) = (int16_t)pb; S16(E, OBJE_PD) = (int16_t)pd;
            int32_t hbw = bw >> 1, hbh = bh >> 1;
            if (pa == 0x100 && pd == 0x100 && (pb | pc) == 0) {
                fl = 0;
                if (dbl) {                                     /* identity, double size: a plain sprite, centred */
                    int32_t q = bh >> 2;
                    S16(E, OBJE_Y) = (int16_t)(y + q);
                    x = (int16_t)(x + (bw >> 2));
                    ystart = q + y;
                    if ((uint32_t)ystart > 191 && (uint32_t)(ystart + hbh) <= 255) continue;
                    if (x > 255) continue;
                    if (x + hbw <= 0) continue;
                    bw = hbw; bh = hbh;
                }
            } else {                                           /* rotation / scaling */
                int32_t tw8 = hbw << 8, th8 = hbh << 8, tx0, ty0;
                if (dbl) { tx0 = tw8 >> 1; ty0 = th8 >> 1; }
                else { tx0 = tw8; ty0 = th8; th8 = bh << 8; tw8 = bw << 8; }
                tx0 = (int32_t)((uint32_t)tx0 - (uint32_t)pa * (uint32_t)hbw);
                ty0 = (int32_t)((uint32_t)ty0 - (uint32_t)pc * (uint32_t)hbw);
                if (x < -7) {
                    int32_t c = (-x) & ~7;
                    x = (int16_t)(x + c); bw -= c;
                    tx0 = (int32_t)((uint32_t)tx0 + (uint32_t)pa * (uint32_t)c);
                    ty0 = (int32_t)((uint32_t)ty0 + (uint32_t)pc * (uint32_t)c);
                }
                if (x + bw > 256) bw = (263 - x) & ~7;
                S16(E, OBJE_TX0) = (int16_t)tx0;
                S16(E, OBJE_TY0) = (int16_t)ty0;
                S16(E, OBJE_Y) = (int16_t)(y + hbh);
                int64_t s, st, l;
                spec_video_2d_obj_affine_setup_edges(tx0, pa, tw8 - 1, pb, &s, &st, &l);
                S64(E, OBJE_XS) = s; S64(E, OBJE_XLEN) = l; S64(E, OBJE_XSTEP) = st;
                spec_video_2d_obj_affine_setup_edges(ty0, pc, th8 - 1, pd, &s, &st, &l);
                S64(E, OBJE_YS) = s; S64(E, OBJE_YLEN) = l; S64(E, OBJE_YSTEP) = st;
                fl = 8;
            }
        } else {
            fl = ((a1 >> 12) & 1) << 2;
            if (a1 & 0x2000) {
                S16(E, OBJE_Y) = (int16_t)(y - 1 + bh);
                U8(E, OBJE_VFLIP) = 1;
            }
        }
        uint32_t off, list, lflag;
        if (mode == 3) {                                       /* bitmap sprite */
            uint32_t tile = a2 & 0x3ff, alpha = a2 >> 12;
            if (alpha == 0) continue;
            U8(E, OBJE_ATTR) = (uint8_t)(alpha * 2 + 1);
            if (dc & 0x40) { off = tile << bmpshift; U16(E, OBJE_PITCH) = (uint16_t)(w + w); }
            else if (dc & 0x20) { off = (((tile >> 5) << 8) + (a2 & 0x1f)) << 4; U16(E, OBJE_PITCH) = 0x200; }
            else { off = (((tile >> 4) << 7) + (a2 & 0xf)) << 4; U16(E, OBJE_PITCH) = 0x100; }
            if (!(fl & 8)) {
                if (fl & 4) {
                    off += (uint32_t)(w - 1) << 1;
                    if (x < -7) { int32_t c = (-x) & ~7; x = (int16_t)(x + c); bw -= c; off -= (uint32_t)c << 1; }
                } else {
                    if (x < -7) { int32_t c = (-x) & ~7; x = (int16_t)(x + c); bw -= c; off += (uint32_t)c << 1; }
                }
                if (x + bw > 256) bw = (263 - x) & ~7;
            }
            fl |= 2;
            lflag = 2;
            list = prio;
        } else {                                               /* tiled sprite */
            off = (a2 & 0x3ff) << tshift;
            fl |= (a0 >> 13) & 1;
            if (mode == 1) { U8(E, OBJE_ATTR) = 0x80; list = prio; lflag = 1; }
            else { list = mode == 2 ? 4 : prio; lflag = 0; }
            uint32_t eightbpp = fl & 1, hflip = fl & 4;
            if (!(dc & 0x10)) {                                /* 2D mapping */
                U16(E, OBJE_PITCH) = 0x400;
                uint32_t n = (uint32_t)(w >> 3) - 1;
                if (eightbpp) { off &= ~0x20u; if (hflip) off += n << 6; }
                else if (hflip) off += n << 5;
            } else {                                           /* 1D mapping */
                uint32_t tiles = (uint32_t)w >> 3, n = tiles - 1;
                if (eightbpp) { if (hflip) off += n << 6; U16(E, OBJE_PITCH) = (uint16_t)((tiles & 0x3ff) << 6); }
                else { if (hflip) off += n << 5; U16(E, OBJE_PITCH) = (uint16_t)((tiles & 0x7ff) << 5); }
            }
            if (!(fl & 8)) {
                if (x < -7) {
                    int32_t c = (-x) & ~7;
                    uint32_t bytes = eightbpp ? (uint32_t)c << 3 : (uint32_t)c << 2;
                    x = (int16_t)(x + c); bw -= c;
                    off = hflip ? off - bytes : off + bytes;
                }
                if (x + bw > 256) bw = (263 - x) & ~7;
            }
        }
        if (bw == 0) continue;
        S16(E, OBJE_X) = (int16_t)x;
        PTR(E, OBJE_VRAM) = PTR(eng, 0x08) + objbase + (uint64_t)off;
        U8(E, OBJE_KIND) = (uint8_t)fl;
        U8(E, OBJE_WIDTH) = (uint8_t)bw;
        uint32_t pb = a2 >> 12;
        if (a0 & 0x2000) PTR(E, OBJE_PAL) = extpal ? extpal + ((pb & 15) << 9) : objpal;
        else PTR(E, OBJE_PAL) = objpal + ((pb & 15) << 5);
        if (ystart > 191) { S16(E, OBJE_Y) = (int16_t)(S16(E, OBJE_Y) - 0x100); ystart -= 0x100; }
        int32_t end = ystart + bh;
        for (int32_t l = ystart; l != end; l++) {
            uint32_t yy = (uint32_t)l & 0xff;
            if (yy > 191) continue;
            uint8_t *cnt = eng + OBJT_COUNTS + list * 0xc0 + yy;
            eng[OBJT_LISTS + list * 0x6000 + yy * 0x80 + *cnt] = (uint8_t)i;
            (*cnt)++;
            eng[OBJT_LINEFLAGS + yy] |= (uint8_t)lflag;
        }
    }
}

/* one opaque texel of an OBJ at screen pixel px */
#define PUT(px, v, c)                                                             \
    do {                                                                          \
        int _p = (px);                                                            \
        buf[_p] = (uint8_t)(v); col[_p] = (c); alpha[_p] = attr;                  \
    } while (0)

static void draw_obj(const uint8_t *E, int line, uint8_t *buf, uint16_t *col, uint8_t *alpha) {
    uint32_t fl = U8(E, OBJE_KIND), attr = U8(E, OBJE_ATTR), width = U8(E, OBJE_WIDTH), pitch = U16(E, OBJE_PITCH);
    int x = S16(E, OBJE_X);
    const uint8_t *pal = PTR(E, OBJE_PAL), *vram = PTR(E, OBJE_VRAM);
    if (fl & 8) {                                              /* affine: 8 = 4bpp, 9 = 8bpp, 10 = bitmap */
        if (fl != 8 && fl != 9 && fl != 10) return;
        int32_t dy = line - S16(E, OBJE_Y);
        if (dy < -192) dy += 256;
        int64_t xs = S64(E, OBJE_XS) + (int64_t)dy * S64(E, OBJE_XSTEP);
        int64_t ys = S64(E, OBJE_YS) + (int64_t)dy * S64(E, OBJE_YSTEP);
        int64_t xe = xs + S64(E, OBJE_XLEN), ye = ys + S64(E, OBJE_YLEN);
        int32_t xsi = (int32_t)(xs >> 32), ysi = (int32_t)(ys >> 32), xei = (int32_t)(xe >> 32), yei = (int32_t)(ye >> 32);
        int32_t first = ysi > xsi ? ysi : xsi;
        int32_t last = yei < xei ? yei : xei;
        if (first < 0) first = 0;
        if ((int32_t)width <= last) last = (int32_t)width - 1;
        int32_t cnt = (int32_t)((uint32_t)last - (uint32_t)first + 1u);
        if (cnt <= 0) return;
        uint32_t udy = (uint32_t)dy & 0xffff, uf = (uint32_t)first & 0xffff;
        uint32_t pa = U16(E, OBJE_PA), pc = U16(E, OBJE_PC), pb = U16(E, OBJE_PB), pd = U16(E, OBJE_PD);
        int16_t tx = (int16_t)(U16(E, OBJE_TX0) + udy * pb + pa * uf);
        int16_t ty = (int16_t)(U16(E, OBJE_TY0) + udy * pd + pc * uf);
        for (int32_t k = 0; k < cnt; k++) {
            int p = x + first + k;
            int32_t txs = tx, tys = ty;                    /* arithmetic shifts of the 16-bit coordinates */
            if (fl == 10) {
                uint16_t o = (uint16_t)((uint32_t)(txs >> 8) * 2u + (uint32_t)(tys >> 8) * pitch);
                uint16_t c = *(const uint16_t *)(vram + o);
                if (c & 0x8000) PUT(p, c >> 8, c);
            } else if (fl == 8) {
                uint16_t o = (uint16_t)(((uint32_t)(txs >> 11) << 5) + (uint32_t)(tys >> 11) * pitch +
                                        (((uint32_t)tys >> 8) & 7) * 4 + (((uint32_t)txs >> 9) & 3));
                uint32_t n = (vram[o] >> ((((uint32_t)txs >> 8) & 1) << 2)) & 0xf;
                if (n) PUT(p, n, *(const uint16_t *)(pal + n * 2));
            } else {
                uint16_t o = (uint16_t)(((uint32_t)(txs >> 11) << 6) + (uint32_t)(tys >> 11) * pitch +
                                        (((uint32_t)tys >> 8) & 7) * 8 + (((uint32_t)txs >> 8) & 7));
                uint32_t n = vram[o];
                if (n) PUT(p, n, *(const uint16_t *)(pal + n * 2));
            }
            tx = (int16_t)(tx + (int16_t)pa);
            ty = (int16_t)(ty + (int16_t)pc);
        }
        return;
    }
    int32_t Y = S16(E, OBJE_Y);
    uint32_t row = (uint32_t)(U8(E, OBJE_VFLIP) ? Y - line : line - Y);
    if (fl & 2) {                                              /* bitmap: 2 normal, 6 hflip */
        if (fl != 2 && fl != 6) return;
        const uint16_t *s = (const uint16_t *)(vram + (uint64_t)(uint32_t)(row * pitch));
        for (uint32_t k = 0; k < width; k++) {
            uint16_t c = fl == 2 ? s[k] : *(const uint16_t *)((const uint8_t *)s - 2 * (int64_t)k);
            if (c & 0x8000) PUT(x + (int)k, c >> 8, c);
        }
        return;
    }
    if (fl != 0 && fl != 1 && fl != 4 && fl != 5) return;   /* 3 cannot occur */
    uint32_t eight = fl & 1, hflip = fl & 4;
    const uint8_t *s = vram + (uint64_t)(uint32_t)((row >> 3) * pitch) + ((row & 7) << (eight ? 3 : 2));
    uint32_t tiles = ((width - 1) >> 3) + 1;
    for (uint32_t t = 0; t < tiles; t++) {
        for (int k = 0; k < 8; k++) {
            uint32_t n;
            if (!eight) {
                uint32_t wd = *(const uint32_t *)(hflip ? s - (int64_t)t * 32 : s + t * 32);
                n = (wd >> ((hflip ? 7 - k : k) * 4)) & 0xf;
            } else {
                const uint8_t *b = hflip ? s - (int64_t)t * 64 : s + t * 64;
                n = b[hflip ? 7 - k : k];
            }
            if (n) PUT(x + (int)(t * 8) + k, n, *(const uint16_t *)(pal + n * 2));
        }
    }
}

static void pack_bits(const uint8_t *b, uint8_t *out) {   /* bit i = (b[i] != 0), LSB first */
    for (int j = 0; j < 32; j++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) v |= (uint8_t)((b[j * 8 + k] != 0) << k);
        out[j] = v;
    }
}

uint32_t spec_render_scanline_obj_c(uint8_t *eng, uint16_t *col, uint8_t *alpha, uint8_t *vis, uint8_t *semi, uint8_t *bmp,
                                    uint32_t line) {
    uint32_t ret = 0;
    uint8_t bufmem[256 + 16];
    uint8_t *buf = bufmem + 8;                     /* DraStic: sp+0x258, 8 spare bytes on each side */
    for (int list = 4; list >= 0; list--) {
        uint8_t *out = vis + list * 32;
        uint32_t n = eng[OBJT_COUNTS + list * 0xc0 + line];
        if (!n) { memset(out, 0, 32); continue; }
        memset(buf, 0, 256);
        const uint8_t *ids = eng + OBJT_LISTS + list * 0x6000 + line * 0x80;
        for (int k = (int)n - 1; k >= 0; k--) draw_obj(eng + OBJT_RECORDS + ids[k] * OBJE_SIZE, (int)line, buf, col, alpha);
        pack_bits(buf, out);
        ret = 0x10;
    }
    if (eng[OBJT_LINEFLAGS + line]) {
        uint8_t s[256], b[256];
        for (int i = 0; i < 256; i++) { s[i] = alpha[i] & 0x80; b[i] = alpha[i] & 0x3f; alpha[i] &= 0x3f; }
        pack_bits(s, semi);
        pack_bits(b, bmp);
    } else {
        memset(semi, 0, 32);
        memset(bmp, 0, 32);
    }
    return ret;
}
