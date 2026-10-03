/* blend.c: exact C ports of DraStic r2.5.2.2's translucency blend, alpha combine and hi-res writeback routines.
 *
 * Pixel formats (scanline buffers, see ds3d.h):
 *   colour word  = r6 | g6<<8 | b6<<16 | a5<<24 | fog<<31   (byte 3 = a5 | fog flag 0x80)
 *   attribute    = depth (bits 0..23) | poly id << 24 (bits 24..29) | edge << 30 | shadow << 31
 *     - depth: the polygon's interpolated depth word as produced by render_polygon_interpolate_w/z_asm or
 *       set_buffer32 (w-buffer: interpolated w; z-buffer: z; constant: u16 << 9). Depth tests compare it against
 *       (attribute & 0x00ffffff), so the new word's own byte 3 is expected to be 0.
 *     - byte 3 of the NEW attribute before writeback is 0 except edge pixels: render_polygon_mark_edges_c stores
 *       the byte 0x40 into byte 3 of the first/last pixels of each line and of the whole first/last line.
 *       writeback then ORs in id << 24 (id = polygon attr bits 24..29), so edge pixels get bit 30.
 *     - bit 31 (shadow stencil) is set by shadow-mask polygons (poly id 0, mode 3) in render_polygon_flush_4x;
 *       nothing in this file sets it except by copying through attribute words.
 *   id buffer    = u8 per pixel (ctx+0x20000, 0x200 bytes per line): id of the last translucent polygon drawn
 *                  (cleared to 0xff per bin), used by render_polygon_alpha_id_test_asm.
 *
 * How render_polygon_flush_4x.isra.0 (0x4b2b0) uses them, per batch of n pixels (n = w19 = all span pixels of
 * the polygon in this bin, packed line after line; S = (2n + 29) & ~15, work = x5 of flush):
 *   opaque polygon (alpha 31 / all texels opaque, flush flag bit 0 clear):
 *     [fog: render_polygon_apply_fog_asm sets bit 31 of every colour]; [edge marking: mark_edges_c];
 *     if every pixel passed (pass count == n) writeback_all_pass_4x, else writeback_4x(mask = depth/alpha test
 *     result). Both: colours from work+6S, attrs from work+12S, poly id = (poly attr >> 24) & 63.
 *   translucent polygon (flush flag bit 0 set; DISP3DCNT bit 3 = alpha blending enable):
 *     load_depth_colors_id_4x loads the destination attrs (work+S), colours (work+14S), ids (work+17S);
 *     alpha_blend (blending on) or alpha_pass (off): colours work+6S vs dst colours -> alpha bytes at work+3S;
 *     alpha_id_test(mask work+16S, ids, alphas, n, poly id) clears mask where id matches for translucent pixels;
 *     alpha_combine variant chosen by (poly attr bit 11 = translucent depth update) | (bit 15 = fog) << 1:
 *       0 combine, 1 combine_depth, 2 combine_fog, 3 combine_depth_fog (fog ones set ctx+0x24014 = 1);
 *     writeback_alpha_4x(colours work+6S, combined attrs work+12S, ids work+17S) stores every span pixel.
 *   render_sprite_block_4x uses alpha_blend/pass and the *_constant combine variants (constant attribute word).
 *
 * ---------------------------------------------------------------------------------------------------------------
 * render_polygon_alpha_blend_asm (0x9aac8) (colors x0, dst x1, n w2, alpha_out x3)
 * render_polygon_alpha_pass_asm  (0x9ab68) (same arguments)
 *   Blocks of 16 pixels, do { ... n -= 16 } while (n > 0) with n signed 32-bit: always at least one block,
 *   and the whole last block is read and written (colors, dst, alpha_out rounded up to 16 pixels).
 *   Per pixel (bytes of src = colors[i], dst = dst[i]; all 8-bit lanes, 16-bit accumulators):
 *     sa = src byte 3 (whole byte, fog bit included), da = dst byte 3 & 0x1f
 *     alpha_out[i] = sa
 *     blend only:  if da == 0: w_src = 31, w_dst = 0   else: w_src = sa, w_dst = (31 - sa) & 0xff
 *                  for c in r,g,b (bytes 0..2): c' = ((c_src * (w_src + 1) + c_dst * w_dst) >> 5) & 0xff
 *                  (16-bit umlal accumulators; the wrap at 2^16 cannot affect bits 5..12, so plain ints are exact)
 *                  realistic inputs (sa, c <= 31/63): c' = (c_src * (sa + 1) + c_dst * (31 - sa)) >> 5, or c_src
 *                  when the destination alpha is 0 (no blending onto an empty pixel)
 *     both:        byte 3' = max(sa, da)     (pass leaves r,g,b unchanged)
 *   colors[i] is updated in place.
 *
 * render_polygon_alpha_combine*_asm (0x9af2c ... 0x9b300)
 *   (colors x0, attrs x1 | const attr w1, dst_colors x2, dst_attrs x3, ids x4, poly_id w5, alpha x6, mask x7,
 *    n [sp] (32-bit)).  Blocks of 16, do { } while ((n -= 16) > 0), whole blocks read/written.
 *   Per pixel: a = alpha[i] (blend's alpha_out), m = mask[i] (0x00/0xff normally; the selects are bitwise),
 *     op = (a == 0x1f) ? 0xff : 0, o = m & op (opaque pass), t = m & ~op (translucent pass), pid = poly_id & 0xff.
 *     sel(x, y, k) = (x & ~k) | (y & k) bytewise; M, O = m, o replicated into all 4 bytes.
 *     colour:  colors[i] = sel(dst_colors[i], colors[i], M)
 *              fog variants: src byte 3 |= 0x80 first; byte 3 select mask is
 *                 combine_fog / combine_depth_fog:  m & (op | 0x7f)   (translucent pixels keep dst's fog bit)
 *                 *_fog_constant:                   m               (fog bit always set where m)
 *     ids:     ids[i] = sel(ids[i], pid, t)                      (only translucent passing pixels record the id)
 *     attr:    new = (attrs[i] or const attr) | pid << 24
 *              plain/fog:  out = sel(dst_attrs[i], new, O)       (only opaque pixels update depth/id/edge)
 *              depth:      bytes 0..2 = sel(dst, new, m), byte 3 = sel(dst, new, o)
 *                          (translucent pixels update depth, keep dst id/edge/shadow byte)
 *     outputs: non-constant: colors, attrs (= out, overwriting the polygon's attr buffer), ids;
 *              constant:     colors, dst_attrs (= out, in place), ids.  dst_* read only otherwise.
 *
 * render_polygon_writeback_asm_4x (0x9c850)
 *   (spans x0, color_lines x1, attr_lines x2, nlines w3, poly_id w4, colors x5, attrs x6, mask x7)
 *   for each line l (do-while on nlines, so 0 = 2^32 lines): start = u16 spans[0x580 + 4l], cnt = u16
 *   spans[0x630 + 4l] (do-while: cnt 0 would run 2^32 pixels; never happens); for j < cnt, reading colors/attrs/
 *   mask sequentially (packed over lines): if mask byte != 0:
 *       attr_lines[l*512 + start + j] = attrs[k] | poly_id << 24;  color_lines[l*512 + start + j] = colors[k]
 *
 * render_polygon_writeback_all_pass_asm_4x (0x9c8b0)
 *   (spans x0, color_lines x1, attr_lines x2, nlines w3, poly_id w4, colors x5, attrs x6)
 *   same span walk (cnt 0 allowed here), every pixel written as above (no mask). Stores are exact; the 8-pixel
 *   tail loads read up to 8 words past the batch end. Requires start + cnt <= 512 (line pointers advance by
 *   512 - start - cnt, zero-extended).
 *
 * render_polygon_writeback_alpha_asm_4x (0x9ca08)
 *   (spans x0, color_lines x1, attr_lines x2, id_lines x3 (u8, 0x200 per line), nlines w4, colors x5, attrs x6,
 *    ids x7)
 *   same span walk (cnt 0 allowed); every pixel: color = colors[k], attr = attrs[k] (no id OR: already combined),
 *   id_lines[l*512 + start + j] = ids[k].
 *   QUIRK: per line the pixels are stored 8 at a time and the remaining cnt % 8 by a jump table; for a
 *   remainder of 5 the id of tail pixel 4 is taken from ids[k + 1] (st1 {v4.b}[5] instead of [4]), i.e. the id
 *   of the next pixel (possibly the next line's first pixel or the byte past the batch).
 */
#include "blend.h"

#define SPAN_START(sp, l) (((const uint16_t *)((const uint8_t *)(sp) + 0x580))[2 * (l)])
#define SPAN_COUNT(sp, l) (((const uint16_t *)((const uint8_t *)(sp) + 0x630))[2 * (l)])

static void blend_common(uint32_t *colors, const uint32_t *dst, int32_t n, uint8_t *alpha_out, int blend) {
    do {
        for (int i = 0; i < 16; i++) {
            uint32_t s = colors[i], d = dst[i];
            uint8_t sa = (uint8_t)(s >> 24), da = (uint8_t)(d >> 24) & 0x1f;
            uint32_t out = s & 0x00ffffff;
            if (blend) {
                uint8_t ws = da ? sa : 0x1f, wd = da ? (uint8_t)(0x1f - sa) : 0;
                out = 0;
                for (int c = 0; c < 3; c++) {
                    uint16_t cs = (uint8_t)(s >> (8 * c)), cd = (uint8_t)(d >> (8 * c));
                    uint16_t acc = (uint16_t)(cs + cs * ws + cd * wd);
                    out |= (uint32_t)(uint8_t)(acc >> 5) << (8 * c);
                }
            }
            alpha_out[i] = sa;
            colors[i] = out | (uint32_t)(sa > da ? sa : da) << 24;
        }
        colors += 16; dst += 16; alpha_out += 16;
        n -= 16;
    } while (n > 0);
}

void spec_render_polygon_alpha_blend(uint32_t *colors, const uint32_t *dst_colors, int32_t n, uint8_t *alpha_out) {
    blend_common(colors, dst_colors, n, alpha_out, 1);
}
void spec_render_polygon_alpha_pass(uint32_t *colors, const uint32_t *dst_colors, int32_t n, uint8_t *alpha_out) {
    blend_common(colors, dst_colors, n, alpha_out, 0);
}

static inline uint32_t sel(uint32_t x, uint32_t y, uint32_t k) { return (x & ~k) | (y & k); }

enum { CB_DEPTH = 1, CB_FOG = 2, CB_FOGC = 4, CB_CONST = 8 };

static void combine_common(uint32_t *colors, uint32_t *attrs, uint32_t cattr, const uint32_t *dcol,
                           uint32_t *dattr, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha,
                           const uint8_t *mask, int32_t n, int fl) {
    uint8_t pid = (uint8_t)poly_id;
    do {
        for (int i = 0; i < 16; i++) {
            uint8_t m = mask[i], op = alpha[i] == 0x1f ? 0xff : 0;
            uint8_t o = m & op, t = m & (uint8_t)~op;
            uint32_t M = m * 0x01010101u, O = o * 0x01010101u;
            /* colour */
            uint32_t src = colors[i], cm = M;
            if (fl & (CB_FOG | CB_FOGC)) {
                src |= 0x80000000u;
                if (fl & CB_FOG) cm = (M & 0x00ffffffu) | (uint32_t)(m & (op | 0x7f)) << 24;
            }
            colors[i] = sel(dcol[i], src, cm);
            /* id */
            ids[i] = (uint8_t)sel(ids[i], pid, t);
            /* attribute */
            uint32_t nw = ((fl & CB_CONST) ? cattr : attrs[i]) | (uint32_t)pid << 24;
            uint32_t am = (fl & CB_DEPTH) ? ((M & 0x00ffffffu) | (O & 0xff000000u)) : O;
            uint32_t out = sel(dattr[i], nw, am);
            if (fl & CB_CONST) dattr[i] = out; else attrs[i] = out;
        }
        colors += 16; dcol += 16; dattr += 16; ids += 16; alpha += 16; mask += 16;
        if (!(fl & CB_CONST)) attrs += 16;
        n -= 16;
    } while (n > 0);
}

#define COMBINE(name, fl) \
void spec_render_polygon_alpha_combine##name(uint32_t *colors, uint32_t *attrs, const uint32_t *dst_colors, \
    const uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n) \
{ combine_common(colors, attrs, 0, dst_colors, (uint32_t *)dst_attrs, ids, poly_id, alpha, mask, n, fl); }
#define COMBINE_C(name, fl) \
void spec_render_polygon_alpha_combine##name(uint32_t *colors, uint32_t attr, const uint32_t *dst_colors, \
    uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n) \
{ combine_common(colors, 0, attr, dst_colors, dst_attrs, ids, poly_id, alpha, mask, n, (fl) | CB_CONST); }

COMBINE(, 0)
COMBINE(_depth, CB_DEPTH)
COMBINE(_fog, CB_FOG)
COMBINE(_depth_fog, CB_DEPTH | CB_FOG)
COMBINE_C(_constant, 0)
COMBINE_C(_depth_constant, CB_DEPTH)
COMBINE_C(_fog_constant, CB_FOGC)
COMBINE_C(_depth_fog_constant, CB_DEPTH | CB_FOGC)

void spec_render_polygon_writeback_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint32_t nlines, uint32_t poly_id, const uint32_t *colors, const uint32_t *attrs, const uint8_t *mask) {
    uint32_t id = poly_id << 24, l = 0;
    do {
        uint32_t start = SPAN_START(spans, l), cnt = SPAN_COUNT(spans, l), j = 0;
        do {
            uint8_t m = *mask++;
            uint32_t a = *attrs++ | id, c = *colors++;
            if (m) { attr_lines[start + j] = a; color_lines[start + j] = c; }
            j++;
        } while (--cnt);
        color_lines += 512; attr_lines += 512; l++;
    } while (--nlines);
}

void spec_render_polygon_writeback_all_pass_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint32_t nlines, uint32_t poly_id, const uint32_t *colors, const uint32_t *attrs) {
    uint32_t id = poly_id << 24, l = 0;
    do {
        uint32_t start = SPAN_START(spans, l), cnt = SPAN_COUNT(spans, l);
        for (uint32_t j = 0; j < cnt; j++) {
            attr_lines[start + j] = *attrs++ | id;
            color_lines[start + j] = *colors++;
        }
        color_lines += 512; attr_lines += 512; l++;
    } while (--nlines);
}

void spec_render_polygon_writeback_alpha_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint8_t *id_lines, uint32_t nlines, const uint32_t *colors, const uint32_t *attrs, const uint8_t *ids) {
    uint32_t l = 0;
    do {
        uint32_t start = SPAN_START(spans, l), cnt = SPAN_COUNT(spans, l);
        uint32_t tail = cnt & 7;
        for (uint32_t j = 0; j < cnt; j++) {
            attr_lines[start + j] = attrs[j];
            color_lines[start + j] = colors[j];
            uint32_t k = j;
            if (tail == 5 && j == cnt - 1) k = j + 1;   /* st1 {v4.b}[5] quirk */
            id_lines[start + j] = ids[k];
        }
        colors += cnt; attrs += cnt; ids += cnt;
        color_lines += 512; attr_lines += 512; id_lines += 512; l++;
    } while (--nlines);
}
