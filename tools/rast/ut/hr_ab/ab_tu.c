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
