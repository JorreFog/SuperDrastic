/* one side of the A/B: hr.c (HRFILE) with its exported names prefixed by TAG, wrappers for the static stages */
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define PFX(x) CAT(TAG, x)
#define hr_vertices PFX(hr_vertices)
#define hr_render_bins PFX(hr_render_bins)
#define layout_3x PFX(layout_3x)
#define hr_vcheck PFX(hr_vcheck)
#define hr_frame PFX(hr_frame)
#include HRFILE
void PFX(ds)(const uint32_t *in, uint8_t *out, uint32_t clear) { hr_downsample(in, out, clear); }
void PFX(fog)(uint32_t *c, const uint32_t *attr, const uint8_t *table, uint32_t params, uint32_t fogc, int full) { fog_line(c, attr, table, params, fogc, full); }
/* the resolve and the downsample of one bin from the context bytes ctx (HR_CTX_SIZE) */
void PFX(rds)(const uint8_t *ctx, uint8_t *sys, uint8_t *geom, uint8_t *out) {
    hr_t *H = hr_get();
    memcpy(H->ctx, ctx, HR_CTX_SIZE);
    const uint32_t *res = hr_resolve_bin(H, sys, geom, 0);
    hr_downsample(res, out, U32(sys, SYS_CLEAR_COLOR));
}
unsigned PFX(ctx_size)(void) { return HR_CTX_SIZE; }
/* zeroes the stack below the caller's frame: what a callee might read without writing it first is then the same in
 * both versions */
static __attribute__((noinline)) void PFX(zero_stack)(void) {
    volatile uint8_t z[8192];
    for (unsigned i = 0; i < sizeof z; i++) z[i] = 0;
}
/* hr_polygon on a polygon record and the vertex records, with the 3x and 2x coordinates hv and hv2 (HR_NVTX each) */
void PFX(poly)(uint8_t *poly, uint8_t *verts, const void *hv, const void *hv2, unsigned bin_top, unsigned bin_bot, int lb,
               uint32_t d3, int defer) {
    memcpy(hr_vtx[0], hv, sizeof hr_vtx[0]); memcpy(hr_vtx2[0], hv2, sizeof hr_vtx2[0]);
    /* a span block of the test's own, zeroed with 4 KiB on both sides for every polygon: with each version's heap
     * block and the previous polygon's spans left in it, two copies of the same hr.c hashed a few polygons in 5000
     * differently (8 to 10 vertices; the cause is not known yet) */
    static uint8_t spans[HR_SPANS + 8192] __attribute__((aligned(64)));
    hr_t *H = hr_get();
    memset(spans, 0, sizeof spans);
    H->spans = spans + 4096;
    PFX(zero_stack)();
    hr_polygon(H, poly, verts, hr_vtx[0], hr_vtx2[0], bin_top, bin_bot, lb, d3, defer);
}
