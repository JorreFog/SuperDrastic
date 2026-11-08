/* edges_recip.h: ceil(2^30 / i), the value DraStic stores in reciprocal_table.
 * The table is that value for 1..512 and 0 for 0 and for 513..1023. A 2x edge fits in 384 lines, so the 2x
 * walker must keep the 512 limit to stay bit-exact. Vertex y is a u16, and a 3x edge across the DS view is
 * 576 pixels, so the 3x walker passes a limit of 65535. 0x3fffffff + i stays inside a u32 up to that limit. */
#ifndef EDGES_RECIP_H
#define EDGES_RECIP_H
#include <stdint.h>
static inline uint32_t edges_reciprocal_at(int32_t i, int32_t max_i) {
    if (i >= 1 && i <= max_i) return (0x3fffffffu + (uint32_t)i) / (uint32_t)i;
    return 0;
}
#endif
