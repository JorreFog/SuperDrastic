/* dshook.c: where DraStic is loaded and how its functions are hooked (the display path's direct rendering in
 * dsflip.c, the rasterizer and the compositor in rast/). DraStic r2.5.2.2, the Linux aarch64 build, is the only one
 * these offsets are for: every hook checks the function's first four instructions before it touches anything. */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "rast/rast.h"

uintptr_t ds_base;

static int cb(struct dl_phdr_info *i, size_t s, void *d) {
    (void)s; (void)d;
    if (!i->dlpi_name[0] && !ds_base) ds_base = i->dlpi_addr;
    return 0;
}
uintptr_t ds_find_base(void) {
    if (!ds_base) dl_iterate_phdr(cb, 0);
    return ds_base;
}

static void patch_jump(uint32_t *at, void *to) {
    at[0] = 0x58000050;                   /* ldr x16, #8 */
    at[1] = 0xd61f0200;                   /* br x16 */
    memcpy(at + 2, &to, 8);
}

/* patch DraStic's function at `off` (whose first 4 instructions must be `expect`) to jump to `to`; returns a
 * trampoline that runs the original (valid only when those 4 instructions are position independent), or 0 */
void *rast_hook(uintptr_t off, const uint32_t expect[4], void *to) {
    if (!ds_find_base()) return 0;
    uint32_t *entry = (uint32_t *)(ds_base + off);
    if (memcmp(entry, expect, 16)) return 0;
    static uint32_t *tramp; static int used;
    if (!tramp) {
        tramp = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tramp == MAP_FAILED) { tramp = 0; return 0; }
    }
    if (used + 8 > 4096 / 4) return 0;
    uint32_t *t = tramp + used; used += 8;
    memcpy(t, entry, 16);
    patch_jump(t + 4, entry + 4);
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    uintptr_t pg = (uintptr_t)entry & ~4095ul;
    if (mprotect((void *)pg, 8192, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
    patch_jump(entry, to);
    __builtin___clear_cache((char *)entry, (char *)(entry + 4));
    mprotect((void *)pg, 8192, PROT_READ | PROT_EXEC);
    return t;
}

/* point one of DraStic's imported functions (its GOT slot at `got_off`; the GOT is read-only after loading) at `to`;
 * returns what the slot held, or 0. Only DraStic's own calls of the function change. */
void *ds_got_patch(uintptr_t got_off, void *to) {
    if (!ds_find_base()) return 0;
    void **slot = (void **)(ds_base + got_off), *old = *slot;
    uintptr_t pg = (uintptr_t)slot & ~4095ul;
    if (!old || mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE)) return 0;
    *slot = to;
    mprotect((void *)pg, 4096, PROT_READ);
    return old;
}
