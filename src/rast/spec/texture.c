/* texture.c: exact C ports of DraStic r2.5.2.2's texture stages (render_polygon_flush_4x.isra.0, 0x4b2b0):
 *
 *   setup_uv_interpolants  -> interpolate_uv  -> generate_texture_addresses -> load_texels[_paletted]
 *   setup_rgb_interpolants -> interpolate_rgb   (Gouraud vertex colour, both textured and untextured paths)
 *
 * All routines work on a "batch" of up to 512 pixels spanning several lines of one polygon. Every loop is a
 * do-while over blocks of 8 pixels (`subs wN, wN, #8; b.gt`): a count of 0 still processes one block of 8, and the
 * last block always reads/writes all 8 lanes (up to 7 lanes of padding past the count).
 *
 * Common arguments as flush_4x passes them:
 *   spans   per-line span data (struct of arrays, 44 lines x 4 bytes per array, 0xb0 bytes per array); line i of
 *           array A is at spans + A + 4*i. Pixel count of the line: u16 at +0x630.
 *   lines   number of lines in the batch (>= 1; 0 would loop ~2^32 times).
 *   count   total pixels of the batch (sum of the line counts).
 *   stride  S = ((2*count + 29) & ~15) (flush's "v22"): spacing unit of the planar scratch buffers below. The
 *           routines take it as an argument and use it only to locate their planes.
 *   weights s16[count (+pad to 8)], from render_polygon_setup_perspective_steps_asm: per-pixel perspective-correct
 *           position along the span, Q15 (fcvtzs #15 then xtn: 0..0x7fff for t in [0,1), wraps for t >= 1).
 *   mask    u8 per pixel from the depth test (0 = fail, 0xff = pass); used as an s8 sign-extended to 16 bits.
 * The interpolate/address/load stages run in place in flush (out == in); each 8-pixel block is read completely
 * before it is written, which the ports reproduce.
 *
 * ---------------------------------------------------------------------------------------------------------------
 * render_polygon_setup_uv_interpolants_asm (0x9a338)(spans, out, lines, stride)
 *   reads per line i:  uv0 = u32 at spans+0x2c0+4i (u0 = s16 low half, v0 = s16 high half; 12.4 texel coords)
 *                      duv = u32 at spans+0x370+4i (du = s16 low, dv = s16 high: delta across the whole span)
 *                      n   = u16 at spans+0x630+4i
 *   writes two planes, both advanced by n pixels per line (so the next line overwrites this line's padding):
 *     base plane at out:            s32 pairs {U, V} per pixel (8 bytes/px),
 *        U = (s32)u0 << 15 + (du > 0 ? 0x400 : 0),  V = (s32)v0 << 15 + (dv > 0 ? 0x400 : 0)   (same for every px)
 *     delta plane at out + 4*S:     s16 pairs {du, dv} per pixel (4 bytes/px)
 *   per line, max(n rounded up to 8, 8) pixels are written (block order: 64 bytes base, then 32 bytes delta).
 *
 * render_polygon_interpolate_uv_asm (0x9a3a0)(out, in, weights, count, stride)
 *   in = the buffer setup_uv wrote; delta plane at in + 4*S. For pixel p:
 *     accU = U[p] + du[p] * w[p]   (s32 multiply-accumulate, wraps mod 2^32; w signed s16)
 *     u[p] = (s16)(accU >> 16) >> 3   (shrn #16 keeps bits 16..31, then arithmetic >> 3)    likewise v
 *   i.e. u = (u0<<15 + du*t_q15 + bias) >> 19 = integer texel coordinate (truncated toward -inf, then the 16-bit
 *   wrap of the top half). writes {s16 u, s16 v} per pixel (4 bytes/px) at out; 8 px per block.
 *
 * render_polygon_setup_rgb_interpolants_asm (0x9a3f0)(spans, out, lines, stride)
 *   reads per line i: a = u32 at spans+0x420+4i, c = u32 at spans+0x4d0+4i, b = u32 at spans+0x580+4i,
 *                     d = u32 at spans+0x630+4i (low half = pixel count n, high half = blue delta)
 *     channel 0 (R): base = (u16)a.lo << 15, delta = (s16)c.lo
 *     channel 1 (G): base = (u16)a.hi << 15, delta = (s16)c.hi
 *     channel 2 (B): base = (u16)b.hi << 15, delta = (s16)d.hi     (b.lo is not read)
 *   bases are zero-extended (u16), unlike setup_uv. Writes 6 planes, layout in units of S bytes:
 *     out+0*S: s16 delta R   out+1*S: s16 delta G   out+2*S: s16 delta B         (2 bytes/px)
 *     out+3*S: u32 base R    out+5*S: u32 base G    out+7*S: u32 base B          (4 bytes/px)
 *   per line max(n rounded up to 8, 8) px written, all pointers advanced by n px. Block store order: base R, G, B
 *   (32 bytes each), delta R, G, B (16 bytes each).
 *
 * render_polygon_interpolate_rgb_asm (0x9a4a0)(out, in, weights, count, stride)
 *   planes of `in` as above. For pixel p and channel c:
 *     acc = base_c[p] + delta_c[p] * w[p]   (s32 MAC, wraps)
 *     col_c[p] = (u8)((u32)acc >> 18)         (shrn #16 then shrn #2)
 *   writes u8 planes: R at out, G at out + S, B at out + 2*S (8 bytes per block each).
 *
 * render_polygon_generate_texture_addresses (0x47b90, C)(poly, out, uv, count, mask)
 *   tex = *(void **)(poly + 0x10) (texture cache entry), W = u16 at tex+0x40, H = u16 at tex+0x42,
 *   m = u16 at poly+2 & 0xf: bit0 repeat S, bit1 repeat T, bit2 flip S, bit3 flip T.
 *     S mode = !bit0 ? clamp : bit2 ? flip : wrap;   T mode = !bit1 ? clamp : bit3 ? flip : wrap
 *   tail-calls render_polygon_generate_texture_addresses_<S>_<T>_asm(out, uv, count, W, H, mask).
 *
 * render_polygon_generate_texture_addresses_<S>_<T>_asm (0x9a52c..0x9a888)(out, uv, count, W, H, mask)
 *   per pixel: u = s16 uv[2p], v = s16 uv[2p+1], k = (s16)(s8)mask[p]   (all arithmetic on 16-bit lanes;
 *   W, H truncated to 16 bits, Wm = (u16)(W-1), Hm = (u16)(H-1))
 *     clamp: u = min(max(u, 0), (s16)Wm)          (signed)
 *     wrap:  u = u & Wm
 *     flip:  if (u & (W & ~Wm)) u = ~u;  u = u & Wm    (W & ~Wm = W for a power of two: odd repeat -> mirror)
 *     same for v with H/Hm.  Then u &= k, v &= k (masked pixels fetch texel 0).
 *     out[p] = (u32)(u16)u + (u32)(u16)v * (u16)W      (u32 per pixel, 8 per block)
 *
 * render_polygon_load_texels_asm (0x9a8fc)(out, addrs, texels, count)
 *   out[p] = texels[addrs[p]]  (u32 texels, u32 addresses zero-extended). 8 per block, all 8 loaded first.
 *   flush passes texels = tex->+0x10 when tex->+0x4a (paletted flag) == 0.
 * render_polygon_load_texels_paletted_asm (0x9a88c)(out, addrs, indices, palette, count)
 *   out[p] = palette[indices[addrs[p]]]  (u8 indices, u32 palette). count is decremented as a 64-bit register
 *   (flush zero-extends a u32). flush passes indices = tex->+0x10, palette = tex->+0x18 when tex->+0x4a != 0.
 *
 * Texel format (output of load_texels, i.e. what the texture cache stores; from texture_cache_build_pixel*,
 * texture_cache_convert_palette*, not unit-tested here): the scanline pixel format
 *     r6 | g6 << 8 | b6 << 16 | a5 << 24,   r6 = r5*2 + (r5 != 0) (rast_expand555)
 *   alpha: direct colour (format 7): bit15 ? 31 : 0; palette formats 2/3/4 (4/16/256 colours): 31, except
 *   palette entry 0 has alpha 0 when TEXIMAGE_PARAM bit 29 (colour 0 transparent) is set; A3I5 (format 1): a 256-entry
 *   palette indexed by the raw texel byte, entry (a3 << 5 | i5) = colour[i5] | (4*a3 + a3/2) << 24; A5I3 (format 6)
 *   likewise with a5. bytes_per_dest_texel (0x11e4b8) = {0,1,1,1,1,4,1,4} per format: formats 1,2,3,4,6 are stored
 *   as u8 indices (paletted flag set by the palette converters), formats 5 (4x4 compressed) and 7 as u32 texels.
 *   Texel (u, v) is at index u + v*W (row-major, no padding).
 */
#include <stdint.h>
#include <string.h>
#include "texture.h"

static inline uint16_t ld16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void st16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void st32(void *p, uint32_t v) { memcpy(p, &v, 4); }

void spec_render_polygon_setup_uv_interpolants(const void *spans, void *out, uint32_t lines, uint32_t stride) {
    const uint8_t *sp = spans;
    uint8_t *base = out, *delta = (uint8_t *)out + (uint64_t)stride * 4;
    do {
        int32_t n = ld16(sp + 0x630);
        uint32_t uv0 = ld32(sp + 0x2c0), duv = ld32(sp + 0x370);
        int16_t du = (int16_t)duv, dv = (int16_t)(duv >> 16);
        uint32_t U = ((uint32_t)(int32_t)(int16_t)uv0 << 15) + (du > 0 ? 0x400 : 0);
        uint32_t V = ((uint32_t)(int32_t)(int16_t)(uv0 >> 16) << 15) + (dv > 0 ? 0x400 : 0);
        sp += 4;
        do {
            for (int i = 0; i < 8; i++) { st32(base + 8 * i, U); st32(base + 8 * i + 4, V); }
            for (int i = 0; i < 8; i++) st32(delta + 4 * i, duv);
            base += 64; delta += 32;
            n -= 8;
        } while (n > 0);
        base += (int64_t)n * 8; delta += (int64_t)n * 4;
    } while (--lines);
}

void spec_render_polygon_interpolate_uv(void *out, const void *in, const int16_t *weights, uint32_t count,
                                        uint32_t stride) {
    const uint8_t *b = in, *d = (const uint8_t *)in + (uint64_t)stride * 4, *w = (const uint8_t *)weights;
    uint8_t *o = out;
    int32_t n = (int32_t)count;
    do {
        uint32_t r[16];
        for (int i = 0; i < 8; i++) {
            int32_t wt = (int16_t)ld16(w + 2 * i);
            int32_t du = (int16_t)ld16(d + 4 * i), dv = (int16_t)ld16(d + 4 * i + 2);
            uint32_t au = ld32(b + 8 * i) + (uint32_t)(du * wt);
            uint32_t av = ld32(b + 8 * i + 4) + (uint32_t)(dv * wt);
            r[2 * i] = (uint16_t)((int16_t)(au >> 16) >> 3);
            r[2 * i + 1] = (uint16_t)((int16_t)(av >> 16) >> 3);
        }
        for (int i = 0; i < 16; i++) st16(o + 2 * i, (uint16_t)r[i]);
        w += 16; b += 64; d += 32; o += 32;
        n -= 8;
    } while (n > 0);
}

void spec_render_polygon_setup_rgb_interpolants(const void *spans, void *out, uint32_t lines, uint32_t stride) {
    const uint8_t *sp = spans;
    uint8_t *o = out;
    uint8_t *d0 = o, *d1 = o + stride, *d2 = o + 2 * (uint64_t)stride;
    uint8_t *b0 = o + 3 * (uint64_t)stride, *b1 = o + 5 * (uint64_t)stride, *b2 = o + 7 * (uint64_t)stride;
    do {
        uint32_t a = ld32(sp + 0x420), bb = ld32(sp + 0x580), c = ld32(sp + 0x4d0), dd = ld32(sp + 0x630);
        int32_t n = (uint16_t)dd;
        uint32_t B0 = (a & 0xffff) << 15, B1 = (a >> 16) << 15, B2 = (bb >> 16) << 15;
        uint16_t D0 = (uint16_t)c, D1 = (uint16_t)(c >> 16), D2 = (uint16_t)(dd >> 16);
        sp += 4;
        do {
            for (int i = 0; i < 8; i++) st32(b0 + 4 * i, B0);
            for (int i = 0; i < 8; i++) st32(b1 + 4 * i, B1);
            for (int i = 0; i < 8; i++) st32(b2 + 4 * i, B2);
            for (int i = 0; i < 8; i++) st16(d0 + 2 * i, D0);
            for (int i = 0; i < 8; i++) st16(d1 + 2 * i, D1);
            for (int i = 0; i < 8; i++) st16(d2 + 2 * i, D2);
            b0 += 32; b1 += 32; b2 += 32; d0 += 16; d1 += 16; d2 += 16;
            n -= 8;
        } while (n > 0);
        b0 += (int64_t)n * 4; b1 += (int64_t)n * 4; b2 += (int64_t)n * 4;
        d0 += (int64_t)n * 2; d1 += (int64_t)n * 2; d2 += (int64_t)n * 2;
    } while (--lines);
}

void spec_render_polygon_interpolate_rgb(void *out, const void *in, const int16_t *weights, uint32_t count,
                                         uint32_t stride) {
    const uint8_t *i8 = in, *w = (const uint8_t *)weights;
    uint64_t S = stride;
    const uint8_t *dl[3] = { i8, i8 + S, i8 + 2 * S }, *bs[3] = { i8 + 3 * S, i8 + 5 * S, i8 + 7 * S };
    uint8_t *ol[3] = { out, (uint8_t *)out + S, (uint8_t *)out + 2 * S };
    int32_t n = (int32_t)count;
    do {
        uint8_t r[3][8];
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 8; i++) {
                int32_t wt = (int16_t)ld16(w + 2 * i), dt = (int16_t)ld16(dl[c] + 2 * i);
                uint32_t acc = ld32(bs[c] + 4 * i) + (uint32_t)(dt * wt);
                r[c][i] = (uint8_t)(acc >> 18);
            }
        for (int c = 0; c < 3; c++) { memcpy(ol[c], r[c], 8); ol[c] += 8; dl[c] += 16; bs[c] += 32; }
        w += 16;
        n -= 8;
    } while (n > 0);
}

enum { CLAMP, WRAP, FLIP };

static inline uint16_t texcoord(uint16_t x, int mode, uint16_t size) {
    uint16_t m = (uint16_t)(size - 1);
    switch (mode) {
    case CLAMP: { int16_t s = (int16_t)x; if (s < 0) s = 0; if (s > (int16_t)m) s = (int16_t)m; return (uint16_t)s; }
    case WRAP: return x & m;
    default: { uint16_t f = size & (uint16_t)~m; if (x & f) x = (uint16_t)~x; return x & m; }
    }
}

static void texaddr(uint32_t *out, const void *uv, uint32_t count, uint32_t width, uint32_t height,
                    const uint8_t *mask, int ms, int mt) {
    const uint8_t *p = uv;
    uint8_t *o = (uint8_t *)out;
    uint16_t W = (uint16_t)width, H = (uint16_t)height;
    int32_t n = (int32_t)count;
    do {
        uint32_t r[8];
        for (int i = 0; i < 8; i++) {
            uint16_t k = (uint16_t)(int16_t)(int8_t)mask[i];
            uint16_t u = texcoord(ld16(p + 4 * i), ms, W) & k;
            uint16_t v = texcoord(ld16(p + 4 * i + 2), mt, H) & k;
            r[i] = (uint32_t)u + (uint32_t)v * W;
        }
        for (int i = 0; i < 8; i++) st32(o + 4 * i, r[i]);
        p += 32; o += 32; mask += 8;
        n -= 8;
    } while (n > 0);
}

#define SPEC_TEXADDR(s, t, MS, MT) \
    void spec_render_polygon_generate_texture_addresses_##s##_##t(uint32_t *out, const void *uv, uint32_t count, \
                                                                 uint32_t width, uint32_t height, const uint8_t *mask) \
    { texaddr(out, uv, count, width, height, mask, MS, MT); }
SPEC_TEXADDR(clamp, clamp, CLAMP, CLAMP) SPEC_TEXADDR(wrap, clamp, WRAP, CLAMP) SPEC_TEXADDR(flip, clamp, FLIP, CLAMP)
SPEC_TEXADDR(clamp, wrap, CLAMP, WRAP)   SPEC_TEXADDR(wrap, wrap, WRAP, WRAP)   SPEC_TEXADDR(flip, wrap, FLIP, WRAP)
SPEC_TEXADDR(clamp, flip, CLAMP, FLIP)   SPEC_TEXADDR(wrap, flip, WRAP, FLIP)   SPEC_TEXADDR(flip, flip, FLIP, FLIP)

void spec_render_polygon_generate_texture_addresses(const void *poly, uint32_t *out, const void *uv, uint32_t count,
                                                    const uint8_t *mask) {
    const uint8_t *tex;
    memcpy(&tex, (const uint8_t *)poly + POLY_TEXCACHE, sizeof tex);
    uint32_t m = ld16((const uint8_t *)poly + POLY_TEXWRAP) & 0xf;
    int ms = !(m & 1) ? CLAMP : (m & 4) ? FLIP : WRAP;
    int mt = !(m & 2) ? CLAMP : (m & 8) ? FLIP : WRAP;
    texaddr(out, uv, count, ld16(tex + TEXC_WIDTH), ld16(tex + TEXC_HEIGHT), mask, ms, mt);
}

void spec_render_polygon_load_texels(uint32_t *out, const uint32_t *addrs, const uint32_t *texels, uint32_t count) {
    int32_t n = (int32_t)count;
    do {
        uint32_t r[8];
        for (int i = 0; i < 8; i++) r[i] = texels[addrs[i]];
        for (int i = 0; i < 8; i++) out[i] = r[i];
        addrs += 8; out += 8;
        n -= 8;
    } while (n > 0);
}

void spec_render_polygon_load_texels_paletted(uint32_t *out, const uint32_t *addrs, const uint8_t *indices,
                                              const uint32_t *palette, uint32_t count) {
    int64_t n = count;
    do {
        uint32_t r[8];
        for (int i = 0; i < 8; i++) r[i] = palette[indices[addrs[i]]];
        for (int i = 0; i < 8; i++) out[i] = r[i];
        addrs += 8; out += 8;
        n -= 8;
    } while (n > 0);
}
