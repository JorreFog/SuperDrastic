/* binsched.h: which bins a render thread renders (binsched.c): DraStic's fixed stride, or claimed from the frame's
 * queue, longest first */
#ifndef BINSCHED_H
#define BINSCHED_H
#include <stdint.h>

/* a render thread's walk over its bins for one hook_entry call: for (b; (b = sched_next(&it)) >= 0;) render bin b */
typedef struct {
    unsigned k, nb, first, stride;      /* the fixed walk: bin first + k * stride, k < nb; claimed: the next of the order */
    int dyn;                            /* claiming from the frame's queue */
    int bin;                            /* the bin being rendered, or -1 */
    uint64_t t0;                        /* its start (counter ticks) */
    uint64_t c0;                        /* RAST_SCHEDSTATS: its start (thread CPU time, ns) */
} sched_it;

void sched_init(void);                  /* rast_init: RAST_SCHED, RAST_SCHEDDIFF, RAST_SCHEDSTATS, RAST_SCHEDHASH */
extern int sched_mode, sched_diff;      /* RAST_SCHED: 0 DraStic's fixed bins, 1 claimed; RAST_SCHEDDIFF: also in diff */
/* allow_dyn: claim when the frame's queue is open (rast.c: not in RAST=diff unless RAST_SCHEDDIFF=1) */
void sched_start(sched_it *it, const uint8_t *ctx, int allow_dyn);
int sched_next(sched_it *it);           /* the next bin, or -1 (the previous one is finished: its time recorded) */
/* comp.c's update_frame_3d_4x wrapper: the frame's queue opens before DraStic wakes the render threads and closes
 * after they all returned (sys: the frame's system struct, for RAST_SCHEDHASH) */
void sched_frame_begin(void);
void sched_frame_end(const uint8_t *sys);
#endif
