/* resolve.c: exact C ports of DraStic r2.5.2.2's hi-res ("4x") bin resolve: after all polygons of a bin (32 lines of
 * 512 pixels) are drawn into a render context's scanline buffers, these convert the colour buffer into the output
 * frame, optionally applying fog and edge marking. Verified bit-exact against the originals by
 * tools/rast/ut/t_resolve.c (whole output block, context buffers and system-struct gap buffers compared).
 *
 * ===================================================================================================================
 * MEMORY
 * ===================================================================================================================
 * Render context ctx (0x24100 bytes, see ds3d.h):
 *   ctx+0x00000  u32 colour[32][512]  (line y at +y*0x800): r6 | g6<<8 | b6<<16 | a5<<24 | fog<<31
 *                (byte 0 = r, 1 = g, 2 = b, byte 3 = alpha in bits 0-4 (bits 5,6 normally 0), bit 7 = fog flag)
 *   ctx+0x10000  u32 attr[32][512]    (line y at +0x10000 + y*0x800). Fields used here:
 *                bits 0-23  : compared as a 24-bit unsigned number for edge detection ("depth" ordering key);
 *                bits 9-23  : 15-bit fog depth (attr >> 9) & 0x7fff;
 *                bits 24-29 : polygon id (6 bits); bit 30: edge-id flag (see edge identify); bit 31 ignored.
 *   ctx+0x24000  u64 sys  (system struct)
 *   ctx+0x24008  u64 geom (geometry state)
 *   ctx+0x24014  u32 "fog used in this bin" flag (fog drivers fall back to the non-fog resolve when 0)
 *
 * System struct sys (offsets read/written here):
 *   sys+0x34eb40 u32 DISP3DCNT          fog shift = (DISP3DCNT >> 8) & 0xf
 *   sys+0x34eb4c u32 clear attribute    (the attribute used for pixels outside the frame in edge identification)
 *   sys+0x34eb50 u32 fog active flag    (video_3d_prepare_fog: -1, or 0 when the fog table is all zero);
 *                                       fog drivers fall back to the non-fog resolve when 0
 *   sys+0x34eb58 u64 output frame       (12 bins x 0x10000), read by the gap passes only
 *   sys+0x2c1748 u64 geom pointer       read by the gap passes only
 *   sys+0x32db40 attr gap buffers:   11 slots of 0x2000 (slot g = boundary between bin g and bin g+1):
 *                                    slot+0x0000 attr line 30 of bin g, +0x0800 attr line 31 of bin g,
 *                                    slot+0x1000 attr line 0 of bin g+1, +0x1800 attr line 1 of bin g+1
 *   sys+0x343b40 colour gap buffers: 11 slots of 0x1000: +0x000 colour line 31 of bin g, +0x800 colour line 0 of
 *                                    bin g+1 (unfogged copies; the fog gap passes fog them in place)
 *   The gap passes receive vb = sys + 0x1056c0 (all their offsets above are vb-relative in the binary:
 *   vb+0x228480 attr gaps, vb+0x23e480 colour gaps, vb+0x249480 DISP3DCNT, vb+0x24948c clear attr,
 *   vb+0x249498 output, vb+0x1bc088 geom).
 *
 * Geometry state geom:
 *   geom+0x9974 u8 fog_table[32]  fog densities (0..127)
 *   geom+0x9994 s8 fog_delta[32]  fog_table[i+1] - fog_table[i] (prepared by video_3d_prepare_fog)
 *   geom+0x99b4 u8 edge_r[8], +0x99bc edge_g[8], +0x99c4 edge_b[8]   (6-bit edge colours, planar)
 *   geom+0x9a9c u32 fog colour (scanline format; byte 0..2 = r,g,b, byte 3 = alpha)
 *   geom+0x9aaa u16 fog offset (& 0x7fff used)
 *
 * OUTPUT FRAME: one 0x10000-byte block per bin; bin line y (0..31) at out + y*0x800; within a line the 256 even
 * pixels come first, then the 256 odd ones:
 *      pixel (x, y)  ->  out + y*0x800 + (x & 1)*0x400 + (x >> 1)*4,  a u32 in scanline format.
 * (Plain resolve masks the value to & 0x1fffffff; fog resolve keeps alpha bits 5,6 and clears bit 31; edge mark keeps
 *  alpha bits 0-4 only.)
 *
 * ===================================================================================================================
 * LEAF ROUTINES (512 pixels per call, no padding writes)
 * ===================================================================================================================
 * resolve_bin(out, ctx): for all 32 lines and 512 x: out_px(x,y) = colour[y][x] & 0x1fffffff.
 *
 * fog_calculate_weights(attr, w, table, params): params = shift | off << 16 (drivers: shift = (DISP3DCNT>>8)&15,
 *   off = (fog_offset & 0x7fff) + (0x400 >> shift)). Per pixel (u16/s16 lanes):
 *      d   = (attr >> 9) & 0x7fff;   d = sat_u16(d - off)  (i.e. max(d - off, 0));
 *      d   = SQSHL16(d, (s8)(params & 0xff))  (signed saturating shift left: min(d << shift, 0x7fff) for shift 0..15)
 *      i   = d >> 10 (0..31);  f = d & 0x3ff;
 *      w   = (u8)(table[i] + (u8)((((s8)table[32 + i] * 32) * f * 2) >> 16))   [= table[i] + floor(delta*f/1024)]
 *   Output u8 weights[512] (0..127 for valid tables; 127 = full fog).
 *
 * fog_modulate_full_{intermediate,resolve}(dst, src, w, fogc): per pixel with bytes c0..c3 of src, fog bytes F0..F3:
 *      k  = (u8)(-w[x]) - (w[x] == 127)   (i.e. -w as s8, and -128 for 127)  ;  k = 0 if c3 bit 7 (fog flag) clear
 *      c3 &= 0x7f;  for each byte j: cj = (u8)(cj + (u8)(((s8)(u8)(cj - Fj) * (s8)k) >> 7))
 *   => cj += (Fj - cj)*w/128 (arith. shift, i.e. floor), exact fog colour for w = 127. Intermediate writes the
 *   (fog-flag-cleared) pixel to dst[x] (dst may equal src); resolve writes it to out_px(x) of a single output line.
 * fog_modulate_alpha_{intermediate,resolve}: same, but only byte 3 is modulated (towards F3); r,g,b copied
 *   unchanged (c3 still & 0x7f).
 *
 * edge_identify(edge, above, cur, below, clear): per pixel C = cur[x] with the 4 neighbours N: cur[x-1] (clear for
 *   x = 0), cur[x+1] (clear for x = 511), above[x], below[x]:
 *      c3 = ((C >> 24) & 0x7f) ^ 0x40;  n3 = (N >> 24) & 0x3f;
 *      edge_N = (N & 0xffffff) > (C & 0xffffff) && c3 != n3
 *      edge[x] = (c3 >> 3) | (any edge_N ? 0 : 0xff)
 *   i.e. an edge is a neighbour with a larger 24-bit key, unless C has bit 30 set and the same polygon id; the
 *   value is the edge colour index id >> 3 (+8 when bit 30 is clear, which disables marking), 0xff = no edge.
 * edge_identify_top(edge, cur, below, clear): above = clear everywhere.
 * edge_identify_bottom(edge, cur, other, clear): neighbours left, right, other[x], clear.
 * edge_mark(out_line, colour_line, edge, ec): per pixel, e = edge[x], j = (u8)(2*e):
 *      if j < 16 (e in 0..7 or 128..135): r,g,b bytes = ec[e&7], ec[8 + (e&7)], ec[16 + (e&7)]
 *      byte 3 = (c3 & 0x1f) | ((e >> 3) & 0xe0) (= c3 & 0x1f);   out_px(x) = result
 *
 * ===================================================================================================================
 * DRIVERS
 * ===================================================================================================================
 * resolve_bin_fog_{full,alpha}(ctx, out, mode) (mode unused): if !ctx[0x24014] || !sys[0x34eb50] ->
 *   resolve_bin(out, ctx). Else per line y: weights(attr[y]); modulate_*_resolve(out + y*0x800, colour[y], fogc).
 *
 * resolve_bin_edge_mark(ctx, out, bin), with ec = geom+0x99b4, clear = sys[0x34eb4c], edge = temp u8[512]:
 *   bin == 0 : line 0 = identify_top(attr0, attr1) + mark(out line 0, colour 0)
 *   bin != 0 : memcpy(attr gap slot bin-1 + 0x1000, attr lines 0..1, 0x1000);
 *              memcpy(colour gap slot bin-1 + 0x800, colour line 0, 0x800); line 0 not written
 *   lines 1..30: identify(attr[y-1], attr[y], attr[y+1]) + mark(out line y, colour[y])
 *   bin == 11: line 31 = identify_bottom(cur = attr30 (!), other = attr31) + mark(out line 31, colour 31)
 *              (DraStic bug: the bottom line's edges are those of line 30)
 *   bin != 11: memcpy(attr gap slot bin, attr lines 30..31, 0x1000); memcpy(colour gap slot bin, colour 31, 0x800);
 *              line 31 not written
 * resolve_bin_edge_mark_fog_{full,alpha}(ctx, out, bin): fallback to resolve_bin_edge_mark when fog is off (as
 *   above). Otherwise as resolve_bin_edge_mark, but before identifying/marking a line y the colour line is fogged in
 *   place (ctx colour[y] modified) with weights(attr[y]) and modulate_*_intermediate; for bin 11 line 31 the weights
 *   come from attr line 30 (DraStic bug). The gap copies are made before fogging (and line 0 / 31 of the buffer
 *   that go to the gaps are never fogged here).
 * resolve_bin_edge_mark[_fog_*]_gaps(vb): for each boundary g = 0..10 (slot A = attr gap g, K = colour gap g,
 *   O = output frame + g*0x10000):
 *      [fog: weights(A+0x800), modulate_*_intermediate(K, K)]
 *      identify(A, A+0x800, A+0x1000) + mark(O + 0xf800, K)                   -> line 31 of bin g
 *      [fog: weights(A+0x1000), modulate_*_intermediate(K+0x800, K+0x800)]
 *      identify(A+0x800, A+0x1000, A+0x1800) + mark(O + 0x10000, K+0x800)     -> line 0 of bin g+1
 *   (no fog-enable checks here; update_frame_3d_4x picks the variant from DISP3DCNT bits 5-7.)
 */
#include "resolve.h"
#include <string.h>

#define LINE 512
#define LINE_BYTES 0x800

static inline uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint8_t *rdptr(const void *p) { uint64_t v; memcpy(&v, p, 8); return (uint8_t *)(uintptr_t)v; }
static inline void wr32(void *p, uint32_t v) { memcpy(p, &v, 4); }
/* pixel x of an output line: even pixels in the first 0x400 bytes, odd ones in the second */
static inline void out_px(uint8_t *line, int x, uint32_t v) { wr32(line + (x & 1) * 0x400 + (x >> 1) * 4, v); }

void spec_video_3d_resolve_bin_4x(void *out, void *ctx) {
    const uint8_t *c = ctx;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < LINE; x++)
            out_px((uint8_t *)out + y * LINE_BYTES, x, rd32(c + y * LINE_BYTES + x * 4) & 0x1fffffff);
}

/* SQSHL on a 16-bit lane, shift = signed low byte of the shift lane */
static int32_t sqshl16(int32_t v, int8_t s) {
    if (s >= 0) {
        if (v == 0) return 0;
        if (s >= 16) return v > 0 ? 0x7fff : -0x8000;
        int32_t r = v * (1 << s);
        return r > 0x7fff ? 0x7fff : r < -0x8000 ? -0x8000 : r;
    }
    int n = -s;
    if (n >= 16) return v < 0 ? -1 : 0;
    return v >> n;
}

void spec_video_3d_fog_calculate_weights_4x(const uint32_t *attr, uint8_t *weights, const uint8_t *table,
                                            uint32_t params) {
    uint32_t off = params >> 16;
    int8_t sh = (int8_t)(params & 0xff);
    for (int x = 0; x < LINE; x++) {
        uint32_t d = (rd32(&attr[x]) >> 9) & 0x7fff;
        d = d > off ? d - off : 0;
        uint16_t v = (uint16_t)sqshl16((int32_t)d, sh);
        uint32_t i = (uint32_t)(v >> 10) & 0x3f;  /* shrn #8 then ushr #2: (v >> 8) & 0xff >> 2 */
        int32_t f = v & 0x3ff;
        int32_t a = (int32_t)(int8_t)table[32 + i] * 32;
        int32_t p = (2 * a * f) >> 16;            /* sqdmulh; cannot saturate here */
        weights[x] = (uint8_t)(table[i] + (uint8_t)p);
    }
}

static inline uint8_t fog_k(uint8_t w, uint8_t c3) {
    uint8_t k = (uint8_t)(-w + (w == 0x7f ? 0xff : 0));
    return c3 > 0x7f ? k : 0;
}
static inline uint8_t fog_ch(uint8_t c, uint8_t f, uint8_t k) {
    int16_t p = (int16_t)((int8_t)(uint8_t)(c - f) * (int8_t)k);
    return (uint8_t)(c + (uint8_t)((uint16_t)p >> 7));
}
static uint32_t fog_full(uint32_t px, uint8_t w, uint32_t fogc) {
    uint8_t c[4] = {px, px >> 8, px >> 16, px >> 24};
    uint8_t k = fog_k(w, c[3]);
    c[3] &= 0x7f;
    for (int j = 0; j < 4; j++) c[j] = fog_ch(c[j], (uint8_t)(fogc >> (8 * j)), k);
    return c[0] | c[1] << 8 | c[2] << 16 | (uint32_t)c[3] << 24;
}
static uint32_t fog_alpha(uint32_t px, uint8_t w, uint32_t fogc) {
    uint8_t c3 = px >> 24;
    uint8_t k = fog_k(w, c3);
    c3 = fog_ch(c3 & 0x7f, (uint8_t)(fogc >> 24), k);
    return (px & 0xffffff) | (uint32_t)c3 << 24;
}

void spec_video_3d_fog_modulate_full_intermediate_4x(uint32_t *dst, const uint32_t *src, const uint8_t *w,
                                                     uint32_t fogc) {
    for (int x = 0; x < LINE; x++) wr32(&dst[x], fog_full(rd32(&src[x]), w[x], fogc));
}
void spec_video_3d_fog_modulate_full_resolve_4x(void *out, const uint32_t *src, const uint8_t *w, uint32_t fogc) {
    for (int x = 0; x < LINE; x++) out_px(out, x, fog_full(rd32(&src[x]), w[x], fogc));
}
void spec_video_3d_fog_modulate_alpha_intermediate_4x(uint32_t *dst, const uint32_t *src, const uint8_t *w,
                                                      uint32_t fogc) {
    for (int x = 0; x < LINE; x++) wr32(&dst[x], fog_alpha(rd32(&src[x]), w[x], fogc));
}
void spec_video_3d_fog_modulate_alpha_resolve_4x(void *out, const uint32_t *src, const uint8_t *w, uint32_t fogc) {
    for (int x = 0; x < LINE; x++) out_px(out, x, fog_alpha(rd32(&src[x]), w[x], fogc));
}

void spec_video_3d_edge_mark_4x(void *out, const uint32_t *color, const uint8_t *edge, const uint8_t *ec) {
    for (int x = 0; x < LINE; x++) {
        uint32_t px = rd32(&color[x]);
        uint8_t e = edge[x], j = (uint8_t)(e * 2);
        uint8_t r = px, g = px >> 8, b = px >> 16, a = px >> 24;
        if (j < 16) { r = ec[j >> 1]; g = ec[8 + (j >> 1)]; b = ec[16 + (j >> 1)]; }
        a = (uint8_t)((a & 0x1f) | ((e >> 3) & 0xe0));
        out_px(out, x, r | g << 8 | b << 16 | (uint32_t)a << 24);
    }
}

static inline int edge_vs(uint32_t c, uint32_t n) {
    uint8_t c3 = (uint8_t)(((c >> 24) & 0x7f) ^ 0x40), n3 = (n >> 24) & 0x3f;
    return (n & 0xffffff) > (c & 0xffffff) && c3 != n3;
}
/* v0/v1 = vertical neighbour lines, NULL = the clear attribute */
static void identify(uint8_t *edge, const uint32_t *cur, const uint32_t *v0, const uint32_t *v1, uint32_t clear) {
    for (int x = 0; x < LINE; x++) {
        uint32_t c = rd32(&cur[x]);
        uint32_t l = x ? rd32(&cur[x - 1]) : clear, r = x < LINE - 1 ? rd32(&cur[x + 1]) : clear;
        uint32_t a = v0 ? rd32(&v0[x]) : clear, b = v1 ? rd32(&v1[x]) : clear;
        int any = edge_vs(c, l) | edge_vs(c, r) | edge_vs(c, a) | edge_vs(c, b);
        edge[x] = (uint8_t)(((((c >> 24) & 0x7f) ^ 0x40) >> 3) | (any ? 0 : 0xff));
    }
}
void spec_video_3d_edge_identify_4x(uint8_t *edge, const uint32_t *above, const uint32_t *cur, const uint32_t *below,
                                    uint32_t clear) {
    identify(edge, cur, above, below, clear);
}
void spec_video_3d_edge_identify_top_4x(uint8_t *edge, const uint32_t *cur, const uint32_t *below, uint32_t clear) {
    identify(edge, cur, NULL, below, clear);
}
void spec_video_3d_edge_identify_bottom_4x(uint8_t *edge, const uint32_t *cur, const uint32_t *other, uint32_t clear) {
    identify(edge, cur, other, NULL, clear);
}

/* ---------------------------------------------------------------------------------------------------------------- */

#define SYS_DISP3DCNT  0x34eb40
#define SYS_CLEAR_ATTR 0x34eb4c
#define SYS_FOG_ON     0x34eb50
#define SYS_OUTPUT     0x34eb58
#define SYS_GEOM       0x2c1748
#define SYS_ATTR_GAPS  0x32db40
#define SYS_COLOR_GAPS 0x343b40
#define VB_OFS         0x1056c0
#define GEOM_FOG_TABLE 0x9974
#define GEOM_EDGE_COL  0x99b4
#define GEOM_FOG_COLOR 0x9a9c
#define GEOM_FOG_OFS   0x9aaa

enum { FOG_NONE, FOG_FULL, FOG_ALPHA };

typedef void modfn(uint32_t *, const uint32_t *, const uint8_t *, uint32_t);

static uint32_t fog_params(const uint8_t *sys, const uint8_t *geom) {
    uint32_t sh = (rd32(sys + SYS_DISP3DCNT) >> 8) & 0xf;
    uint32_t off = (rd16(geom + GEOM_FOG_OFS) & 0x7fff) + (0x400u >> sh);
    return sh | off << 16;
}
static int fog_on(const uint8_t *ctx) {
    return rd32(ctx + 0x24014) && rd32(rdptr(ctx + 0x24000) + SYS_FOG_ON);
}

static void resolve_fog(void *ctxv, void *outv, int mode) {
    uint8_t *ctx = ctxv, *out = outv;
    if (!fog_on(ctx)) { spec_video_3d_resolve_bin_4x(out, ctx); return; }
    const uint8_t *sys = rdptr(ctx + 0x24000), *geom = rdptr(ctx + 0x24008);
    uint32_t params = fog_params(sys, geom);
    uint8_t w[LINE];
    for (int y = 0; y < 32; y++) {
        spec_video_3d_fog_calculate_weights_4x((const uint32_t *)(ctx + 0x10000 + y * LINE_BYTES), w,
                                               geom + GEOM_FOG_TABLE, params);
        (mode == FOG_FULL ? spec_video_3d_fog_modulate_full_resolve_4x : spec_video_3d_fog_modulate_alpha_resolve_4x)(
            out + y * LINE_BYTES, (const uint32_t *)(ctx + y * LINE_BYTES), w, rd32(geom + GEOM_FOG_COLOR));
    }
}
void spec_video_3d_resolve_bin_fog_full_4x(void *ctx, void *out, uint32_t mode) { (void)mode; resolve_fog(ctx, out, FOG_FULL); }
void spec_video_3d_resolve_bin_fog_alpha_4x(void *ctx, void *out, uint32_t mode) { (void)mode; resolve_fog(ctx, out, FOG_ALPHA); }

static void resolve_edge(void *ctxv, void *outv, uint32_t bin, int mode) {
    uint8_t *ctx = ctxv, *out = outv;
    if (mode != FOG_NONE && !fog_on(ctx)) mode = FOG_NONE;
    uint8_t *sys = rdptr(ctx + 0x24000), *geom = rdptr(ctx + 0x24008);
    const uint8_t *ec = geom + GEOM_EDGE_COL;
    uint32_t clear = rd32(sys + SYS_CLEAR_ATTR);
    uint32_t params = mode != FOG_NONE ? fog_params(sys, geom) : 0;
    modfn *mod = mode == FOG_FULL ? spec_video_3d_fog_modulate_full_intermediate_4x
                                  : spec_video_3d_fog_modulate_alpha_intermediate_4x;
#define COL(y) ((uint32_t *)(ctx + (y) * LINE_BYTES))
#define ATT(y) ((uint32_t *)(ctx + 0x10000 + (y) * LINE_BYTES))
#define FOG(wy, y) do { if (mode != FOG_NONE) { \
        spec_video_3d_fog_calculate_weights_4x(ATT(wy), w, geom + GEOM_FOG_TABLE, params); \
        mod(COL(y), COL(y), w, rd32(geom + GEOM_FOG_COLOR)); } } while (0)
    uint8_t w[LINE], edge[LINE];
    if (bin == 0) {
        FOG(0, 0);
        spec_video_3d_edge_identify_top_4x(edge, ATT(0), ATT(1), clear);
        spec_video_3d_edge_mark_4x(out, COL(0), edge, ec);
    } else {
        memcpy(sys + SYS_ATTR_GAPS + (bin - 1) * 0x2000 + 0x1000, ATT(0), 0x1000);
        memcpy(sys + SYS_COLOR_GAPS + (bin - 1) * 0x1000 + 0x800, COL(0), 0x800);
    }
    for (int y = 1; y < 31; y++) {
        FOG(y, y);
        spec_video_3d_edge_identify_4x(edge, ATT(y - 1), ATT(y), ATT(y + 1), clear);
        spec_video_3d_edge_mark_4x(out + y * LINE_BYTES, COL(y), edge, ec);
    }
    if (bin == 11) {
        FOG(30, 31);
        spec_video_3d_edge_identify_bottom_4x(edge, ATT(30), ATT(31), clear);
        spec_video_3d_edge_mark_4x(out + 31 * LINE_BYTES, COL(31), edge, ec);
    } else {
        memcpy(sys + SYS_ATTR_GAPS + bin * 0x2000, ATT(30), 0x1000);
        memcpy(sys + SYS_COLOR_GAPS + bin * 0x1000, COL(31), 0x800);
    }
#undef COL
#undef ATT
#undef FOG
}
void spec_video_3d_resolve_bin_edge_mark_4x(void *ctx, void *out, uint32_t bin) { resolve_edge(ctx, out, bin, FOG_NONE); }
void spec_video_3d_resolve_bin_edge_mark_fog_full_4x(void *ctx, void *out, uint32_t bin) { resolve_edge(ctx, out, bin, FOG_FULL); }
void spec_video_3d_resolve_bin_edge_mark_fog_alpha_4x(void *ctx, void *out, uint32_t bin) { resolve_edge(ctx, out, bin, FOG_ALPHA); }

static void resolve_gaps(void *vb, int mode) {
    uint8_t *sys = (uint8_t *)vb - VB_OFS;
    uint8_t *geom = rdptr(sys + SYS_GEOM), *outf = rdptr(sys + SYS_OUTPUT);
    const uint8_t *ec = geom + GEOM_EDGE_COL;
    uint32_t clear = rd32(sys + SYS_CLEAR_ATTR);
    uint32_t params = mode != FOG_NONE ? fog_params(sys, geom) : 0;
    modfn *mod = mode == FOG_FULL ? spec_video_3d_fog_modulate_full_intermediate_4x
                                  : spec_video_3d_fog_modulate_alpha_intermediate_4x;
    uint8_t w[LINE], edge[LINE];
    for (int g = 0; g < 11; g++) {
        uint8_t *a = sys + SYS_ATTR_GAPS + g * 0x2000, *k = sys + SYS_COLOR_GAPS + g * 0x1000;
        uint8_t *o = outf + g * 0x10000;
        for (int h = 0; h < 2; h++) {
            uint32_t *kc = (uint32_t *)(k + h * 0x800);
            if (mode != FOG_NONE) {
                spec_video_3d_fog_calculate_weights_4x((const uint32_t *)(a + 0x800 + h * 0x800), w,
                                                       geom + GEOM_FOG_TABLE, params);
                mod(kc, kc, w, rd32(geom + GEOM_FOG_COLOR));
            }
            spec_video_3d_edge_identify_4x(edge, (const uint32_t *)(a + h * 0x800), (const uint32_t *)(a + 0x800 + h * 0x800),
                                           (const uint32_t *)(a + 0x1000 + h * 0x800), clear);
            spec_video_3d_edge_mark_4x(o + 0xf800 + h * 0x800, kc, edge, ec);
        }
    }
}
void spec_video_3d_resolve_bin_edge_mark_gaps_4x(void *vb) { resolve_gaps(vb, FOG_NONE); }
void spec_video_3d_resolve_bin_edge_mark_fog_full_gaps_4x(void *vb) { resolve_gaps(vb, FOG_FULL); }
void spec_video_3d_resolve_bin_edge_mark_fog_alpha_gaps_4x(void *vb) { resolve_gaps(vb, FOG_ALPHA); }
