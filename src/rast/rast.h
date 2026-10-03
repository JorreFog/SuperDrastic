#ifndef RAST_H
#define RAST_H
#include <stdint.h>

extern uintptr_t ds_base;

/* DS BGR555 + alpha bit -> scanline format (r6 | g6<<8 | b6<<16 | a5<<24); 5->6 bits as c*2 + (c != 0) */
static inline uint32_t rast_expand555(uint32_t c) {
    uint32_t p = (c & 0x1f) | ((c >> 5) & 0x1f) << 8 | ((c >> 10) & 0x1f) << 16;
    return (p << 1) + (((p + 0x1f1f1f) >> 5) & 0x010101);
}
static inline uint32_t rast_pixel_embedded_alpha(uint32_t c) { return rast_expand555(c) | ((c >> 15) & 1) * 31u << 24; }
void b0_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                 unsigned flags, uint8_t *v0);
void f_setup_4x(uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0);
#endif
