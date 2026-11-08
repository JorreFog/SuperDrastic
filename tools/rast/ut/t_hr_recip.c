/* A 3x polygon edge taller than 512 pixels used to get DraStic's reciprocal of 0, so the edge was vertical
 * at its top vertex (x and depth never stepped). The 2x walker still stops at 512.
 *
 * The screen x below is interp_x in src/rast/spec/edges_impl.h: xs = (dx * recip) >> 12, x starts at xa<<18,
 * advances by xs once per line, and the stored x is the high 14 bits. skip is the lines above this bin.
 *
 * cc -O2 -Wall -Wextra -Werror -o /tmp/t_hr_recip tools/rast/ut/t_hr_recip.c && /tmp/t_hr_recip
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../../src/rast/spec/edges_recip.h"

static int fails;

static void expect(int cond, const char *what) {
    if (!cond) { fprintf(stderr, "FAIL %s\n", what); fails++; }
}

/* stored screen x of one edge at `line` within the walked range */
static uint32_t edge_x(int32_t xa, int32_t dx, int32_t dy, uint32_t skip, uint32_t line, int32_t max_i) {
    uint32_t r = edges_reciprocal_at(dy, max_i);
    uint64_t prod = (uint64_t)(int64_t)dx * r;
    uint32_t xs = (uint32_t)((dx < 0 ? prod + 0xfff : prod) >> 12);
    uint32_t x = (uint32_t)xa << 18;
    x += xs * skip;
    for (uint32_t i = 0; i < line; i++) x += xs;
    return x >> 18;
}

static int contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[256 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != 0;
}

static void find_root(char *out, size_t n) {
    char cwd[480];
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
    for (int up = 0; up < 6; up++) {
        snprintf(out, n, "%s/src/rast/hr.c", cwd);
        if (access(out, R_OK) == 0) { snprintf(out, n, "%s", cwd); return; }
        char *s = strrchr(cwd, '/');
        if (!s || s == cwd) break;
        *s = 0;
    }
    out[0] = 0;
}

int main(void) {
    expect(edges_reciprocal_at(0, 512) == 0, "recip 0");
    expect(edges_reciprocal_at(-4, 65535) == 0, "recip negative");
    expect(edges_reciprocal_at(512, 512) == (0x3fffffffu + 512) / 512, "2x table at 512");
    expect(edges_reciprocal_at(513, 512) == 0, "2x table is 0 past 512");
    expect(edges_reciprocal_at(576, 512) == 0, "the old 3x value");
    expect(edges_reciprocal_at(576, 65535) == (0x3fffffffu + 576) / 576, "3x formula at 576");
    expect(edges_reciprocal_at(576, 65535) != 0, "3x reciprocal is not 0");
    expect(edges_reciprocal_at(65535, 65535) != 0, "longest u16 edge");

    /* (0,0) -> (300,576): a diagonal across the 3x view. DraStic's limit pins every line at x = 0. */
    expect(edge_x(0, 300, 576, 0, 0, 65535) == 0, "top of the edge");
    expect(edge_x(0, 300, 576, 0, 47, 512) == 0, "old code, end of the first bin");
    expect(edge_x(0, 300, 576, 288, 0, 512) == 0, "old code, middle bin");
    expect(edge_x(0, 300, 576, 0, 47, 65535) == 24, "first bin, line 47");
    expect(edge_x(0, 300, 576, 288, 0, 65535) == 149, "middle bin starts near mid-x");
    expect(edge_x(0, 300, 576, 0, 575, 65535) == 299, "last line meets the end vertex");

    char root[512], path[600];
    find_root(root, sizeof root);
    expect(root[0] != 0, "repo root");
    snprintf(path, sizeof path, "%s/src/rast/hr.c", root);
    expect(contains(path, "#define EDGES_RECIP_MAX 65535"), "hr.c raises the reciprocal limit");
    snprintf(path, sizeof path, "%s/src/rast/spec/edges_impl.h", root);
    expect(contains(path, "edges_reciprocal_at(i, EDGES_RECIP_MAX)"), "the walker uses the limit");
    snprintf(path, sizeof path, "%s/src/rast/spec/edges.c", root);
    expect(!contains(path, "EDGES_RECIP_MAX"), "the 2x walker keeps DraStic's 512");

    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    return 0;
}
