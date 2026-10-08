/* res2.h: the 2x bin resolve in NEON (res2.c). */
#ifndef RES2_H
#define RES2_H
#include <stdint.h>

/* DraStic's video_3d_resolve_bin_asm_4x(out, ctx) (col = the context's colour lines); with bits/flags (the bin's 64
 * table entries, comp_bin_table()) also render_scanline_set_3d_visibility of each half-row of the written block */
void res2_resolve(uint8_t *out, const uint32_t *col, uint8_t (*bits)[32], uint8_t *flags);
/* render_scanline_set_3d_visibility of each of the 64 half-rows of a written output block (comp_bin()) */
void res2_vis_bin(const uint8_t *blk, uint8_t (*bits)[32], uint8_t *flags);
#endif
