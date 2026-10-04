/* comp.h: the 2D compositor's 3D visibility step (render_scanline_set_3d_visibility) replaced, and precomputed per
 * finished bin on the render threads. See comp.c. */
#ifndef COMP_H
#define COMP_H
#include <stdint.h>

void comp_init(void);                                   /* rast_init, renderer hooked: installs per RAST_COMP */
void comp_bins_begin(uint8_t *sys);                     /* a render thread starts on the output frame */
void comp_bin(uint8_t *sys, unsigned bin, int count);   /* the bin's output block is final: its 64 table entries */
#endif
