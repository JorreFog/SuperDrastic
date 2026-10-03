/* edges.h: exact C ports of DraStic r2.5.2.2's polygon edge walkers and span setup (see edges.c for the layout and
 * math). Pointer arguments are raw byte pointers into DraStic's structures; offsets are documented in edges.c. */
#ifndef SPEC_EDGES_H
#define SPEC_EDGES_H
#include <stdint.h>

/* per-line span block ("struct of arrays", 44 entries x 4 bytes per array); offsets from the LEFT-edge base */
#define SPAN_N          44
#define SPAN_ARR        0x0b0   /* bytes per array; the right edge's array follows its left one */
#define SPAN_W          0x000   /* s32 w            | after setup_spans: w_left,  +0x0b0: w_right - w_left   */
#define SPAN_Z          0x160   /* u32 z (depth<<9) | after setup_spans: z_left,  +0x210: z_right - z_left   */
#define SPAN_ST         0x2c0   /* s16 s | t<<16    | after setup_spans: st_left, +0x370: st_right - st_left */
#define SPAN_RG         0x420   /* u16 r | g<<16    | after setup_spans: rg_left, +0x4d0: rg_right - rg_left */
#define SPAN_XB         0x580   /* u16 x | b<<16    | after setup_spans: x0|b_left, +0x630: width|(b_r-b_l)  */
#define SPAN_SCRATCH    0x6e0   /* edge-walker scratch (floats / Q15 steps); afterwards u16 edge-marker pairs */

/* vertex record (16 bytes) */
#define VTX_W 0x0   /* s32 */
#define VTX_X 0x4   /* u16 */
#define VTX_Y 0x6   /* u16 */
#define VTX_Z 0x8   /* u16 */
#define VTX_C 0xa   /* u16 BGR555 (bit 15 ignored) */
#define VTX_S 0xc   /* s16 */
#define VTX_T 0xe   /* s16 */

typedef uint8_t vtx_t;  /* vertices are handled as byte pointers */

/* 0x4d290 (never called; the two constprop clones below are what the renderers call) */
void spec_render_polygon_interpolate_edges(void *unused, uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                           uint32_t y_start, uint32_t y_end, int32_t dir, uint32_t flags);
/* 0x4cf10: dir = +1 (walks vptr[0], vptr[1], ...) */
void spec_render_polygon_interpolate_edges_constprop_0(uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                                       uint32_t y_start, uint32_t y_end, uint32_t flags);
/* 0x4d0d0: dir = -1 (walks vptr[0], vptr[-1], ...) */
void spec_render_polygon_interpolate_edges_constprop_1(uint8_t *spans, uint8_t *scratch, vtx_t **vptr,
                                                       uint32_t y_start, uint32_t y_end, uint32_t flags);

/* 0x9abd0 */
void spec_render_polygon_edge_perspective_coefficients(float *out, vtx_t **pairs, const uint8_t *counts, uint32_t n,
                                                       int32_t skip);
/* 0x9ace4 */
void spec_render_polygon_edge_perspective_steps(int16_t *out, const float *in, int32_t total);
/* 0x9ad40 */
void spec_render_polygon_edge_interpolate_w(vtx_t **pairs, uint8_t *spans, const int16_t *steps,
                                            const uint8_t *counts, uint32_t n);
/* 0x9ade8 */
void spec_render_polygon_edge_interpolate_parameters(vtx_t **pairs, uint8_t *spans, const int16_t *steps,
                                                     const uint8_t *counts, uint32_t n);
/* 0x4c930 */
void spec_render_polygon_edge_interpolate_xz_c(vtx_t **pairs, uint8_t *spans, const uint8_t *counts, uint32_t n,
                                               uint32_t skip);
/* 0x4ccd0 */
void spec_render_polygon_edge_interpolate_x_c(vtx_t **pairs, uint8_t *spans, const uint8_t *counts, uint32_t n,
                                              uint32_t skip);
/* 0x9cd10 */
void spec_render_polygon_setup_spans_4x(uint8_t *spans, int32_t lines);
/* 0x4d680 */
void spec_render_polygon_setup_edge_markers_c(uint8_t *p, uint32_t lines, uint32_t clip);

/* DraStic's reciprocal_table[i] (initialize_video_3d): (0x3fffffff + i) / i for 1..512, 0 for 0 and 513..1023 */
uint32_t spec_edges_reciprocal(int32_t i);
#endif
