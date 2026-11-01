/* gx2q.c: gx2.c in 8-row units (4 DS lines; render.gengis_units=2 at 2x), walking the whole frame bin's window as
 * gx4e.c does, so the spans are the unsplit bins'; checked by render.gengis_verify against gx2.c */
#define GX_SCALE 2
#define GX_QUARTER 1
#include "gx.c"
