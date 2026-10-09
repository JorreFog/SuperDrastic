/* res2.h: the 2x bin resolve in NEON (res2.c). */
#ifndef RES2_H
#define RES2_H
#include <stdint.h>

/* DraStic's video_3d_resolve_bin_asm_4x(out, ctx) (col = the context's colour lines); with bits/flags (the bin's 64
 * table entries, comp_bin_table()) also render_scanline_set_3d_visibility of each half-row of the written block */
void res2_resolve(uint8_t *out, const uint32_t *col, uint8_t (*bits)[32], uint8_t *flags);
/* render_scanline_set_3d_visibility of each of the 64 half-rows of a written output block (comp_bin()) */
void res2_vis_bin(const uint8_t *blk, uint8_t (*bits)[32], uint8_t *flags);
/* DraStic's 2x resolves with fog and / or edge marking (m: rast.c's mode, 2 fog, 3 fog alpha-only, 4 or 5 edge
 * marking, 6 and 7 edge marking with fog; the context's colour lines are fogged and marked in place); with bits/flags
 * the written half-rows' entries too. Returns 0 when the caller must compute the bin's entries from the block
 * (comp_bin()) */
int res2_resolve_fx(uint8_t *ctx, uint8_t *out, unsigned bin, unsigned m, uint8_t (*bits)[32], uint8_t *flags);
#endif
