/* gx3e.c: gx3.c in 6-row units (2 DS lines; render.gengis_units=3 at 3x), walking the whole frame bin's window as
 * gx4e.c does, so the spans are the unsplit bins'; checked by render.gengis_verify against gx3.c */
#define GX_SCALE 3
#define GX_EIGHTH 1
#include "gx.c"
