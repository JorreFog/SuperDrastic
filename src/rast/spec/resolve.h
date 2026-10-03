/* resolve.h: exact C ports of DraStic's per-bin resolve (scanline buffers -> output frame, with edge marking and
 * fog), hi-res ("4x") variants. See resolve.c for the documentation. */
#ifndef SPEC_RESOLVE_H
#define SPEC_RESOLVE_H
#include <stdint.h>

/* leaf routines (originals are hand-written NEON *_asm_4x) */
void spec_video_3d_resolve_bin_4x(void *out, void *ctx);
void spec_video_3d_fog_calculate_weights_4x(const uint32_t *attr, uint8_t *weights, const uint8_t *fog_table,
                                            uint32_t params);
void spec_video_3d_fog_modulate_full_intermediate_4x(uint32_t *dst, const uint32_t *src, const uint8_t *weights,
                                                     uint32_t fog_color);
void spec_video_3d_fog_modulate_full_resolve_4x(void *out, const uint32_t *src, const uint8_t *weights,
                                                uint32_t fog_color);
void spec_video_3d_fog_modulate_alpha_intermediate_4x(uint32_t *dst, const uint32_t *src, const uint8_t *weights,
                                                      uint32_t fog_color);
void spec_video_3d_fog_modulate_alpha_resolve_4x(void *out, const uint32_t *src, const uint8_t *weights,
                                                 uint32_t fog_color);
void spec_video_3d_edge_mark_4x(void *out, const uint32_t *color, const uint8_t *edge, const uint8_t *edge_colors);
void spec_video_3d_edge_identify_4x(uint8_t *edge, const uint32_t *above, const uint32_t *cur, const uint32_t *below,
                                    uint32_t clear_attr);
void spec_video_3d_edge_identify_top_4x(uint8_t *edge, const uint32_t *cur, const uint32_t *below, uint32_t clear_attr);
void spec_video_3d_edge_identify_bottom_4x(uint8_t *edge, const uint32_t *cur, const uint32_t *other,
                                           uint32_t clear_attr);

/* per-bin drivers (C functions in DraStic) */
void spec_video_3d_resolve_bin_fog_full_4x(void *ctx, void *out, uint32_t mode);
void spec_video_3d_resolve_bin_fog_alpha_4x(void *ctx, void *out, uint32_t mode);
void spec_video_3d_resolve_bin_edge_mark_4x(void *ctx, void *out, uint32_t bin);
void spec_video_3d_resolve_bin_edge_mark_fog_full_4x(void *ctx, void *out, uint32_t bin);
void spec_video_3d_resolve_bin_edge_mark_fog_alpha_4x(void *ctx, void *out, uint32_t bin);
/* gap passes: vb = sys + 0x1056c0 (the pointer update_frame_3d_4x passes) */
void spec_video_3d_resolve_bin_edge_mark_gaps_4x(void *vb);
void spec_video_3d_resolve_bin_edge_mark_fog_full_gaps_4x(void *vb);
void spec_video_3d_resolve_bin_edge_mark_fog_alpha_gaps_4x(void *vb);
#endif
