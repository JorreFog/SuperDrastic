/* ut.h: unit tests of DraStic's 3D routines against our C ports, run inside DraStic's own process (the test .so is
 * preloaded; its constructor runs the tests and exits before DraStic's main). See run.sh. */
#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uintptr_t ut_base;
static int ut_cb(struct dl_phdr_info *i, size_t s, void *d) { if (!i->dlpi_name[0] && !ut_base) ut_base = i->dlpi_addr; return 0; }
/* a DraStic function or object at a file offset (see re/drastic.dis) */
#define DS(type, off) ((type)(ut_base + (off)))

/* xorshift64 */
static uint64_t ut_rs = 0x9E3779B97F4A7C15ull;
static inline uint64_t rnd64(void) { ut_rs ^= ut_rs << 13; ut_rs ^= ut_rs >> 7; ut_rs ^= ut_rs << 17; return ut_rs; }
static inline uint32_t rnd(uint32_t n) { return n ? (uint32_t)(rnd64() % n) : 0; }
static inline int32_t rndr(int32_t lo, int32_t hi) { return lo + (int32_t)rnd((uint32_t)(hi - lo + 1)); }
static void rndfill(void *p, size_t n) { uint8_t *b = p; for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rnd64(); }

static int ut_fail;
/* compare two buffers; print the first few differing bytes/words */
static int ut_cmp(const char *what, const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) {
        size_t w = i & ~3ul;
        fprintf(stderr, "FAIL %s: byte %zu differs (word@%zu drastic %08x ours %08x)\n", what, i, w,
                *(const uint32_t *)(x + w), *(const uint32_t *)(y + w));
        ut_fail++;
        return 1;
    }
    return 0;
}
void ut_main(void);
__attribute__((constructor)) static void ut_init(void) {
    dl_iterate_phdr(ut_cb, 0);
    const char *s = getenv("UT_SEED"); if (s) ut_rs = strtoull(s, 0, 0) | 1;
    ut_main();
    fprintf(stderr, ut_fail ? "UT: %d FAILURES\n" : "UT: all passed\n", ut_fail);
    fflush(stderr);
    _exit(ut_fail ? 1 : 0);
}
