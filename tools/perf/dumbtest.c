// dumbtest: is a DRM dumb buffer's mapping cached memory? Times sequential writes, reads and read-modify-writes on a
// 512x384x4 dumb buffer against the same on malloc'd memory.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static volatile uint64_t sink;
static void bench(const char *nm, uint32_t *p, size_t n) {
    double t0 = now(); for (int r = 0; r < 20; r++) for (size_t i = 0; i < n; i++) p[i] = (uint32_t)(i + r);
    double t1 = now(); uint64_t s = 0; for (int r = 0; r < 20; r++) for (size_t i = 0; i < n; i++) s += p[i];
    double t2 = now(); for (int r = 0; r < 20; r++) for (size_t i = 0; i < n; i++) p[i] = (p[i] >> 1) & 0x7f7f7f7f;
    double t3 = now(); static uint32_t *src; if (!src) { src = malloc(n * 4); memset(src, 1, n * 4); }
    for (int r = 0; r < 20; r++) memcpy(p, src, n * 4);
    double t4 = now(); sink = s;
    printf("%-8s write %.2f ms  read %.2f ms  rmw %.2f ms  memcpy-in %.2f ms   (per 786 KB pass)\n", nm, (t1 - t0) / 20, (t2 - t1) / 20, (t3 - t2) / 20, (t4 - t3) / 20);
}
int main(int argc, char **argv) {
    size_t n = 512 * 384;
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct drm_mode_create_dumb c = { .width = 512, .height = 384, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) { perror("create_dumb"); return 1; }
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { perror("map_dumb"); return 1; }
    uint32_t *d = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if (d == MAP_FAILED) { perror("mmap"); return 1; }
    uint32_t *h = aligned_alloc(64, n * 4); memset(h, 0, n * 4); memset(d, 0, n * 4);
    for (int k = 0; k < 2; k++) { bench("malloc", h, n); bench("dumb", d, n); }
    return 0;
}
