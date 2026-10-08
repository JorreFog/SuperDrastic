#include <stdint.h>
#include "fused.h"
uintptr_t ds_base;
int rast_defer, rast_texfilter;
int f_run(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
          unsigned flags, uint8_t *v0, int dmode, unsigned idx) { return 0; }
void defer_poly(const layout_t *L, uint8_t *ctx, uint8_t *spans, uint8_t *poly, uint8_t *buf, unsigned line0, unsigned nlines,
                unsigned flags, uint8_t *v0) {}
void defer_flush(const layout_t *L, uint8_t *ctx) {}
void f_begin_frame(void) {}
void comp_bin(uint8_t *sys, unsigned bin, int count) {}
