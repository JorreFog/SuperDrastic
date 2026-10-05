// memprobe: LD_PRELOAD ahead of the library; counts memcpy/memmove/memset calls of >= MEMPROBE_MIN bytes (default
// 4096) per caller and prints the table to stderr every 10 s (callers as offsets into their mapping).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
static void *(*r_memcpy)(void *, const void *, size_t), *(*r_memmove)(void *, const void *, size_t), *(*r_memset)(void *, int, size_t);
static size_t minlen = 4096;
static struct ent { uintptr_t ra; unsigned long n, bytes; char kind; long tid; } tab[256];
static int ntab; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static time_t last;
static __thread int busy;
static void report(void) {
    fprintf(stderr, "[memprobe] ---- 10 s\n");
    for (int i = 0; i < ntab; i++) {
        if (!tab[i].n) continue;
        Dl_info di; const char *nm = "?"; uintptr_t off = tab[i].ra;
        if (dladdr((void *)tab[i].ra, &di) && di.dli_fname) { nm = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname; off = tab[i].ra - (uintptr_t)di.dli_fbase; }
        fprintf(stderr, "[memprobe] %c tid %ld %s+0x%lx (%s): %lu calls, %.1f MB, avg %lu\n", tab[i].kind, tab[i].tid, nm, (unsigned long)off,
                di.dli_sname ? di.dli_sname : "", tab[i].n, tab[i].bytes / 1048576.0, tab[i].bytes / tab[i].n);
        tab[i].n = tab[i].bytes = 0;
    }
}
static void note(char kind, uintptr_t ra, size_t n) {
    if (busy) return; busy = 1;
    long tid = (long)pthread_self();
    pthread_mutex_lock(&mu);
    int i; for (i = 0; i < ntab; i++) if (tab[i].ra == ra && tab[i].kind == kind && tab[i].tid == tid) break;
    if (i == ntab && ntab < 256) { tab[ntab].ra = ra; tab[ntab].kind = kind; tab[ntab].tid = tid; ntab++; }
    if (i < 256) { tab[i].n++; tab[i].bytes += n; }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    if (!last) last = ts.tv_sec;
    if (ts.tv_sec - last >= 10) { last = ts.tv_sec; report(); }
    pthread_mutex_unlock(&mu);
    busy = 0;
}
static void init(void) {
    static int done; if (done) return; done = 1;
    r_memcpy = dlsym(RTLD_NEXT, "memcpy"); r_memmove = dlsym(RTLD_NEXT, "memmove"); r_memset = dlsym(RTLD_NEXT, "memset");
    const char *e = getenv("MEMPROBE_MIN"); if (e) minlen = strtoul(e, 0, 0);
}
void *memcpy(void *d, const void *s, size_t n) {
    if (!r_memcpy) { if (busy) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; } busy = 1; init(); busy = 0; }
    if (n >= minlen) note('c', (uintptr_t)__builtin_return_address(0), n);
    return r_memcpy(d, s, n);
}
void *memmove(void *d, const void *s, size_t n) {
    if (!r_memmove) { if (busy) { char *a = d; const char *b = s; if (a < b) while (n--) *a++ = *b++; else while (n--) a[n] = b[n]; return d; } busy = 1; init(); busy = 0; }
    if (n >= minlen) note('m', (uintptr_t)__builtin_return_address(0), n);
    return r_memmove(d, s, n);
}
void *memset(void *d, int c, size_t n) {
    if (!r_memset) { if (busy) { char *a = d; while (n--) *a++ = (char)c; return d; } busy = 1; init(); busy = 0; }
    if (n >= minlen) note('s', (uintptr_t)__builtin_return_address(0), n);
    return r_memset(d, c, n);
}
