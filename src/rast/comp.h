/* comp.h: the 2D compositor's 3D visibility step (render_scanline_set_3d_visibility) replaced, and precomputed per
 * finished bin on the render threads. See comp.c. */
#ifndef COMP_H
#define COMP_H
#include <stdint.h>

void comp_init(void);                                   /* rast_init, renderer hooked: installs per RAST_COMP */
void comp_bins_begin(uint8_t *sys);                     /* a render thread starts on the output frame */
void comp_bin(uint8_t *sys, unsigned bin, int count);   /* the bin's output block is final: its 64 table entries */
/* or, for a resolve that computes them (res2.c): the bin's entries (0: no table), then comp_bin_done() */
int comp_bin_table(uint8_t *sys, unsigned bin, uint8_t (**bits)[32], uint8_t **flags);
void comp_bin_done(void);
extern void (*comp_uf4_done)(uint8_t *sys);             /* after an update_frame_3d_4x that rendered all bins (RAST_HRCHECK) */
#endif
