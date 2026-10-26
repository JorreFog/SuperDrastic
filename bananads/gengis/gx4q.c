/* gx4q.c: gx4.c with 16-line bins (render.gengis_units=2: a bin context of 18 lines of 1024 pixels, 166 KB, instead
 * of 34 lines, 313 KB, so that three render threads' contexts come nearer to fitting the shared 512 KB L3). Not exact
 * by construction (the walker's float weights start at another line: P4); checked by render.gengis_verify against
 * the 32-line bins */
#define GX_SCALE 4
#define GX_QUARTER 1
#include "gx.c"
