/* defer.c: deferred shading of the opaque polygons of a bin.
 *
 * Opaque polygons are rendered in two passes instead of one: the visibility pass (kernels rast_kern_v*) runs each
 * polygon's depth test (and alpha test, for the textures that can fail it), writes the attribute words and the
 * edge marks as the one-pass kernels do, and records in the owner buffer which polygon wrote each pixel last. The
 * shade pass (rast_kern_s*) then shades each polygon's pixels that it still owns: a pixel is shaded once however
 * many polygons covered it. Same bits as the one-pass render: the attribute buffer is written in the same order
 * with the same values, and the colour of a pixel is that of its last writer either way.
 *
 * The queue holds, per polygon of the bin, what the shade pass needs: the polygon, its span block (DraStic's
 * render_polygon_4x keeps it on its stack; only the polygon's lines of each array) and the batch arguments. Polygons
 * the one-pass kernels do not handle (toon, highlight and shadow modes) flush the queue and render immediately
 * (rast.c).
 *
 * The owner indices are unique between resets of the owner buffer: a flush's polygons take base + i, and the next
 * flush starts after them, so the entries an earlier flush left match no later polygon and need no clearing (the
 * buffer was cleared after every flush, 32 KiB a bin at 2x); it is cleared when the indices would reach 0xffff
 * (none), every ~15 flushes of the largest queue. (The shade kernels' only reads of an owner entry are the equality
 * tests with kargs.idx16, kerngen.py's owner_test() and head_block(); the visibility kernels only merge their index
 * into the entries they load: a stale index means nothing to either.)
 *
 * The per-bin choice (RAST_DEFER=2, an option; rast.c's render_bins, hr.c's hr_render_bins): deferred shading costs a
 * visibility pass over every rasterized group (vis_group(), ~29 modeled A55 cycles a group with z depth) and the shade
 * pass's own per-line setup, and saves the shading of the pixels that a later polygon covers again (~110-150 cycles a
 * group). The overdraw (written opaque pixels per covered one, measured per bin with a C NEON build's RAST_STATS): the
 * stress ROM's bins 1.25-1.75 (depth complexity, rasterized pixels per bin pixel, 1.5-2.0), the fog and edge-marking
 * scene S4's 1.0 in 75% of its bins (complexity 1.0-1.5), 1.25-1.5 at 1.9-2.0 and 1.5-2.25 at 2.0-2.3, the field scene
 * S7's 1.0 everywhere. A bin is deferred when the opaque polygons' span pixels of the frame before (f_run() adds them
 * up; the same bin, whichever thread rendered it) are above twice its pixels: S4's heaviest bins. Modeled (the opaque
 * kernels, f_run and the queue, M cycles a frame): the stress ROM all deferred 7.73 -> 7.72 at 2x, 15.35 -> 14.74 at
 * 3x (a line holds 1.5 times the groups to share its setup), but S4 per bin 4.68 -> 4.80 at 2x and 9.92 -> 10.16 at 3x:
 * the depth complexity does not tell polygons drawn back to front (overdraw) from front to back (failing groups, cheap
 * in one pass), and S4's flat paletted kernels save little a group (kpath 133 one pass, 122 shaded). So it stays an
 * option, off by default with nearest filtering (README, Next: the overdraw itself as the measure). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>
#include "ds3d.h"
#include "fused.h"

int rast_defer, defer_auto;

typedef struct { uint8_t *poly, *v0; unsigned flags, line0, nlines; uint8_t spans[0xb00]; } dq_t;
#define DQ_MAX 4096
#define OWNER_MAX (50 * 768 * 2)        /* the largest layout's (hr.c) */
static __thread dq_t *dq;
static __thread unsigned dq_n, dq_cap, dq_base;
static __thread uint16_t *owner;

uint16_t *defer_owner(void) {
    /* the kernels store whole groups of 8: the last line's last group may run 7 entries past the end */
    if (!owner) { owner = aligned_alloc(64, OWNER_MAX + 64); memset(owner, 0xff, OWNER_MAX + 64); }
    return owner;
}

void defer_poly(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0,
                unsigned nlines, unsigned flags, uint8_t *v0) {
    if (dq_n == DQ_MAX) defer_flush(L, ctx);
    if (dq_n == dq_cap) { dq_cap = dq_cap ? 2 * dq_cap : 256; dq = realloc(dq, dq_cap * sizeof *dq); }
    if (!f_run(L, ctx, spans, poly, buf, line0, nlines, flags, v0, 1, dq_base + dq_n)) return;   /* no pixel: nothing to shade */
    dq_t *e = &dq[dq_n++];
    e->poly = poly; e->v0 = v0; e->flags = flags; e->line0 = line0; e->nlines = nlines;
    /* the polygon's lines of the span arrays the shade pass reads (SPO indices: 0 W0 and 1 dW, the perspective
     * steps; 4 st0 and 5 dst, the texture coordinates; 6 rg0 and 7 drg, the vertex colour; 8 xb, x and b; 9 cdb,
     * the count and db: kerngen.py's SP dict and fused_asm.c's flat colour and white tests; f_run's batch loop reads
     * 9), not the depth's 2 and 3 nor the edges' 10, which only the visibility pass read; 16 bytes a step (the
     * arrays hold 44 or 64 entries: a step reads at most 12 bytes past line nlines - 1, within the array); f_run and
     * the kernels read no other entries but batch_end()'s lanes past nlines, which it ignores */
    const unsigned n16 = (4 * nlines + 15) / 16;
    for (unsigned k = 0; k < 10; k++) {
        if (k == 2) k = 4;
        const uint8_t *s = spans + k * L->sps; uint8_t *d = e->spans + k * L->sps;
        for (unsigned j = 0; j < n16; j++) vst1q_u8(d + 16 * j, vld1q_u8(s + 16 * j));
    }
}

void defer_flush(const layout_t *L, uint8_t *ctx) {
    if (!dq_n) return;
    for (unsigned i = 0; i < dq_n; i++) {
        dq_t *e = &dq[i];
        f_run(L, ctx, e->spans, e->poly, 0, e->line0, e->nlines, e->flags, e->v0, 2, dq_base + i);
    }
    dq_base += dq_n;
    if (dq_base + DQ_MAX >= 0xffff) { memset(owner, 0xff, L->lines * L->width * 2); dq_base = 0; }
    dq_n = 0;
}

/* the per-bin choice: f_run() adds the opaque polygons' span pixels to defer_px; the count of this thread's previous
 * bin is stored when its next bin starts (or its first bin of the next frame), and read for the same bin a frame later
 * (relaxed atomics: a bin is rendered by one thread a frame, not always the same one) */
__thread unsigned long defer_px;
static unsigned bin_px[NBINS];
static __thread int px_bin = -1;
void defer_measure_reset(void) { defer_px = 0; }     /* hr.c's RAST_DCHECK: between its two renders of a bin */
int defer_bin(unsigned bin, unsigned px) {
    if (px_bin >= 0) __atomic_store_n(&bin_px[px_bin], (unsigned)defer_px, __ATOMIC_RELAXED);
    px_bin = bin < NBINS ? (int)bin : -1; defer_px = 0;
    if (!defer_auto || px_bin < 0) return rast_defer;
    return __atomic_load_n(&bin_px[bin], __ATOMIC_RELAXED) > px;
}
