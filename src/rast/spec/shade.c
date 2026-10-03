/* shade.c: exact ports of DraStic r2.5.2.2's polygon shading stage and the per-pixel tests/flags around it.
 *
 * Where these run: render_polygon_flush_4x.isra.0 (0x4b2b0) processes a batch of `count` pixels (<= 512, spanning
 * several lines of one polygon in one bin) through a scratch area `S` (its x5). With N = (2*count + 29) & ~15 (a
 * stride in bytes, always >= 16 and >= count + 15 for count >= 1):
 *   S + N    u32[count]  dest colour per pixel (loaded from the scanline buffer)
 *   S + 3N   u8 r[N], g[N], b[N]: interpolated vertex colour planes ("rgb", 6-bit values); for untextured flat
 *            polygons (flush flag bit 2) filled with poly colour >> 3 per channel; later reused by alpha_blend /
 *            alpha_pass as a per-pixel u8 alpha array (alpha_id_test's 3rd argument)
 *   S + 6N   u32[]  uv -> texel addresses -> texels (scanline format) -> shaded colour ("col"); shade runs in place
 *   S + 12N  u32[count]  depth/attribute word per pixel ("attr"); byte 3 receives the edge flag 0x40
 *   S + 14N  u32[] dest attribute (load_depth*)
 *   S + 16N  u8[count]  pass mask (0xff = pixel still drawn, from the depth test); pass count kept in a local u32
 *   S + 17N  u8[count]  dest translucent polygon id
 * Call sequence in flush (after the depth test, if any pixel passed):
 *   textured  (flag bit 1): ... load_texels -> render_polygon_shade(sys+0x1056c0, geom, poly, col, col, rgb, N, A, count)
 *                           -> render_polygon_alpha_test_asm(mask, col, *(u32*)(sys+0x34eb44), count, &pass);
 *                              stop if pass == 0
 *   untextured:  only if A > *(u32*)(sys+0x34eb44) (polygon-level alpha test, unsigned):
 *                render_polygon_shade_untextured(sys+0x1056c0, geom, poly, col, rgb, N, A, count)
 *   translucent batch (flag bit 0): alpha_blend/alpha_pass, then render_polygon_alpha_id_test_asm(mask, S+17N,
 *                rgb (now u8 alpha), count, ID), alpha_combine*, writeback_alpha.
 *   opaque batch:   if (poly attr bit 15, fog) { ctx[0x24014] = 1; render_polygon_apply_fog_asm(col, count) }
 *                   if (DISP3DCNT bit 5, edge marking) render_polygon_mark_edges_c(spans, S+12N, lines)
 *                   writeback / writeback_all_pass (pass == count).
 *   where sys = *(u64*)(ctx+0x24000), geom = *(u64*)(ctx+0x24008), poly = the 32-byte polygon record,
 *   A = (attr >> 16) & 31 (polygon alpha), ID = (attr >> 24) & 63.
 *
 * Polygon record +4 (u32 attr; = DS POLYGON_ATTR): bits 4-5 mode (0 modulate, 1 decal, 2 toon/highlight, 3 shadow),
 *   bit 11 / bit 15 (set-new-depth / fog) select alpha_combine variants, bit 14 depth-equal test, bit 15 fog,
 *   bits 16-20 alpha, bits 24-29 polygon id. Only bits 4-5 are read by the routines in this file (the rest by flush).
 * a0 (= sys + 0x1056c0): u32 at a0+0x249480 = sys+0x34eb40 = DISP3DCNT; bit 1 = highlight (vs toon) shading.
 *   (flush also reads DISP3DCNT bit 3 alpha blending, bit 5 edge marking; sys+0x34eb44 = alpha test reference.)
 * geom + 0x99cc: toon table as three u8[32] planes, r at +0x00, g at +0x20, b at +0x40, each entry c5 ? 2*c5+1 : 0
 *   (geometry_store_toon_table_entry 0x5f930; raw BGR555 entries are at geom+0x9934). Lookups index with
 *   i = rgb_r >> 1 (0..127): the scalar lookups below read geom+0x99cc+i(+0x20/+0x40) with no range limit (indices
 *   >= 32 read the next plane / bytes past the table), the NEON toon_load gives 0 for i >= 32.
 *
 * Shading per pixel (vertex colour v = (vr, vg, vb) from the rgb planes, texel t = (tr, tg, tb, ta) bytes of tex,
 * all 8-bit bytes of the u32 incl. any high bits; A = polygon alpha):
 *   modulate (mode 0 and 3):  c = ((v+1)*(t+1)-1) >> 6 per channel, a = ((A+1)*(ta+1)-1) >> 5, each & 0xff
 *   decal (mode 1):           ta' = ta == 31 ? 32 : ta;  w = ta == 31 ? 0 : ta == 0 ? 32 : 31 - ta  (u32 wrap)
 *                             c = (t*ta' + v*w) >> 5 per channel, u32 math, NOT masked; out = r | g<<8 | b<<16 | A<<24
 *                             (alpha comes from the polygon only)
 *   toon (mode 2, DISP3DCNT bit1 = 0): v := toon[vr >> 1] (tbl lookup: 0 for index >= 32), then modulate
 *   highlight (mode 2, bit1 = 1):  s = vr; c = modulate with (s, s, s) (red plane used for all three channels);
 *                             then for i < count: c' = min(63, toon_ch[s>>1] + (c_ch & 63)) per channel,
 *                             alpha bits = c & 0x1f000000 (bits 29-31 cleared)
 *   untextured, mode != 2:    out = vr | vg<<8 | vb<<16 | (A&0xff)<<24 (combine_colors)
 *   untextured toon:          out = T_r | T_g<<8 | T_b<<16 | A<<24, T = toon planes at index vr>>1 (no limit)
 *   untextured highlight:     out = min(63, T_r + vr) | min(63, T_g + vg)<<8 | min(63, T_b + vb)<<16 | A<<24
 * The NEON routines process 16 (alpha_id_test: 32) elements per iteration, do-while: count <= 16 (incl. 0) does one
 * chunk, and they write whole chunks past count. The C routines (decal, highlight loops, untextured toon) write
 * exactly count elements (none for count 0).
 */
#include "shade.h"
#include <string.h>

#define U8(p, o)  (((const uint8_t *)(p))[o])
#define U16(p, o) (*(const uint16_t *)((const uint8_t *)(p) + (o)))
#define U32(p, o) (*(const uint32_t *)((const uint8_t *)(p) + (o)))

/* render_polygon_modulate_asm (0x9a94c): (dst x0, tex x1, rgb x2, stride w3, alpha w4, count w5)
 * Reads tex[16k..16k+15] (u32), rgb[i], rgb[stride+i], rgb[2*stride+i]; writes dst[16k..16k+15] for each chunk
 * k < max(1, ceil(count/16)) (count as signed int). Per pixel, per channel (bytes):
 *   r = ((vr+1)*(tr+1)-1) >> 6, g, b likewise, a = ((alpha+1)*(ta+1)-1) >> 5, all truncated to 8 bits.
 * Each chunk is loaded completely before it is stored (in-place dst == tex is the normal use). */
void spec_render_polygon_modulate(uint32_t *dst, const uint32_t *tex, const uint8_t *rgb, uint32_t stride,
                                  uint32_t alpha, uint32_t count) {
    const uint8_t *pr = rgb, *pg = rgb + stride, *pb = rgb + 2 * (uint64_t)stride;
    uint32_t a8 = alpha & 0xff;
    int32_t n = (int32_t)count;
    do {
        uint8_t vr[16], vg[16], vb[16]; uint32_t t[16], o[16];
        memcpy(vr, pr, 16); memcpy(vg, pg, 16); memcpy(vb, pb, 16); memcpy(t, tex, 64);
        for (int i = 0; i < 16; i++) {
            uint32_t tr = t[i] & 0xff, tg = (t[i] >> 8) & 0xff, tb = (t[i] >> 16) & 0xff, ta = t[i] >> 24;
            uint32_t r = ((vr[i] * tr + vr[i] + tr) & 0xffff) >> 6;
            uint32_t g = ((vg[i] * tg + vg[i] + tg) & 0xffff) >> 6;
            uint32_t b = ((vb[i] * tb + vb[i] + tb) & 0xffff) >> 6;
            uint32_t a = ((a8 * ta + a8 + ta) & 0xffff) >> 5;
            o[i] = (r & 0xff) | (g & 0xff) << 8 | (b & 0xff) << 16 | (a & 0xff) << 24;
        }
        memcpy(dst, o, 64);
        pr += 16; pg += 16; pb += 16; tex += 16; dst += 16;
        n -= 16;
    } while (n > 0);
}

/* render_polygon_modulate_red_asm (0x9aa1c): (dst x0, tex x1, red x2, alpha w3, count x4 (64-bit, zero-extended))
 * Same as modulate but all three colour channels use the red plane red[i]: c_ch = ((red+1)*(t_ch+1)-1) >> 6. */
void spec_render_polygon_modulate_red(uint32_t *dst, const uint32_t *tex, const uint8_t *red, uint32_t alpha,
                                      uint32_t count) {
    uint32_t a8 = alpha & 0xff;
    int64_t n = count;
    do {
        uint8_t v[16]; uint32_t t[16], o[16];
        memcpy(v, red, 16); memcpy(t, tex, 64);
        for (int i = 0; i < 16; i++) {
            uint32_t tr = t[i] & 0xff, tg = (t[i] >> 8) & 0xff, tb = (t[i] >> 16) & 0xff, ta = t[i] >> 24;
            uint32_t r = ((v[i] * tr + v[i] + tr) & 0xffff) >> 6;
            uint32_t g = ((v[i] * tg + v[i] + tg) & 0xffff) >> 6;
            uint32_t b = ((v[i] * tb + v[i] + tb) & 0xffff) >> 6;
            uint32_t a = ((a8 * ta + a8 + ta) & 0xffff) >> 5;
            o[i] = (r & 0xff) | (g & 0xff) << 8 | (b & 0xff) << 16 | (a & 0xff) << 24;
        }
        memcpy(dst, o, 64);
        red += 16; tex += 16; dst += 16;
        n -= 16;
    } while (n > 0);
}

/* render_polygon_toon_load_asm (0x9a9d8): (toon x0, rgb x1, stride w2, count w3)
 * Loads the 96-byte toon table first, then per chunk of 16: i = rgb_r[j] >> 1; writes rgb_r[j] = toon_r[i],
 * rgb_g[j] = toon_g[i], rgb_b[j] = toon_b[i] (each 0 when i >= 32); r, g, b planes stored in that order. */
void spec_render_polygon_toon_load(const uint8_t *toon, uint8_t *rgb, uint32_t stride, uint32_t count) {
    uint8_t tab[96];
    memcpy(tab, toon, 96);
    uint8_t *pr = rgb, *pg = rgb + stride, *pb = rgb + 2 * (uint64_t)stride;
    int32_t n = (int32_t)count;
    do {
        uint8_t idx[16], r[16], g[16], b[16];
        memcpy(idx, pr, 16);
        for (int j = 0; j < 16; j++) {
            uint32_t i = idx[j] >> 1;
            r[j] = i < 32 ? tab[i] : 0;
            g[j] = i < 32 ? tab[0x20 + i] : 0;
            b[j] = i < 32 ? tab[0x40 + i] : 0;
        }
        memcpy(pr, r, 16); memcpy(pg, g, 16); memcpy(pb, b, 16);
        pr += 16; pg += 16; pb += 16;
        n -= 16;
    } while (n > 0);
}

/* render_polygon_combine_colors_asm (0x9aa98): (dst x0, rgb x1, stride w2, count w3, alpha w4)
 * dst[i] = r[i] | g[i]<<8 | b[i]<<16 | (alpha & 0xff)<<24, whole chunks of 16. */
void spec_render_polygon_combine_colors(uint32_t *dst, const uint8_t *rgb, uint32_t stride, uint32_t count,
                                        uint32_t alpha) {
    const uint8_t *pr = rgb, *pg = rgb + stride, *pb = rgb + 2 * (uint64_t)stride;
    int32_t n = (int32_t)count;
    do {
        uint8_t r[16], g[16], b[16]; uint32_t o[16];
        memcpy(r, pr, 16); memcpy(g, pg, 16); memcpy(b, pb, 16);
        for (int i = 0; i < 16; i++) o[i] = r[i] | (uint32_t)g[i] << 8 | (uint32_t)b[i] << 16 | (alpha & 0xff) << 24;
        memcpy(dst, o, 64);
        pr += 16; pg += 16; pb += 16; dst += 16;
        n -= 16;
    } while (n > 0);
}

/* render_polygon_decal_c (0x486c0): (dst x0, tex x1, rgb x2, stride w3, alpha w4, count w5)
 * For i < count (exactly; nothing for 0): t = tex[i]; ta = t >> 24 (full byte);
 *   a = ta == 31 ? 32 : ta;  w = ta == 31 ? 0 : (ta == 0 ? 32 : 31 - ta)    (u32, wraps for ta > 31)
 *   r = (tr*a + vr*w) >> 5, g, b likewise (u32, unmasked);  dst[i] = r | g<<8 | b<<16 | alpha<<24 (u32 ops).
 * (The original's NEON fast path is taken only without overlap and computes the same.) */
void spec_render_polygon_decal_c(uint32_t *dst, const uint32_t *tex, const uint8_t *rgb, uint32_t stride,
                                 uint32_t alpha, uint32_t count) {
    const uint8_t *pg = rgb + stride, *pb = rgb + 2 * (uint64_t)stride;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t t = tex[i];
        uint32_t tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff, ta = t >> 24;
        uint32_t vr = rgb[i], vg = pg[i], vb = pb[i];
        uint32_t a, w;
        if (ta == 31) { a = 32; w = 0; }
        else { a = ta; w = ta ? 31 - ta : 32; }
        uint32_t r = (tr * a + vr * w) >> 5, g = (tg * a + vg * w) >> 5, b = (tb * a + vb * w) >> 5;
        dst[i] = (b << 16 | g << 8) | (r | alpha << 24);
    }
}

/* render_polygon_shade (0x48cc0): (a0 x0, geom x1, poly x2, dst x3, tex x4, rgb x5, stride w6, alpha w7,
 *                                  count [sp] u32)
 * mode = (u32 at poly+4 >> 4) & 3:
 *   1: decal_c(dst, tex, rgb, stride, alpha, count)
 *   2, DISP3DCNT (u32 at a0+0x249480) bit 1 set (highlight):
 *      modulate_red_asm(dst, tex, rgb, alpha, count); then for i < count:
 *        s = rgb[i] >> 1; T = geom+0x99cc; c = dst[i];
 *        dst[i] = min(63, T[s] + (c&63)) | min(63, T[0x20+s] + (c>>8&63))<<8 | min(63, T[0x40+s] + (c>>16&63))<<16
 *                 | (c & 0x1f000000)
 *   2, bit 1 clear (toon): toon_load_asm(geom+0x99cc, rgb, stride, count) (rewrites the rgb planes!),
 *      then modulate_asm(dst, tex, rgb, stride, alpha, count)
 *   0, 3: modulate_asm(dst, tex, rgb, stride, alpha, count) */
void spec_render_polygon_shade(const void *a0, const void *geom, const void *poly, uint32_t *dst, const uint32_t *tex,
                               uint8_t *rgb, uint32_t stride, uint32_t alpha, uint32_t count) {
    uint32_t mode = (U32(poly, 4) >> 4) & 3;
    if (mode == 1) { spec_render_polygon_decal_c(dst, tex, rgb, stride, alpha, count); return; }
    if (mode == 2) {
        const uint8_t *T = (const uint8_t *)geom + SHADE_GEOM_TOON;
        if (U32(a0, SHADE_A0_DISP3DCNT) & 2) {
            spec_render_polygon_modulate_red(dst, tex, rgb, alpha, count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t s = rgb[i] >> 1, c = dst[i];
                uint32_t r = T[s] + (c & 63), g = T[0x20 + s] + ((c >> 8) & 63), b = T[0x40 + s] + ((c >> 16) & 63);
                if (r > 63) r = 63;
                if (g > 63) g = 63;
                if (b > 63) b = 63;
                dst[i] = b << 16 | g << 8 | r | (c & 0x1f000000);
            }
            return;
        }
        spec_render_polygon_toon_load(T, rgb, stride, count);
    }
    spec_render_polygon_modulate(dst, tex, rgb, stride, alpha, count);
}

/* render_polygon_shade_untextured (0x4a620): (a0 x0, geom x1, poly x2, dst x3, rgb x4, stride w5, alpha w6, count w7)
 * mode != 2: combine_colors_asm(dst, rgb, stride, count, alpha) (16-pixel chunks).
 * mode == 2 (exactly count pixels): s = rgb_r[i] >> 1, T = geom+0x99cc (no index limit), A = alpha << 24:
 *   toon (DISP3DCNT bit 1 clear): dst[i] = T[s] | T[0x20+s]<<8 | T[0x40+s]<<16 | A
 *   highlight (bit 1 set):        dst[i] = min(63, T[s]+r) | min(63, T[0x20+s]+g)<<8 | min(63, T[0x40+s]+b)<<16 | A
 *   with r = rgb[i], g = rgb[stride+i], b = rgb[2*stride+i]. */
void spec_render_polygon_shade_untextured(const void *a0, const void *geom, const void *poly, uint32_t *dst,
                                          const uint8_t *rgb, uint32_t stride, uint32_t alpha, uint32_t count) {
    uint32_t mode = (U32(poly, 4) >> 4) & 3;
    if (mode != 2) { spec_render_polygon_combine_colors(dst, rgb, stride, count, alpha); return; }
    const uint8_t *T = (const uint8_t *)geom + SHADE_GEOM_TOON;
    uint32_t A = alpha << 24;
    if (U32(a0, SHADE_A0_DISP3DCNT) & 2) {
        const uint8_t *pg = rgb + stride, *pb = pg + stride;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = rgb[i], s = v >> 1;
            uint32_t r = T[s] + v, g = T[0x20 + s] + pg[i], b = T[0x40 + s] + pb[i];
            if (r > 63) r = 63;
            if (g > 63) g = 63;
            if (b > 63) b = 63;
            dst[i] = (b << 16 | g << 8) | (r | A);
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = rgb[i] >> 1;
            dst[i] = ((uint32_t)T[0x40 + s] << 16 | (uint32_t)T[0x20 + s] << 8) | (T[s] | A);
        }
    }
}

/* ushl on one 64-bit lane: shift by the signed low byte of the shift element */
static uint64_t ushl64(uint64_t x, uint32_t sh) {
    int s = (int8_t)(sh & 0xff);
    if (s >= 64 || s <= -64) return 0;
    return s >= 0 ? x << s : x >> -s;
}

/* render_polygon_alpha_test_asm (0x9a270): (mask x0, color x1, ref w2, count w3, pass x4)
 * Per chunk of 16 (count signed, at least one chunk): mask[i] &= (color[i] >> 24) > (ref & 0xff) ? 0xff : 0
 * (unsigned compare on the whole top byte); whole chunks are written. *pass (u32) = sum over 16 byte lanes of
 * (-sum over chunks of the new mask bytes) mod 256 per lane, only the first `count` pixels counted (the last chunk's
 * excess lanes are shifted out; count 0 counts nothing). With 0x00/0xff masks this is the number of passing pixels. */
void spec_render_polygon_alpha_test(uint8_t *mask, const uint32_t *color, uint32_t ref, uint32_t count,
                                    uint32_t *pass) {
    uint8_t acc[16] = {0}, r8 = ref & 0xff;
    int32_t n = (int32_t)count;
    for (;;) {
        n -= 16;
        uint8_t m[16], v[16]; uint32_t c[16];
        memcpy(m, mask, 16); memcpy(c, color, 64);
        for (int i = 0; i < 16; i++) v[i] = m[i] & ((c[i] >> 24) > r8 ? 0xff : 0);
        memcpy(mask, v, 16);
        if (n > 0) {
            for (int i = 0; i < 16; i++) acc[i] -= v[i];
            mask += 16; color += 16;
            continue;
        }
        uint32_t w6 = (0u - (uint32_t)n) << 3, w5 = w6 - 64;
        if ((int32_t)w5 < 0) w5 = 0;
        uint64_t d0, d1;
        memcpy(&d0, v, 8); memcpy(&d1, v + 8, 8);
        d0 = ushl64(d0, w5); d1 = ushl64(d1, w6);
        memcpy(v, &d0, 8); memcpy(v + 8, &d1, 8);
        for (int i = 0; i < 16; i++) acc[i] -= v[i];
        break;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += acc[i];
    *pass = sum & 0xffff;
}

/* render_polygon_alpha_id_test_asm (0x9a2f0): (mask x0, ids x1, alpha x2, count w3, id w4)
 * Per chunk of 32 bytes (count signed, at least one chunk, whole chunks written):
 *   mask[i] &= ~((ids[i] == (id & 0xff) && alpha[i] != 0x1f) ? 0xff : 0)
 * i.e. a pixel is dropped when the destination translucent id equals this polygon's id, unless alpha[i] == 31. */
void spec_render_polygon_alpha_id_test(uint8_t *mask, const uint8_t *ids, const uint8_t *alpha, uint32_t count,
                                       uint32_t id) {
    uint8_t id8 = id & 0xff;
    int32_t n = (int32_t)count;
    do {
        uint8_t m[32], d[32], a[32];
        memcpy(m, mask, 32); memcpy(d, ids, 32); memcpy(a, alpha, 32);
        for (int i = 0; i < 32; i++) if (d[i] == id8 && a[i] != 0x1f) m[i] = 0;
        memcpy(mask, m, 32);
        mask += 32; ids += 32; alpha += 32;
        n -= 32;
    } while (n > 0);
}

/* render_polygon_apply_fog_asm (0x9b3a8): (color x0, count x1 (64-bit, zero-extended); w2 = 1 from flush, unused)
 * color[i] |= 0x80000000 (fog flag) for whole chunks of 16, at least one. */
void spec_render_polygon_apply_fog(uint32_t *color, uint32_t count) {
    int64_t n = count;
    do {
        for (int i = 0; i < 16; i++) color[i] |= 0x80000000u;
        color += 16;
        n -= 16;
    } while (n > 0);
}

/* render_polygon_mark_edges_c (0x4a460): (spans x0, attr x1 (u32 per pixel, packed lines), lines w2 >= 1)
 * Span arrays (stride 4 per line l): C(l) = u16 at spans+0x630+4l (pixel count), L(l) = u16 at +0x6e0+4l
 * (left edge pixels), R(l) = u16 at +0x6e2+4l (right edge pixels). Lines are packed back to back in attr. Writes
 * byte 3 (bits 24-31) of attr words = 0x40 for the first L(l) and the last R(l) pixels of each line l. Exactly:
 *   p = attr+3; mark L(0); p += 4*L(0);
 *   for l in 0..lines-2: p += (int32)((C(l) - L(l) - R(l)) << 2); mark R(l) + L(l+1) bytes at stride 4; advance;
 *   p += (int32)((C(last) - L(last) - R(last)) << 2); mark R(last).
 * (So with C < L + R the pointer walks backwards; no clamping.) */
void spec_render_polygon_mark_edges_c(const void *spans, void *attr, uint32_t lines) {
    uint8_t *p = (uint8_t *)attr + 3;
    uint32_t w5 = U16(spans, 0x6e0);
    for (uint32_t k = 0; k < w5; k++) p[4 * (uint64_t)k] = 0x40;
    p += 4 * (uint64_t)w5;
    uint64_t last = 0;
    if (lines != 1) {
        for (uint64_t l = 0; l < (uint64_t)(uint32_t)(lines - 2) + 1; l++) {
            uint32_t c = U16(spans, 0x630 + 4 * l), r = U16(spans, 0x6e2 + 4 * l);
            p += (int32_t)((c - w5 - r) << 2);
            w5 = U16(spans, 0x6e4 + 4 * l);
            uint32_t n = r + w5;
            for (uint32_t k = 0; k < n; k++) p[4 * (uint64_t)k] = 0x40;
            p += 4 * (uint64_t)n;
        }
        last = (uint64_t)(uint32_t)(lines - 2) + 1;
    }
    uint32_t c = U16(spans, 0x630 + 4 * last), r = U16(spans, 0x6e2 + 4 * last);
    p += (int32_t)((c - w5 - r) << 2);
    for (uint32_t k = 0; k < r; k++) p[4 * (uint64_t)k] = 0x40;
}
