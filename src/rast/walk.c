/* walk.c: DraStic's polygon walker for the 2x bins (render_polygon_4x, spec/edges.c section 1) on NEON forms of its
 * edge routines: the five routines of each edge chain fused into one pass per edge (walk_chain), the span setup with
 * the edge-marking fix-up (walk_spans) and the edge markers (walk_markers), 4 to 8 lines a step. They write DraStic's
 * values for the lines render_polygon_setup_4x and the markers read; edges_impl.h's NEON forms (DraStic's bytes,
 * overruns included, tools/rast/ut/t_edges.c) are the per-routine reference. The routines are in walk_impl.h, a
 * template for this layout and hr.c's 3x one (which walks every polygon with them).
 *
 * walk_polygon_4x() does what render_polygon_4x(ctx, poly, verts, bin_top, bin_bot) does for an ordinary polygon:
 * the vertex pointers from the walk-order table (nibble k of the polygon's entry, the ninth vertex the base one), the
 * clipping against the bin's lines, the two edge chains, with edge marking the extra neighbour lines and the
 * exclusive larger x, the span setup and the edge markers, and then render_polygon_setup_4x's work (the `setup`
 * argument: rast.c's hook). It leaves to DraStic (returns 0) the polygons whose handling differs or is not defined:
 * sprites (the axis-aligned quad path, attr bit 14), shadow polygons (mode 3: b0.c's stage-by-stage path, which wants
 * DraStic's scratch buffer), vertex counts 0 and 10-15 (DraStic fills nine vertex pointers and reads stale stack
 * slots beyond) and records whose bottom y lies below all their vertices (the walks run past the vertices). Not
 * done, as it changes nothing: render_polygon_4x clears the vertical-edge bit of the x values before the span setup
 * when edge marking is off; the span setup masks it itself.
 * Checked by tools/rast/ut/t_walk.c (the span arrays render_polygon_setup_4x gets, against DraStic's walker on random
 * polygons) and RAST=diff, which renders every bin with DraStic's renderer and ours (tools/rast/dev/regress.sh). */
#include <arm_neon.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ds3d.h"
#include "rast.h"
#include "spec/edges.h"

#pragma STDC FP_CONTRACT OFF
#define U16(p, o) (*(uint16_t *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define PTR(p, o) ((uint8_t *)*(uint64_t *)((uint8_t *)(p) + (o)))

/* the walker's routines (walk_impl.h, shared with hr.c's 3x walker) in DraStic's layout */
#define WALK_ARR   SPAN_ARR
#define WALK_XMAX  0x200
#define WALK_RECIP ((const uint32_t *)(ds_base + DS_RECIP_TABLE))  /* initialize_video_3d fills it; 0 past it */
#define WALK_CAP   0
#define WALK_HR    0
#include "walk_impl.h"

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
    uint8_t *span = walk_mem, *buf = walk_mem + WALK_SPANS;

    const uint32_t seq = ((const uint32_t *)(ds_base + DS_VERTEX_ORDERS))[(a8 >> 16) & 0x7f];
    const size_t base = U16(poly, 0x1a);
    /* vp[k] for the walk order's k = 0..count-1, and vp[count] = vp[0] as DraStic's. Both chains stop at the lowest
     * vertex at the latest, within vp[0..count], when the record's bottom y is not below every vertex; a record whose
     * is walks on (up to 16 edges either way) into stale stack slots in DraStic: left to DraStic */
    vtx_t *vp[10];
    unsigned ymax = 0;
    for (unsigned k = 0; k < count; k++) {
        vp[k] = verts + 16 * (base + (k < 8 ? (seq >> (4 * k)) & 15 : 0));
        unsigned y = U16(vp[k], 6);
        if (y > ymax) ymax = y;
    }
    if (ybot > ymax) return 0;
    vp[count] = vp[0];

    unsigned y_top = U16(vp[0], 6), y_end = ybot, skip = 0, clip = 0;
    int lines = (int)(ybot - y_top);
    if (y_top < bin_top) { skip = bin_top - y_top; lines -= (int)skip; clip = 1; }
    if (ybot > bin_bot) { lines += (int)(bin_bot - ybot); y_end = bin_bot; clip |= 2; }
    if (lines <= 0) return 1;
    unsigned line0 = y_top - bin_top + skip;           /* the bin line of the first drawn line */

    uint8_t *sys = PTR(ctx, CTX_SYS);
    /* edge marking: a neighbour line above (top clipped) and below (bottom clipped) for the markers; per line the
     * larger x becomes exclusive (+1) unless that edge is vertical (bit 15) or x has bit 9 set (x >= 512) */
    const int marks = (U32(sys, SYS_DISP3DCNT) >> 5) & 1;
    unsigned ys = bin_top, ye = y_end, nl = (unsigned)lines;
    uint8_t *sp = span;
    if (marks && (clip & 1)) { ys--; nl++; sp = span + 4; }
    if (marks && (clip & 2)) { ye++; nl++; }
    /* the forward chain into the left arrays, the backward one into the right (one inlined copy of the walk) */
    for (int c = 0; c < 2; c++) {
        vtx_t **v = c ? &vp[count] : &vp[0];
        if (!(flags & 0x18)) walk_chain(span + c * SPAN_ARR, v, c ? -1 : 1, ys, ye, 1);
        else walk_chain(span + c * SPAN_ARR, v, c ? -1 : 1, ys, ye, 0);
    }
    if (!marks) {
        walk_spans(span, lines, 0);
        setup(ctx, span, poly, buf, line0, (unsigned)lines, flags, vp[0]);
        return 1;
    }
    walk_spans(span, (int32_t)nl, 1);         /* (with the larger x made exclusive first) */
    walk_markers(sp, (uint32_t)lines, clip);
    setup(ctx, sp, poly, buf, line0, (unsigned)lines, flags, vp[0]);
    return 1;
}
