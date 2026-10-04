/* compfuse.h: the planes of a "3D + backdrop" quarter of render_scanline_2d_composite's simple path in one NEON
 * pass, for comp.c's hook. Same bytes as DraStic's select_pixels -> binary_scalar -> expand_6bit_split -> binary32
 * chain (spec/composite.c) whenever comp_fused_kind() says the quarter qualifies; tools/rast/ut/t_composite.c tests
 * it against the C port and the originals.
 *
 * The chain, for a quarter whose layer mask holds BG0 (the 3D layer) and whose masks from the priority encoder are
 * excl[0] (BG0 is the top layer), excl[1..4] (another layer is) and excl[5] (nothing is: the backdrop):
 *      tmp[i]  = excl[k] bit i ? layer k [i] : BG0's own u16 line [i]      for the other layers k of the mask
 *      tmp[i]  = excl[5] bit i ? backdrop : tmp[i]
 *      planes  = expand(tmp)      R = (c & 0x1f) << 1, G = ((c >> 5) & 0x1f) << 1, B = ((c >> 10) & 0x1f) << 1
 *      planes  = excl[0] bit i ? bytes 0, 1, 2 of the 3D pixel i (as they are) : planes
 * BG0's own u16 line holds whatever was last written there (BG0 is the 3D layer, not a tile layer), and shows only
 * where no mask claims the pixel. So when every pixel is claimed by excl[0] or excl[5] and the other layers of the
 * mask claim none, the planes are, for every pixel,
 *      R[i] = excl[0] bit i ? byte0(px[i]) : (bd & 0x1f) << 1       (bd = the backdrop colour, BGR555)
 *      G[i] = excl[0] bit i ? byte1(px[i]) : ((bd >> 5) & 0x1f) << 1
 *      B[i] = excl[0] bit i ? byte2(px[i]) : ((bd >> 10) & 0x1f) << 1
 * independent of that u16 line. When excl[0] is all ones the last step rewrites every byte, so the other masks do
 * not matter at all. comp_fused_kind() tells the three shapes apart (all 3D, all backdrop, mixed), or says no.
 *
 * The kernels (inline asm, as compvis.h: the compiler's version of the same intrinsics copies registers around the
 * two-register stores and the destructive bit selects), per 32 pixels: two ld4 de-interleave the pixels' bytes into
 * r, g, b (and a) registers; stp stores 32 bytes of a plane from any two registers. Mixed: the mask word's 4 bytes,
 * each replicated to 8 lanes (ld1r + tbl), tested against the bit weights 1, 2, .., 128 (cmtst) give a 0xff / 0
 * byte per pixel, as DraStic's binary32 does it; bif keeps the 3D byte where the mask is set and inserts the
 * backdrop's byte elsewhere. The pixel loads of a block are issued before the previous block's selects and stores
 * (two register groups), so the in-order core has them in flight. 40 instructions for all 3D, 27 all backdrop, ~140
 * mixed; the original chain is ~620 (and reads and writes the u16 line and the planes twice). */
#ifndef COMPFUSE_H
#define COMPFUSE_H
#include <stdint.h>
#include <string.h>

enum { CF_KIND_NONE = 0, CF_KIND_3D = 1, CF_KIND_BACKDROP = 2, CF_KIND_MIXED = 3 };

static inline uint64_t cf_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* excl: the priority encoder's six 32-byte masks (BG0..BG3, OBJ, backdrop); lmask: the layer mask (bit k = layer k
 * is composited; bit 0 = BG0 must be set, bits above 4 clear: the caller checks). Returns the kind of fused quarter,
 * or CF_KIND_NONE when another layer of the mask claims a pixel, or a pixel is neither BG0's nor the backdrop's
 * (BG0's u16 line would show there). */
static inline int comp_fused_kind(const uint8_t *excl, uint32_t lmask) {
    uint64_t e0 = cf_u64(excl), e1 = cf_u64(excl + 8), e2 = cf_u64(excl + 16), e3 = cf_u64(excl + 24);
    if ((e0 & e1 & e2 & e3) == ~0ull) return CF_KIND_3D;             /* BG0 on top everywhere: nothing else shows */
    for (unsigned k = 1; k <= 4; k++)
        if (lmask >> k & 1) {
            const uint8_t *e = excl + 32 * k;
            if (cf_u64(e) | cf_u64(e + 8) | cf_u64(e + 16) | cf_u64(e + 24)) return CF_KIND_NONE;
        }
    const uint8_t *b = excl + 0xa0;
    if (((e0 | cf_u64(b)) & (e1 | cf_u64(b + 8)) & (e2 | cf_u64(b + 16)) & (e3 | cf_u64(b + 24))) != ~0ull)
        return CF_KIND_NONE;
    return (e0 | e1 | e2 | e3) ? CF_KIND_MIXED : CF_KIND_BACKDROP;
}

/* one block of 32 pixels into register group A (v0..v7) or B (v16..v23): r, g, b, a of pixels 0..15 in the first
 * four, 16..31 in the last four */
#define CF_LDA "ld4 {v0.16b, v1.16b, v2.16b, v3.16b}, [%[p]], #64\n\t" "ld4 {v4.16b, v5.16b, v6.16b, v7.16b}, [%[p]], #64\n\t"
#define CF_LDB "ld4 {v16.16b, v17.16b, v18.16b, v19.16b}, [%[p]], #64\n\t" "ld4 {v20.16b, v21.16b, v22.16b, v23.16b}, [%[p]], #64\n\t"
/* the three planes of block k (pixels 32k..) from group A or B */
#define CF_STA(k) "stp q0, q4, [%[o], #" #k "*32]\n\t" "stp q1, q5, [%[o], #" #k "*32+0x100]\n\t" "stp q2, q6, [%[o], #" #k "*32+0x200]\n\t"
#define CF_STB(k) "stp q16, q20, [%[o], #" #k "*32]\n\t" "stp q17, q21, [%[o], #" #k "*32+0x100]\n\t" "stp q18, q22, [%[o], #" #k "*32+0x200]\n\t"
/* the next mask word (4 bytes = 32 pixels) expanded to v31 (pixels 0..15) and v30 (16..31): 0xff where set */
#define CF_MASK \
    "ld1r  {v30.4s}, [%[m]], #4\n\t" \
    "tbl   v31.16b, {v30.16b}, v24.16b\n\t"  "tbl   v30.16b, {v30.16b}, v25.16b\n\t" \
    "cmtst v31.16b, v31.16b, v26.16b\n\t"    "cmtst v30.16b, v30.16b, v26.16b\n\t"
/* the backdrop (v27 R, v28 G, v29 B) into the lanes whose mask is clear */
#define CF_SELA \
    "bif v0.16b, v27.16b, v31.16b\n\t"  "bif v4.16b, v27.16b, v30.16b\n\t" \
    "bif v1.16b, v28.16b, v31.16b\n\t"  "bif v5.16b, v28.16b, v30.16b\n\t" \
    "bif v2.16b, v29.16b, v31.16b\n\t"  "bif v6.16b, v29.16b, v30.16b\n\t"
#define CF_SELB \
    "bif v16.16b, v27.16b, v31.16b\n\t"  "bif v20.16b, v27.16b, v30.16b\n\t" \
    "bif v17.16b, v28.16b, v31.16b\n\t"  "bif v21.16b, v28.16b, v30.16b\n\t" \
    "bif v18.16b, v29.16b, v31.16b\n\t"  "bif v22.16b, v29.16b, v30.16b\n\t"
#define CF_CLOBBER_A "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"
#define CF_CLOBBER_B "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23"

/* out: the quarter's planes (R6[256], G6[256], B6[256]); px: its 256 3D pixels; mask: excl[0]; bd: the backdrop
 * colour (BGR555, as the u16 is); kind: from comp_fused_kind(), not CF_KIND_NONE. px and mask must not overlap out. */
static inline __attribute__((always_inline)) void comp_fused_planes(uint8_t *out, const uint32_t *px, const uint8_t *mask,
                                                                     uint32_t bd, int kind) {
    const uint8_t *p = (const uint8_t *)px;
    if (kind == CF_KIND_3D) {
        __asm__(CF_LDA CF_LDB CF_STA(0) CF_LDA CF_STB(1) CF_LDB CF_STA(2) CF_LDA CF_STB(3)
                CF_LDB CF_STA(4) CF_LDA CF_STB(5) CF_LDB CF_STA(6) CF_STB(7)
                : [p] "+&r"(p), "=m"(*(uint8_t (*)[0x300])out)
                : [o] "r"(out), "m"(*(const uint8_t (*)[0x400])px)
                : CF_CLOBBER_A, CF_CLOBBER_B);
        return;
    }
    uint32_t r = (bd & 0x1f) << 1, g = ((bd >> 5) & 0x1f) << 1, b = ((bd >> 10) & 0x1f) << 1;
    if (kind == CF_KIND_BACKDROP) {
        __asm__("dup v0.16b, %w[r]\n\t" "dup v1.16b, %w[g]\n\t" "dup v2.16b, %w[b]\n\t"
                "stp q0, q0, [%[o]]\n\t" "stp q0, q0, [%[o], #0x20]\n\t" "stp q0, q0, [%[o], #0x40]\n\t" "stp q0, q0, [%[o], #0x60]\n\t"
                "stp q0, q0, [%[o], #0x80]\n\t" "stp q0, q0, [%[o], #0xa0]\n\t" "stp q0, q0, [%[o], #0xc0]\n\t" "stp q0, q0, [%[o], #0xe0]\n\t"
                "stp q1, q1, [%[o], #0x100]\n\t" "stp q1, q1, [%[o], #0x120]\n\t" "stp q1, q1, [%[o], #0x140]\n\t" "stp q1, q1, [%[o], #0x160]\n\t"
                "stp q1, q1, [%[o], #0x180]\n\t" "stp q1, q1, [%[o], #0x1a0]\n\t" "stp q1, q1, [%[o], #0x1c0]\n\t" "stp q1, q1, [%[o], #0x1e0]\n\t"
                "stp q2, q2, [%[o], #0x200]\n\t" "stp q2, q2, [%[o], #0x220]\n\t" "stp q2, q2, [%[o], #0x240]\n\t" "stp q2, q2, [%[o], #0x260]\n\t"
                "stp q2, q2, [%[o], #0x280]\n\t" "stp q2, q2, [%[o], #0x2a0]\n\t" "stp q2, q2, [%[o], #0x2c0]\n\t" "stp q2, q2, [%[o], #0x2e0]\n\t"
                : "=m"(*(uint8_t (*)[0x300])out)
                : [o] "r"(out), [r] "r"(r), [g] "r"(g), [b] "r"(b)
                : "v0", "v1", "v2");
        return;
    }
    /* mixed: lane j of the two index vectors names the mask byte of pixel 16h + j, the weights its bit */
    static const uint8_t tab[48] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
                                     1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
    __asm__("ldp q24, q25, [%[t]]\n\t" "ldr q26, [%[t], #32]\n\t"
            "dup v27.16b, %w[r]\n\t" "dup v28.16b, %w[g]\n\t" "dup v29.16b, %w[b]\n\t"
            CF_LDA
            CF_LDB CF_MASK CF_SELA CF_STA(0)
            CF_LDA CF_MASK CF_SELB CF_STB(1)
            CF_LDB CF_MASK CF_SELA CF_STA(2)
            CF_LDA CF_MASK CF_SELB CF_STB(3)
            CF_LDB CF_MASK CF_SELA CF_STA(4)
            CF_LDA CF_MASK CF_SELB CF_STB(5)
            CF_LDB CF_MASK CF_SELA CF_STA(6)
            CF_MASK CF_SELB CF_STB(7)
            : [p] "+&r"(p), [m] "+&r"(mask), "=m"(*(uint8_t (*)[0x300])out)
            : [o] "r"(out), [t] "r"(tab), [r] "r"(r), [g] "r"(g), [b] "r"(b), "m"(*(const uint8_t (*)[0x400])px),
              "m"(*(const uint8_t (*)[32])mask)
            : CF_CLOBBER_A, CF_CLOBBER_B, "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
#undef CF_LDA
#undef CF_LDB
#undef CF_STA
#undef CF_STB
#undef CF_MASK
#undef CF_SELA
#undef CF_SELB
#undef CF_CLOBBER_A
#undef CF_CLOBBER_B
#endif
