/* depth.h: exact ports of DraStic's perspective-step setup, W/Z interpolation, depth test, depth/colour/id loading and
 * buffer fill routines (see depth.c). spans = the polygon's per-line span struct (arrays of 44 x 4 bytes, see depth.c). */
#ifndef SPEC_DEPTH_H
#define SPEC_DEPTH_H
#include <stdint.h>

/* perspective-correct interpolation factors */
void spec_render_polygon_setup_perspective_coefficients(const void *spans, float *num, float *den, uint32_t nlines);
void spec_render_polygon_setup_perspective_steps(int16_t *steps, const float *num, const float *den, int32_t n);
void spec_render_polygon_setup_perspective_steps_w_constant(int16_t *steps, const void *spans, uint32_t nlines,
    const uint32_t *recip_u);

/* per-pixel depth values */
void spec_render_polygon_interpolate_w(uint32_t *depth, const void *spans, const int16_t *steps, uint32_t nlines);
void spec_render_polygon_interpolate_z(uint32_t *depth, const void *spans, uint32_t nlines, const uint32_t *recip);

/* depth tests: mask[i] = 0xff (pass) / 0; *pass_count = number of passing pixels (see depth.c for lane wrap) */
void spec_render_polygon_depth_compare_equal(uint8_t *mask, const uint32_t *depth, const uint32_t *attrs, int32_t n,
    uint32_t *pass_count);
void spec_render_polygon_depth_compare_equal_constant(uint8_t *mask, uint32_t depth, const uint32_t *attrs, int32_t n,
    uint32_t *pass_count);
void spec_render_polygon_depth_compare_less_than(uint8_t *mask, const uint32_t *depth, const uint32_t *attrs,
    int32_t n, uint32_t *pass_count);
void spec_render_polygon_depth_compare_less_than_constant(uint8_t *mask, uint32_t depth, const uint32_t *attrs,
    int32_t n, uint32_t *pass_count);

/* gather the polygon's pixels from the scanline buffers (512 px per line) into packed per-pixel arrays */
void spec_render_polygon_load_depth_4x(uint32_t *attrs, const uint32_t *attr_lines, const void *spans, uint32_t nlines);
void spec_render_polygon_load_depth_colors_id_4x(uint32_t *attrs, uint32_t *colors, uint8_t *ids,
    const uint32_t *attr_lines, const uint32_t *color_lines, const uint8_t *id_lines, const void *spans,
    uint32_t nlines);

/* fills */
void spec_render_polygon_set_buffer8(uint8_t *dst, uint32_t value, int32_t n);
void spec_render_polygon_set_buffer32(uint32_t *dst, uint32_t value, int32_t n);
#endif
