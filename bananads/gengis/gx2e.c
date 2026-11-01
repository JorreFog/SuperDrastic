/* gx2e.c: gx2.c in 4-row units (2 DS lines; render.gengis_units=3 at 2x), walking the whole frame bin's window as
 * gx4e.c does, so the spans are the unsplit bins'; checked by render.gengis_verify against gx2.c */
#define GX_SCALE 2
#define GX_EIGHTH 1
#include "gx.c"
