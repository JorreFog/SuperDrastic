/* gx4e.c: gx4.c with 8-line bins (render.gengis_units=3: a bin context of 10 lines of 1024 pixels, ~80 KB, so three
 * render threads' contexts sit well inside the shared 512 KB L3). Each walks its 32-line bin's window, as gx4q.c's
 * 16-line bins do, so the spans are the 32-line bins'; checked by render.gengis_verify against the 32-line bins */
#define GX_SCALE 4
#define GX_EIGHTH 1
#include "gx.c"
