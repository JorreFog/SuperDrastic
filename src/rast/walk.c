/* walk.c: DraStic's polygon walker for the 2x bins (render_polygon_4x, spec/edges.c section 1) with the NEON forms of
 * its edge routines (spec/edges_impl.h with EDGES_NEON: the same bytes as DraStic's routines, tools/rast/ut/t_edges.c).
 *
 * walk_polygon_4x() does what render_polygon_4x(ctx, poly, verts, bin_top, bin_bot) does for an ordinary polygon:
 * the vertex pointers from the walk-order table (nibble k of the polygon's entry, the ninth vertex the base one), the
 * clipping against the bin's lines, the two edge chains, with edge marking the extra neighbour lines and the
 * exclusive larger x, the span setup and the edge markers, and then render_polygon_setup_4x's work (the `setup`
 * argument: rast.c's hook). It leaves to DraStic (returns 0) the polygons whose handling differs or is not defined:
 * sprites (the axis-aligned quad path, attr bit 14), shadow polygons (mode 3: b0.c's stage-by-stage path, which wants
 * DraStic's scratch buffer), and vertex counts 0 and 10-15 (DraStic fills nine vertex pointers and reads stale stack
 * slots beyond). Not done, as it changes nothing: render_polygon_4x clears the vertical-edge bit of the x values
 * before the span setup when edge marking is off; the span setup masks it itself.
 * Checked by RAST=diff, which renders every bin with DraStic's renderer and ours (tools/rast/dev/regress.sh). */
#include <arm_neon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "spec/edges.h"

#define EDGES_FN(name) w2_##name
#define EDGES_ARR  SPAN_ARR
#define EDGES_XMAX 0x200
#define EDGES_LINKAGE static
#define EDGES_NEON 1
#include "spec/edges_impl.h"

#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)*(uint64_t *)((uint8_t *)(p) + (o)))

/* the span block (DraStic's is on render_polygon_4x's stack at sp + 0x140) and the buffer it passes to
 * render_polygon_setup_4x (span + 0x840 up to the end of its 0xdcd0-byte frame: the scratch of DraStic's own setup,
 * which RAST_PDIFF runs on it) */
#define WALK_SPANS 0x840
#define WALK_BUF   0xd500
static __thread uint8_t *walk_mem;

int walk_polygon_4x(uint8_t *ctx, uint8_t *poly, uint8_t *verts, unsigned bin_top, unsigned bin_bot, walk_setup_fn *setup) {
    uint32_t a8 = U32(poly, 8);
    unsigned count = a8 & 15, flags = (a8 >> 8) & 0xff, ybot = a8 >> 23;
    if (!count || count > 9 || (flags & 0x40) || ((U32(poly, 4) >> 4) & 3) == 3) return 0;
    if (!walk_mem) { walk_mem = aligned_alloc(64, WALK_SPANS + WALK_BUF); if (!walk_mem) return 0; memset(walk_mem, 0, WALK_SPANS + WALK_BUF); }
    uint8_t *span = walk_mem, *buf = walk_mem + WALK_SPANS, *scratch = span + 0x6e0;

    const uint32_t seq = ((const uint32_t *)(ds_base + DS_VERTEX_ORDERS))[(a8 >> 16) & 0x7f];
    const size_t base = U16(poly, 0x1a);
    /* vp[k] for the walk order's k = 0..count-1, then cyclically on both sides: DraStic sets vptr[count] = vptr[0];
     * the chains of a valid polygon stop at its bottom vertex, but a record whose bottom y lies below its vertices
     * walks on (up to 16 edges either way), into stale stack slots in DraStic and around the polygon here */
    vtx_t *vpa[2 * 18 + 9], **vp = vpa + 18;
    for (unsigned k = 0; k < count; k++) vp[k] = verts + 16 * (base + (k < 8 ? (seq >> (4 * k)) & 15 : 0));
    for (int k = (int)count; k < 18 + 9; k++) vp[k] = vp[k - (int)count];
    for (int k = -1; k >= -18; k--) vp[k] = vp[k + (int)count];

    unsigned y_top = U16(vp[0], 6), y_end = ybot, skip = 0, clip = 0;
    int lines = (int)(ybot - y_top);
    if (y_top < bin_top) { skip = bin_top - y_top; lines -= (int)skip; clip = 1; }
    if (ybot > bin_bot) { lines += (int)(bin_bot - ybot); y_end = bin_bot; clip |= 2; }
    if (lines <= 0) return 1;
    unsigned line0 = y_top - bin_top + skip;           /* the bin line of the first drawn line */

    uint8_t *sys = PTR(ctx, CTX_SYS);
    if (!(U32(sys, SYS_DISP3DCNT) & 0x20)) {
        w2_render_polygon_interpolate_edges_constprop_0(span, scratch, &vp[0], bin_top, y_end, flags);
        w2_render_polygon_interpolate_edges_constprop_1(span + SPAN_ARR, scratch, &vp[count], bin_top, y_end, flags);
        w2_render_polygon_setup_spans_4x(span, lines);
        setup(ctx, span, poly, buf, line0, (unsigned)lines, flags, vp[0]);
        return 1;
    }
    /* edge marking: a neighbour line above (top clipped) and below (bottom clipped) for the markers; per line the
     * larger x becomes exclusive (+1) unless that edge is vertical (bit 15) or x has bit 9 set (x >= 512) */
    unsigned ys = bin_top, ye = y_end, nl = (unsigned)lines;
    uint8_t *sp = span;
    if (clip & 1) { ys--; nl++; sp = span + 4; }
    if (clip & 2) { ye++; nl++; }
    w2_render_polygon_interpolate_edges_constprop_0(span, scratch, &vp[0], ys, ye, flags);
    w2_render_polygon_interpolate_edges_constprop_1(span + SPAN_ARR, scratch, &vp[count], ys, ye, flags);
    for (unsigned i = 0; i < nl; i++) {
        uint16_t *xl = (uint16_t *)(span + 8 * SPAN_ARR + 4 * i), *xr = (uint16_t *)(span + 9 * SPAN_ARR + 4 * i);
        unsigned L = *xl, R = *xr, l = L & 0x7fff, r = R & 0x7fff;
        if (l > r) { if (!(L & 0x8200)) l++; }
        else if (!(R & 0x8200)) r++;
        *xl = (uint16_t)l; *xr = (uint16_t)r;
    }
    w2_render_polygon_setup_spans_4x(span, (int32_t)nl);
    w2_render_polygon_setup_edge_markers_c(sp, (uint32_t)lines, clip);
    setup(ctx, sp, poly, buf, line0, (unsigned)lines, flags, vp[0]);
    return 1;
}
