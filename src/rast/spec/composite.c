/* composite.c: exact C ports of DraStic r2.5.2.2's 2D scanline compositor routines that the 3D screen goes through:
 * the 3D visibility step, and the simple (non-blending) path of render_scanline_2d_composite with everything under
 * it. Verified bit-exact against the originals by tools/rast/ut/t_composite.c (all output bytes plus guard bytes
 * around them, the return values).
 *
 * ===================================================================================================================
 * WHERE IT RUNS
 * ===================================================================================================================
 * render_scanline_2d (0x3ef00) composites a DS line of engine A (the screen with the 3D layer as BG0). In the hi-res
 * path it runs once per quarter q = 0..3 of the line (two output rows x the even / odd pixels), with
 *      px = render_scanline_3d(sys, line) + q * 0x400       (frame + line*0x1000 + q*0x400, 256 pixels)
 * or a BG0HOFS-shifted copy of that quarter on its stack; in the 1x path once per line with the 1x line or the
 * downsampled line. The 3D frame's pixel format is r6 | g6<<8 | b6<<16 | a<<24 with a = the 5-bit alpha (0 =
 * transparent: the layer below shows; 31 = opaque; 1..30 = translucent, blended with the layer below).
 *     flags_q = flags_line | render_scanline_set_3d_visibility(S + 0xda0, px)
 * The bitmap at S+0xda0 is BG0's visibility for the priority encoder; flag 2 (bit 1) sends the quarter down the
 * complex (blending) path; 0x10 says the 3D layer shows somewhere, opaque everywhere it shows.
 *
 * ===================================================================================================================
 * render_scanline_gather_3d_alpha_asm(u8 a[256], const u32 px[256])  (0xa0a48, hand-written NEON)
 * ===================================================================================================================
 * Four ld4.16b per 64 pixels (the 4th register holds byte 3 of 16 consecutive pixels), stored in order:
 *      a[i] = px[i] >> 24          for i = 0..255; writes exactly a[0..255], reads exactly px[0..255].
 *
 * ===================================================================================================================
 * render_scanline_set_3d_visibility(u8 bits[32], const u32 px[256]) -> u32  (0x3c2c0, C with NEON intrinsics)
 * ===================================================================================================================
 * Gathers the 256 alpha bytes into a stack array (above), then folds them into bits, 128 pixels per iteration:
 * ld2.4s splits the alpha words into even / odd 4-byte groups; per byte, nz(x) is formed as (x | x >> 4) & 0x0f
 * (even group) and (x | x << 4) & 0xf0 (odd group), folded with >> 2 and >> 1 and masked with 0x11, so byte k of a
 * word holds bit 0 = (alpha[8j+k] != 0) and bit 4 = (alpha[8j+4+k] != 0); >> 7 and >> 14 folds and two xtn
 * narrowings then pack the 8 bits of pixels 8j..8j+7 into bitmap byte j. The same folds run on alpha ^ 0x1f (non-zero
 * where alpha != 31) and are ANDed with the visibility folds before an OR accumulation: the fold maps every bit
 * position of a word to the same pixel in both, so the AND is exact per pixel. Result, for any byte values:
 *      bits[j] bit b = (alpha[8j + b] != 0)                 j = 0..31, b = 0..7 (LSB first); all 32 bytes written
 *      return 2    if some pixel has alpha not in {0, 0x1f} (alpha 0x20..0xff counts as translucent too),
 *             0x10 else if some pixel has alpha != 0 (then every such pixel has alpha 0x1f),
 *             0    else (no pixel visible).
 * Bytes 0..2 of the pixels are never looked at. No other memory is written (the gather array is the original's
 * own stack frame).
 *
 * ===================================================================================================================
 * THE SIMPLE PATH OF render_scanline_2d_composite (one quarter: 256 pixels)
 * ===================================================================================================================
 * Formats. Layer line buffers: u16 BGR555 (r | g<<5 | b<<10, bit 15 = opaque flag, ignored here), 256 pixels at
 * layers[k] + 0x10 (the table holds pointers 0x10 bytes before the first pixel). Bitmaps ("masks"): 256 bits, 32
 * bytes, pixel i = byte i>>3, bit i&7 (as little-endian u32 words: word w bit b = pixel 32w + b). Planes ("out"): the
 * quarter's 6-bit colour, planar: R6[256] at out+0, G6[256] at +0x100, B6[256] at +0x200. 3D pixels: u32
 * r6 | g6<<8 | b6<<16 | a<<24. The 2D engine struct eng: u16 *[eng+0x18] = the backdrop colour (palette entry 0),
 * u8 [eng+0xb3] = number of entries of the layer priority list, u8 [eng+0x84 + k] = the list, front to back
 * (video_2d_reorder_layers: per priority 0..3, OBJ slot 4+priority if OBJ is on, then the BGs 0..3 of that priority
 * that are on). The scratch area S (render_scanline_2d's stack): vis[8][32] at S+0xda0 (BG0..BG3, then OBJ by
 * priority 0..3), excl[6][32] at S+0x10c0 (BG0..BG3, OBJ, backdrop).
 *
 * All the NEON routines below work in blocks of 32 pixels, loading a block's inputs before storing its outputs;
 * the ports keep that order, so they give the same bytes for overlapping buffers too.
 *
 * render_scanline_priority_encode_single_asm(eng, vis, excl)  (0x9ffc0)
 *   covered = objcovered = objex = 0 (256 bits each); for k = 0 .. [eng+0xb3]-1, s = [eng+0x84+k], o = s*32:
 *     s & 4 == 0 (BG):  excl[o..o+31] = vis[o..] & ~covered;  covered |= vis[o..]
 *     s & 4 != 0 (OBJ): v = vis[o..] & ~objcovered; objex |= v & ~covered; covered |= v; objcovered |= v
 *   then excl+0xa0 (backdrop) = ~covered, excl+0x80 (OBJ) = objex. So each covered pixel is in exactly one written
 *   mask, the frontmost layer's that shows it. Writes only those 32-byte slots: the excl slot of a BG that is not in
 *   the list keeps its old (stale) bytes, and the consumers below read it all the same.
 * render_scanline_select_pixels_binary_asm(dst, src, layer, mask)  (0xa0640, u16[256] each)
 *   dst[i] = mask bit i ? layer[i] : src[i]. Per 32-pixel block: with dst == src a zero mask word skips the block;
 *   with dst != src the block is always copied (layer read only where the mask word is non-zero).
 * render_scanline_select_pixels_binary_scalar_asm(dst, src, colour, mask)  (0xa0560)
 *   dst[i] = mask bit i ? (u16)colour : src[i]; the same skipping rule.
 * render_scanline_expand_6bit_split_asm(out, c)  (0xa0818): for i = 0..255
 *   R[i] = (c[i] & 0x1f) << 1,  G[i] = ((c[i] >> 5) & 0x1f) << 1,  B[i] = ((c[i] >> 10) & 0x1f) << 1.
 * render_scanline_select_pixels_binary32_asm(out, alpha, px, mask)  (0xa0730; alpha != NULL: the _alpha variant)
 *   where mask bit i is set: R[i] = byte0(px[i]), G[i] = byte1, B[i] = byte2 (and alpha[i] = byte3); the 3D bytes are
 *   inserted as they are (not masked to 6 bits). Every byte of out (and alpha) is rewritten, unchanged where the bit
 *   is clear.
 * render_scanline_select_pixels(eng, out, excl, layers, p3d, alpha, lmask)  (0x39330, C)
 *   tmp = u16[256] on its stack. For the set bits k of lmask, lowest first: the first layer's buffer becomes src;
 *   each later one is merged: binary(tmp, src, layer k, excl + 32k), src = tmp. Then, if a layer was found,
 *   binary_scalar(tmp, src, *[eng+0x18], excl + 0xa0); else (lmask == 0) tmp = the backdrop everywhere.
 *   expand_6bit_split(out, tmp); then, if p3d != NULL and lmask & 1, binary32(out, alpha, p3d, excl).
 *   So the 3D layer always counts as the first layer (its own u16 buffer is not the 3D colour: whatever it holds
 *   shows only where neither excl[0], a later layer nor the backdrop mask claims a pixel).
 * render_scanline_2d_composite(eng, out, S, layers, p3d, alpha, lmask, bldcnt, flags, line)  (0x3c6d0, C)
 *   (flags & 7) == 0 is the simple path: priority_encode_single(eng, S+0xda0, S+0x10c0), then, if flags & 8 == 0,
 *   a tail call of select_pixels(eng, out, S+0x10c0, layers, p3d, NULL, lmask) (the alpha argument is dropped).
 *   Ported for (flags & 0xf) == 0 only (spec_..._simple); flags & 8 (brightness) and bits 0-2 (blending) take
 *   paths that are not ported. Memory the simple path writes: excl (as above), out[0..0x2ff], and its own and
 *   select_pixels' stack frames (dead after the return; tmp is fully written before it is read).
 *
 * The 3D + backdrop case (what comp.c's fused path computes, compfuse.h): when lmask & 1, p3d != NULL, excl[k] == 0
 * for every other k in lmask (k <= 4) and (excl[0] | excl[5]) has all 256 bits set, the merges change nothing, tmp
 * is the backdrop wherever excl[0] is clear, and for every pixel
 *      R[i] = excl0 bit i ? byte0(p3d[i]) : (bd & 0x1f) << 1        (bd = *[eng+0x18])
 *      G[i] = excl0 bit i ? byte1(p3d[i]) : ((bd >> 5) & 0x1f) << 1
 *      B[i] = excl0 bit i ? byte2(p3d[i]) : ((bd >> 10) & 0x1f) << 1
 * whatever BG0's own u16 buffer holds; and when excl[0] has all 256 bits set, binary32 rewrites every byte of out,
 * so the planes are the 3D bytes whatever the other masks and buffers hold. */
#include <string.h>
#include "composite.h"

void spec_render_scanline_gather_3d_alpha(uint8_t a[256], const uint32_t px[256]) {
    for (int i = 0; i < 256; i++) a[i] = (uint8_t)(px[i] >> 24);
}

uint32_t spec_render_scanline_set_3d_visibility(uint8_t bits[32], const uint32_t px[256]) {
    uint8_t a[256];
    spec_render_scanline_gather_3d_alpha(a, px);
    int any = 0, transl = 0;
    memset(bits, 0, 32);
    for (int i = 0; i < 256; i++) {
        if (!a[i]) continue;
        bits[i >> 3] |= (uint8_t)(1u << (i & 7));
        any = 1;
        transl |= a[i] != 0x1f;
    }
    return transl ? 2 : any ? 0x10 : 0;
}

/* ---- the simple path of render_scanline_2d_composite ---- */

void spec_render_scanline_priority_encode_single(const uint8_t *eng, const uint8_t *vis, uint8_t *excl) {
    uint8_t cov[32] = { 0 }, objcov[32] = { 0 }, objex[32] = { 0 };
    unsigned n = eng[0xb3];
    for (unsigned k = 0; k < n; k++) {
        uint32_t o = (uint32_t)eng[0x84 + k] << 5;
        uint8_t v[32];
        memcpy(v, vis + o, 32);
        if (o & 0x80) {                                     /* OBJ slot (4..7): merged into the one OBJ mask */
            for (int j = 0; j < 32; j++) {
                v[j] &= (uint8_t)~objcov[j];
                objex[j] |= v[j] & (uint8_t)~cov[j];
                cov[j] |= v[j];
                objcov[j] |= v[j];
            }
        } else {
            uint8_t e[32];
            for (int j = 0; j < 32; j++) e[j] = v[j] & (uint8_t)~cov[j];
            memcpy(excl + o, e, 32);
            for (int j = 0; j < 32; j++) cov[j] |= v[j];
        }
    }
    uint8_t bd[32];
    for (int j = 0; j < 32; j++) bd[j] = (uint8_t)~cov[j];
    memcpy(excl + 0xa0, bd, 32);
    memcpy(excl + 0x80, objex, 32);
}

/* the 32-bit mask word of block b (pixels 32b..32b+31) */
static inline uint32_t mask_word(const uint8_t *mask, int b) {
    uint32_t m;
    memcpy(&m, mask + 4 * b, 4);
    return m;
}

void spec_render_scanline_select_pixels_binary(uint16_t *dst, const uint16_t *src, const uint16_t *layer,
                                               const uint8_t *mask) {
    int same = src == dst;
    for (int b = 0; b < 8; b++, src += 32, dst += 32, layer += 32) {
        uint32_t m = mask_word(mask, b);
        if (same && !m) continue;
        uint16_t s[32], l[32];
        memcpy(s, src, 64);
        if (m) {
            memcpy(l, layer, 64);
            for (int j = 0; j < 32; j++) if (m >> j & 1) s[j] = l[j];
        }
        memcpy(dst, s, 64);
    }
}

void spec_render_scanline_select_pixels_binary_scalar(uint16_t *dst, const uint16_t *src, uint32_t colour,
                                                      const uint8_t *mask) {
    int same = src == dst;
    for (int b = 0; b < 8; b++, src += 32, dst += 32) {
        uint32_t m = mask_word(mask, b);
        if (same && !m) continue;
        uint16_t s[32];
        memcpy(s, src, 64);
        for (int j = 0; j < 32; j++) if (m >> j & 1) s[j] = (uint16_t)colour;
        memcpy(dst, s, 64);
    }
}

void spec_render_scanline_expand_6bit_split(uint8_t *out, const uint16_t *c) {
    for (int b = 0; b < 8; b++) {
        uint16_t s[32];
        uint8_t r[32], g[32], bl[32];
        memcpy(s, c + 32 * b, 64);
        for (int j = 0; j < 32; j++) {
            r[j] = (uint8_t)((s[j] & 0x1f) << 1);
            g[j] = (uint8_t)(((s[j] >> 5) & 0x1f) << 1);
            bl[j] = (uint8_t)(((s[j] >> 10) & 0x1f) << 1);
        }
        memcpy(out + 32 * b, r, 32);
        memcpy(out + 0x100 + 32 * b, g, 32);
        memcpy(out + 0x200 + 32 * b, bl, 32);
    }
}

void spec_render_scanline_select_pixels_binary32(uint8_t *out, uint8_t *alpha, const uint32_t *px,
                                                 const uint8_t *mask) {
    for (int b = 0; b < 8; b++) {
        uint32_t m = mask_word(mask, b), p[32];
        uint8_t r[32], g[32], bl[32], a[32];
        memcpy(p, px + 32 * b, 128);
        memcpy(r, out + 32 * b, 32);
        memcpy(g, out + 0x100 + 32 * b, 32);
        memcpy(bl, out + 0x200 + 32 * b, 32);
        if (alpha) memcpy(a, alpha + 32 * b, 32);
        for (int j = 0; j < 32; j++) {
            if (!(m >> j & 1)) continue;
            r[j] = (uint8_t)p[j]; g[j] = (uint8_t)(p[j] >> 8); bl[j] = (uint8_t)(p[j] >> 16); a[j] = (uint8_t)(p[j] >> 24);
        }
        memcpy(out + 32 * b, r, 32);
        memcpy(out + 0x100 + 32 * b, g, 32);
        memcpy(out + 0x200 + 32 * b, bl, 32);
        if (alpha) memcpy(alpha + 32 * b, a, 32);
    }
}

void spec_render_scanline_select_pixels(uint8_t *eng, uint8_t *out, uint8_t *excl, uint8_t **layers,
                                        const uint32_t *p3d, uint8_t *alpha, uint32_t lmask) {
    uint16_t tmp[256];
    const uint16_t *src = 0;
    int found = 0;
    unsigned k = 0;
    for (uint32_t m = lmask; m; m >>= 1, k++) {
        if (!(m & 1)) continue;
        const uint16_t *l = (const uint16_t *)(layers[k] + 0x10);
        if (found) { spec_render_scanline_select_pixels_binary(tmp, src, l, excl + 32 * k); src = tmp; }
        else src = l;
        found++;
    }
    const uint16_t *bd = *(const uint16_t **)(eng + 0x18);
    if (found) spec_render_scanline_select_pixels_binary_scalar(tmp, src, *bd, excl + 0xa0);
    else for (int i = 0; i < 256; i++) tmp[i] = *bd;
    spec_render_scanline_expand_6bit_split(out, tmp);
    if (p3d && (lmask & 1)) spec_render_scanline_select_pixels_binary32(out, alpha, p3d, excl);
}

void spec_render_scanline_2d_composite_simple(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers,
                                              const uint32_t *p3d, uint32_t lmask) {
    spec_render_scanline_priority_encode_single(eng, S + 0xda0, S + 0x10c0);
    spec_render_scanline_select_pixels(eng, out, S + 0x10c0, layers, p3d, 0, lmask);
}
