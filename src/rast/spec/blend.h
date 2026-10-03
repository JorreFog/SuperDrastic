/* blend.h: exact ports of DraStic's translucency blend, alpha combine and 4x writeback routines (see blend.c). */
#ifndef SPEC_BLEND_H
#define SPEC_BLEND_H
#include <stdint.h>

void spec_render_polygon_alpha_blend(uint32_t *colors, const uint32_t *dst_colors, int32_t n, uint8_t *alpha_out);
void spec_render_polygon_alpha_pass(uint32_t *colors, const uint32_t *dst_colors, int32_t n, uint8_t *alpha_out);

/* per-pixel attribute words from a buffer (polygons) */
void spec_render_polygon_alpha_combine(uint32_t *colors, uint32_t *attrs, const uint32_t *dst_colors,
    const uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_depth(uint32_t *colors, uint32_t *attrs, const uint32_t *dst_colors,
    const uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_fog(uint32_t *colors, uint32_t *attrs, const uint32_t *dst_colors,
    const uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_depth_fog(uint32_t *colors, uint32_t *attrs, const uint32_t *dst_colors,
    const uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);

/* one constant attribute word (sprites); the combined attributes are written over dst_attrs */
void spec_render_polygon_alpha_combine_constant(uint32_t *colors, uint32_t attr, const uint32_t *dst_colors,
    uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_depth_constant(uint32_t *colors, uint32_t attr, const uint32_t *dst_colors,
    uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_fog_constant(uint32_t *colors, uint32_t attr, const uint32_t *dst_colors,
    uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);
void spec_render_polygon_alpha_combine_depth_fog_constant(uint32_t *colors, uint32_t attr, const uint32_t *dst_colors,
    uint32_t *dst_attrs, uint8_t *ids, uint32_t poly_id, const uint8_t *alpha, const uint8_t *mask, int32_t n);

/* scanline writeback; spans = the polygon's span struct (u16 start at +0x580, u16 count at +0x630, stride 4) */
void spec_render_polygon_writeback_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint32_t nlines, uint32_t poly_id, const uint32_t *colors, const uint32_t *attrs, const uint8_t *mask);
void spec_render_polygon_writeback_all_pass_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint32_t nlines, uint32_t poly_id, const uint32_t *colors, const uint32_t *attrs);
void spec_render_polygon_writeback_alpha_4x(const void *spans, uint32_t *color_lines, uint32_t *attr_lines,
    uint8_t *id_lines, uint32_t nlines, const uint32_t *colors, const uint32_t *attrs, const uint8_t *ids);
#endif
