/* ds2d.h: DraStic's 2D scanline routines replaced by exact faster ones (ds2d.c, ds2d_asm.S). */
#ifndef DS2D_H
#define DS2D_H
#include <stdint.h>

/* DraStic r2.5.2.2 code offsets */
#define DS_CONV_32_2X 0xa0ae0   /* render_scanline_color_convert_direct_32_2x_asm(a planes, b planes, u32 out[512]) */
#define DS_CONV_32_1X 0xa0a80   /* render_scanline_color_convert_direct_32_1x_asm(planes, u32 out[256]) */
#define DS_SPAN4 0x9efa0         /* render_scanline_tiled_span_4bpp_asm(dst, rows, pal, tiles, entries, flags) */
#define DS_SPAN4_CONSTS 0x9ef90  /* its const_hflip_test_mask_bits */
#define DS_SELECT_PIXELS_2D 0x39330 /* render_scanline_select_pixels(eng, out, excl, layers, p3d, alpha, lmask) */
#define DS_PRIO_SINGLE 0x9ffc0      /* render_scanline_priority_encode_single_asm(eng, vis, excl) */
#define DS_BLEND_ENABLE 0xa0070     /* render_scanline_select_blend_enable_asm(u8 out[32], masks, a, b) */
#define DS_EFFECTS_APPLY 0xa0378    /* render_scanline_color_effects_apply_asm(out planes, 2 x 3 planes, wa[256], wb[256]) */
#define DS_PAL8 0x9f504             /* render_scanline_palette_lookup_8bpp_asm(u16 dst, u8 src, pal, n) */
#define DS_PAL12 0x9f574            /* render_scanline_palette_lookup_12bpp_asm(u16 dst, u16 src, pal, n) */
#define DS_PRIO_DOUBLE 0x9fe98      /* render_scanline_priority_encode_double_asm(eng, vis, excl, excl2) */
#define DS_SETUP_ALPHA_BASE 0xa042c /* render_scanline_color_effects_setup_alpha_base_asm(wa, wb, alpha, mask) */
#define DS_SETUP_ALPHA 0xa04b4      /* render_scanline_color_effects_setup_alpha_asm(wa, wb, alpha, mask) */
#define DS_SETUP_BLEND_BASE 0xa0254 /* render_scanline_color_effects_setup_blend_base_asm(bldalpha, wa, wb, mask) */
#define DS_SETUP_BLEND 0xa02e0      /* render_scanline_color_effects_setup_blend_asm(bldalpha, wa, wb, mask) */
#define DS_SHADE 0xa0108            /* render_scanline_shade_asm(eng, out planes, in planes, mask) */
#define DS_SHADE_BITS 0xa00f8       /* its const_shade_enable_bits (the setup_alpha routines' too) */
#define DS_SPAN8N 0x9f0f0           /* render_scanline_tiled_span_8bpp_normal_palette_asm(u8 dst, tiles, entries, flags) */
#define DS_SPAN8E 0x9f188           /* render_scanline_tiled_span_8bpp_ext_palette_asm(u16 dst, tiles, entries, flags) */
#define DS_SPAN8_CONSTS 0x9f0e0     /* their const_hflip_test_mask_bits_8bpp */

void ds2d_init(void);           /* rast_init, renderer hooked: installs per RAST_2D */

void ds2d_select(uint8_t *eng, uint8_t *out, const uint8_t *excl, uint8_t **layers, const uint32_t *p3d,
                 uint8_t *alpha, uint64_t lmask);

void ds2d_prio_single(uint8_t *eng, uint8_t *vis, uint8_t *excl);
void ds2d_prio_double(uint8_t *eng, uint8_t *vis, uint8_t *excl, uint8_t *excl2);
void ds2d_setup_alpha_base(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask);
void ds2d_setup_alpha(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask);
void ds2d_setup_blend_base(uint32_t bldalpha, uint8_t *wa, uint8_t *wb, const uint8_t *mask);
void ds2d_setup_blend(uint32_t bldalpha, uint8_t *wa, uint8_t *wb, const uint8_t *mask);
void ds2d_shade(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask);
void ds2d_span8n(uint8_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags);
void ds2d_span8e(uint16_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags);
void ds2d_blend_enable(uint8_t *out, const uint8_t *masks, uint64_t a, uint64_t b);

/* ds2d_asm.S */
void ds2d_conv32_2x(const uint8_t *a, const uint8_t *b, uint32_t *out);
void ds2d_conv32_1x(const uint8_t *a, uint32_t *out);
void ds2d_span4(uint16_t *dst, uint8_t *rows, const uint16_t *pal, const uint8_t *tiles, const uint16_t *entries,
                const uint8_t *flags);
void ds2d_span4_orig(uint16_t *dst, uint8_t *rows, const uint16_t *pal, const uint8_t *tiles, const uint16_t *entries,
                     const uint8_t *flags);
void ds2d_effects_apply(uint8_t *out, const uint8_t *planes, const uint8_t *wa, const uint8_t *wb);
void ds2d_pal8(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n);
void ds2d_pal8_plain(uint16_t *dst, const uint8_t *src, const uint16_t *pal, int n);
void ds2d_pal12(uint16_t *dst, const uint16_t *src, const uint16_t *pal, int n);
void ds2d_pal12_plain(uint16_t *dst, const uint16_t *src, const uint16_t *pal, int n);
/* the check mode's entries to the originals that start with an adr (ds2d_orig_vars[i] = { constant, 5th insn }) */
void ds2d_alpha_base_orig(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask);
void ds2d_alpha_orig(uint8_t *wa, uint8_t *wb, const uint8_t *alpha, const uint8_t *mask);
void ds2d_span8n_orig(uint8_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags);
void ds2d_span8e_orig(uint16_t *dst, const uint8_t *tiles, const uint16_t *entries, const uint8_t *flags);
#endif
