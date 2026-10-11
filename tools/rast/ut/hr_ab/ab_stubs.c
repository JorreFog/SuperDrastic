/* the rest of librast that hr.c calls: f_run and defer_poly record what the polygon walker hands the kernels (the
 * lines, the flags, the first vertex and the span block) into a running hash, so that hr_polygon can be compared */
#include <stdint.h>
#include "fused.h"
#include "binsched.h"
uintptr_t ds_base;
int rast_defer, rast_texfilter;
uint64_t ab_hash = 14695981039346656037ull;
unsigned ab_calls;
/* the polygon's bin has edge marking: only then does anything read span array 10, the edge markers (the kernels' flag
 * bit 6, line_px under DISP3DCNT bit 5). Without it the array holds the walker's scratch: edges_impl.h's per-routine
 * walker leaves its float coefficients and Q15 steps there, the fused one (walk_impl.h) nothing */
int ab_edges = 1;
static void mix(const void *p, unsigned n) {
    const uint8_t *b = p;
    for (unsigned i = 0; i < n; i++) ab_hash = (ab_hash ^ b[i]) * 1099511628211ull;
}
static void rec(const layout_t *L, uint8_t *spans, unsigned line0, unsigned nlines, unsigned flags, uint8_t *v0, int d) {
    unsigned v[4] = { line0, nlines, flags, (unsigned)d };
    mix(v, sizeof v); mix(v0, 16);
    for (unsigned k = 0; k < (ab_edges ? 11u : 10u); k++) mix(spans + k * L->sps, 4 * nlines);   /* the span arrays' entries of its lines */
    ab_calls++;
}
int f_run(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
          unsigned flags, uint8_t *v0, int dmode, unsigned idx) { rec(L, spans, line0, nlines, flags, v0, 0); return 0; }
void defer_poly(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0) { rec(L, spans, line0, nlines, flags, v0, 1); }
void defer_flush(const layout_t *L, uint8_t *ctx) {}
int defer_bin(unsigned bin, unsigned area) { return rast_defer; }
void defer_measure_reset(void) {}
void f_begin_frame(void) {}
void comp_bin(uint8_t *sys, unsigned bin, int count) {}
/* hr_render_bins' bin walk (binsched.c; the bin loop is not under test here) */
void sched_start(sched_it *it, const uint8_t *ctx, int allow_dyn) { it->bin = -1; }
int sched_next(sched_it *it) { return -1; }
