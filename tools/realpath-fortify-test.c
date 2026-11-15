/* glibc's fortified realpath aborts when the resolved-path buffer is smaller than PATH_MAX,
 * before it looks at the path (__realpath_chk). touch_find used 512 bytes, so a build with
 * _FORTIFY_SOURCE (BananaLinux) died in the touch thread at startup and the game never showed a frame.
 *
 *   gcc -O2 -D_FORTIFY_SOURCE=2 -o /tmp/realpath-fortify-test tools/realpath-fortify-test.c
 *   /tmp/realpath-fortify-test
 *
 * Run from the repo root. The PATH_MAX call must succeed. The old 512-byte call must die with SIGABRT.
 * src/dsflip.c's touch_find buffer must be PATH_MAX.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int source_ok(void) {
    FILE *f = fopen("src/dsflip.c", "r");
    if (!f) { fprintf(stderr, "run from the repo root (src/dsflip.c not found)\n"); return 0; }
    char line[800];
    int good = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "real_[512]")) bad = 1;
        if (strstr(line, "real_[PATH_MAX]")) good = 1;
    }
    fclose(f);
    if (bad || !good) {
        fprintf(stderr, "touch_find buffer: PATH_MAX present %d, 512-byte present %d\n", good, bad);
        return 0;
    }
    return 1;
}

/* the buffer touch_find used to pass. object size is visible, so fortify checks it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattribute-warning"
static int short_buf(void) {
    char buf[512];
    return realpath("/proc/self/exe", buf) ? 0 : 2;
}
#pragma GCC diagnostic pop

static int short_aborts(void) {
    pid_t p = fork();
    if (p < 0) return 0;
    if (p == 0) {
        int n = open("/dev/null", O_WRONLY);
        if (n >= 0) dup2(n, 2);
        _exit(short_buf());
    }
    int st = 0;
    if (waitpid(p, &st, 0) < 0) return 0;
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

int main(void) {
    if (!source_ok()) return 1;
    char buf[PATH_MAX];
    if (!realpath("/proc/self/exe", buf)) { perror("realpath PATH_MAX"); return 1; }
    if (!short_aborts()) {
        fprintf(stderr, "a 512-byte realpath buffer did not abort; compile with -O2 -D_FORTIFY_SOURCE=2\n");
        return 1;
    }
    printf("realpath-fortify-test: ok\n");
    return 0;
}
