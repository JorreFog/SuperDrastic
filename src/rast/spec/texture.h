/* texture.h: exact C ports of DraStic r2.5.2.2's texture-coordinate / colour interpolation, texture addressing and
 * texel fetch routines (see texture.c for the documentation). */
#ifndef SPEC_TEXTURE_H
#define SPEC_TEXTURE_H
#include <stdint.h>

/* texture cache entry (texture_cache_lookup/texture_cache_create; malloc(80)), as far as the rasterizer needs it */
#define TEXC_PARAMS    0x00  /* u32: TEXIMAGE_PARAM & 0xfff0ffff (repeat/flip bits cleared: not part of the key) */
#define TEXC_PAGES     0x04  /* u32: mask of the texture VRAM pages the texture covers (invalidation) */
#define TEXC_TEXELS    0x10  /* ptr: u32 texels (direct/compressed) or u8 palette indices (paletted formats) */
#define TEXC_PALETTE   0x18  /* ptr: u32 palette (paletted formats only), already in texel format */
#define TEXC_NEXT      0x20  /* ptr: hash chain */
#define TEXC_PREV      0x28  /* ptr: hash chain */
#define TEXC_LRU_NEXT  0x30
#define TEXC_LRU_PREV  0x38
#define TEXC_WIDTH     0x40  /* u16: 8 << ((params >> 20) & 7) */
#define TEXC_HEIGHT    0x42  /* u16: 8 << ((params >> 23) & 7) */
#define TEXC_BUCKET    0x44  /* u16: hash bucket */
#define TEXC_PLTT_BASE 0x46  /* u16: palette base (key) */
#define TEXC_DIRTY     0x48  /* u8: needs reconversion */
#define TEXC_FORMAT    0x49  /* u8: (params >> 26) & 7 */
#define TEXC_PALETTED  0x4a  /* u8: nonzero -> texels are u8 indices into TEXC_PALETTE (load_texels_paletted) */

/* polygon (render-side polygon record, 32 bytes) fields read by the dispatcher */
#define POLY_TEXWRAP   0x02  /* u16: low 4 bits = TEXIMAGE_PARAM bits 16..19: repeat S, repeat T, flip S, flip T */
#define POLY_TEXCACHE  0x10  /* ptr: texture cache entry */

void spec_render_polygon_setup_uv_interpolants(const void *spans, void *out, uint32_t lines, uint32_t stride);
void spec_render_polygon_interpolate_uv(void *out, const void *in, const int16_t *weights, uint32_t count,
                                        uint32_t stride);
void spec_render_polygon_setup_rgb_interpolants(const void *spans, void *out, uint32_t lines, uint32_t stride);
void spec_render_polygon_interpolate_rgb(void *out, const void *in, const int16_t *weights, uint32_t count,
                                         uint32_t stride);

void spec_render_polygon_generate_texture_addresses(const void *poly, uint32_t *out, const void *uv, uint32_t count,
                                                    const uint8_t *mask);
#define SPEC_TEXADDR_PROTO(s, t) \
    void spec_render_polygon_generate_texture_addresses_##s##_##t(uint32_t *out, const void *uv, uint32_t count, \
                                                                 uint32_t width, uint32_t height, const uint8_t *mask);
SPEC_TEXADDR_PROTO(clamp, clamp) SPEC_TEXADDR_PROTO(wrap, clamp) SPEC_TEXADDR_PROTO(flip, clamp)
SPEC_TEXADDR_PROTO(clamp, wrap)  SPEC_TEXADDR_PROTO(wrap, wrap)  SPEC_TEXADDR_PROTO(flip, wrap)
SPEC_TEXADDR_PROTO(clamp, flip)  SPEC_TEXADDR_PROTO(wrap, flip)  SPEC_TEXADDR_PROTO(flip, flip)
#undef SPEC_TEXADDR_PROTO

void spec_render_polygon_load_texels(uint32_t *out, const uint32_t *addrs, const uint32_t *texels, uint32_t count);
void spec_render_polygon_load_texels_paletted(uint32_t *out, const uint32_t *addrs, const uint8_t *indices,
                                              const uint32_t *palette, uint32_t count);
#endif
