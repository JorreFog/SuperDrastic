/* fogzero.h: the fog's depth screen bound, shared by the 2x fog (res2.c, fog_line2x) and the 3x one (hr.c, fog_line).
 *
 * The bound, once per bin: the largest depth byte b2 (attribute bits 16-23) such that every fog-flagged pixel whose
 * b2 is at most it has the factor k = 0, or -1 (no screen). In FOG_K, a pixel's 15-bit depth d = attribute bits
 * 9-23 gives v = min((d -sat off) << sh, 0x7fff), i = v >> 10, r = v & 1023 and (sqdmulh of delta[i] << 5 by v:
 * delta[i] * v / 1024, at most 2^27, no saturation; ntab[i] = delta[i] * i - table[i])
 *     k = -(table[i] + floor(delta[i] * r / 1024)) mod 256,
 * before the 0x81 -> 0x80 adjust, which neither makes nor unmakes a 0 (delta signed, table and k bytes). From v = 0
 * up, segment i (v = 1024 i + r) is 0 throughout if table[i] = delta[i] = 0; else, with table[i] = 0, it is 0 up to
 * r = 1023 / delta[i] for delta[i] > 0 (then 1 <= floor(..) <= 126) and only at r = 0 for delta[i] < 0 (then -1);
 * table[i] != 0 ends the run before the segment. So k = 0 for every v <= z, which holds exactly when
 * d <= off + (z >> sh) = dmax (sh <= 15 here), i.e. (b2 << 8 | b1) <= 2 * dmax + 1, which every b2 <= (2 * dmax +
 * 1 - 255) >> 8 satisfies whatever b1 (the screen gives up the steps whose largest depth is within 128 of dmax).
 * The fog scene S4 (table 4 i, offset 0x6000) has z = 255: the steps before the fog offset. A pixel without the fog
 * flag has k = 0 whatever its depth, so a step whose 32 depth bytes are all at most the bound has every factor 0
 * whether or not its flags are looked at. */
#ifndef FOGZERO_H
#define FOGZERO_H
#include <stdint.h>

/* table: the 32 densities and their 32 deltas (geom + 0x9974); params: the shift in bits 0-7 and off = the fog
 * offset + (0x400 >> shift) in bits 16-31, as the fog passes take them */
static inline int fog_zero_b2(const uint8_t *table, uint32_t params) {
    int z = 0x7fff;
    for (int i = 0; i < 32; i++) {
        int t = table[i], dl = (int8_t)table[32 + i];
        if (t) { z = 1024 * i - 1; break; }
        if (dl > 0) { z = 1024 * i + 1023 / dl; break; }
        if (dl < 0) { z = 1024 * i; break; }
    }
    uint32_t sh = params & 0xff;
    if (z < 0 || sh > 15) return -1;
    if (z >= 0x7fff) return 255;
    uint32_t lim = 2 * ((params >> 16) + ((uint32_t)z >> sh)) + 1;
    if (lim < 255) return -1;
    lim = (lim - 255) >> 8;
    return lim > 255 ? 255 : (int)lim;
}
#endif
