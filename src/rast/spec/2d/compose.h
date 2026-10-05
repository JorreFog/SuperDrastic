/* compose.h: exact C ports of DraStic r2.5.2.2's 2D compositor, from the layer lines and bitmaps to the scanout: the
 * windows, the priority encoders and the layer selection into 6-bit planes, every path of render_scanline_2d_composite
 * (blending, brightness, per-pixel alpha), the 3D layer's horizontal shift, display capture and the 32-bit scanout
 * conversion. compose.c documents them; pixel.h is the per-pixel closed form of the composite; the analysis is
 * tools/rast/re2d/compose.md. Elsewhere: the 3D layer's visibility step (render_scanline_set_3d_visibility) in
 * ../composite.c, render_scanline_disable_blank_layers_asm in bg.c. */
#ifndef SPEC_2D_COMPOSE_H
#define SPEC_2D_COMPOSE_H
#include <stdint.h>

/* render_scanline_2d's scratch area S (its stack frame + 0x180; the frame is 0x1d30 bytes): byte offsets */
enum {
    SCR_3DSHIFT = 0x000,  /* u32[256]: the BG0HOFS-shifted 3D line or quarter (over BG0's line buffer) */
    SCR_LINES = 0x1e0,    /* BG0..BG3 line buffers, 0x220 bytes each: 8 + 256 + 8 u16 (layers[k] = S + 0x1e0 + k*0x220) */
    SCR_OBJLINE = 0xa60,  /* the OBJ line buffer (layers[4]; pixels at S + 0xa70) */
    SCR_ALPHA = 0xc90,    /* u8[256]: the OBJ attribute plane, the 3D alpha written over it where BG0 is on top */
    SCR_VIS = 0xda0,      /* u8[8][32]: visibility, BG0..BG3 then OBJ priority 0..3 */
    SCR_OBJWIN = 0xea0,   /* the OBJ window mask */
    SCR_SEMI = 0xec0,     /* the top OBJ pixel is semi-transparent */
    SCR_BMP = 0xee0,      /* the top OBJ pixel is a bitmap OBJ */
    SCR_INH = 0xf00,      /* u8[5][32]: window inhibit masks, BG0..BG3, OBJ */
    SCR_FX = 0xfa0,       /* colour effects disabled by the windows */
    SCR_ALPHA2X = 0xfc0,  /* 2x: the per-quarter copy of the alpha plane (quarters 0..2) */
    SCR_EXCL = 0x10c0,    /* simple path: excl[6][32] of the single encoder (BG0..BG3, OBJ, backdrop) */
    SCR_SHIN = 0x1180,    /* simple path with brightness: the planes before the shade */
    SCR_SHMASK = 0x1480,  /* simple path with brightness: the shade mask */
    SCR_EVA = 0x10c0,     /* complex path: u8[256] coefficient planes EVA, EVB, OFF */
    SCR_EVB = 0x11c0,
    SCR_OFF = 0x12c0,
    SCR_TOP = 0x13c0,     /* complex path: top and second-layer masks [6][32] (BG0..BG3, OBJ +0x80, backdrop +0xa0) */
    SCR_SEC = 0x1480,
    SCR_PLANES2 = 0x1540, /* complex path: the top planes R, G, B, then the second planes (6 x 0x100 bytes) */
    SCR_T1 = 0x1b40,      /* complex path: 1st-target, 2nd-target and blend masks */
    SCR_T2 = 0x1b60,
    SCR_M = 0x1b80,
    SCR_SIZE = 0x1bb0     /* S .. S + 0x1bb0 is the rest of the frame */
};

/* ---- windows ---- */
/* render_scanline_update_window_mask (0x3a1c0): WINxH -> 256-bit mask */
void spec_render_scanline_update_window_mask(uint8_t *mask, uint32_t winh);
/* render_scanline_window_inhibit_masks_single / _double / _triple (0x3b060 / 0x3ab10 / 0x3a380) */
void spec_render_scanline_window_inhibit_masks_single(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *w,
                                                      uint32_t win_inh, uint32_t out_inh);
void spec_render_scanline_window_inhibit_masks_double(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *a,
                                                      const uint8_t *b, uint32_t ia, uint32_t ib, uint32_t out_inh);
void spec_render_scanline_window_inhibit_masks_triple(uint8_t *inh, uint8_t *fx, uint32_t lmask, const uint8_t *a,
                                                      const uint8_t *b, const uint8_t *c, uint32_t ia, uint32_t ib,
                                                      uint32_t ic, uint32_t out_inh);
/* render_scanline_generate_window_masks (0x3b360): the window Y state, the region masks -> inh[5], fx */
void spec_render_scanline_generate_window_masks(uint8_t *eng, uint8_t *inh, uint8_t *fx, const uint8_t *objwin,
                                                uint32_t lmask, uint32_t line);
/* render_scanline_apply_windows (0x3b650): vis[k] &= ~inh[k] */
void spec_render_scanline_apply_windows(const uint8_t *eng, uint8_t *vis, const uint8_t *inh, uint32_t lmask);

/* ---- the composite ---- */
/* render_scanline_priority_encode_single_asm (0x9ffc0): excl[slot] per listed BG, excl[4] (OBJ), excl[5] (backdrop) */
void spec_render_scanline_priority_encode_single(const uint8_t *eng, const uint8_t *vis, uint8_t *excl);
/* render_scanline_priority_encode_double_asm (0x9fe98): the top and the second layer's masks */
void spec_render_scanline_priority_encode_double(const uint8_t *eng, const uint8_t *vis, uint8_t *top, uint8_t *sec);
/* render_scanline_select_pixels_binary_asm (0xa0640): dst[i] = mask bit i ? layer[i] : src[i] */
void spec_render_scanline_select_pixels_binary(uint16_t *dst, const uint16_t *src, const uint16_t *layer,
                                               const uint8_t *mask);
/* render_scanline_select_pixels_binary_scalar_asm (0xa0560): dst[i] = mask bit i ? colour : src[i] */
void spec_render_scanline_select_pixels_binary_scalar(uint16_t *dst, const uint16_t *src, uint32_t colour,
                                                      const uint8_t *mask);
/* render_scanline_expand_6bit_split_asm (0xa0818): BGR555 -> R6 / G6 / B6 planes (out, +0x100, +0x200) */
void spec_render_scanline_expand_6bit_split(uint8_t *out, const uint16_t *c);
/* render_scanline_select_pixels_binary32_asm (0xa0730, alpha == NULL) and _alpha (0xa07ac): 3D bytes into the planes */
void spec_render_scanline_select_pixels_binary32(uint8_t *out, uint8_t *alpha, const uint32_t *px, const uint8_t *mask);
/* render_scanline_select_pixels (0x39330): the layers' lines merged by the masks, the backdrop, 6-bit planes, 3D */
void spec_render_scanline_select_pixels(uint8_t *eng, uint8_t *out, uint8_t *excl, uint8_t **layers,
                                        const uint32_t *p3d, uint8_t *alpha, uint32_t lmask);
/* render_scanline_select_blend_enable_asm (0xa0070): the pixels whose owner (in excl) is selected in bits */
void spec_render_scanline_select_blend_enable(uint8_t *out, const uint8_t *excl, uint32_t lmask, uint32_t bits);
/* render_scanline_shade_asm (0xa0108): BLDCNT brightness where mask is set */
void spec_render_scanline_shade(const uint8_t *eng, uint8_t *out, const uint8_t *in, const uint8_t *mask);
/* render_scanline_color_effects_setup_blend_base_asm / _blend_asm / _alpha_base_asm / _alpha_asm
 * (0xa0254 / 0xa02e0 / 0xa042c / 0xa04b4): the EVA / EVB coefficient planes */
void spec_render_scanline_color_effects_setup_blend_base(uint32_t bldalpha, uint8_t *eva, uint8_t *evb, const uint8_t *mask);
void spec_render_scanline_color_effects_setup_blend(uint32_t bldalpha, uint8_t *eva, uint8_t *evb, const uint8_t *mask);
void spec_render_scanline_color_effects_setup_alpha_base(uint8_t *eva, uint8_t *evb, const uint8_t *alpha,
                                                         const uint8_t *mask);
void spec_render_scanline_color_effects_setup_alpha(uint8_t *eva, uint8_t *evb, const uint8_t *alpha, const uint8_t *mask);
/* render_scanline_color_effects_apply_asm (0xa0378) / render_scanline_color_effects_apply_offset_c (0x39f00) */
void spec_render_scanline_color_effects_apply(uint8_t *out, const uint8_t *src6, const uint8_t *eva, const uint8_t *evb);
void spec_render_scanline_color_effects_apply_offset_c(uint8_t *out, const uint8_t *src6, const uint8_t *eva,
                                                       const uint8_t *evb, const uint8_t *off);
/* render_scanline_2d_composite (0x3c6d0): one 256-pixel line or quarter of planes, every path (line is not read) */
void spec_render_scanline_2d_composite(uint8_t *eng, uint8_t *out, uint8_t *S, uint8_t **layers, const uint32_t *p3d,
                                       uint8_t *alpha, uint32_t lmask, uint32_t bldcnt, uint32_t flags, uint32_t line);
/* render_scanline_horizontal_shift_3d (0x3c630): dst[i] = src[i + hofs] inside the line, else 0 */
void spec_render_scanline_horizontal_shift_3d(uint32_t *dst, const uint32_t *src, int32_t hofs);

/* ---- display capture (C = the capture descriptor, video + 0x458820) ---- */
/* render_scanline_capture_direct_asm (0xa0910) / _direct_3d_asm (0xa09b0) / _blended (0x3bc60) / _blended_3d (0x3bdb0) */
void spec_render_scanline_capture_direct(const uint8_t *C, uint16_t *dst, const uint8_t *planes);
void spec_render_scanline_capture_direct_3d(const uint8_t *C, uint16_t *dst, const uint32_t *px);
void spec_render_scanline_capture_blended(const uint8_t *C, uint16_t *dst, const uint16_t *srcb, const uint8_t *planes);
void spec_render_scanline_capture_blended_3d(const uint8_t *C, uint16_t *dst, const uint16_t *srcb, const uint32_t *px);

/* ---- the 32-bit scanout ---- */
/* render_scanline_color_convert_direct_32_1x_asm (0xa0a80) / _direct_32_2x_asm (0xa0ae0) / _shade_32_1x_asm (0xa0c90) /
 * _shade_32_2x_asm (0xa0d80) */
void spec_render_scanline_color_convert_direct_32_1x(const uint8_t *planes, uint32_t *dst);
void spec_render_scanline_color_convert_direct_32_2x(const uint8_t *e, const uint8_t *o, uint32_t *dst);
void spec_render_scanline_color_convert_shade_32_1x(const uint8_t *planes, uint32_t *dst, uint32_t factor, uint32_t add);
void spec_render_scanline_color_convert_shade_32_2x(const uint8_t *e, const uint8_t *o, uint32_t *dst, uint32_t factor,
                                                    uint32_t add);
#endif
