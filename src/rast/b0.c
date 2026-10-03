/* b0.c: DraStic's per-polygon pixel pipeline (render_polygon_setup_4x + render_polygon_flush_4x.isra.0) rebuilt in C
 * from the verified stage ports in spec/. This is the bit-exact reference the fused rasterizer is checked against,
 * and the proof that we understand how the stages are wired.
 *
 * setup_4x(ctx, spans, poly, buf, line0, nlines, flags, v0) is called by render_polygon_4x once the polygon's spans
 * for this bin are ready. It cuts the lines into batches (runs of lines with pixels, at most 512 pixels) and flushes
 * each batch through the stages. flags = polygon record byte 9 (attr word >> 8):
 *   bit 0 translucent, bit 1 textured, bit 2 flat colour, bit 3 w-buffer, bit 4 constant depth, bit 5 constant w */
#include <stdint.h>
#include "ds3d.h"
#include "rast.h"
#include "spec/depth.h"
#include "spec/texture.h"
#include "spec/shade.h"
#include "spec/blend.h"

#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(uint64_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)U64(p, o))

#define DS_RECIP   0x3f27120   /* reciprocal_table:   ceil(2^30 / i) */
#define DS_RECIP_U 0x3f28520   /* reciprocal_table_u: ceil(2^31 / i) */

void b0_flush(uint8_t *ctx, uint8_t *spans, uint8_t *poly, unsigned line0, unsigned nlines, uint8_t *buf,
                  unsigned n, unsigned flags, uint8_t *v0) {
    uint8_t *sys = PTR(ctx, CTX_SYS), *geom = PTR(ctx, CTX_GEOM);
    unsigned L = (2 * n + 29) & ~15u;
    int16_t *steps = (int16_t *)buf;
    uint32_t *dattr = (uint32_t *)(buf + L), *depth = (uint32_t *)(buf + 12 * L), *dcol = (uint32_t *)(buf + 14 * L);
    uint8_t *rgb = buf + 3 * L, *mask = buf + 16 * L, *ids = buf + 17 * L;
    uint32_t *col = (uint32_t *)(buf + 6 * L);
    uint32_t *color_lines = (uint32_t *)(ctx + CTX_COLOR + line0 * 0x800);
    uint32_t *attr_lines = (uint32_t *)(ctx + CTX_ATTR + line0 * 0x800);
    uint8_t *id_lines = ctx + CTX_IDBUF + line0 * 0x200;
    uint32_t attr = U32(poly, 4), pid = (attr >> 24) & 63, A = (attr >> 16) & 31, cdepth = flags & 0x10, K = 0, count;
    uint32_t d3 = U32(sys, SYS_DISP3DCNT), aref = U32(sys, 0x34eb44);

    if (flags & 0x20) spec_render_polygon_setup_perspective_steps_w_constant(steps, spans, nlines, (const uint32_t *)(ds_base + DS_RECIP_U));
    else {
        spec_render_polygon_setup_perspective_coefficients(spans, (float *)buf, (float *)(buf + 8 * L), nlines);
        spec_render_polygon_setup_perspective_steps(steps, (float *)buf, (float *)(buf + 8 * L), n);
    }
    if (flags & 8) {
        if (cdepth) { K = U32(v0, 0); spec_render_polygon_set_buffer32(depth, K, n); }
        else spec_render_polygon_interpolate_w(depth, spans, steps, nlines);
    } else {
        if (cdepth) { K = (uint32_t)U16(v0, 8) << 9; spec_render_polygon_set_buffer32(depth, K, n); }
        else spec_render_polygon_interpolate_z(depth, spans, nlines, (const uint32_t *)(ds_base + DS_RECIP));
    }
    if (flags & 1) spec_render_polygon_load_depth_colors_id_4x(dattr, dcol, ids, attr_lines, color_lines, id_lines, spans, nlines);
    else spec_render_polygon_load_depth_4x(dattr, attr_lines, spans, nlines);
    if (attr & (1u << 14)) {
        if (cdepth) spec_render_polygon_depth_compare_equal_constant(mask, K, dattr, n, &count);
        else spec_render_polygon_depth_compare_equal(mask, depth, dattr, n, &count);
    } else {
        if (cdepth) spec_render_polygon_depth_compare_less_than_constant(mask, K, dattr, n, &count);
        else spec_render_polygon_depth_compare_less_than(mask, depth, dattr, n, &count);
    }

    if (((attr >> 4) & 3) == 3) {                               /* shadow polygons: stencil in attr bit 31 */
        uint32_t *lm = (uint32_t *)(ctx + CTX_LINEMASK), bits = ((1u << nlines) - 1) << line0, old = *lm;
        if (pid) {                                              /* shadow: only where the mask set the stencil */
            *lm = old & ~bits;
            int32_t c = 0;
            for (unsigned i = 0; i < n; i++) {
                uint32_t a = dattr[i];
                if (((a >> 24) & 63) == pid || !(a >> 31)) mask[i] = 0;
                else c -= (int8_t)mask[i];
            }
            count = (uint32_t)c;
        } else {                                                /* mask: set the stencil where depth fails */
            if (bits & ~old) {
                *lm = old | bits;
                uint32_t m = old >> line0;
                for (unsigned l = 0; l < nlines; l++, m >>= 1)
                    if (!(m & 1)) for (int i = 0; i < 512; i++) attr_lines[l * 512 + i] &= 0x7fffffffu;
            }
            for (unsigned i = 0; i < n; i++) if (!mask[i]) dattr[i] |= 0x80000000u;
            uint32_t *src = dattr;
            for (unsigned l = 0; l < nlines; l++) {
                unsigned x = U16(spans, 0x580 + 4 * l), c = U16(spans, 0x630 + 4 * l);
                for (unsigned i = 0; i < c; i++) attr_lines[l * 512 + x + i] = src[i];
                src += c;
            }
            return;
        }
    }
    if (!count) return;
#ifdef RAST_BUG
    if (n > 300) return;
#endif

    if (flags & 4) {
        spec_render_polygon_set_buffer8(rgb, U16(spans, 0x420) >> 3, n);
        spec_render_polygon_set_buffer8(rgb + L, U16(spans, 0x422) >> 3, n);
        spec_render_polygon_set_buffer8(rgb + 2 * L, U16(spans, 0x582) >> 3, n);
    } else {
        spec_render_polygon_setup_rgb_interpolants(spans, rgb, nlines, L);
        spec_render_polygon_interpolate_rgb(rgb, rgb, steps, n, L);
    }
    if (flags & 2) {
        uint8_t *tex = PTR(poly, 0x10);
        spec_render_polygon_setup_uv_interpolants(spans, col, nlines, L);
        spec_render_polygon_interpolate_uv(col, col, steps, n, L);
        spec_render_polygon_generate_texture_addresses(poly, col, col, n, mask);
        if (tex[0x4a]) spec_render_polygon_load_texels_paletted(col, col, PTR(tex, 0x10), (const uint32_t *)PTR(tex, 0x18), n);
        else spec_render_polygon_load_texels(col, col, (const uint32_t *)PTR(tex, 0x10), n);
        spec_render_polygon_shade(sys + 0x1056c0, geom, poly, col, col, rgb, L, A, n);
        spec_render_polygon_alpha_test(mask, col, aref, n, &count);
        if (!count) return;
    } else {
        if (A <= aref) return;
        spec_render_polygon_shade_untextured(sys + 0x1056c0, geom, poly, col, rgb, L, A, n);
    }

    if (flags & 1) {
        if (d3 & 8) spec_render_polygon_alpha_blend(col, dcol, n, rgb);
        else spec_render_polygon_alpha_pass(col, dcol, n, rgb);
        spec_render_polygon_alpha_id_test(mask, ids, rgb, n, pid);
        switch (((attr >> 11) & 1) | ((attr >> 14) & 2)) {
        case 0: spec_render_polygon_alpha_combine(col, depth, dcol, dattr, ids, pid, rgb, mask, n); break;
        case 1: spec_render_polygon_alpha_combine_depth(col, depth, dcol, dattr, ids, pid, rgb, mask, n); break;
        case 2: U32(ctx, CTX_FOGUSED) = 1; spec_render_polygon_alpha_combine_fog(col, depth, dcol, dattr, ids, pid, rgb, mask, n); break;
        case 3: U32(ctx, CTX_FOGUSED) = 1; spec_render_polygon_alpha_combine_depth_fog(col, depth, dcol, dattr, ids, pid, rgb, mask, n); break;
        }
        spec_render_polygon_writeback_alpha_4x(spans, color_lines, attr_lines, id_lines, nlines, col, depth, ids);
        return;
    }
    if (attr & (1u << 15)) { U32(ctx, CTX_FOGUSED) = 1; spec_render_polygon_apply_fog(col, n); }
    if (d3 & (1u << 5)) spec_render_polygon_mark_edges_c(spans, depth, nlines);
    if (count == n) spec_render_polygon_writeback_all_pass_4x(spans, color_lines, attr_lines, nlines, pid, col, depth);
    else spec_render_polygon_writeback_4x(spans, color_lines, attr_lines, nlines, pid, col, depth, mask);
}

void b0_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                 unsigned flags, uint8_t *v0) {
    unsigned line = line0, left = nlines, i = 0;              /* i: index of the next line to look at */
    while (left) {
        while (left && !U16(spans, 0x630 + 4 * i)) { i++; line++; left--; }
        if (!left) return;
        unsigned first = i, k = 0, n = 0;
        while (left) {
            unsigned c = U16(spans, 0x630 + 4 * i);
            if (!c) break;
            if (n + c > 512) break;
            n += c; k++; i++; left--;
        }
        b0_flush(ctx, spans + 4 * first, poly, line, k, buf, n, flags, v0);
        line += k;
    }
}
