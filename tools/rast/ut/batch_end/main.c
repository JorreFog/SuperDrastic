/* main.c: batch_end and one_batch (src/rast/fused.c, cut out by run.sh) against f_run's scalar batch loop of e02a298 */
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
    long its = argc > 1 ? atol(argv[1]) : 1000000, bad = 0, batches = 0, ones = 0;
    srand(argc > 2 ? (unsigned)atoi(argv[2]) : 1);
    static uint16_t blk[2 * 64];                /* array 9 of the largest layout: 64 words, a count in each low half */
    for (long it = 0; it < its; it++) {
        unsigned hr = rand() & 1, bmax = hr ? 4096 : 512, maxc = hr ? 768 : 512, nlines = 1 + rand() % (hr ? 50 : 34);
        int mode = rand() % 5;
        for (int j = 0; j < 128; j++) blk[j] = (uint16_t)rand();           /* garbage, then the lines' counts */
        for (unsigned l = 0; l < nlines; l++)
            blk[2 * l] = (uint16_t)(mode == 0 ? rand() % (maxc + 1) : mode == 1 ? rand() % 40
                                  : mode == 2 ? (rand() % 8 ? 1 + rand() % 100 : 0) : mode == 3 ? maxc - rand() % 3
                                  : (rand() % 64 ? 1 : 0) + rand() % (2 * bmax / nlines < maxc ? 2 * bmax / nlines : maxc));
        /* one_batch: all lines one batch, as the loop from line 0 (line 0 with pixels) finds it; totals about bmax */
        if (rand() % 3 == 0) for (unsigned l = 0, z = rand() % 3; l < z && l < nlines; l++) blk[2 * l] = 0;          /* tips */
        if (rand() % 3 == 0) for (unsigned l = 0, z = rand() % 3; l < z && l < nlines; l++) blk[2 * (nlines - 1 - l)] = 0;
        /* one_batch: the lines with pixels one batch [f, e) as the loop finds it: its first batch from f (the first line
         * with pixels) ends at e and no line after e has pixels */
        unsigned f = 0, want = 0, of = 0, ob = one_batch((const uint8_t *)blk, nlines, bmax, &of);
        while (f < nlines && !blk[2 * f]) f++;
        if (f < nlines) {
            unsigned e = ref_end(blk, f + 1, nlines, blk[2 * f], bmax), z = e;
            while (z < nlines && !blk[2 * z]) z++;
            if (z == nlines) want = e;
        }
        ones += ob != 0;
        if ((ob != want || (ob && of != f)) && bad++ < 5)
            printf("one_batch mismatch: %u lines, bmax %u: %u from %u, want %u from %u\n", nlines, bmax, ob, of, want, f);
        for (unsigned i = 0; i < nlines;) {
            unsigned n = blk[2 * i];
            if (!n) { i++; continue; }
            unsigned e = batch_end((const uint8_t *)blk, i + 1, nlines, n, bmax), r = ref_end(blk, i + 1, nlines, n, bmax);
            batches++;
            if (e != r && bad++ < 5) printf("mismatch: %u lines, batch from %u: %u, want %u\n", nlines, i, e, r);
            i = r;
        }
    }
    printf("%ld batches (%ld polygons one batch), %ld differ: %s\n", batches, ones, bad, bad ? "FAIL" : "PASS");
    return bad != 0;
}
