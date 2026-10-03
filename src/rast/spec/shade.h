/* shade.h: exact ports of DraStic's polygon shading / per-pixel test routines (see shade.c for the math). */
#ifndef SPEC_SHADE_H
#define SPEC_SHADE_H
#include <stdint.h>

/* offsets used by the routines below */
#define SHADE_A0_DISP3DCNT   0x249480  /* from render_polygon_shade's a0 (= sys + 0x1056c0): u32 DISP3DCNT */
#define SHADE_GEOM_TOON      0x99cc    /* from geom (ctx+0x24008): u8 toon_r[32], toon_g[32] (+0x20), toon_b[32] (+0x40) */
#define SHADE_SYS_A0         0x1056c0  /* flush passes sys + this as a0 */
#define SHADE_SYS_ALPHA_REF  0x34eb44  /* sys: u32 alpha test reference read by flush */

/* texture-mapped polygon: dst/tex are u32 pixels (scanline format), rgb is three u8 planes (r, g, b) of stride bytes */
void spec_render_polygon_shade(const void *a0, const void *geom, const void *poly, uint32_t *dst, const uint32_t *tex,
                               uint8_t *rgb, uint32_t stride, uint32_t alpha, uint32_t count);
void spec_render_polygon_shade_untextured(const void *a0, const void *geom, const void *poly, uint32_t *dst,
                                          const uint8_t *rgb, uint32_t stride, uint32_t alpha, uint32_t count);

void spec_render_polygon_modulate(uint32_t *dst, const uint32_t *tex, const uint8_t *rgb, uint32_t stride,
                                  uint32_t alpha, uint32_t count);
void spec_render_polygon_modulate_red(uint32_t *dst, const uint32_t *tex, const uint8_t *rgb, uint32_t alpha,
                                      uint32_t count);
void spec_render_polygon_toon_load(const uint8_t *toon, uint8_t *rgb, uint32_t stride, uint32_t count);
void spec_render_polygon_combine_colors(uint32_t *dst, const uint8_t *rgb, uint32_t stride, uint32_t count,
                                        uint32_t alpha);
void spec_render_polygon_decal_c(uint32_t *dst, const uint32_t *tex, const uint8_t *rgb, uint32_t stride,
                                 uint32_t alpha, uint32_t count);

void spec_render_polygon_alpha_test(uint8_t *mask, const uint32_t *color, uint32_t ref, uint32_t count,
                                    uint32_t *pass);
void spec_render_polygon_alpha_id_test(uint8_t *mask, const uint8_t *ids, const uint8_t *alpha, uint32_t count,
                                       uint32_t id);
void spec_render_polygon_apply_fog(uint32_t *color, uint32_t count);
void spec_render_polygon_mark_edges_c(const void *spans, void *attr, uint32_t lines);
#endif
