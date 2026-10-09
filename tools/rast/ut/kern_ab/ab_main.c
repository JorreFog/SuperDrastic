/* ab_main.c: the kernel A/B (run.sh): every nearest-filtering kernel of the base rast_kern.S (old_) and the
 * worktree's (new_), 2x and hi-res sets, on the same random batches; compares everything a kernel writes.
 * The inputs need not be a real polygon's (both versions must compute the same from any input); they are kept in
 * the ranges that keep every access inside the buffers (texture coordinates masked by W-1 and H-1, palette indices
 * bytes) and mostly in the ranges real spans have (depths in 24 bits, weights in [0, 1)), so that the depth test
 * passes and fails, whole groups pass (the straight store) and partial ones store through the masks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {                /* fused_asm.c's kargs_t (kerngen.py's K dict) */
    const uint32_t *recip, *recip_u;
    const void *tex; const uint32_t *pal;
    uint32_t pid24[4];
    uint8_t bytes[8];
    uint32_t K; uint16_t tw; uint16_t pad;
    uint16_t s_and[8], t_and[8], s_lo[8], t_lo[8], s_hi[8], t_hi[8], s_flip[8], t_flip[8];
    uint8_t fraclut[16];
    uint16_t *owner;
    const void *kern;
    uint16_t idx16[8];
    uint32_t lstride, attr_off, id_off, id_stride, owner_stride;
    uint32_t kflags, kopt, pad3;
    uint8_t pal16[4][16];
} kargs_t;
_Static_assert(sizeof(kargs_t) == 0x150, "kargs_t layout");
typedef uint64_t kern_fn(const kargs_t *, const uint8_t *bs, uint32_t k, uint32_t line, uint8_t *ctx, uint32_t flags, uint8_t *id0);

#define X(n, h, m, d, t, r) extern kern_fn old_rast_kern_##n, new_rast_kern_##n;
#include "names.h"
#undef X
static const struct { const char *name; kern_fn *a, *b; int hr, m, d, t, r; } kt[] = {
#define X(n, h, m, d, t, r) { #n, old_rast_kern_##n, new_rast_kern_##n, h, m, d, t, r },
#include "names.h"
#undef X
};

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t rr(uint32_t n) { return rnd() % n; }

#define CTX2 0x24000            /* 2x: colours + line * 0x800, attributes + 0x10000, ids + 0x20000 (0x200 a line) */
#define CTXH 0x51000            /* hi-res: lines of 768, 48 of them: attributes + 0x24000, ids + 0x48000 */
#define PAD 0x1000
static uint8_t ctx_a[CTXH + PAD], ctx_b[CTXH + PAD], ctx0[CTXH + PAD];
static uint16_t own_a[48 * 768 + PAD], own_b[48 * 768 + PAD], own0[48 * 768 + PAD];
static uint32_t tex[256 * 256 + PAD], pal[256], recip[1024], recip_u[1024];
static uint8_t bs[11 * 0x100 + PAD];

int main(int argc, char **argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 100;
    rs = argc > 2 ? strtoull(argv[2], 0, 0) : 0x9e3779b97f4a7c15ull;
    if (!rs) rs = 1;
    for (int c = 1; c < 1024; c++) { recip[c] = (1u << 30) / c; recip_u[c] = 0x80000000u / c; }
    unsigned long calls = 0, bad = 0;
    for (size_t ki = 0; ki < sizeof kt / sizeof kt[0]; ki++) {
        int hr = kt[ki].hr, M = kt[ki].m, D = kt[ki].d, T = kt[ki].t, R = kt[ki].r;
        int lines = hr ? 48 : 32, width = hr ? 768 : 512, sps = hr ? 0x100 : 0xb0;
        unsigned kbad = 0;
        for (int it = 0; it < iters; it++) {
            kargs_t a; memset(&a, 0, sizeof a);
            a.recip = recip; a.recip_u = recip_u; a.tex = tex; a.pal = pal;
            int ws = 3 + rr(6), hs = 3 + rr(6), W = 1 << ws, H = 1 << hs;
            for (int i = 0; i < W * H; i++) tex[i] = rnd() << 8 ^ rnd();
            for (int i = 0; i < 256; i++) pal[i] = rnd() << 8 ^ rnd();
            for (int i = 0; i < 64; i++) a.pal16[i >> 4][i & 15] = rnd();
            uint8_t pid = rr(64);
            for (int i = 0; i < 4; i++) a.pid24[i] = (uint32_t)pid << 24;
            a.bytes[0] = rr(4) ? 31 : rr(32); a.bytes[1] = rr(32); a.bytes[2] = rr(2) << 7; a.bytes[3] = pid;
            for (int i = 4; i < 7; i++) a.bytes[i] = rr(4) ? 0xff : rnd();
            a.K = rnd() & 0xffffff; a.tw = W;
            for (int i = 0; i < 8; i++) {
                a.s_and[i] = W - 1; a.t_and[i] = H - 1; a.s_flip[i] = rr(2) ? W : 0; a.t_flip[i] = rr(2) ? H : 0;
                a.idx16[i] = 0;
            }
            uint16_t idx = 1 + rr(200);
            for (int i = 0; i < 8; i++) a.idx16[i] = idx;
            a.owner = rr(2) ? own_a : own_b;   /* (set below per side) */
            a.lstride = width * 4; a.attr_off = lines * width * 4; a.id_off = 2 * lines * width * 4;
            a.id_stride = width; a.owner_stride = width * 2;
            /* the batch: k lines from bin line l */
            uint32_t l = rr(lines), k = 1 + rr(lines - l < 8 ? lines - l : 8);
            if (rr(4) == 0) k = 1 + rr(lines - l);
            memset(bs, 0, sizeof bs);
            int mode = rr(4);            /* depths: 0 mixed, 1 all pass, 2 all fail, 3 mixed near */
            uint32_t zbase = rnd() & 0x7fffff;
            for (uint32_t i = 0; i < k; i++) {
                uint32_t C = rr(8) ? rr(4) ? 1 + rr(40) : 1 + rr(200) : 1 + rr(width - 1);
                uint32_t Xs = rr(width - C + 1);
                if (rr(8) == 0) C = 0;
                int32_t W0 = 1 + rr(0xffff), W1 = 1 + rr(0xffff);
                if (rr(4) == 0) W1 = W0;
                uint32_t *f = (uint32_t *)bs;
                #define F(n) f[(n) * sps / 4 + i]
                F(0) = W0; F(1) = W1 - W0;
                F(2) = mode == 1 ? 0 : mode == 2 ? 0xfff000 : zbase + rr(0x1000);
                F(3) = (int32_t)(rr(0x2000)) - 0x1000;
                F(4) = rnd(); F(5) = rr(2) ? rnd() : (rnd() & 0x0fff0fff);
                F(6) = rr(0x200) | rr(0x200) << 16;
                F(7) = rr(16) ? ((rr(0x400) - 0x200) & 0xffff) | ((rr(0x400) - 0x200) & 0xffff) << 16 : 0x80008000u;
                if (rr(4) == 0) { F(6) = 0x1ff01ff; F(7) = 0; }
                F(8) = Xs | (rr(4) ? 0x1ff : rr(0x200)) << 16;
                F(9) = C | ((rr(4) ? 0 : rr(0x400) - 0x200) & 0xffff) << 16;
                F(10) = rr(C + 2) | rr(C + 2) << 16;
                #undef F
            }
            uint32_t flags = 0;
            if (rr(4) == 0) flags |= 1;          /* affine */
            if (rr(8) == 0) flags |= 2;          /* depth equal */
            if (rr(4) == 0) flags |= 4;          /* white */
            if (R) flags |= rr(8) << 3;          /* blend, fog, depth update */
            if (rr(2)) flags |= 1 << 6;          /* edge marking */
            if (rr(2)) flags |= 1 << 7;          /* 16 colours */
            if (rr(2)) flags |= 1 << 8;          /* no alpha test */
            if (rr(4) == 0) flags |= 1 << 9;     /* white lines */
            if (rr(4)) flags |= 1 << 10;         /* A is 31 */
            flags |= rr(4) << 11;                /* clamps */
            /* the buffers: random colours and ids, attribute words by the depth mode */
            for (size_t i = 0; i < sizeof ctx0 / 4; i++) ((uint32_t *)ctx0)[i] = rnd() << 8 ^ rnd();
            uint32_t *at = (uint32_t *)(ctx0 + (hr ? a.attr_off : 0x10000));
            for (int i = 0; i < lines * (hr ? width : 512); i++) {
                uint32_t d = mode == 1 ? 0xffffff : mode == 2 ? 0 : mode == 3 ? zbase + rr(0x3000) : rnd() & 0xffffff;
                if (hr) at[i] = (at[i] & 0xff000000) | d;
                else at[(i / 512) * 512 + (i % 512)] = (at[i] & 0xff000000) | d;
            }
            for (size_t i = 0; i < sizeof own0 / 2; i++) own0[i] = rr(2) ? idx : rr(256);
            memcpy(ctx_a, ctx0, sizeof ctx0); memcpy(ctx_b, ctx0, sizeof ctx0);
            memcpy(own_a, own0, sizeof own0); memcpy(own_b, own0, sizeof own0);
            uint8_t id_a[64], id_b[64];
            memset(id_a, 0x5a, sizeof id_a); memset(id_b, 0x5a, sizeof id_b);
            a.owner = own_a;
            uint64_t ra = kt[ki].a(&a, bs, k, l, ctx_a, flags, id_a);
            a.owner = own_b;
            uint64_t rb = kt[ki].b(&a, bs, k, l, ctx_b, flags, id_b);
            calls++;
            int diff = ra != rb || memcmp(ctx_a, ctx_b, sizeof ctx_a) || memcmp(own_a, own_b, sizeof own_a) ||
                       memcmp(id_a, id_b, sizeof id_a);
            if (diff) {
                if (kbad++ < 3) {
                    size_t o = 0; while (o < sizeof ctx_a && ctx_a[o] == ctx_b[o]) o++;
                    printf("kern_ab: %s (M %d D %d T %d R %d) iteration %d flags %#x lines %u+%u: differ (ret %llx %llx, ctx byte %#zx)\n",
                           kt[ki].name, M, D, T, R, it, flags, l, k, (unsigned long long)ra, (unsigned long long)rb, o);
                }
            }
        }
        bad += kbad;
    }
    printf("kern_ab: %zu kernels, %lu batches, %lu differ\n", sizeof kt / sizeof kt[0], calls, bad);
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
