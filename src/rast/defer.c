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
 * render_polygon_4x keeps it on its stack) and the batch arguments. Polygons the one-pass kernels do not handle
 * (toon, highlight and shadow modes) flush the queue and render immediately (rast.c). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fused.h"

int rast_defer;

typedef struct { uint8_t *poly, *v0; unsigned flags, line0, nlines; uint8_t spans[0x800]; } dq_t;
#define DQ_MAX 4096
static __thread dq_t *dq;
static __thread unsigned dq_n, dq_cap;
static __thread uint16_t *owner;

uint16_t *defer_owner(void) {
    /* the kernels store whole groups of 8: the last line's last group may run 7 entries past the end */
    if (!owner) { owner = aligned_alloc(64, 32 * 512 * 2 + 64); memset(owner, 0xff, 32 * 512 * 2 + 64); }
    return owner;
}

void defer_poly(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0) {
    if (dq_n == DQ_MAX) defer_flush(ctx);
    if (dq_n == dq_cap) { dq_cap = dq_cap ? 2 * dq_cap : 256; dq = realloc(dq, dq_cap * sizeof *dq); }
    if (!f_run_4x(ctx, spans, poly, buf, line0, nlines, flags, v0, 1, dq_n)) return;   /* no pixel: nothing to shade */
    dq_t *e = &dq[dq_n++];
    e->poly = poly; e->v0 = v0; e->flags = flags; e->line0 = line0; e->nlines = nlines;
    memcpy(e->spans, spans, sizeof e->spans);
}

void defer_flush(uint8_t *ctx) {
    if (!dq_n) return;
    for (unsigned i = 0; i < dq_n; i++) {
        dq_t *e = &dq[i];
        f_run_4x(ctx, e->spans, e->poly, 0, e->line0, e->nlines, e->flags, e->v0, 2, i);
    }
    memset(owner, 0xff, 32 * 512 * 2);
    dq_n = 0;
}
