/* bg.c: exact C ports of DraStic r2.5.2.2's BG layer renderers (key "bg", tools/rast/re2d/bg.md), on DraStic's own
 * engine and layer struct layout. Verified against the originals in DraStic's process by tools/rast/ut/t_bg2d.c (the
 * bitmaps and guard bytes byte for byte, every pixel the port writes, the layer and engine structs afterwards), and
 * during the analysis end to end on 4.96 M lines of the ds2d scenes (every BG line of engine A, 0 differences).
 *
 * ===================================================================================================================
 * WHERE IT RUNS
 * ===================================================================================================================
 * render_scanline_2d (0x3ef00) calls render_scanline_bg(eng, S + 0x1e0, S + 0xda0, line) once per DS line, in 1x and
 * in 2x mode alike (the four 2x quarters reuse the 1x line: BGs are never rendered at 2x). S is render_scanline_2d's
 * scratch area (its stack frame + 0x180). Engine B runs the same code on video_render_thread.
 *
 * Output of every layer renderer, for BG n:
 *   buf = lines + n*0x220 + 0x10: u16[256], the palette RAM entry (or the VRAM pixel of a 16-bit bitmap) AS STORED,
 *         bit 15 included. A slot is 8 + 256 + 8 u16; the original renderers scribble into the padding (text 4bpp
 *         and 8bpp ext: bytes -14..527 relative to buf; text 8bpp: the index line left at bytes 512..535, up to 8
 *         bytes into the next slot's padding; affine: 512..541; affine lines also get up to 7 junk entries past the
 *         drawn span). Nothing reads those bytes; the ports write buf[0..255] only, the same values the originals
 *         write there, and only where the original writes too.
 *   vis = vis + n*32: 256 bits, bit x (byte x >> 3, bit x & 7) = pixel x is opaque (index != 0; 16-bit bitmap: bit 15).
 * Only buf[x] where vis bit x is set is observable downstream (the compositor selects by the bitmaps), and both are
 * exact. There is no BGR555 conversion, no 6-bit step and no "bit 15 = opaque" convention in BG lines.
 *
 * Inputs: the layer struct (bg.h's BGL_* offsets: the renderer at L+0x30 installed by video_2d_update_bg_mode, the
 * bases, sizes and registers set by the event replay), the render list eng+0x8c (count eng+0xb2: the enabled BGs in
 * priority order, BG0 excluded when it is the 3D layer), MOSAIC eng+0xa8, VRAM through the linear alias [L+8] (no
 * masking to the engine's BG area: offsets up to map + 0x3f3f, char + 64 KiB, bitmap + 512 KiB, bitmap offsets 32-bit
 * sign-extended), the palette [L+0x10], the ext palette slot [L+0x18] (bank memory). Side effects on the struct: the
 * clip edges, L+0x4c/0x50 and L+0xae (affine and bitmap renderers); render_scanline_bg restores VOFS / X / Y after
 * mosaic. No caches: each line re-reads map entries, tiles and palettes.
 *
 * Which renderer (video_2d_update_bg_mode 0x41b60, by DISPCNT mode; BG0/BG1 always text except BG1 in mode 6):
 *      mode 0: text text text text   1: text text text affine   2: text text affine affine
 *      3: text text text ext(3)      4: text text affine ext(3)  5: text text ext(2) ext(3)
 *      6: text null bitmap_8bpp(large, base 0) null              7: BG2/BG3 keep their renderer
 * ext(c) = BGcCNT.7 ? (BGcCNT.2 ? bitmap_16bpp : bitmap_8bpp) : affine_extended. Engine B keeps the mode bits.
 *
 * ===================================================================================================================
 * render_scanline_tiled_ext(L, buf, vis, line)  (0xa1d20 + setup_tile_map_entries_{4,8}bpp_asm, span_*_asm,
 *                                                 set_visibility_{4,8,12}bpp_asm, palette_lookup_{8,12}bpp_asm)
 * ===================================================================================================================
 *   y = VOFS + line (only bits 0..8 count: the map is at most 512 tall);  m = L+0x38
 *   CNT.15 (512 tall) and y & 0x100:  m += CNT.14 ? 0x1000 : 0x800            the lower 256 rows
 *   row = m + ((y & 0xf8) << 3);  A = B = row;  CNT.14 (512 wide): HOFS < 256 ? B = row + 0x800 : A = row + 0x800
 *   t = (HOFS >> 3) & 31, fine = HOFS & 7, fy = y & 7;  33 tiles k = 0..32: e = t+k < 32 ? A[t+k] : B[t+k-32] (u16)
 *   tile = e & 0x3ff, hflip = e.10, vflip = e.11, pal = e >> 12, r = vflip ? 7 - fy : fy
 *   4bpp: index = nibble (hflip ? 7-p : p) of the u32 at chr + tile*32 + r*4;   colour = PAL[pal*16 + index]
 *   8bpp: index = chr[(tile*64 + r*8 + (hflip ? 7-p : p)) & 0xffff];  colour = ext ? EXT[pal << 8 | index] : PAL[index]
 *   tile pixel 8k + p lands on screen x = 8k + p - fine;  vis bit x = index != 0;  buf[x] = colour (also for index 0)
 * chr = [L+8] + L+0x3c. Ext palettes apply iff CNT.7 and L+0xad. Trap: with ext palettes on and the slot pointer
 * NULL (possible only before any ext palette bank was mapped since reset) the original returns WITHOUT WRITING:
 * buf and vis keep what the slot held (the same layer's previous line on that thread).
 *
 * ===================================================================================================================
 * render_scanline_affine_normal_ext / _extended_ext(L, buf, vis, line)  (0xa4730 / 0xa4b90; line unused)
 * ===================================================================================================================
 * Per pixel (the rule the original's tile-run pipeline reproduces, bg.md 4.1/4.4):
 *   sx = (X + PA*x) >> 8, sy = (Y + PC*x) >> 8 (32-bit, arithmetic), X = L+0x90, Y = L+0x94; size = (L+0xab + 1)*8
 *   normal:   e = map[(sx >> 3) + ((sy >> 3) << L+0xac)] (u8);  index = chr[e*64 + (sy & 7)*8 + (sx & 7)]
 *   extended: e = map16[..]; o = (sx & 7) | (sy & 7) << 3, ^7 if e.10, ^0x38 if e.11;  index = chr[(e & 0x3ff)*64 + o]
 *   colour = PAL[index], or EXT[(e >> 12) << 8 | index] (extended with ext palettes on);  vis = index != 0
 * Wrap mode (CNT.13): sx, sy & (size - 1), all 256 pixels. Clip mode: only the span of the clip edges (below).
 * The original works in tile runs: run boundaries from the per-axis tile crossings ((v + k*R) >> 20 with R = L+0x4c
 * / L+0x50, merged), the map entry looked up at each run's first pixel, the in-tile offset per pixel. bg.md 4.4 shows
 * the run method equals per-pixel evaluation; the port keeps the runs (crossings, merge) as the original computes them.
 * Traps:
 *   - The general path: wrap mode with |PA| or |PC| > 2047 (((P + 0x7ff) & 0xffff) > 0xffe) evaluates map indexes
 *     per pixel, but setup_tile_offsets_asm writes 288 bytes into the 256-byte offset array and overwrites the map
 *     indexes of pixels 0..15: pixel i < 16 uses map index off(256+2i) | off(257+2i) << 8 (off = the in-tile offset of
 *     that pixel number; a byte offset for the extended variant). Clip mode always takes the run method.
 *   - Clip edges (render_scanline_update_affine_variables, video_2d_bg_layer_affine_setup_edges, clip_span below):
 *     recomputed only when L+0xae is set (line 0 for BG2/BG3, PA..PD / X / Y writes, savestate load), at the top of
 *     every affine render, wrap or clip; then stepped once per line the layer renders in CLIP mode (before any early
 *     exit). A line where the layer is not rendered, is in wrap mode or returns for a NULL ext slot leaves the edges
 *     behind while X / Y move on: the window lags until the next recompute (history-dependent output).
 *   - NULL ext slot (extended with ext palettes on): returns before anything, edges not stepped, buf / vis stale.
 *   - Degenerate axes (PA or PC = 0): integer line counts with truncating division, see setup_edges.
 *
 * ===================================================================================================================
 * video_2d_bg_layer_affine_setup_edges(X, P, W, Q, &start, &step, &width)  (0x326f0)
 * render_scanline_update_affine_variables(L)  (0x34550)
 * ===================================================================================================================
 * One axis: the screen x interval [start, start + width] (32.32) where X + P*x lies in [0, W], and its step per line
 * (Q = PB or PD). All divisions are exact 64-bit ceilings (the original's four sign cases of sdiv with +-(d-1)):
 *   P > 0: start = ceil(2^32 (P-1-X) / P),  end = ceil(2^32 (W-X) / P)
 *   P < 0: start = ceil(2^32 (P+W+1-X) / P), end = ceil(2^32 (-X) / P)
 *          width = end - start,  step = ceil(2^32 (-Q) / P)        (numerators formed in 32 bits, then shifted)
 *   P = 0, Q = 0: X in [0, W] ? (start 0, width 256 << 32) : (start -1, width 0);  step 0
 *   P = 0, Q != 0: a = trunc((W-X-1) / Q), b = trunc(-X / Q) (Q > 0; swapped for Q < 0), 32-bit
 *          start = (-256 a) << 32,  width = ((-256 b) << 32) - start,  step = 256 << 32
 * The P = 0 case encodes a per-line on/off as a span that moves 256 pixels a line: fully open on lines b < n <= a,
 * only pixel 0 on line n = b (the hardware rule is ceil(-X/Q) <= n <= floor((W-X)/Q)). Trap: (-256 a) << 32 overflows
 * 64 bits for |a| >= 2^23 (a reference far outside the map with a small Q): the edges are then garbage, in DraStic as
 * in the port, and a bitmap's clip span can open on rows anywhere within +-2 GiB of the alias (the sign-extended
 * 32-bit offset), which DraStic reads.
 * update_affine_variables: W = (L+0xab << 11) + 0x7ff; X axis edges(L+0x90, PA, W, PB) -> L+0x58/0x68/0x60, Y axis
 * edges(L+0x94, PC, W, PD) -> L+0x70/0x80/0x78; L+0x4c = (2^31 + |PA| - 1) / |PA| if PA != 0, L+0x50 likewise for
 * PC; L+0xae = 0.
 * Per clip-mode line (clip_span): lo = max(hi32(XS), hi32(YS)), hi = min(hi32(XS + XW), hi32(YS + YW)); XS += XD,
 * YS += YD (always); nothing visible if hi < 0 or lo > 255, else clamp to [0, 255], nothing if hi < lo; the pixels
 * lo..hi are rendered from (X + lo*PA, Y + lo*PC); vis has bits lo..hi only, buf outside lo..hi is not written.
 *
 * ===================================================================================================================
 * render_scanline_bitmap_16bpp / _8bpp(L, buf, vis, line)  (0x32880 / 0x33700; line unused)
 * ===================================================================================================================
 * 16bpp: pixel = u16 at VRAM + (s32)(L+0x40 + 2*o), vis = bit 15, buf = the pixel. 8bpp: index = byte at VRAM +
 * (s32)(L+0x40 + o), buf = PAL[index], vis = index != 0. o = (row << L+0xaa) + col, wm = L+0xa6, hm = L+0xa8:
 *   PA = 0x100, PC = 0, wrap:  row = (Y >> 8) & hm, col = (X >> 8) & wm, then (col + 1) & wm per pixel; all 256
 *   same, clip:                row = Y >> 8 must be in [0, hm] (else nothing), pixels max(0, -(X>>8)) .. min(255,
 *                              wm - (X>>8)) from col max(0, X >> 8); the edges are neither used nor recomputed
 *   general, wrap:             row = ((Y + PC*x) >> 8) & hm, col = ((X + PA*x) >> 8) & wm; all 256
 *   general, clip:             the clip_span of the edges, computed HERE when L+0xae (setup_edges(X, PA, (wm << 8) +
 *                              0xff, PB) and (Y, PC, (hm << 8) + 0xff, PD), no reciprocals), unmasked row / col
 * So a bitmap consumes L+0xae only on general clip lines (a wrap-to-clip switch after line 0 still recomputes; for
 * an affine tile BG it does not). No general-path bug for bitmaps. Mode 6's large bitmap is bitmap_8bpp with
 * 1024x512 or 512x1024 masks and base 0.
 *
 * ===================================================================================================================
 * render_scanline_bg(eng, lines, vis, line)  (0x36880; mosaic inlined: render_scanline_apply_mosaic is dead code)
 * ===================================================================================================================
 *   for i in 0 .. eng[0xb2] - 1: n = eng[0x8c + i], L = eng + 0xc0 + n*0xb0
 *       skip if L+0x20 (a direct bitmap layer: render_scanline_2d uses the VRAM rows itself, decided per frame)
 *       BGnCNT.6 clear: [L+0x30](L, lines + n*0x220 + 0x10, vis + n*32, line)
 *       BGnCNT.6 set (MOSAIC = eng+0xa8: H = bits 0-3, V = bits 4-7): m = V ? line % (V+1) : 0;
 *           VOFS -= m (u16), X -= PB*m, Y -= PD*m (32-bit, PB / PD as they are now); call; restore all three
 *           (a clip-edge recompute on such a line uses the adjusted X / Y); then if H: buf[x] = buf[x - x % (H+1)]
 *           and vis bit x = vis bit (x - x % (H+1)) for the whole line (the bitmap by the mosaic_masks table
 *           0x11deb0 per 32-bit word, block starts shifted by the word's phase, with the last pixel of each word
 *           carried into the next word's first block)
 * The buffer is mosaicked whatever the renderer wrote: render_scanline_null (mode 6 BG1/BG3) and a NULL ext slot
 * leave the slot's stale line in place.
 *
 * ===================================================================================================================
 * render_scanline_disable_blank_layers_asm(vis, &lmask)  (0xa089c)
 * ===================================================================================================================
 * Called by render_scanline_2d after the windows (with the 3D bitmap in BG0's slot when BG0 is 3D). For n = 0..3: if
 * the 32 bytes at vis + 32n are all zero, clear bit n of *lmask. Bits 4..15 are kept; bits 16..31 are cleared only
 * when all four bitmaps are empty. The resulting lmask is visible to the composite (stale masks of dropped layers are
 * then ignored). */
#include <string.h>
#include "bg.h"

#define BP(p, o)   ((uint8_t *)(p) + (o))
#define BU8(p, o)  (*(uint8_t *)BP(p, o))
#define BU16(p, o) (*(uint16_t *)BP(p, o))
#define BS16(p, o) (*(int16_t *)BP(p, o))
#define BU32(p, o) (*(uint32_t *)BP(p, o))
#define BS32(p, o) (*(int32_t *)BP(p, o))
#define BS64(p, o) (*(int64_t *)BP(p, o))
#define BPTR(p, o) (*(uint8_t **)BP(p, o))

/* ---- text BGs ---- */
void spec_render_scanline_tiled_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line) {
    uint32_t cnt = BU16(L, BGL_CNT), hofs = BU16(L, BGL_HOFS);
    uint32_t y = BU16(L, BGL_VOFS) + line;          /* only bits 0..8 are used: (VOFS + line) mod 512 */
    uint8_t *vram = BPTR(L, BGL_VRAM), *chr = vram + BU32(L, BGL_CHR);
    const uint16_t *pal = (const uint16_t *)BPTR(L, BGL_PAL);
    uint32_t m = BU32(L, BGL_MAP);
    if ((cnt & 0x8000) && (y & 0x100)) m += (cnt & 0x4000) ? 0x1000 : 0x800;   /* lower 256 rows of a 512-tall map */
    uint32_t off = m + ((y & 0xf8) << 3), a = off, b = off;                     /* row of 32 entries = 64 bytes */
    if (cnt & 0x4000) { if (hofs < 0x100) b = off + 0x800; else a = off + 0x800; }   /* 512 wide: two blocks */
    const uint16_t *ma = (const uint16_t *)(vram + a), *mb = (const uint16_t *)(vram + b);
    uint32_t t = (hofs >> 3) & 31, fine = hofs & 7, fy = y & 7;
    int bpp8 = (cnt & 0x80) != 0, ext = bpp8 && BU8(L, BGL_EXTON);
    const uint16_t *xp = (const uint16_t *)BPTR(L, BGL_EXTPAL);
    if (ext && !xp) return;                             /* no ext palette slot: nothing is written (a stale line) */
    uint16_t col[264];
    uint8_t op[264];
    for (uint32_t k = 0; k < 33; k++) {
        uint32_t e = t + k < 32 ? ma[t + k] : mb[t + k - 32];
        uint32_t tile = e & 0x3ff, hf = e & 0x400, row = (e & 0x800) ? 7 - fy : fy, pn = e >> 12;
        for (uint32_t p = 0; p < 8; p++) {
            uint32_t q = hf ? 7 - p : p, idx, c;
            if (!bpp8) {
                uint32_t d = *(const uint32_t *)(chr + tile * 32 + row * 4);
                idx = (d >> (4 * q)) & 15;
                c = pal[pn * 16 + idx];
            } else {
                idx = chr[(tile * 64 + row * 8 + q) & 0xffff];
                c = ext ? xp[(pn << 8) | idx] : pal[idx];
            }
            col[8 * k + p] = (uint16_t)c;
            op[8 * k + p] = idx != 0;
        }
    }
    memset(vis, 0, 32);
    for (uint32_t x = 0; x < 256; x++) {
        buf[x] = col[x + fine];
        if (op[x + fine]) vis[x >> 3] |= (uint8_t)(1u << (x & 7));
    }
}

/* ---- the affine clip edges ---- */
static int64_t cdiv(int64_t n, int64_t d) {                     /* the exact ceiling of n/d, d != 0, as DraStic does */
    if (d > 0) return n >= 0 ? (n + d - 1) / d : n / d;
    return n >= 0 ? n / d : (n + d + 1) / d;
}
static int64_t sh32(int32_t v) { return (int64_t)((uint64_t)(uint32_t)v << 32); }

void spec_video_2d_bg_layer_affine_setup_edges(int32_t X, int32_t P, int32_t W, int32_t Q, int64_t *start, int64_t *step,
                                               int64_t *width) {
    if (P == 0) {
        if (Q == 0) {
            int in = X >= 0 && W >= X;
            *start = in ? 0 : -1;
            *width = in ? (int64_t)1 << 40 : 0;
            *step = 0;
            return;
        }
        int32_t a, b, wx = (int32_t)((uint32_t)W - (uint32_t)X - 1u), nx = (int32_t)(0u - (uint32_t)X);
        if (Q > 0) { a = wx; b = nx; } else { a = nx; b = wx; }
        a /= Q; b /= Q;                                         /* sdiv: truncation */
        int64_t s = (int64_t)((uint64_t)((int64_t)a * -256) << 32), e = (int64_t)((uint64_t)((int64_t)b * -256) << 32);
        *start = s;
        *width = (int64_t)((uint64_t)e - (uint64_t)s);
        *step = (int64_t)1 << 40;
        return;
    }
    int32_t n1, n2;
    if (P > 0) { n1 = (int32_t)((uint32_t)P - 1u - (uint32_t)X); n2 = (int32_t)((uint32_t)W - (uint32_t)X); }
    else { n1 = (int32_t)((uint32_t)P + (uint32_t)W + 1u - (uint32_t)X); n2 = (int32_t)(0u - (uint32_t)X); }
    int64_t s = cdiv(sh32(n1), P), e = cdiv(sh32(n2), P);
    *start = s;
    *width = (int64_t)((uint64_t)e - (uint64_t)s);
    *step = cdiv(sh32((int32_t)(0u - (uint32_t)Q)), P);
}

void spec_render_scanline_update_affine_variables(uint8_t *L) {
    int32_t W = (int32_t)(((uint32_t)BU8(L, BGL_TMASK) << 11) + 0x7ff);
    spec_video_2d_bg_layer_affine_setup_edges(BS32(L, BGL_CURX), BS16(L, BGL_PA), W, BS16(L, BGL_PB),
                                              &BS64(L, BGL_XS), &BS64(L, BGL_XD), &BS64(L, BGL_XW));
    spec_video_2d_bg_layer_affine_setup_edges(BS32(L, BGL_CURY), BS16(L, BGL_PC), W, BS16(L, BGL_PD),
                                              &BS64(L, BGL_YS), &BS64(L, BGL_YD), &BS64(L, BGL_YW));
    int32_t pa = BS16(L, BGL_PA), pc = BS16(L, BGL_PC);
    uint32_t a = (uint32_t)(pa < 0 ? -pa : pa) & 0xffff, c = (uint32_t)(pc < 0 ? -pc : pc) & 0xffff;
    if (a) BU32(L, BGL_RECA) = (uint32_t)((0x80000000ull + a - 1) / a);
    if (c) BU32(L, BGL_RECC) = (uint32_t)((0x80000000ull + c - 1) / c);
    BU8(L, BGL_DIRTY) = 0;
}

/* the visible span of a clip-mode line from the edges; steps them (always, also when nothing is visible) */
static int clip_span(uint8_t *L, int *lo, int *hi) {
    int64_t a = BS64(L, BGL_XS), b = BS64(L, BGL_YS);
    int32_t la = (int32_t)(a >> 32), lb = (int32_t)(b >> 32);
    int32_t ha = (int32_t)((int64_t)((uint64_t)a + (uint64_t)BS64(L, BGL_XW)) >> 32);
    int32_t hb = (int32_t)((int64_t)((uint64_t)b + (uint64_t)BS64(L, BGL_YW)) >> 32);
    BS64(L, BGL_XS) = (int64_t)((uint64_t)a + (uint64_t)BS64(L, BGL_XD));
    BS64(L, BGL_YS) = (int64_t)((uint64_t)b + (uint64_t)BS64(L, BGL_YD));
    int32_t l = lb > la ? lb : la, h = ha <= hb ? ha : hb;
    if (h < 0 || l > 255) return 0;
    if (l < 0) l = 0;
    if (h > 255) h = 255;
    if (h < l) return 0;
    *lo = l; *hi = h;
    return 1;
}

/* ---- affine tile BGs: the tile-run pipeline (render_scanline_affine_setup_arrays_{normal,extended} 0xa3df0 / 0xa3780,
 * merge_tile_widths_c, setup_map_indexes_*_asm, load_tile_map_entries_*_asm, diff_tile_widths_asm,
 * setup_tile_offsets_asm, setup_flip_masks_asm, render_tiles_*_asm) ---- */
/* the span pixels (1..count) where S + P*x enters a new 8-pixel tile; R = ceil(2^31/|P|) (L+0x4c / L+0x50) */
static int crossings(int32_t S, int32_t P, uint32_t R, uint32_t count, uint8_t *out) {
    if (!P) return 0;
    int32_t E = (int32_t)(count * (uint32_t)P + (uint32_t)S);
    uint32_t frac = (uint32_t)S & 0x7ff, w8;
    int32_t n;
    if (P > 0) { w8 = (uint32_t)P - frac + 0x7ff; n = (E >> 11) - (S >> 11); }
    else { w8 = frac - (uint32_t)P; n = (S >> 11) - (E >> 11); }
    uint32_t v = (uint32_t)(((uint64_t)w8 * R) >> 11);
    for (int32_t k = 0; k < n; k++) out[k] = (uint8_t)((v + (uint32_t)k * R) >> 20);
    return n;
}

/* render_scanline_affine_merge_tile_widths_c 0xa2610: a sorted merge, equal values once */
static int merge(const uint8_t *a, int na, const uint8_t *b, int nb, uint8_t *out) {
    if (!na) { memcpy(out, b, (size_t)nb); return nb; }
    if (!nb) { memcpy(out, a, (size_t)na); return na; }
    uint8_t *o = out;
    uint8_t x = *a, y = *b;
    for (;;) {
        if (x < y) {
            *o++ = x;
            if (--na == 0) { memcpy(o, b, (size_t)nb); return (int)(o - out) + nb; }
            x = *++a;
            continue;
        }
        if (x != y) *o++ = y;
        if (--nb == 0) { memcpy(o, a, (size_t)na); return (int)(o - out) + na; }
        y = *++b;
    }
}

/* the 6-bit in-tile offset (sx & 7) | (sy & 7) << 3 of span pixel p (setup_tile_offsets_asm: 16-bit lanes) */
static uint8_t toff(int32_t X, int32_t Y, int32_t PA, int32_t PC, uint32_t p) {
    uint16_t sx = (uint16_t)((uint32_t)X + (uint32_t)PA * p), sy = (uint16_t)((uint32_t)Y + (uint32_t)PC * p);
    return (uint8_t)(((sx >> 8) & 7) | (((sy >> 8) & 7) << 3));
}

/* count+1 pixels from the span origin (X, Y) into out8 (index bytes: normal, and extended with the standard palette)
 * or out16 ((pal << 8) | index: extended with ext palettes) */
static void affine_span(uint8_t *L, int32_t X, int32_t Y, uint32_t count, int extended, uint8_t *out8, uint16_t *out16) {
    int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    uint32_t mask = BU8(L, BGL_TMASK), lg = BU8(L, BGL_LOG2T);
    uint8_t *vram = BPTR(L, BGL_VRAM), *map = vram + BU32(L, BGL_MAP), *chr = vram + BU32(L, BGL_CHR);
    /* the run boundaries: both axes' tile crossings merged, then repeats removed */
    uint8_t la[300], lb[300], runs[600];
    int na = crossings(X, PA, BU32(L, BGL_RECA), count, la);
    int nb = crossings(Y, PC, BU32(L, BGL_RECC), count, lb);
    int n = merge(la, na, lb, nb, runs), nd = 0;
    uint32_t prev = 0x100;
    for (int i = 0; i < n; i++) { uint32_t v = runs[i]; runs[nd] = (uint8_t)v; if (v != prev) nd++; prev = v; }
    /* per run [b0, end): the map entry at its first pixel, the in-tile offset per pixel */
    for (int r = 0; r <= nd; r++) {
        uint32_t b0 = r ? runs[r - 1] : 0, end = r < nd ? runs[r] : count + 1;
        uint32_t sx = (uint32_t)X + b0 * (uint32_t)PA, sy = (uint32_t)Y + b0 * (uint32_t)PC;
        uint32_t idx = ((sx >> 11) & mask) + (((sy >> 11) & mask) << lg);
        for (uint32_t x = b0; x < end; x++) {
            uint8_t o = toff(X, Y, PA, PC, x);
            if (!extended) {
                out8[x] = chr[map[idx] * 64 + o];
            } else {
                uint32_t e = *(const uint16_t *)(map + 2 * idx);
                o ^= (uint8_t)(((e & 0x400) ? 7 : 0) | ((e & 0x800) ? 0x38 : 0));
                uint8_t ix = chr[((e & 0x3ff) << 6) + o];
                if (out16) out16[x] = (uint16_t)(ix | (e >> 12) << 8); else out8[x] = ix;
            }
        }
    }
}

/* the general path (wrap mode, |PA| or |PC| > 2047): per-pixel map entries, except that setup_tile_offsets_asm writes
 * 288 bytes (9 x 32) into the 256-byte offset array, over the map indexes of pixels 0..15: those take the in-tile
 * offset bytes of pixels 256..287 */
static void affine_general(uint8_t *L, int32_t X, int32_t Y, int extended, uint8_t *out8, uint16_t *out16) {
    int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    uint32_t mask = BU8(L, BGL_TMASK), lg = BU8(L, BGL_LOG2T);
    uint8_t *vram = BPTR(L, BGL_VRAM), *map = vram + BU32(L, BGL_MAP), *chr = vram + BU32(L, BGL_CHR);
    for (uint32_t x = 0; x < 256; x++) {
        uint32_t px = (uint32_t)X + x * (uint32_t)PA, py = (uint32_t)Y + x * (uint32_t)PC;
        uint32_t i2 = (((px >> 11) & mask) + (((py >> 11) & mask) << lg)) & 0xffff;
        if (extended) i2 = (i2 << 1) & 0xffff;                              /* a byte offset from here on */
        if (x < 16) i2 = toff(X, Y, PA, PC, 256 + 2 * x) | (uint32_t)toff(X, Y, PA, PC, 257 + 2 * x) << 8;
        uint8_t o = toff(X, Y, PA, PC, x);
        if (!extended) {
            out8[x] = chr[map[i2] * 64 + o];
        } else {
            uint32_t e = *(const uint16_t *)(map + i2);
            o ^= (uint8_t)(((e & 0x400) ? 7 : 0) | ((e & 0x800) ? 0x38 : 0));
            uint8_t ix = chr[((e & 0x3ff) << 6) + o];
            if (out16) out16[x] = (uint16_t)(ix | (e >> 12) << 8); else out8[x] = ix;
        }
    }
}

static int big(int32_t v) { return ((uint32_t)(v + 0x7ff) & 0xffff) > 0xffe; }   /* v outside [-2047, 2047] */

static void affine_common(uint8_t *L, uint16_t *buf, uint8_t *vis, int extended) {
    if (BU8(L, BGL_DIRTY)) spec_render_scanline_update_affine_variables(L);
    uint32_t cnt = BU16(L, BGL_CNT);
    int32_t X = BS32(L, BGL_CURX), Y = BS32(L, BGL_CURY), PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    int ext = extended && BU8(L, BGL_EXTON);
    const uint16_t *pal = (const uint16_t *)BPTR(L, ext ? BGL_EXTPAL : BGL_PAL);
    if (ext && !pal) return;                            /* no ext palette slot: nothing written, edges not stepped */
    uint8_t i8[256];
    uint16_t i16[256];
    int lo = 0, hi = 255;
    if (cnt & 0x2000) {
        if (big(PA) || big(PC)) affine_general(L, X, Y, extended, i8, ext ? i16 : 0);
        else affine_span(L, X, Y, 255, extended, i8, ext ? i16 : 0);
    } else {
        if (!clip_span(L, &lo, &hi)) { memset(vis, 0, 32); return; }
        affine_span(L, (int32_t)((uint32_t)X + (uint32_t)lo * (uint32_t)PA),
                    (int32_t)((uint32_t)Y + (uint32_t)lo * (uint32_t)PC), (uint32_t)(hi - lo), extended, i8 + lo,
                    ext ? i16 + lo : 0);
    }
    memset(vis, 0, 32);
    for (int x = lo; x <= hi; x++) {
        uint32_t ix = ext ? (i16[x] & 0xff) : i8[x];
        buf[x] = ext ? pal[i16[x] & 0xfff] : pal[ix];
        if (ix) vis[x >> 3] |= (uint8_t)(1u << (x & 7));
    }
}

void spec_render_scanline_affine_normal_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line) {
    (void)line;
    affine_common(L, buf, vis, 0);
}
void spec_render_scanline_affine_extended_ext(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line) {
    (void)line;
    affine_common(L, buf, vis, 1);
}

/* ---- bitmap BGs ---- */
static void bitmap(uint8_t *L, uint16_t *buf, uint8_t *vis, int b16) {
    int32_t PA = BS16(L, BGL_PA), PC = BS16(L, BGL_PC);
    uint32_t cnt = BU16(L, BGL_CNT), lg = BU8(L, BGL_LOG2W), wm = BU16(L, BGL_WMASK), hm = BU16(L, BGL_HMASK);
    uint32_t base = BU32(L, BGL_BMP);
    int32_t X = BS32(L, BGL_CURX), Y = BS32(L, BGL_CURY);
    uint8_t *vram = BPTR(L, BGL_VRAM);
    const uint16_t *pal = (const uint16_t *)BPTR(L, BGL_PAL);
    /* the pixel at offset o (in pixels) from the base; the byte offset is a sign-extended 32-bit value */
#define FETCH(o) (b16 ? *(const uint16_t *)(vram + (int64_t)(int32_t)(base + ((uint32_t)(o) << 1))) \
                      : pal[vram[(int64_t)(int32_t)(base + (uint32_t)(o))]])
#define OPAQ(o)  (b16 ? (*(const uint16_t *)(vram + (int64_t)(int32_t)(base + ((uint32_t)(o) << 1))) >> 15) \
                      : (vram[(int64_t)(int32_t)(base + (uint32_t)(o))] != 0))
    int lo, hi;
    memset(vis, 0, 32);
    if (PA == 0x100 && PC == 0) {                           /* identity in x: the fast path, no edges */
        int32_t y = Y >> 8, x = X >> 8;
        if (cnt & 0x2000) {
            uint32_t row = ((uint32_t)y & hm) << lg, c = (uint32_t)x & wm;
            for (int i = 0; i < 256; i++) {
                buf[i] = FETCH(row + c);
                if (OPAQ(row + c)) vis[i >> 3] |= (uint8_t)(1u << (i & 7));
                c = (c + 1) & wm;
            }
            return;
        }
        if (y < 0 || hm < (uint32_t)y) return;
        int32_t last = (int32_t)wm - x, first = 0, c = x;
        if (x <= 0) { first = -x; c = 0; }
        if (last > 255) last = 255;
        if (last < first) return;
        uint32_t row = (uint32_t)y << lg;
        for (int i = first; i <= last; i++, c++) {
            buf[i] = FETCH(row + (uint32_t)c);
            if (OPAQ(row + (uint32_t)c)) vis[i >> 3] |= (uint8_t)(1u << (i & 7));
        }
        return;
    }
    if (cnt & 0x2000) {
        for (int i = 0; i < 256; i++) {
            uint32_t o = ((((uint32_t)(Y >> 8)) & hm) << lg) + ((uint32_t)(X >> 8) & wm);
            buf[i] = FETCH(o);
            if (OPAQ(o)) vis[i >> 3] |= (uint8_t)(1u << (i & 7));
            X = (int32_t)((uint32_t)X + (uint32_t)PA); Y = (int32_t)((uint32_t)Y + (uint32_t)PC);
        }
        return;
    }
    if (BU8(L, BGL_DIRTY)) {                                /* the bitmap's own edges, no reciprocals */
        spec_video_2d_bg_layer_affine_setup_edges(X, PA, (int32_t)((wm << 8) + 0xff), BS16(L, BGL_PB),
                                                  &BS64(L, BGL_XS), &BS64(L, BGL_XD), &BS64(L, BGL_XW));
        spec_video_2d_bg_layer_affine_setup_edges(Y, PC, (int32_t)((hm << 8) + 0xff), BS16(L, BGL_PD),
                                                  &BS64(L, BGL_YS), &BS64(L, BGL_YD), &BS64(L, BGL_YW));
        BU8(L, BGL_DIRTY) = 0;
    }
    if (!clip_span(L, &lo, &hi)) return;
    X = (int32_t)((uint32_t)X + (uint32_t)lo * (uint32_t)PA);
    Y = (int32_t)((uint32_t)Y + (uint32_t)lo * (uint32_t)PC);
    for (int i = lo; i <= hi; i++) {
        uint32_t o = ((uint32_t)(Y >> 8) << lg) + (uint32_t)(X >> 8);
        buf[i] = FETCH(o);
        if (OPAQ(o)) vis[i >> 3] |= (uint8_t)(1u << (i & 7));
        X = (int32_t)((uint32_t)X + (uint32_t)PA); Y = (int32_t)((uint32_t)Y + (uint32_t)PC);
    }
#undef FETCH
#undef OPAQ
}
void spec_render_scanline_bitmap_16bpp(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line) { (void)line; bitmap(L, buf, vis, 1); }
void spec_render_scanline_bitmap_8bpp(uint8_t *L, uint16_t *buf, uint8_t *vis, uint32_t line) { (void)line; bitmap(L, buf, vis, 0); }

/* ---- render_scanline_bg ---- */
spec_bg_fn spec_bg_renderer(uintptr_t off) {
    switch (off) {
    case DS_RENDER_SCANLINE_TILED_EXT: return spec_render_scanline_tiled_ext;
    case DS_RENDER_SCANLINE_AFFINE_NORMAL_EXT: return spec_render_scanline_affine_normal_ext;
    case DS_RENDER_SCANLINE_AFFINE_EXTENDED_EXT: return spec_render_scanline_affine_extended_ext;
    case DS_RENDER_SCANLINE_BITMAP_16BPP: return spec_render_scanline_bitmap_16bpp;
    case DS_RENDER_SCANLINE_BITMAP_8BPP: return spec_render_scanline_bitmap_8bpp;
    default: return 0;                                      /* render_scanline_null: returns at once */
    }
}

static const uint32_t mosaic_masks[16] = {   /* 0x11deb0: bits at multiples of (H+1) */
    0xffffffff, 0x55555555, 0x49249249, 0x11111111, 0x42108421, 0x41041041, 0x10204081, 0x01010101,
    0x08040201, 0x40100401, 0x00400801, 0x01001001, 0x04002001, 0x10004001, 0x40008001, 0x00010001 };

void spec_render_scanline_bg(uint8_t *eng, uint8_t *lines, uint8_t *vis, uint32_t line, uintptr_t ds_base) {
    uint32_t mos = BU16(eng, 0xa8), hs = mos & 15, vs = (mos >> 4) & 15;
    uint32_t vmod = vs ? line % (vs + 1) : 0;
    for (uint32_t i = 0; i < BU8(eng, 0xb2); i++) {
        uint32_t n = BU8(eng, 0x8c + i);
        uint8_t *L = eng + 0xc0 + n * 0xb0;
        if (BPTR(L, BGL_DIRECT)) continue;
        uint16_t *buf = (uint16_t *)(lines + n * 0x220 + 0x10);
        uint8_t *v = vis + n * 32;
        spec_bg_fn fn = spec_bg_renderer((uintptr_t)BPTR(L, BGL_FN) - ds_base);
        if (!(BU16(L, BGL_CNT) & 0x40)) { if (fn) fn(L, buf, v, line); continue; }
        /* vertical mosaic: render with VOFS / X / Y moved back by line % (V+1) lines, then restore them */
        int32_t cx = BS32(L, BGL_CURX), cy = BS32(L, BGL_CURY);
        uint16_t vo = BU16(L, BGL_VOFS);
        BU16(L, BGL_VOFS) = (uint16_t)(vo - vmod);
        BS32(L, BGL_CURX) = (int32_t)((uint32_t)cx - (uint32_t)BS16(L, BGL_PB) * vmod);
        BS32(L, BGL_CURY) = (int32_t)((uint32_t)cy - (uint32_t)BS16(L, BGL_PD) * vmod);
        if (fn) fn(L, buf, v, line);
        BS32(L, BGL_CURX) = cx; BS32(L, BGL_CURY) = cy; BU16(L, BGL_VOFS) = vo;
        if (!hs) continue;
        /* horizontal mosaic of the bitmap, word by word: the block starts of word k (the mask table shifted by the
         * word's phase) smeared over the next H bits, and the last pixel of a word carried into the next word's
         * pixels before its first block start */
        uint32_t h1 = hs + 1, mk = mosaic_masks[hs], w11 = h1 - (32 % h1), ph = 0;
        uint32_t *vw = (uint32_t *)v, w5 = mk & vw[0], w4 = w5;
        for (int k = 0;; k++) {
            ph = (w11 + ph) % h1;
            for (uint32_t j = 0; j < hs; j++) { w4 += w4; w5 |= w4; }
            vw[k] = w5;
            uint32_t carry = w5 >> 31;
            if (k == 7) break;
            w4 = (mk << ph) & vw[k + 1];
            w5 = w4;
            for (uint32_t j = 0; j < ph; j++) w5 |= carry << j;
        }
        /* and of the line: pixel x takes pixel x - x % (H+1) */
        for (uint32_t x = 0; x < 256; x += h1) {
            uint16_t c = buf[x];
            for (uint32_t j = x + 1; j < x + h1 && j <= 255; j++) buf[j] = c;
        }
    }
}

void spec_render_scanline_disable_blank_layers(const uint8_t *vis, uint32_t *lmask) {
    uint32_t keep = 0xfff0;
    for (int n = 0; n < 4; n++) {
        int any = 0;
        for (int i = 0; i < 32; i++) any |= vis[n * 32 + i];
        if (any) keep |= 0xfffffff0u | (1u << n);
    }
    *lmask &= keep;
}
