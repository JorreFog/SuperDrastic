/* main.c: batch_end (src/rast/fused.c, cut out by run.sh) against f_run's scalar batch loop of e02a298 */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "batch_end.h"

/* the loop it replaced: from line i on (n: the batch's pixels so far), two lines a step */
static unsigned ref_end(const uint16_t *cnt, unsigned i, unsigned nlines, unsigned n, unsigned bmax) {
    for (; i + 1 < nlines; i += 2) {
        unsigned c0 = cnt[2 * i], c1 = cnt[2 * i + 2];
        if (c0 - 1 >= bmax - n) return i;
        n += c0;
        if (c1 - 1 >= bmax - n) return i + 1;
        n += c1;
    }
    if (i < nlines && cnt[2 * i] - 1u < bmax - n) i++;
    return i;
}

int main(int argc, char **argv) {
    long its = argc > 1 ? atol(argv[1]) : 1000000, bad = 0, batches = 0;
    srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    static uint16_t blk[2 * 64];                /* array 9 of the largest layout: 64 words, a count in each low half */
    for (long it = 0; it < its; it++) {
        unsigned hr = rand() & 1, bmax = hr ? 4096 : 512, maxc = hr ? 768 : 512, nlines = 1 + rand() % (hr ? 50 : 34);
        int mode = rand() % 4;
        for (int j = 0; j < 128; j++) blk[j] = (uint16_t)rand();           /* garbage, then the lines' counts */
        for (unsigned l = 0; l < nlines; l++)
            blk[2 * l] = (uint16_t)(mode == 0 ? rand() % (maxc + 1) : mode == 1 ? rand() % 40
                                  : mode == 2 ? (rand() % 8 ? 1 + rand() % 100 : 0) : maxc - rand() % 3);
        for (unsigned i = 0; i < nlines;) {
            unsigned n = blk[2 * i];
            if (!n) { i++; continue; }
            unsigned e = batch_end((const uint8_t *)blk, i + 1, nlines, n, bmax), r = ref_end(blk, i + 1, nlines, n, bmax);
            batches++;
            if (e != r && bad++ < 5) printf("mismatch: %u lines, batch from %u: %u, want %u\n", nlines, i, e, r);
            i = r;
        }
    }
    printf("%ld batches, %ld differ: %s\n", batches, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
