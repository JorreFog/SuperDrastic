/* depth.c: exact C ports of DraStic r2.5.2.2's per-polygon depth stage (hand-written NEON):
 *   render_polygon_setup_perspective_coefficients_asm   0x99d50
 *   render_polygon_setup_perspective_steps_asm          0x99df0
 *   render_polygon_setup_perspective_steps_w_constant_asm 0x99eb0
 *   render_polygon_interpolate_w_asm                    0x99f04
 *   render_polygon_interpolate_z_asm                    0x99f78
 *   render_polygon_depth_compare_equal_asm              0x99ff0
 *   render_polygon_depth_compare_equal_constant_asm     0x9a0a0
 *   render_polygon_depth_compare_less_than_asm          0x9a150
 *   render_polygon_depth_compare_less_than_constant_asm 0x9a1e0
 *   render_polygon_load_depth_asm_4x                    0x9c7b0
 *   render_polygon_load_depth_colors_id_asm_4x          0x9c7f0
 *   render_polygon_set_buffer8_asm                      0x9ab98
 *   render_polygon_set_buffer32_asm                     0x9abb0
 * All verified bit-exact against the originals by tools/rast/ut/t_depth.c.
 *
 * ---------------------------------------------------------------------------------------------------------------
 * SPAN STRUCT (`spans`, one per polygon per bin): struct of arrays, 44 entries x 4 bytes per array, entry l = line l
 * of the polygon within the bin. Fields used here (l = line index, stride 4):
 *   +0x000 + 4l  s32  W0   w at the span's left end (the perspective "w" of the left edge)
 *   +0x0b0 + 4l  s32  dW   W1 - W0 (w at the right end minus W0)
 *   +0x160 + 4l  u32  Z0   z at the left end (24-bit depth scale: DS z * 2^9 range)
 *   +0x210 + 4l  s32  dZ   z slope per pixel, unscaled (multiplied by 2^30/count here)
 *   +0x580 + 4l  u16  X    first pixel of the span within the 512-pixel hi-res line
 *   +0x630 + 4l  u16  C    pixel count of the span
 * (W0/dW naming is inferred from the math: it gives exact perspective-correct interpolation; Z0/dZ from use.)
 *
 * PER-PIXEL ARRAYS: everything after the span stage works on PACKED arrays: the C pixels of line 0, then the C pixels
 * of line 1, ..., N = sum C pixels in total (the flush's batch count). Line routines (those taking `spans`/`nlines`)
 * process each line in whole vectors (8 or 16 or 4 elements, at least one vector even when C = 0), writing up to one
 * vector minus one element past the line's end, then advance the output pointer by exactly C, so the next line
 * overwrites the overrun; only the last line's overrun survives (past element N). Batch routines (count n) process
 * ceil(n/V) vectors, at least one even when n <= 0 (do { } while ((n -= V) > 0)), and write whole vectors.
 *
 * How render_polygon_flush_4x (0x4b2b0) uses them: buf = scratch, N = batch pixel count, L = (2N + 29) & ~15 (bytes
 * of a padded u16 array; a u32 array takes 2L bytes, a u8 array L/2):
 *   W constant (flag bit 5):  steps_w_constant(buf, spans, nlines, reciprocal_table_u)
 *   otherwise:                setup_perspective_coefficients(spans, num=buf, den=buf+8L, nlines);
 *                             setup_perspective_steps(steps=buf, num=buf, den=buf+8L, N)       (in place on num)
 *   depth (u32 at buf+12L):   interpolate_w(buf+12L, spans, steps=buf, nlines)    (w-buffer, flag bit 3)
 *                             interpolate_z(buf+12L, spans, nlines, reciprocal_table) (z-buffer)
 *                             or a constant (flag bit 4: set_buffer32(buf+12L, depth, N) / the *_constant compares)
 *   dst attrs at buf+L (u32): load_depth_4x(buf+L, attr_lines, spans, nlines) or, when blending (flag bit 0),
 *                             load_depth_colors_id_4x(buf+L, colors=buf+14L, ids=buf+17L, attr_lines, color_lines,
 *                                                     id_lines, spans, nlines)
 *   depth test -> mask at buf+16L (u8 per pixel), pass count on the stack:
 *                             depth_compare_less_than[_constant](buf+16L, depth, buf+L, N, &count) or
 *                             depth_compare_equal[_constant]   (polygon attr DEPTH_EQUAL)
 *   set_buffer8 fills per-pixel u8 arrays with constants (e.g. 5-bit colour components) for N pixels.
 *   attr_lines / color_lines are the render context's 512 x u32 line buffers (0x800 bytes per line, line = first line
 *   of the polygon in the bin); id_lines its 512 x u8 polygon-id lines (0x200 bytes per line).
 *
 * ATTRIBUTE WORD (scanline attribute buffer, and the per-pixel copy at buf+L):
 *   bits 0..23   depth (24-bit; Z = DS z15 << 9, or interpolated w / z)
 *   bits 24..29  polygon id (writeback ORs (polygon_attr >> 24 & 63) << 24 into the stored depth)
 *   bit  30      (not interpreted here)
 *   bit  31      flag tested by shadow polygons
 * The depth compares clear bits 24..31 of the stored word (BIC #0xff, LSL #24); the NEW depth is used as a full
 * unmasked u32.
 *
 * ---------------------------------------------------------------------------------------------------------------
 * setup_perspective_coefficients(spans, num, den, nlines)     [all float math IEEE single, round-to-nearest]
 *   per line: W0 = s32 +0x000, D = s32 +0x0b0, C = u16 +0x630;
 *     S  = (float)(int32)(W0 + D) * (float)C            (int add wraps; one fmul)
 *     fW0 = (float)W0, fD = (float)D, E0 = 8.0f * fW0, E1 = 8.0f * fD
 *     for j = 0..7:  num[j] = (float)j * fW0;   den[j] = fma(-(float)j, fD, S)   (FMLS: fused, single rounding)
 *     for i >= 8:    num[i] = num[i-8] + E0;    den[i] = den[i-8] - E1           (sequential float add/sub)
 *     writes max(8, roundup8(C)) floats to each of num and den, then num += C, den += C.
 *   i.e. num_i = i*W0, den_i = (W0+D)*C - i*D = (C-i)*W1 + i*W0 (W1 = W0+D): t_i = num_i/den_i is the
 *   perspective-correct fraction of the way from the left to the right end at pixel i.
 *
 * setup_perspective_steps(steps, num, den, n)                 [may run in place: steps == num]
 *   per element: r = frecpe(den); r = r * frecps(r, den); r = r * frecps(r, den)  (two Newton-Raphson steps,
 *     frecps(a,b) = 2 - a*b fused);  steps[i] = (int16)(low 16 bits of) fcvtzs_sat_s32(num * r * 2^15)
 *   (fcvtzs #15 = round toward zero with saturation to s32, NaN -> 0; then XTN keeps the low 16 bits, so 1.0
 *   becomes -32768.) Blocks of 16: reads 16 floats from each of num, den and writes 16 s16 per block,
 *   max(1, ceil(n/16)) blocks. Each block reads all its inputs before writing, so steps == num is safe.
 *
 * setup_perspective_steps_w_constant(steps, spans, nlines, recip_u)   [recip_u = reciprocal_table_u, 0x3f28520]
 *   recip_u[c] = (0x7fffffff + c) / c (u32, c = 1..512; recip_u[0] = 0), i.e. ceil(2^31 / c).
 *   per line: C = u16 +0x630, R = recip_u[C]; steps[i] = (int16)((uint32)(i * R) >> 16) for i < max(8, roundup8(C))
 *   (u32 wrapping multiply), then steps += C. Note: reads the count entry of line nlines (one past the end).
 *
 * interpolate_w(depth, spans, steps, nlines)
 *   per line: W0 = s32 +0x000, D = s32 +0x0b0, C = u16 +0x630;
 *     depth[i] = W0 + (uint32)(((int64)D * (int64)steps[i]) >> 15)   (low 32 bits, u32 wrapping add)
 *   for i < max(8, roundup8(C)) (reads that many steps), then depth += C, steps += C.
 *
 * interpolate_z(depth, spans, nlines, recip)                  [recip = reciprocal_table, 0x3f27120]
 *   recip[c] = (0x3fffffff + c) / c = ceil(2^30 / c) (c = 1..512, recip[0] = 0).
 *   per line: Z0 = u32 +0x160, dZ = s32 +0x210, C = u16 +0x630;
 *     step = (int64)dZ * (int64)(int32)recip[C] + (dZ < 0 ? 0x3fffffff : 0)   (bias added to the STEP, so it
 *            accumulates per pixel)
 *     depth[i] = (uint32)((((uint64)Z0 << 30) + i * step) >> 30)    (u64 wrapping, low 32 bits)
 *   for i < max(4, roundup4(C)), then depth += C.
 *
 * depth_compare_less_than(mask, depth, attrs, n, &count) / _constant (depth[i] = the constant):
 *   pass_i = (attrs[i] & 0x00ffffff) > depth[i]   (unsigned; i.e. new depth strictly less than stored depth)
 * depth_compare_equal(mask, depth, attrs, n, &count) / _constant:
 *   d = depth[i] - (attrs[i] & 0x00ffffff) (u32 wrap); |d| as s32 ABS (ABS(0x80000000) = 0x80000000);
 *   pass_i = (uint32)|d| < 0x100           (constant version: d = constant - stored, same |d|)
 *   mask[i] = pass_i ? 0xff : 0x00, for i < max(8, roundup8(n)) (whole 8-pixel blocks; lanes past n are computed
 *   from the bytes past the inputs' end and WRITTEN too). Reads max(8, roundup8(n)) u32 of attrs (and depth).
 *   count: eight u8 lane accumulators acc[0..7] (start 0). Every full block k (pixels 8k..8k+7) adds
 *   pass_{8k+j} to acc[j] (mod 256). The last block (holding pixels with e = roundup8(n) - n excess lanes,
 *   e = 8 when n <= 0) is shifted up by e lanes (USHL by 8e bits) so excess lanes drop out: acc[j+e] += pass of
 *   lane j for j < 8-e. *count = (u32)(acc[0] + ... + acc[7]) (u16 sum). For n < 2048 this is exactly the number
 *   of passing pixels among the first n; beyond that the per-lane u8 sums wrap.
 *
 * load_depth_4x(attrs, attr_lines, spans, nlines)
 *   per line l: X = u16 +0x580, C = u16 +0x630; copies max(16, roundup16(C)) u32 from attr_lines + l*512 + X
 *   to attrs, then attrs += C (reads up to 15 words past the span, possibly into the next line).
 * load_depth_colors_id_4x(attrs, colors, ids, attr_lines, color_lines, id_lines, spans, nlines)
 *   per line l: X, C as above; for i < max(8, roundup8(C)):
 *     attrs[i] = attr_lines[l*512 + X + i]; colors[i] = color_lines[l*512 + X + i]; ids[i] = id_lines[l*512 + X + i]
 *   then attrs += C, colors += C, ids += C.
 *
 * set_buffer8(dst, value, n):  dst[i] = (uint8)value  for i < max(16, roundup16(n))
 * set_buffer32(dst, value, n): dst[i] = value         for i < max(16, roundup16(n))
 *
 * Line routines' `nlines` loops are do-while on a u32 (nlines = 0 would run 2^32 lines; callers never pass 0).
 */
#include <arm_neon.h>
#include <math.h>
#include <string.h>
#include "depth.h"

#pragma clang fp contract(off)

#define SPAN32(s, off, l) (((const int32_t *)((const uint8_t *)(s) + (off)))[l])
#define SPAN16(s, off, l) (*(const uint16_t *)((const uint8_t *)(s) + (off) + 4 * (l)))
#define SPAN_W0 0x000
#define SPAN_DW 0x0b0
#define SPAN_Z0 0x160
#define SPAN_DZ 0x210
#define SPAN_X  0x580
#define SPAN_C  0x630

/* elements a vectorised line/batch loop of vector size v writes for count c (at least one vector) */
static inline int32_t vec_len(int32_t c, int32_t v) {
    int32_t n = 0;
    do { n += v; c -= v; } while (c > 0);
    return n;
}

void spec_render_polygon_setup_perspective_coefficients(const void *spans, float *num, float *den, uint32_t nlines) {
    uint32_t l = 0;
    do {
        int32_t w0 = SPAN32(spans, SPAN_W0, l), dw = SPAN32(spans, SPAN_DW, l);
        uint32_t c = SPAN16(spans, SPAN_C, l);
        float s = (float)(int32_t)((uint32_t)w0 + (uint32_t)dw) * (float)(int32_t)c;
        float fw0 = (float)w0, fdw = (float)dw;
        float e0 = 8.0f * fw0, e1 = 8.0f * fdw;
        float nv[8], dv[8];
        for (int j = 0; j < 8; j++) {
            nv[j] = (float)j * fw0;
            dv[j] = fmaf(-(float)j, fdw, s);
        }
        int32_t len = vec_len((int32_t)c, 8);
        for (int32_t i = 0; i < len; i += 8)
            for (int j = 0; j < 8; j++) {
                num[i + j] = nv[j]; den[i + j] = dv[j];
                nv[j] = nv[j] + e0; dv[j] = dv[j] - e1;
            }
        num += c; den += c;
        l++;
    } while (--nlines);
}

void spec_render_polygon_setup_perspective_steps(int16_t *steps, const float *num, const float *den, int32_t n) {
    do {
        float nb[16], db[16];
        memcpy(nb, num, sizeof nb); memcpy(db, den, sizeof db);
        for (int j = 0; j < 16; j++) {
            float a = db[j];
            float r = vrecpes_f32(a);
            r = r * vrecpss_f32(r, a);
            r = r * vrecpss_f32(r, a);
            steps[j] = (int16_t)vcvts_n_s32_f32(nb[j] * r, 15);
        }
        num += 16; den += 16; steps += 16;
        n -= 16;
    } while (n > 0);
}

void spec_render_polygon_setup_perspective_steps_w_constant(int16_t *steps, const void *spans, uint32_t nlines,
    const uint32_t *recip_u) {
    uint32_t l = 0;
    do {
        uint32_t c = SPAN16(spans, SPAN_C, l);
        uint32_t r = recip_u[c];
        int32_t len = vec_len((int32_t)c, 8);
        for (int32_t i = 0; i < len; i++) steps[i] = (int16_t)(((uint32_t)i * r) >> 16);
        steps += c;
        l++;
    } while (--nlines);
}

void spec_render_polygon_interpolate_w(uint32_t *depth, const void *spans, const int16_t *steps, uint32_t nlines) {
    uint32_t l = 0;
    do {
        int32_t w0 = SPAN32(spans, SPAN_W0, l), dw = SPAN32(spans, SPAN_DW, l);
        uint32_t c = SPAN16(spans, SPAN_C, l);
        int32_t len = vec_len((int32_t)c, 8);
        for (int32_t i = 0; i < len; i++)
            depth[i] = (uint32_t)w0 + (uint32_t)(((int64_t)dw * steps[i]) >> 15);
        depth += c; steps += c;
        l++;
    } while (--nlines);
}

void spec_render_polygon_interpolate_z(uint32_t *depth, const void *spans, uint32_t nlines, const uint32_t *recip) {
    uint32_t l = 0;
    do {
        uint32_t z0 = (uint32_t)SPAN32(spans, SPAN_Z0, l);
        int32_t dz = SPAN32(spans, SPAN_DZ, l);
        uint32_t c = SPAN16(spans, SPAN_C, l);
        uint64_t step = (uint64_t)((int64_t)dz * (int64_t)(int32_t)recip[c]) + (dz < 0 ? 0x3fffffffu : 0);
        uint64_t base = (uint64_t)z0 << 30;
        int32_t len = vec_len((int32_t)c, 4);
        for (int32_t i = 0; i < len; i++) depth[i] = (uint32_t)((base + (uint64_t)i * step) >> 30);
        depth += c;
        l++;
    } while (--nlines);
}

/* shared tail of the depth compares: pass[] holds one 0/1 per pixel for `blocks` 8-pixel blocks */
static uint32_t pass_count(const uint8_t *mask, int32_t blocks, int32_t n) {
    uint8_t acc[8] = {0};
    for (int32_t k = 0; k < blocks - 1; k++)
        for (int j = 0; j < 8; j++) acc[j] += mask[8 * k + j] ? 1 : 0;
    /* last block: excess lanes e = 8*blocks - n (n<=0 -> e = 8 - n); USHL by the signed low byte of 8e */
    int32_t e = 8 * blocks - n;
    int8_t sh = (int8_t)(uint8_t)((uint32_t)e << 3);
    uint64_t last;
    memcpy(&last, mask + 8 * (blocks - 1), 8);
    if (sh >= 64 || sh <= -64) last = 0;
    else if (sh >= 0) last <<= sh;
    else last >>= -sh;
    for (int j = 0; j < 8; j++) acc[j] -= (uint8_t)(last >> (8 * j));
    uint32_t sum = 0;
    for (int j = 0; j < 8; j++) sum += acc[j];
    return sum & 0xffff;
}

static inline int pass_equal(uint32_t z, uint32_t attr) {
    uint32_t d = z - (attr & 0x00ffffffu);
    if ((int32_t)d < 0) d = 0u - d;
    return d < 0x100;
}

void spec_render_polygon_depth_compare_equal(uint8_t *mask, const uint32_t *depth, const uint32_t *attrs, int32_t n,
    uint32_t *pass_count_out) {
    int32_t len = vec_len(n, 8);
    for (int32_t i = 0; i < len; i++) mask[i] = pass_equal(depth[i], attrs[i]) ? 0xff : 0;
    *pass_count_out = pass_count(mask, len / 8, n);
}

void spec_render_polygon_depth_compare_equal_constant(uint8_t *mask, uint32_t depth, const uint32_t *attrs, int32_t n,
    uint32_t *pass_count_out) {
    int32_t len = vec_len(n, 8);
    for (int32_t i = 0; i < len; i++) mask[i] = pass_equal(depth, attrs[i]) ? 0xff : 0;
    *pass_count_out = pass_count(mask, len / 8, n);
}

void spec_render_polygon_depth_compare_less_than(uint8_t *mask, const uint32_t *depth, const uint32_t *attrs,
    int32_t n, uint32_t *pass_count_out) {
    int32_t len = vec_len(n, 8);
    for (int32_t i = 0; i < len; i++) mask[i] = (attrs[i] & 0x00ffffffu) > depth[i] ? 0xff : 0;
    *pass_count_out = pass_count(mask, len / 8, n);
}

void spec_render_polygon_depth_compare_less_than_constant(uint8_t *mask, uint32_t depth, const uint32_t *attrs,
    int32_t n, uint32_t *pass_count_out) {
    int32_t len = vec_len(n, 8);
    for (int32_t i = 0; i < len; i++) mask[i] = (attrs[i] & 0x00ffffffu) > depth ? 0xff : 0;
    *pass_count_out = pass_count(mask, len / 8, n);
}

void spec_render_polygon_load_depth_4x(uint32_t *attrs, const uint32_t *attr_lines, const void *spans, uint32_t nlines) {
    uint32_t l = 0;
    do {
        uint32_t x = SPAN16(spans, SPAN_X, l), c = SPAN16(spans, SPAN_C, l);
        memmove(attrs, attr_lines + x, (size_t)vec_len((int32_t)c, 16) * 4);
        attrs += c; attr_lines += 512;
        l++;
    } while (--nlines);
}

void spec_render_polygon_load_depth_colors_id_4x(uint32_t *attrs, uint32_t *colors, uint8_t *ids,
    const uint32_t *attr_lines, const uint32_t *color_lines, const uint8_t *id_lines, const void *spans,
    uint32_t nlines) {
    uint32_t l = 0;
    do {
        uint32_t x = SPAN16(spans, SPAN_X, l), c = SPAN16(spans, SPAN_C, l);
        int32_t len = vec_len((int32_t)c, 8);
        for (int32_t i = 0; i < len; i++) {
            attrs[i] = attr_lines[x + i];
            colors[i] = color_lines[x + i];
            ids[i] = id_lines[x + i];
        }
        attrs += c; colors += c; ids += c;
        attr_lines += 512; color_lines += 512; id_lines += 512;
        l++;
    } while (--nlines);
}

void spec_render_polygon_set_buffer8(uint8_t *dst, uint32_t value, int32_t n) {
    memset(dst, (uint8_t)value, (size_t)vec_len(n, 16));
}

void spec_render_polygon_set_buffer32(uint32_t *dst, uint32_t value, int32_t n) {
    int32_t len = vec_len(n, 16);
    for (int32_t i = 0; i < len; i++) dst[i] = value;
}
