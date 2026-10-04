/* composite.h: exact C ports of DraStic's scanline compositor routines that the 3D screen goes through (the 3D
 * layer's alpha -> BG0 visibility bitmap and flags; the simple path of render_scanline_2d_composite: priority
 * encoding, layer selection, 6-bit planes). See composite.c for the documentation. */
#ifndef SPEC_COMPOSITE_H
#define SPEC_COMPOSITE_H
#include <stdint.h>

/* render_scanline_gather_3d_alpha_asm (0xa0a48): a[i] = px[i] >> 24, i = 0..255 */
void spec_render_scanline_gather_3d_alpha(uint8_t a[256], const uint32_t px[256]);
/* render_scanline_set_3d_visibility (0x3c2c0): the 256-bit bitmap of non-zero alpha bytes; returns 2, 0x10 or 0 */
uint32_t spec_render_scanline_set_3d_visibility(uint8_t bits[32], const uint32_t px[256]);

/* render_scanline_priority_encode_single_asm (0x9ffc0): excl[slot] per listed BG, excl[4] (OBJ), excl[5] (backdrop) */
void spec_render_scanline_priority_encode_single(const uint8_t *eng, const uint8_t *vis, uint8_t *excl);
/* render_scanline_select_pixels_binary_asm (0xa0640): dst[i] = mask bit i ? layer[i] : src[i] */
void spec_render_scanline_select_pixels_binary(uint16_t *dst, const uint16_t *src, const uint16_t *layer,
                                               const uint8_t *mask);
/* render_scanline_select_pixels_binary_scalar_asm (0xa0560): dst[i] = mask bit i ? colour : src[i] */
void spec_render_scanline_select_pixels_binary_scalar(uint16_t *dst, const uint16_t *src, uint32_t colour,
                                                      const uint8_t *mask);
/* render_scanline_expand_6bit_split_asm (0xa0818): BGR555 -> R6/G6/B6 planes (out, +0x100, +0x200) */
void spec_render_scanline_expand_6bit_split(uint8_t *out, const uint16_t *c);
/* render_scanline_select_pixels_binary32_asm (0xa0730, alpha == NULL) and _alpha: 3D bytes into the planes */
void spec_render_scanline_select_pixels_binary32(uint8_t *out, uint8_t *alpha, const uint32_t *px,
                                                 const uint8_t *mask);
/* render_scanline_select_pixels (0x39330) */
void spec_render_scanline_select_pixels(uint8_t *eng, uint8_t *out, uint8_t *excl, uint8_t **layers,
                                        const uint32_t *p3d, uint8_t *alpha, uint32_t lmask);
/* render_scanline_2d_composite (0x3c6d0) for (flags & 0xf) == 0: priority encode + select_pixels(.., NULL, lmask) */
void spec_render_scanline_2d_composite_simple(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers,
                                              const uint32_t *p3d, uint32_t lmask);
#endif
