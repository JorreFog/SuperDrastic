/* simshim.so: what DraStic (ROCKNIX's Linux aarch64 build) needs to run under qemu-aarch64 user mode on a PC, and
 * diagnostics for that. Preloaded only in the simulator, never on a handheld.
 *   - SDL_JoystickName(NULL): DraStic asks for joystick 0's name whether or not one exists
 *   - remap_file_pages: qemu-user doesn't implement it; done as the kernel does, an mmap of the same file
 *   - a crash handler that names the DraStic function, and long condition-variable waits with their caller */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static unsigned long base;   /* DraStic's load address */
static int cb(struct dl_phdr_info *i, size_t s, void *d) { if (!i->dlpi_name[0] && !base) base = i->dlpi_addr; return 0; }

static void where(const char *what, unsigned long a) {
    Dl_info d; memset(&d, 0, sizeof d); dladdr((void *)a, &d);
    fprintf(stderr, "  %s %#lx exe+%#lx %s %s\n", what, a, a - base, d.dli_fname ? d.dli_fname : "?", d.dli_sname ? d.dli_sname : "");
}
static void on_fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *u = uc_;
    fprintf(stderr, "[sim] signal %d at %p\n", sig, si->si_addr);
    where("pc", u->uc_mcontext.pc); where("lr", u->uc_mcontext.regs[30]);
    unsigned long *fp = (unsigned long *)u->uc_mcontext.regs[29];
    for (int k = 0; k < 12 && fp; k++) { where("ret", fp[1]); fp = (unsigned long *)fp[0]; }
    _exit(128 + sig);
}
__attribute__((constructor)) static void init(void) {
    dl_iterate_phdr(cb, 0);
    fprintf(stderr, "[sim] drastic base %#lx\n", base);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0); sigaction(SIGBUS, &sa, 0); sigaction(SIGILL, &sa, 0);
}

const char *SDL_JoystickName(void *j) {
    static const char *(*real)(void *); if (!real) real = (const char *(*)(void *))dlsym(RTLD_NEXT, "SDL_JoystickName");
    return j ? real(j) : "simulated pad";
}

static struct { unsigned long a, len; int fd; } shmap[64]; static int nshmap;
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
void *mmap(void *a, size_t len, int prot, int fl, int fd, off_t off) {
    if (!real_mmap) real_mmap = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    void *r = real_mmap(a, len, prot, fl, fd, off);
    if (r != MAP_FAILED && fd >= 0 && (fl & MAP_SHARED) && nshmap < 64) {
        shmap[nshmap].a = (unsigned long)r; shmap[nshmap].len = len; shmap[nshmap].fd = dup(fd); nshmap++;
    }
    return r;
}
void *mmap64(void *a, size_t len, int prot, int fl, int fd, off_t off) { return mmap(a, len, prot, fl, fd, off); }
int remap_file_pages(void *addr, size_t size, int prot, size_t pgoff, int flags) {
    unsigned long x = (unsigned long)addr;
    for (int i = nshmap - 1; i >= 0; i--)
        if (x >= shmap[i].a && x + size <= shmap[i].a + shmap[i].len) {
            void *r = real_mmap(addr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shmap[i].fd, (off_t)pgoff * 4096);
            return r == MAP_FAILED ? -1 : 0;
        }
    errno = EINVAL; return -1;
}

static double nowd(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* waits in progress, reported by a watchdog when they pass 3 s */
static struct { volatile double t0; volatile unsigned long ra; } waits[64];
static volatile int nwaits;
static void *watchdog(void *a) {
    for (;;) {
        sleep(5); double n = nowd();
        for (int i = 0; i < nwaits; i++) if (waits[i].t0 > 0 && n - waits[i].t0 > 3)
            fprintf(stderr, "[sim] waiting %.0f s in a cond wait from exe+%#lx\n", n - waits[i].t0, waits[i].ra - base);
    }
    return 0;
}
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    static int (*real)(pthread_cond_t *, pthread_mutex_t *);
    if (!real) real = (int (*)(pthread_cond_t *, pthread_mutex_t *))dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.17");
    unsigned long ra = (unsigned long)__builtin_return_address(0);
    static __thread int slot = -1;
    if (slot < 0) { slot = __atomic_fetch_add(&nwaits, 1, __ATOMIC_RELAXED) % 64;
        static int started; if (!__atomic_exchange_n(&started, 1, __ATOMIC_RELAXED)) { pthread_t t; pthread_create(&t, 0, watchdog, 0); } }
    double t0 = nowd(); waits[slot].ra = ra; waits[slot].t0 = t0;
    int r = real(c, m); double dt = nowd() - t0; waits[slot].t0 = 0;
    if (dt > 1.0) fprintf(stderr, "[sim] cond wait %.1f s from exe+%#lx\n", dt, ra - base);
    return r;
}
