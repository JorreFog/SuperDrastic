/* composite.h: exact C ports of DraStic's per-quarter 3D visibility routines of the scanline compositor (the 3D
 * layer's alpha -> BG0 visibility bitmap and flags). See composite.c for the documentation. */
#ifndef SPEC_COMPOSITE_H
#define SPEC_COMPOSITE_H
#include <stdint.h>

/* render_scanline_gather_3d_alpha_asm (0xa0a48): a[i] = px[i] >> 24, i = 0..255 */
void spec_render_scanline_gather_3d_alpha(uint8_t a[256], const uint32_t px[256]);
/* render_scanline_set_3d_visibility (0x3c2c0): the 256-bit bitmap of non-zero alpha bytes; returns 2, 0x10 or 0 */
uint32_t spec_render_scanline_set_3d_visibility(uint8_t bits[32], const uint32_t px[256]);
#endif
