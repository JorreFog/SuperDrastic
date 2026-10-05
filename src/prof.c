/* prof.c: a sampling profiler built into the library, for the handhelds (there is no perf on them).
 *
 * DSFLIP_PROF=1 (or =<seconds between reports>, default 10) samples where each thread of the process spends its CPU
 * time. Every thread gets its own CPU-time timer (timer_create on CLOCK_THREAD_CPUTIME_ID, delivered with
 * SIGEV_THREAD_ID): after each 1/DSFLIP_PROF_HZ s (default 1000 Hz) of that thread's own CPU it is sent SIGPROF, and
 * the handler counts the interrupted PC under that thread. The kernel checks CPU timers at its tick, so a thread gets
 * at most CONFIG_HZ samples a second of CPU; the report gives each thread's rate. A process-wide ITIMER_PROF would
 * need one timer only, but the kernel hands its SIGPROF to another thread whenever the running one blocks SIGPROF
 * (SDL's threads all do), which then counts a sleeping thread's PC and wakes it early: measured under qemu, a
 * SIGPROF-blocking busy thread's whole CPU time showed up as the report thread's clock_nanosleep. A thread's own timer
 * only ever interrupts that thread, while it runs. ITIMER_PROF remains the fallback if timer_create fails.
 *
 * The handler is async-signal-safe (no allocation, no locks, a few dozen instructions): it adds 1 to a fixed
 * open-addressing table keyed by (the instruction's address, the thread's slot). Instructions, not 16-byte blocks:
 * 159 of DraStic's 2481 functions don't start on a 16-byte boundary (its JIT's arm64_load_* / arm64_store_* memory
 * handlers among them), and a block would give their first instructions to the function before. DraStic's JIT code
 * is keyed by its page instead: it lives in DraStic's .bss (the translation cache in nds_system, made rwx), and over a
 * long session the translated blocks move all over its 19 MB. The handler runs on an alternate signal stack, so the
 * interrupted code's own stack (the JIT's too) is never written below its stack pointer. A sample costs what the
 * kernel's signal delivery costs: raise(SIGPROF) in a loop took 1.51-1.58 us with this handler and 1.49-1.58 with an
 * empty one (x86 host, natively, the syscall included), and a busy loop ran as fast with the profiler as without it
 * (within the 0.5% noise). At a 250-1000 Hz tick that is 0.04-0.15% of a busy thread, a few times that on a
 * Cortex-A55. Time in the kernel counts at the instruction that made the system call (the sample is taken on the way
 * back to user space).
 *
 * Threads are seen starting through pthread_create (interposed): the new thread starts its timer and alternate stack,
 * and a thread-specific key's destructor stops them when it exits; a later thread with the same start routine reuses
 * an exited one's slot (a dsf-http request, a save). DraStic doesn't name its threads, so they are named after their
 * start routine (video_render_thread); the others by their name (dsf-*, rast-3d, SDL's). The main thread is DraStic's
 * emulation thread: the JIT, engine A's 2D (render_scanline_*), the geometry engine, one of the 3D bin groups.
 *
 * The report, prof-<pid>.txt in DSFLIP_PROF_OUT (default: the directory of DSFLIP_LOG, else /tmp), is rewritten every
 * period (.tmp + rename: always a whole one), at exit, on SIGTERM and before the SIGKILL of a quit-with-save
 * (resume.c), so the last one survives the kill -9 the exit hotkey ends a game with. It lists the threads (samples,
 * CPU time, % of a core), the modules (DraStic, its JIT, this library, libc ...), the top functions of all threads and
 * of each busy thread. Functions come from the ELF symbol tables of the loaded objects: .symtab where present (DraStic
 * and this library keep theirs), else .dynsym with the IFUNCs (libc's memcpy ...) named at their resolved
 * implementation; a stripped library's other functions come from its .eh_frame_hdr as <library>+0x<offset>. DraStic's
 * are read at init; the others in the report thread, the first time a report finds samples in that object (a report
 * from a signal handler reads no files and allocates nothing: what isn't read yet is named "symbols not read yet").
 *
 * DraStic r2.5.2.2 never sets a timer and never changes a signal disposition (its only signal() calls are in unrar's
 * ErrorHandler::SetSignalHandlers, which nothing calls), so SIGPROF and its timers are ours. SIGTERM is SDL's:
 * SDL_Init installs a handler that turns it into a quit event, but only while the disposition is still the default,
 * and the handler puts itself back with signal() each time it runs. So ours is installed after SDL's (the report
 * thread looks once a second), writes a report and then calls SDL's. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef DSFLIP_VERSION
#define DSFLIP_VERSION "dev"
#endif
void dsflip_log(const char *fmt, ...) __attribute__((weak));   /* dsflip.c; absent in the rasterizer's test build */

enum {
    TAB_BITS = 18, TAB = 1 << TAB_BITS, PROBES = 32,     /* samples: (pc >> 2, slot) -> count; 4 MB */
    SLOT_BITS = 8, MAXSLOT = (1 << SLOT_BITS) - 1,        /* thread slots 1..MAXSLOT-1; MAXSLOT collects the rest */
    AGG_BITS = 16, AGG = 1 << AGG_BITS,                    /* the report's (function, slot) -> count */
    MAXMOD = 128, MAXREG = 64, TOPN = 60, TOPT = 25, ALTSTACK = 64 << 10,
};
/* a function's code: module << 32 | index. Modules 1..MAXMOD are the loaded objects, then these: */
enum { M_REG = 0x7f00 /* + an executable mapping outside them */, M_JIT = 0x7ff0, M_UNKNOWN = 0x7fff };
enum { R_PERIODIC, R_EXIT, R_SIGTERM, R_QUIT };
static const char *const why_name[] = { "periodic", "exit", "SIGTERM", "quit" };

static int enabled, use_itimer, period = 10, hz = 1000, prof_pid, timer_fail, report_ok = -1;
static long interval_ns;
static long long t_init;
static char outpath[320], proc_comm[17];

static long long mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static inline uint64_t hash(uint64_t k, int bits) { return (k * 0x9E3779B97F4A7C15ull) >> (64 - bits); }

/* ---- threads ---- */
static struct slot {
    int tid, live, nthreads;
    void *(*start)(void *);           /* its pthread_create start routine (0: the main thread, or one we didn't see) */
    unsigned long samples;
    long long cpu_done, t0;           /* CPU ns of its threads that have exited; when its first one started */
    int blocked;                      /* an exited thread of it had SIGPROF blocked (it was never sampled) */
    char name[24];                    /* the thread's name when it isn't the process's: read while it ran, or at exit */
} slots[MAXSLOT + 1];
static int nslots;                    /* slots 1..nslots are taken (it runs past MAXSLOT: those share MAXSLOT) */

/* a slot for a starting thread; one whose thread has exited is reused by a thread with the same start routine */
static int slot_new(void *(*fn)(void *), int tid, int reuse) {
    int n = __atomic_load_n(&nslots, __ATOMIC_ACQUIRE);
    for (int s = 2; reuse && fn && s <= n && s < MAXSLOT; s++) {
        int z = 0;
        if (slots[s].start == fn && __atomic_compare_exchange_n(&slots[s].live, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            slots[s].tid = tid; slots[s].nthreads++;
            return s;
        }
    }
    int s = __atomic_add_fetch(&nslots, 1, __ATOMIC_ACQ_REL);
    if (s >= MAXSLOT) { s = MAXSLOT; slots[s].tid = -1; if (!slots[s].t0) slots[s].t0 = mono_ns(); __atomic_fetch_add(&slots[s].nthreads, 1, __ATOMIC_RELAXED); return s; }
    __atomic_store_n(&slots[s].live, 1, __ATOMIC_RELEASE);   /* first: a reuse search must not take it half made */
    slots[s].tid = tid; slots[s].nthreads = 1; slots[s].t0 = mono_ns();
    __atomic_store_n(&slots[s].start, fn, __ATOMIC_RELEASE);
    return s;
}

/* ---- the sample table (written by the signal handler) ---- */
struct ent { uint64_t key; uint32_t n, pad; };        /* key: (pc >> 2) << SLOT_BITS | slot; 0 = empty */
static struct ent *tab;
static uintptr_t jit_lo, jit_hi;                      /* DraStic's writable segment: its JIT translation cache */
static __thread int my_slot __attribute__((tls_model("initial-exec")));
static unsigned long dropped;
static uintptr_t key_pc(uint64_t k) { return (uintptr_t)(k >> SLOT_BITS) << 2; }

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    int err = errno;
#if defined(__aarch64__)
    uintptr_t pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.pc;
#else                                                  /* a host build of the overhead test */
    uintptr_t pc = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
#endif
    int s = my_slot;
    if (!s) s = my_slot = slot_new(0, (int)syscall(SYS_gettid), 0);   /* a thread we didn't see start (ITIMER_PROF) */
    __atomic_fetch_add(&slots[s].samples, 1, __ATOMIC_RELAXED);
    if (pc - jit_lo < jit_hi - jit_lo) pc &= ~(uintptr_t)0xfff;        /* DraStic's JIT code: by page */
    uint64_t key = (uint64_t)(pc >> 2) << SLOT_BITS | (unsigned)s, h = hash(key, TAB_BITS);
    for (int i = 0; i < PROBES; i++, h = (h + 1) & (TAB - 1)) {
        uint64_t k = __atomic_load_n(&tab[h].key, __ATOMIC_RELAXED);
        if (!k && __atomic_compare_exchange_n(&tab[h].key, &k, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) k = key;
        if (k == key) { __atomic_fetch_add(&tab[h].n, 1, __ATOMIC_RELAXED); errno = err; return; }
    }
    __atomic_fetch_add(&dropped, 1, __ATOMIC_RELAXED);
    errno = err;
}

/* ---- a thread's timer and alternate stack, from its start to its exit ---- */
static pthread_key_t exit_key;
static __thread timer_t my_timer __attribute__((tls_model("initial-exec")));
static __thread int my_timer_on __attribute__((tls_model("initial-exec")));
static __thread void *my_alt __attribute__((tls_model("initial-exec")));

static void sampling_on(int tid) {
    stack_t old;
    if (!sigaltstack(0, &old) && (old.ss_flags & SS_DISABLE)) {
        void *a = mmap(0, ALTSTACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        stack_t st = { .ss_sp = a, .ss_size = ALTSTACK, .ss_flags = 0 };
        if (a != MAP_FAILED) { if (sigaltstack(&st, 0)) munmap(a, ALTSTACK); else my_alt = a; }
    }
    if (use_itimer) return;
    struct sigevent ev; memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_THREAD_ID; ev.sigev_signo = SIGPROF; ev._sigev_un._tid = tid;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &my_timer)) { __atomic_fetch_add(&timer_fail, 1, __ATOMIC_RELAXED); return; }
    struct itimerspec it = { { 0, interval_ns }, { 0, interval_ns } };
    timer_settime(my_timer, 0, &it, 0);
    my_timer_on = 1;
}
static void thread_exit(void *v) {    /* exit_key's destructor: the thread is ending */
    struct slot *sl = v;
    if (my_timer_on) { timer_delete(my_timer); my_timer_on = 0; }
    struct timespec c; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c);
    __atomic_fetch_add(&sl->cpu_done, c.tv_sec * 1000000000LL + c.tv_nsec, __ATOMIC_RELAXED);   /* MAXSLOT's: many at once */
    if (sl != &slots[MAXSLOT]) {
        char nm[17] = ""; prctl(PR_GET_NAME, nm);
        if (nm[0] && strcmp(nm, proc_comm)) memcpy(sl->name, nm, sizeof nm);
    }
    /* the alternate stack goes last, with SIGPROF blocked: a sample already on its way must not land in freed memory */
    sigset_t m, was; sigemptyset(&m); sigaddset(&m, SIGPROF); pthread_sigmask(SIG_BLOCK, &m, &was);
    if (sl != &slots[MAXSLOT] && sigismember(&was, SIGPROF)) sl->blocked = 1;
    if (my_alt) { stack_t st = { .ss_sp = 0, .ss_size = 0, .ss_flags = SS_DISABLE }; sigaltstack(&st, 0); munmap(my_alt, ALTSTACK); my_alt = 0; }
    my_slot = 0;
    if (sl != &slots[MAXSLOT]) __atomic_store_n(&sl->live, 0, __ATOMIC_RELEASE);
}

/* pthread_create, interposed: the new thread starts in tramp_start, which sets up its sampling, then runs fn. With
   the profiler off it only forwards the call. */
struct tramp { void *(*fn)(void *); void *arg; };
static void *tramp_start(void *p) {
    struct tramp t = *(struct tramp *)p; free(p);
    int tid = (int)syscall(SYS_gettid), s = slot_new(t.fn, tid, 1);
    my_slot = s;
    sampling_on(tid);
    pthread_setspecific(exit_key, &slots[s]);
    return t.fn(t.arg);
}
int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    typedef int (*create_fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    static create_fn real;
    create_fn r = __atomic_load_n(&real, __ATOMIC_ACQUIRE);
    if (!r) { r = (create_fn)dlsym(RTLD_NEXT, "pthread_create"); if (!r) return EAGAIN; __atomic_store_n(&real, r, __ATOMIC_RELEASE); }
    if (!enabled) return r(th, attr, fn, arg);
    struct tramp *t = malloc(sizeof *t);
    if (!t) return r(th, attr, fn, arg);
    t->fn = fn; t->arg = arg;
    int e = r(th, attr, tramp_start, t);
    if (e) free(t);
    return e;
}

/* ---- the loaded objects and their symbols (the report's; touched only at init and while holding report_busy) ---- */
struct sym { uintptr_t addr; uint32_t size, name; uint8_t func; };
static struct mod {
    char name[48], path[256];
    uintptr_t base, lo, hi;                 /* load bias; the span of its PT_LOAD segments */
    uintptr_t xlo[4], xhi[4]; int nx;       /* its executable segments */
    const uint8_t *ehdr;                    /* its .eh_frame_hdr (PT_GNU_EH_FRAME), in memory */
    const Elf64_Dyn *dyn;                   /* its dynamic section (PT_DYNAMIC), in memory: the vdso's symbols */
    int loaded, nsyms, nfde, vdso;          /* loaded: the symbols were read (on demand) */
    struct sym *syms; char *strs; uint32_t *fde;   /* fde: function starts (offsets from lo), from .eh_frame_hdr */
} mods[MAXMOD];
static int nmods;
/* DraStic's translation cache: initialize_system calls initialize_translation_cache(&nds_system + 0x8c000) (its
   `add x0, x19, #0x8c, lsl #12` at +0xc4; x19 = &nds_system, from the GOT), and translation_cache_flush_main/itcm/
   alternate give its three parts: 16 MB, 1 MB, 2 MB (initialize_cpu points both CPUs at it). 0 until DraStic's
   symbols are read and that word checked. */
static uintptr_t tcache;
static const char *const jit_names[] = { "jit: main translation cache", "jit: itcm translation cache", "jit: alternate translation cache",
                                         "jit (DraStic's data, outside the translation cache)" };
static struct reg { uintptr_t lo, hi; char name[24]; } regs[MAXREG];   /* executable mappings outside the objects */
static int nregs;

static int in_exec(const struct mod *m, uintptr_t a) { for (int i = 0; i < m->nx; i++) if (a >= m->xlo[i] && a < m->xhi[i]) return 1; return 0; }
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int sym_cmp(const void *a, const void *b) {
    uintptr_t x = ((const struct sym *)a)->addr, y = ((const struct sym *)b)->addr;
    return x < y ? -1 : x > y;
}

/* an encoded .eh_frame_hdr value: the encodings GNU ld and lld write */
static int eh_read(const uint8_t **p, int enc, const uint8_t *hdr, uintptr_t *v) {
    int32_t s; uint32_t u;
    switch (enc & 0x0f) {
    case 0x03: memcpy(&u, *p, 4); *v = u; *p += 4; break;
    case 0x0b: memcpy(&s, *p, 4); *v = (uintptr_t)(intptr_t)s; *p += 4; break;
    default: return 0;
    }
    if ((enc & 0x70) == 0x10) *v += (uintptr_t)(*p - 4);          /* pcrel */
    else if ((enc & 0x70) == 0x30) *v += (uintptr_t)hdr;           /* datarel: from the header's start */
    else if (enc & 0x70) return 0;
    return 1;
}
/* the function starts in an object's .eh_frame_hdr. Read in memory, so only inside dl_iterate_phdr (it holds the
   loader's lock) and only while the object is still loaded: one that was sampled and then dlclose'd (an SDL driver
   whose probe failed) is no longer mapped by the time a report gets to it. */
static int fdes_cb(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz;
    struct mod *m = data; uintptr_t lo = ~(uintptr_t)0; const uint8_t *h = 0;
    if (info->dlpi_addr != m->base) return 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr *ph = &info->dlpi_phdr[i]; uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_GNU_EH_FRAME) h = (const uint8_t *)a;
        if (ph->p_type == PT_LOAD && a < lo) lo = a;
    }
    if (lo != m->lo) return 0;                                      /* another object at that base */
    const uint8_t *p = h + 4; uintptr_t v, n;
    if (!h || h[0] != 1 || !eh_read(&p, h[1], h, &v) || !eh_read(&p, h[2], h, &n)) return 1;
    if (h[3] != 0x3b || !n || n > (1 << 20)) return 1;             /* the table: datarel sdata4 pairs */
    uint32_t *f = malloc(n * sizeof *f); if (!f) return 1;
    for (uintptr_t i = 0; i < n; i++) { int32_t loc; memcpy(&loc, p + 8 * i, 4); f[i] = (uint32_t)((uintptr_t)h + loc - m->lo); }
    m->fde = f; m->nfde = (int)n;
    return 1;
}
static void load_fdes(struct mod *m) { if (m->ehdr) dl_iterate_phdr(fdes_cb, m); }

/* the ELF symbols of an object (.symtab, else .dynsym), function-like ones inside its executable segments */
static void load_syms(struct mod *m) {
    m->loaded = 1;
    load_fdes(m);
    const Elf64_Sym *sy = 0; const char *str = 0, *shs = 0; size_t n = 0, strsz = 0, shsz = 0;
    const Elf64_Shdr *sh = 0; int shnum = 0;
    uint8_t *f = 0; size_t fsz = 0;
    int fd = open(m->path, O_RDONLY | O_CLOEXEC); struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && st.st_size >= (off_t)sizeof(Elf64_Ehdr)) {
        f = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); fsz = (size_t)st.st_size;
        if (f == MAP_FAILED) f = 0;
    }
    if (fd >= 0) close(fd);
    if (f) {
        const Elf64_Ehdr *eh = (const Elf64_Ehdr *)f;
        if (!memcmp(eh->e_ident, ELFMAG, SELFMAG) && eh->e_ident[EI_CLASS] == ELFCLASS64 && eh->e_shentsize == sizeof(Elf64_Shdr) &&
            eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) <= fsz) {
            sh = (const Elf64_Shdr *)(f + eh->e_shoff); shnum = eh->e_shnum;
            const Elf64_Shdr *tabs = 0, *dyn = 0;
            for (int i = 0; i < shnum; i++) { if (sh[i].sh_type == SHT_SYMTAB) tabs = &sh[i]; if (sh[i].sh_type == SHT_DYNSYM) dyn = &sh[i]; }
            const Elf64_Shdr *symtab = tabs ? tabs : dyn;
            if (symtab && (int)symtab->sh_link < shnum && symtab->sh_offset + symtab->sh_size <= fsz &&
                sh[symtab->sh_link].sh_offset + sh[symtab->sh_link].sh_size <= fsz) {
                sy = (const Elf64_Sym *)(f + symtab->sh_offset); n = symtab->sh_size / sizeof(Elf64_Sym);
                str = (const char *)(f + sh[symtab->sh_link].sh_offset); strsz = sh[symtab->sh_link].sh_size;
            }
            if (eh->e_shstrndx < shnum && sh[eh->e_shstrndx].sh_offset + sh[eh->e_shstrndx].sh_size <= fsz) {
                shs = (const char *)(f + sh[eh->e_shstrndx].sh_offset); shsz = sh[eh->e_shstrndx].sh_size;
            }
        }
    } else if (m->vdso) {   /* no file: its dynamic symbols in memory, through PT_DYNAMIC (DT_HASH counts them) */
        for (const Elf64_Dyn *d = m->dyn; d && d->d_tag != DT_NULL; d++) {
            uintptr_t a = m->base + d->d_un.d_ptr;
            if (d->d_tag == DT_SYMTAB) sy = (const Elf64_Sym *)a;
            else if (d->d_tag == DT_STRTAB) str = (const char *)a;
            else if (d->d_tag == DT_STRSZ) strsz = d->d_un.d_val;
            else if (d->d_tag == DT_HASH && a >= m->lo && a + 8 <= m->hi) n = ((const uint32_t *)a)[1];
        }
        if (n > 4096 || (uintptr_t)sy < m->lo || (uintptr_t)(sy + n) > m->hi || (uintptr_t)str < m->lo || (uintptr_t)str + strsz > m->hi) sy = 0;
    }
    /* the names are copied one by one: more than the string table's size when names share a tail (memcpy, wmemcpy) */
    size_t cap = strsz + 4096, pn = 0; int k = 0;
    struct sym *out = sy ? malloc((n + 4) * sizeof *out) : 0; char *pool = sy ? malloc(cap) : 0;
    if (!out || !pool) { free(out); free(pool); if (f) munmap(f, fsz); return; }
    uintptr_t nds = 0, init_sys = 0;
    memcpy(pool, "<plt>", 6); pn = 6;
    for (int i = 0; shs && i < shnum && k < 3; i++) {   /* PLT stubs have no symbols: they'd count as the one before
                                                            them (_init in DraStic, _fini in this library) */
        const char *sn = sh[i].sh_name < shsz ? shs + sh[i].sh_name : "";
        if ((strcmp(sn, ".plt") && strcmp(sn, ".plt.sec") && strcmp(sn, ".iplt")) || !in_exec(m, m->base + sh[i].sh_addr)) continue;
        out[k].addr = m->base + sh[i].sh_addr; out[k].size = (uint32_t)sh[i].sh_size; out[k].name = 0; out[k].func = 1; k++;
    }
    for (size_t i = 1; i < n; i++) {
        int type = ELF64_ST_TYPE(sy[i].st_info);
        if (sy[i].st_shndx == SHN_UNDEF || sy[i].st_shndx == SHN_ABS || !sy[i].st_value || sy[i].st_name >= strsz) continue;
        const char *nm = str + sy[i].st_name; size_t l = strnlen(nm, strsz - sy[i].st_name);
        if (!l || (nm[0] == '.' && nm[1] == 'L') || nm[0] == '$') continue;      /* assembler locals, mapping symbols */
        uintptr_t a = m->base + sy[i].st_value; uint32_t size = (uint32_t)sy[i].st_size;
        if (m == &mods[0] && type == STT_OBJECT && !strcmp(nm, "nds_system") && sy[i].st_size >= 0x8c000 + (19 << 20)) nds = a;
        if (m == &mods[0] && type == STT_FUNC && !strcmp(nm, "initialize_system") && size >= 0xc8) init_sys = a;
        if (type == STT_GNU_IFUNC) {  /* the symbol is the resolver; name the implementation it picked instead */
            a = (uintptr_t)dlsym(RTLD_DEFAULT, nm); size = 0;
            if (!in_exec(m, a)) continue;
        } else if ((type != STT_FUNC && type != STT_NOTYPE) || !in_exec(m, a)) continue;
        if (pn + l + 1 > cap) { char *g = realloc(pool, cap * 2); if (!g) break; pool = g; cap *= 2; }
        memcpy(pool + pn, nm, l); pool[pn + l] = 0;
        out[k].addr = a; out[k].size = size; out[k].name = (uint32_t)pn; out[k].func = type != STT_NOTYPE; k++;
        pn += l + 1;
    }
    if (f) munmap(f, fsz);
    qsort(out, k, sizeof *out, sym_cmp);
    m->syms = out; m->nsyms = k; m->strs = pool;
    if (nds && init_sys && in_exec(m, init_sys + 0xc4) && *(const uint32_t *)(init_sys + 0xc4) == 0x91423260u) tcache = nds + 0x8c000;
}

static int add_module(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz; (void)data;
    uintptr_t lo = ~(uintptr_t)0, hi = 0, xlo[4], xhi[4]; int nx = 0; const uint8_t *eh = 0; const Elf64_Dyn *dyn = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const Elf64_Phdr *ph = &info->dlpi_phdr[i]; uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_GNU_EH_FRAME) eh = (const uint8_t *)a;
        if (ph->p_type == PT_DYNAMIC) dyn = (const Elf64_Dyn *)a;
        if (ph->p_type != PT_LOAD) continue;
        if (a < lo) lo = a;
        if (a + ph->p_memsz > hi) hi = a + ph->p_memsz;
        if ((ph->p_flags & PF_X) && nx < 4) { xlo[nx] = a; xhi[nx] = a + ph->p_memsz; nx++; }
    }
    if (!nx) return 0;
    for (int i = 0; i < nmods; i++) if (mods[i].base == info->dlpi_addr && mods[i].lo == lo) return 0;   /* known */
    if (nmods >= MAXMOD) return 0;
    struct mod *m = &mods[nmods]; memset(m, 0, sizeof *m);
    m->base = info->dlpi_addr; m->lo = lo; m->hi = hi; m->nx = nx; m->ehdr = eh; m->dyn = dyn;
    m->vdso = lo == (uintptr_t)getauxval(AT_SYSINFO_EHDR);   /* the only object read in memory: it's never unloaded */
    memcpy(m->xlo, xlo, sizeof xlo); memcpy(m->xhi, xhi, sizeof xhi);
    if (info->dlpi_name && info->dlpi_name[0]) snprintf(m->path, sizeof m->path, "%s", info->dlpi_name);
    else if (!nmods) {   /* the program: /proc/self/exe (qemu-user emulates it too) */
        strcpy(m->path, "/proc/self/exe");
        char p[256]; ssize_t l = readlink("/proc/self/exe", p, sizeof p - 1);
        if (l > 0) { p[l] = 0; snprintf(m->name, sizeof m->name, "%s", base_name(p)); }
    }
    if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", m->path[0] ? base_name(m->path) : "?");
    nmods++;
    return 0;
}

/* the executable mappings that are none of the objects nor DraStic's JIT: by file name, else "anonymous" (this
   library's hook trampolines are one anonymous rwx page) */
static uintptr_t hexval(const char **p) {
    uintptr_t v = 0;
    for (;; (*p)++) { char c = **p; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (d < 0) return v; v = v * 16 + d; }
}
static void reg_line(const char *l) {
    const char *q = l; uintptr_t lo = hexval(&q); if (*q != '-') return;
    q++; uintptr_t hi = hexval(&q); if (*q != ' ' || q[3] != 'x' || nregs >= MAXREG) return;
    int w = q[2] == 'w';
    for (int i = 0; i < nmods; i++) if (lo < mods[i].hi && hi > mods[i].lo) return;
    if (lo < jit_hi && hi > jit_lo) return;
    for (int f = 0; f < 5 && *q; ) { if (*q == ' ') { f++; while (*q == ' ') q++; } else q++; }   /* perms .. inode */
    struct reg *r = &regs[nregs++]; r->lo = lo; r->hi = hi;
    snprintf(r->name, sizeof r->name, "%s", *q ? base_name(q) : w ? "anonymous rwx" : "anonymous r-x");
}
static void scan_regions(void) {      /* async-signal-safe: read(2) into a stack buffer, line by line */
    nregs = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC); if (fd < 0) return;
    char b[4096]; int have = 0;
    for (;;) {
        int r = (int)read(fd, b + have, sizeof b - 1 - have);
        if (r > 0) have += r;
        int start = 0;
        for (int i = 0; i < have; i++) if (b[i] == '\n') { b[i] = 0; reg_line(b + start); start = i + 1; }
        if (r <= 0) { if (start < have) { b[have] = 0; reg_line(b + start); } break; }
        if (!start && have == (int)sizeof b - 1) start = have;          /* a line longer than the buffer: dropped */
        memmove(b, b + start, have - start); have -= start;
    }
    close(fd);
}

/* the part of DraStic's translation cache pc is in (jit_names) */
static int jit_part(uintptr_t pc) {
    if (!tcache || pc < tcache) return 3;
    uintptr_t o = pc - tcache;
    return o < (16 << 20) ? 0 : o < (17 << 20) ? 1 : o < (19 << 20) ? 2 : 3;
}
/* the loaded object pc is in (1-based), else 0 */
static int mod_at(uintptr_t pc) {
    for (int i = 0; i < nmods; i++) if (pc >= mods[i].lo && pc < mods[i].hi) return i + 1;
    return 0;
}
/* the function at pc: its module << 32 | symbol index + 1, 0x80000000 | an FDE without a symbol, or 0 */
static uint64_t code_of(uintptr_t pc) {
    if (pc - jit_lo < jit_hi - jit_lo) return (uint64_t)M_JIT << 32 | (uint32_t)jit_part(pc);
    for (int i = 0; i < nmods; i++) {
        struct mod *m = &mods[i];
        if (pc < m->lo || pc >= m->hi) continue;
        if (!in_exec(m, pc)) return (uint64_t)(i + 1) << 32 | 0x7fffffffu;  /* code run from its data: generated */
        int lo = 0, hi = m->nsyms;                                         /* the last symbol at or before pc */
        while (lo < hi) { int mid = (lo + hi) / 2; if (m->syms[mid].addr <= pc) lo = mid + 1; else hi = mid; }
        int k = lo - 1;
        if (k >= 0 && !m->syms[k].func)        /* a label: inside a sized function (a C one), that function */
            for (int j = k - 1; j >= 0 && j >= k - 8; j--) if (m->syms[j].func && pc < m->syms[j].addr + m->syms[j].size) { k = j; break; }
        /* a sized symbol covers its own bytes only; DraStic's assembly (size 0) runs to the next symbol, or to the next
           function start the unwind table knows (a function without a symbol: a stripped library's static ones) */
        int covers = k >= 0 && m->syms[k].size && pc < m->syms[k].addr + m->syms[k].size;
        if (k >= 0 && m->syms[k].size && !covers) k = -1;
        lo = 0; hi = covers ? 0 : m->nfde;                                 /* the last function start at or before pc */
        while (lo < hi) { int mid = (lo + hi) / 2; if (m->lo + m->fde[mid] <= pc) lo = mid + 1; else hi = mid; }
        if (lo > 0 && (k < 0 || m->lo + m->fde[lo - 1] > m->syms[k].addr)) return (uint64_t)(i + 1) << 32 | 0x80000000u | (uint32_t)(lo - 1);
        return (uint64_t)(i + 1) << 32 | (uint32_t)(k + 1);
    }
    for (int r = 0; r < nregs; r++) if (pc >= regs[r].lo && pc < regs[r].hi) return (uint64_t)(M_REG + r) << 32;
    return (uint64_t)M_UNKNOWN << 32;
}
static int mod_of(uint64_t code) { return (int)(code >> 32); }
static const char *mod_name(int m) {
    if (m >= 1 && m <= nmods) return mods[m - 1].name;
    if (m == M_JIT) return "jit";
    if (m >= M_REG && m < M_REG + nregs) return regs[m - M_REG].name;
    return "?";
}
static void cat_s(char *out, size_t n, const char *s) { size_t l = strlen(out); while (l < n - 1 && *s) out[l++] = *s++; out[l] = 0; }
static void cat_x(char *out, size_t n, uintptr_t v) { char b[20]; int i = 19; b[i] = 0; do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); cat_s(out, n, "0x"); cat_s(out, n, b + i); }
static void cat_u(char *out, size_t n, unsigned long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); cat_s(out, n, b + i); }
static const char *fn_name(uint64_t code, char *tmp, size_t n) {
    int m = mod_of(code); uint32_t s = (uint32_t)code;
    tmp[0] = 0;
    if (m == M_JIT) return jit_names[s & 3];
    if (m < 1 || m > nmods) return mod_name(m);
    struct mod *md = &mods[m - 1];
    if (s == 0x7fffffffu) { cat_s(tmp, n, md->name); cat_s(tmp, n, " (code in its writable data)"); return tmp; }
    if (s & 0x80000000u) { cat_s(tmp, n, md->name); cat_s(tmp, n, "+"); cat_x(tmp, n, md->lo - md->base + md->fde[s & 0x7fffffff]); return tmp; }
    if (s) return md->strs + md->syms[s - 1].name;
    cat_s(tmp, n, md->name); cat_s(tmp, n, md->loaded ? " (no symbol)" : " (symbols not read yet)");
    return tmp;
}

/* ---- the report (async-signal-safe unless `full`: a static buffer, write(2), no libc formatting) ---- */
static struct agg { uint64_t key, n; } *agg;          /* key: code << SLOT_BITS | slot (slot 0: every thread) */
static int order[AGG];
static unsigned long agg_lost;
static char ob[1 << 15]; static int on, ofd;

static void flush(void) { int w = 0; while (w < on) { int r = (int)write(ofd, ob + w, on - w); if (r <= 0) break; w += r; } on = 0; }
static void out_s(const char *s) { for (; *s; s++) { if (on == (int)sizeof ob) flush(); ob[on++] = *s; } }
static void out_pad(const char *s, int w) { out_s(s); for (int l = (int)strlen(s); l < w; l++) out_s(" "); }
static void out_u(unsigned long long v, int w) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); for (int l = 23 - i; l < w; l++) out_s(" "); out_s(b + i); }
static void out_fix(unsigned long long v, int d, int w) {   /* v / 10^d with d decimals */
    unsigned long long p = 1; for (int i = 0; i < d; i++) p *= 10;
    char b[32] = "", u[24]; int i = 23; u[i] = 0; unsigned long long x = v / p; do { u[--i] = (char)('0' + x % 10); x /= 10; } while (x);
    cat_s(b, sizeof b, u + i);
    if (d) { cat_s(b, sizeof b, "."); i = 23; x = v % p; for (int k = 0; k < d; k++) { u[--i] = (char)('0' + x % 10); x /= 10; } cat_s(b, sizeof b, u + i); }
    for (int l = (int)strlen(b); l < w; l++) out_s(" ");
    out_s(b);
}
static void out_pct(unsigned long long n, unsigned long long tot, int w) { out_fix(tot ? (n * 1000 + tot / 2) / tot : 0, 1, w - 1); out_s("%"); }
static void out_x(uintptr_t v) { char b[24] = ""; cat_x(b, sizeof b, v); out_s(b); }

static void agg_add(uint64_t key, uint64_t n) {
    uint64_t h = hash(key, AGG_BITS);
    for (int i = 0; i < 64; i++, h = (h + 1) & (AGG - 1)) {
        if (agg[h].key == key) { agg[h].n += n; return; }
        if (!agg[h].key) { agg[h].key = key; agg[h].n = n; return; }
    }
    agg_lost += n;
}
static void sift(int *o, int i, int n) {   /* heapsort: a min-heap by count, so the array ends sorted descending */
    for (;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < n && agg[o[l]].n < agg[o[s]].n) s = l;
        if (r < n && agg[o[r]].n < agg[o[s]].n) s = r;
        if (s == i) return;
        int t = o[i]; o[i] = o[s]; o[s] = t; i = s;
    }
}
static int collect(int slot, int modules) {   /* a slot's functions (or modules), busiest first; how many */
    int n = 0;
    for (int i = 0; i < AGG; i++) {
        uint64_t k = agg[i].key;
        if (k && (int)(k & MAXSLOT) == slot && ((uint32_t)(k >> SLOT_BITS) == 0xfffffff0u) == modules) order[n++] = i;
    }
    for (int i = n / 2 - 1; i >= 0; i--) sift(order, i, n);
    for (int k = n - 1; k > 0; k--) { int t = order[0]; order[0] = order[k]; order[k] = t; sift(order, 0, k); }
    return n;
}
/* the 8 busiest of some table entries (addresses no object explains) */
struct top8 { uintptr_t pc[8]; uint32_t n[8]; };
static void top8_add(struct top8 *t, uintptr_t pc, uint32_t n) {
    int w = 0;
    for (int j = 1; j < 8; j++) if (t->n[j] < t->n[w]) w = j;
    if (n > t->n[w]) { t->n[w] = n; t->pc[w] = pc; }
}
static void top8_out(const struct top8 *t, const char *title) {
    if (!t->n[0]) return;
    out_s(title);
    for (int j = 0; j < 8; j++) if (t->n[j]) { out_s(" "); out_x(t->pc[j]); out_s(" ("); out_u(t->n[j], 0); out_s(")"); }
    out_s("\n");
}

static int read_small(const char *path, char *out, int n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) { out[0] = 0; return 0; }
    int r = (int)read(fd, out, n - 1); close(fd);
    if (r < 0) r = 0;
    out[r] = 0; if (r && out[r - 1] == '\n') out[--r] = 0;
    return r;
}
static long long tid_cpu_ns(int tid) {   /* another thread's CPU clock: glibc's MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED) */
    struct timespec t;
    if (clock_gettime((clockid_t)((~(unsigned)tid << 3) | 6u), &t)) return -1;
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void task_path(char *p, size_t n, int tid, const char *leaf) {
    p[0] = 0; cat_s(p, n, "/proc/self/task/"); cat_u(p, n, (unsigned long)tid); cat_s(p, n, "/"); cat_s(p, n, leaf);
}
static int blocks_prof(int tid) {        /* a running thread has SIGPROF blocked: its status's SigBlk mask */
    char p[64], b[2048]; task_path(p, sizeof p, tid, "status");
    if (!read_small(p, b, sizeof b)) return 0;
    const char *q = strstr(b, "SigBlk:"); if (!q) return 0;
    for (q += 7; *q == ' ' || *q == '\t'; q++) ;
    return (int)(hexval(&q) >> (SIGPROF - 1) & 1);
}
static void slot_label(int s, char *out, size_t n) {
    struct slot *sl = &slots[s]; char tmp[96];
    out[0] = 0;
    if (s == MAXSLOT) { cat_s(out, n, "other threads"); return; }
    if (s == 1) { cat_s(out, n, "main (emulation)"); return; }   /* whatever its name: the rasterizer renames it */
    if (__atomic_load_n(&sl->live, __ATOMIC_ACQUIRE)) {     /* its name now (rast-3d names itself on its first frame) */
        char p[64], c[24]; task_path(p, sizeof p, sl->tid, "comm");
        if (read_small(p, c, 17) && strcmp(c, proc_comm)) memcpy(sl->name, c, 17);
    }
    const char *st = sl->start ? fn_name(code_of((uintptr_t)sl->start), tmp, sizeof tmp) : 0;
    if (sl->name[0]) { cat_s(out, n, sl->name); if (st && strcmp(st, sl->name)) { cat_s(out, n, " ("); cat_s(out, n, st); cat_s(out, n, ")"); } }
    else if (st) cat_s(out, n, st);
    else { cat_s(out, n, "tid "); cat_u(out, n, (unsigned long)sl->tid); }
}

static int nreports;
static void write_report(int why, int full) {
    char tmp[340] = ""; cat_s(tmp, sizeof tmp, outpath); cat_s(tmp, sizeof tmp, ".tmp");
    ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (ofd < 0) { report_ok = 0; return; }
    if (full) dl_iterate_phdr(add_module, 0);           /* objects loaded since (SDL's drivers, ...) */
    scan_regions();
    long long now = mono_ns();
    int ns = __atomic_load_n(&nslots, __ATOMIC_ACQUIRE); if (ns > MAXSLOT) ns = MAXSLOT;
    unsigned long long total = 0, ts[MAXSLOT + 1], cpu[MAXSLOT + 1], cpu_sampled = 0;
    for (int s = 1; s <= ns; s++) {
        ts[s] = __atomic_load_n(&slots[s].samples, __ATOMIC_RELAXED); total += ts[s];
        long long c = __atomic_load_n(&slots[s].cpu_done, __ATOMIC_RELAXED);
        if (s != MAXSLOT && __atomic_load_n(&slots[s].live, __ATOMIC_ACQUIRE)) { long long l = tid_cpu_ns(slots[s].tid); if (l > 0) c += l; }
        cpu[s] = c > 0 ? (unsigned long long)c : 0;
        if (ts[s]) cpu_sampled += cpu[s];
    }
    /* the objects with samples, or with a thread's start routine, get their symbols now (the report thread and exit
       only: that reads files and allocates) */
    if (full) {
        int last = -1;
        for (int i = 0; i < TAB; i++) {
            uint64_t k = __atomic_load_n(&tab[i].key, __ATOMIC_RELAXED); if (!k) continue;
            uintptr_t pc = key_pc(k);
            if (last >= 0 && pc >= mods[last].lo && pc < mods[last].hi) continue;
            int m = mod_at(pc); if (!m) continue;
            last = m - 1;
            if (!mods[last].loaded) load_syms(&mods[last]);
        }
        for (int s = 2; s <= ns && s < MAXSLOT; s++) {
            int m = slots[s].start ? mod_at((uintptr_t)slots[s].start) : 0;
            if (m && !mods[m - 1].loaded) load_syms(&mods[m - 1]);
        }
    }
    /* one pass over the table: functions and modules by thread and in all, the JIT's parts, the strays */
    memset(agg, 0, (size_t)AGG * sizeof *agg); agg_lost = 0;
    unsigned long long jit_parts[4] = { 0 };          /* main, itcm, alternate, other (jit_names) */
    unsigned long used = 0;
    struct top8 unknown, jit_other; memset(&unknown, 0, sizeof unknown); memset(&jit_other, 0, sizeof jit_other);
    for (int i = 0; i < TAB; i++) {
        uint64_t k = __atomic_load_n(&tab[i].key, __ATOMIC_RELAXED); if (!k) continue;
        used++;
        uint32_t n = __atomic_load_n(&tab[i].n, __ATOMIC_RELAXED); if (!n) continue;
        uintptr_t pc = key_pc(k);
        uint64_t code = code_of(pc), mcode = (uint64_t)mod_of(code) << 32 | 0xfffffff0u;
        int s = (int)(k & MAXSLOT);
        if (mod_of(code) == M_JIT) { jit_parts[code & 3] += n; if ((code & 3) == 3 && tcache) top8_add(&jit_other, pc, n); }
        if (mod_of(code) == M_UNKNOWN) top8_add(&unknown, pc, n);
        agg_add(code << SLOT_BITS | s, n); agg_add(code << SLOT_BITS, n);
        agg_add(mcode << SLOT_BITS | s, n); agg_add(mcode << SLOT_BITS, n);
    }
    char nm[96], fn[96]; unsigned long long pcpu = 0;
    { struct timespec c; if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c)) pcpu = (unsigned long long)c.tv_sec * 1000000000ULL + c.tv_nsec; }
    unsigned long long wall = (unsigned long long)(now - t_init);
    out_s("# SuperDrastic sampling profiler "); out_s(DSFLIP_VERSION); out_s(", pid "); out_u((unsigned long)getpid(), 0);
    out_s(": report "); out_u((unsigned long)++nreports, 0); out_s(" ("); out_s(why_name[why]); out_s("), "); out_fix(wall / 100000000ULL, 1, 0);
    out_s(" s after the start\n# "); out_s(use_itimer ? "process-wide ITIMER_PROF (timer_create failed)" : "each thread's own CPU-time timer");
    out_s(" at "); out_u((unsigned long)hz, 0); out_s(" Hz (capped by the kernel's tick): "); out_u(total, 0); out_s(" samples for ");
    out_fix(cpu_sampled / 10000000ULL, 2, 0); out_s(" s of the sampled threads' CPU; the process used "); out_fix(pcpu / 10000000ULL, 2, 0);
    out_s(" s of CPU ("); out_fix(wall ? pcpu * 100 / wall : 0, 2, 0); out_s(" cores)\n# table: "); out_u(used, 0); out_s(" of ");
    out_u(TAB, 0); out_s(" entries (an instruction, or a JIT page, of a thread)\n");
    if (dropped || agg_lost || timer_fail) {
        out_s("# lost: "); out_u(__atomic_load_n(&dropped, __ATOMIC_RELAXED), 0); out_s(" samples (table full), "); out_u(agg_lost, 0);
        out_s(" in the report's table; "); out_u((unsigned long)timer_fail, 0); out_s(" threads without a timer\n");
    }

    out_s("\nthreads (share: of all samples; CPU s: its own CPU time; %core: of one core since it started; /s: samples a CPU second)\n");
    out_s("  "); out_pad("thread", 44); out_s("    tid  samples  share    CPU s  %core    /s\n");
    for (int s = 1; s <= ns; s++) {   /* with samples, or 10 ms of CPU without one (SIGPROF blocked: SDL's threads) */
        if (!ts[s] && cpu[s] < 10000000ULL) continue;
        slot_label(s, nm, sizeof nm);
        if (!ts[s]) {
            int live = s != MAXSLOT && __atomic_load_n(&slots[s].live, __ATOMIC_ACQUIRE);
            cat_s(nm, sizeof nm, (live ? blocks_prof(slots[s].tid) : slots[s].blocked) ? ": blocks SIGPROF" : ": no samples");
        }
        out_s("  "); out_pad(nm, 44); out_u((unsigned long)(s == MAXSLOT ? 0 : slots[s].tid), 7); out_u(ts[s], 9); out_pct(ts[s], total, 7);
        out_fix(cpu[s] / 10000000ULL, 2, 9);
        long long span = now - (s == 1 ? t_init : slots[s].t0);
        out_fix(span > 0 ? cpu[s] * 1000 / (unsigned long long)span : 0, 1, 7);
        if (ts[s] && cpu[s] >= 10000000ULL) out_u(ts[s] * 1000000000ULL / cpu[s], 6); else out_s("     -");
        if (slots[s].nthreads > 1) { out_s("  ("); out_u((unsigned long)slots[s].nthreads, 0); out_s(" threads)"); }
        out_s("\n");
    }
    {   /* running threads the profiler never saw start (before it, or not through pthread_create) with 10 ms of CPU;
           not once the slots overflowed: "other threads" keeps no tids */
        int fd = __atomic_load_n(&nslots, __ATOMIC_ACQUIRE) >= MAXSLOT ? -1 : open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        char b[2048]; long r;
        while (fd >= 0 && (r = syscall(SYS_getdents64, fd, b, sizeof b)) > 0)
            for (long o = 0; o < r; ) {
                const char *d = b + o + 19; unsigned short rl; memcpy(&rl, b + o + 16, 2); o += rl;   /* linux_dirent64 */
                int tid = 0; for (const char *c = d; *c >= '0' && *c <= '9'; c++) tid = tid * 10 + (*c - '0');
                if (!tid) continue;
                int s = 0;
                for (int k = 1; k <= ns && k < MAXSLOT; k++) if (slots[k].tid == tid && __atomic_load_n(&slots[k].live, __ATOMIC_ACQUIRE)) { s = k; break; }
                long long cn = s ? 0 : tid_cpu_ns(tid);
                if (s || cn < 10000000LL) continue;
                char p[64], c[24]; task_path(p, sizeof p, tid, "comm"); read_small(p, c, 17);
                nm[0] = 0; cat_s(nm, sizeof nm, c[0] ? c : "?"); cat_s(nm, sizeof nm, ": not sampled");
                out_s("  "); out_pad(nm, 44); out_u((unsigned long)tid, 7); out_u(0, 9); out_pct(0, 1, 7);
                out_fix((unsigned long long)cn / 10000000ULL, 2, 9); out_s("\n");
            }
        if (fd >= 0) close(fd);
    }

    out_s("\nmodules, all threads\n  samples  share  module\n");
    int n = collect(0, 1);
    for (int i = 0; i < n; i++) {
        int m = mod_of(agg[order[i]].key >> SLOT_BITS);
        out_u(agg[order[i]].n, 9); out_pct(agg[order[i]].n, total, 7); out_s("  "); out_s(mod_name(m));
        if (m == M_JIT && tcache) {   /* by the translation cache's parts */
            unsigned long long j = jit_parts[0] + jit_parts[1] + jit_parts[2] + jit_parts[3];
            out_s(" (DraStic's translated DS code: main cache "); out_pct(jit_parts[0], j, 0); out_s(", itcm "); out_pct(jit_parts[1], j, 0);
            out_s(", alternate "); out_pct(jit_parts[2], j, 0); if (jit_parts[3]) { out_s(", other "); out_pct(jit_parts[3], j, 0); } out_s(")");
        }
        if (m > 0 && m <= nmods && mods[m - 1].loaded && !mods[m - 1].nsyms) out_s(" (no symbols)");
        if (m >= M_REG && m < M_REG + nregs) { out_s(" "); out_x(regs[m - M_REG].lo); out_s("-"); out_x(regs[m - M_REG].hi); }
        out_s("\n");
    }

    out_s("\nfunctions, all threads (top "); out_u(TOPN, 0); out_s(")\n  samples  share  "); out_pad("module", 24); out_s("function\n");
    n = collect(0, 0);
    for (int i = 0; i < n && i < TOPN; i++) {
        uint64_t code = agg[order[i]].key >> SLOT_BITS;
        out_u(agg[order[i]].n, 9); out_pct(agg[order[i]].n, total, 7); out_s("  "); out_pad(mod_name(mod_of(code)), 24); out_s(fn_name(code, fn, sizeof fn)); out_s("\n");
    }

    for (int s = 1; s <= ns; s++) {
        if (!ts[s] || ts[s] * 100 < total) continue;               /* threads with 1% of the samples or more */
        slot_label(s, nm, sizeof nm);
        out_s("\nthread "); out_s(nm); out_s(": "); out_u(ts[s], 0); out_s(" samples ("); out_pct(ts[s], total, 0); out_s(" of all); by module:");
        n = collect(s, 1);
        for (int i = 0; i < n && i < 8; i++) { out_s(i ? ", " : " "); out_s(mod_name(mod_of(agg[order[i]].key >> SLOT_BITS))); out_s(" "); out_pct(agg[order[i]].n, ts[s], 0); }
        out_s("\n  samples  share  "); out_pad("module", 24); out_s("function (top "); out_u(TOPT, 0); out_s(", share of the thread)\n");
        n = collect(s, 0);
        for (int i = 0; i < n && i < TOPT; i++) {
            uint64_t code = agg[order[i]].key >> SLOT_BITS;
            out_u(agg[order[i]].n, 9); out_pct(agg[order[i]].n, ts[s], 7); out_s("  "); out_pad(mod_name(mod_of(code)), 24); out_s(fn_name(code, fn, sizeof fn)); out_s("\n");
        }
    }

    top8_out(&unknown, "\nunknown PCs (unmapped since; samples):");
    top8_out(&jit_other, "\njit samples outside the translation cache:");
    out_s("\nobjects:");
    for (int i = 0; i < nmods; i++) {
        if (!mods[i].loaded) continue;
        out_s(" "); out_s(mods[i].name); out_s(" ("); out_u((unsigned long)mods[i].nsyms, 0); out_s(" symbols");
        if (mods[i].nfde) { out_s(", "); out_u((unsigned long)mods[i].nfde, 0); out_s(" FDEs"); }
        out_s(")");
    }
    out_s("; ");
    if (tcache) { out_s("DraStic's translation cache at "); out_x(tcache); out_s(" (nds_system + 0x8c000)"); }
    else out_s("DraStic's translation cache not recognised: its samples are \"jit\"");
    out_s("\n");
    flush(); close(ofd);
    report_ok = !rename(tmp, outpath);
}

static int report_busy, report_tid;
/* full: from a normal context (the report thread, exit): may read files, allocate and look for new objects */
static void report(int why, int full) {
    if (!enabled || getpid() != prof_pid) return;     /* a child that inherited the memory (fork without exec) */
    int tid = (int)syscall(SYS_gettid);
    for (int w = 0; ; w++) {
        int z = 0;
        if (__atomic_compare_exchange_n(&report_busy, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        /* another report is being written: a periodic one gives way; a final one waits for it (it may be the last
           whole one before a SIGKILL), unless it interrupted that very report on this thread */
        if (why == R_PERIODIC || __atomic_load_n(&report_tid, __ATOMIC_RELAXED) == tid || w >= 100) return;
        struct timespec d = { 0, 5000000 }; nanosleep(&d, 0);
    }
    __atomic_store_n(&report_tid, tid, __ATOMIC_RELAXED);
    int err = errno;
    write_report(why, full);
    errno = err;
    __atomic_store_n(&report_tid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&report_busy, 0, __ATOMIC_RELEASE);
}
/* resume.c, before the SIGKILL of a quit-with-save (from its SIGUSR1 handler or its save thread) */
void dsflip_prof_report(void) { report(R_QUIT, 0); }

/* ---- lifetime ---- */
static struct sigaction old_term;     /* the SIGTERM handler ours runs after the report (SDL's) */
static void on_term(int sig, siginfo_t *si, void *uc) {
    int err = errno;
    report(R_SIGTERM, 0);
    struct sigaction o = old_term;
    if (o.sa_flags & SA_SIGINFO) o.sa_sigaction(sig, si, uc);
    else if (o.sa_handler == SIG_DFL) {   /* what SIGTERM would have done: terminate (it is blocked until we return) */
        struct sigaction d; memset(&d, 0, sizeof d); d.sa_handler = SIG_DFL; sigaction(sig, &d, 0); raise(sig);
    } else if (o.sa_handler != SIG_IGN) o.sa_handler(sig);
    errno = err;
}
/* once a second: put ours in front of whatever handles SIGTERM. Not before SDL has installed its handler (it only
   does while the disposition is the default; 10 s covers DraStic's start), and never over an ignored SIGTERM. On the
   alternate stack, like the samples: a report needs ~12 KB, and the thread it lands on may be in the JIT. */
static void wrap_term(int sec) {
    struct sigaction cur;
    if (sigaction(SIGTERM, 0, &cur)) return;
    void (*h)(int) = cur.sa_handler;
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_term) return;
    if (h == SIG_IGN || (h == SIG_DFL && !(cur.sa_flags & SA_SIGINFO) && sec < 10)) return;
    old_term = cur;
    struct sigaction sa = cur;
    sa.sa_sigaction = on_term; sa.sa_flags = (cur.sa_flags | SA_SIGINFO | SA_ONSTACK) & ~SA_RESETHAND;
    sigaction(SIGTERM, &sa, 0);
}
static void at_exit(void) { report(R_EXIT, 1); }
static void *report_thread(void *a) {
    (void)a;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (int sec = 1; ; sec++) {   /* absolute deadlines: a sample landing on this thread cuts a relative sleep short */
        t.tv_sec += 1;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, 0) == EINTR) ;
        wrap_term(sec);
        if (sec % period) continue;
        report(R_PERIODIC, 1);
        if (sec == period && dsflip_log)
            dsflip_log("[prof] sampling %d threads at %d Hz of their CPU time%s; report every %d s: %s%s\n", nslots < MAXSLOT ? nslots : MAXSLOT,
                       hz, use_itimer ? " (process-wide ITIMER_PROF)" : "", period, outpath, report_ok == 1 ? "" : " (can't write it)");
    }
    return 0;
}

static int find_jit(struct dl_phdr_info *info, size_t sz, void *data) {
    (void)sz; (void)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {   /* the program's writable PT_LOAD: .data and .bss (nds_system) */
        const Elf64_Phdr *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W) || (ph->p_flags & PF_X)) continue;
        uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (!jit_lo || a < jit_lo) jit_lo = a;
        if (a + ph->p_memsz > jit_hi) jit_hi = a + ph->p_memsz;
    }
    for (int i = 0; i < info->dlpi_phnum; i++) {   /* and never the program's code (lld puts no code between them) */
        const Elf64_Phdr *ph = &info->dlpi_phdr[i]; uintptr_t a = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X) && a < jit_hi && a + ph->p_memsz > jit_lo) jit_lo = jit_hi = 0;
    }
    return 1;                                      /* the first object is the program */
}

__attribute__((constructor(102))) static void prof_init(void) {   /* before dsflip.c's init marks the game process */
    const char *e = getenv("DSFLIP_PROF");
    if (!e || !*e || *e == '0' || getenv("DSFLIP_IN_GAME")) return;   /* off, or a process DraStic started with the preload */
    {   /* a child started with the environment (and, in the simulator, the preload) isn't profiled */
        const char *pp = getenv("DSFLIP_PROF_PID"); char b[16];
        if (pp && atoi(pp) != getpid()) return;
        snprintf(b, sizeof b, "%d", (int)getpid()); setenv("DSFLIP_PROF_PID", b, 1);
    }
    prof_pid = getpid();
    if (atoi(e) >= 2) period = atoi(e);
    { const char *h = getenv("DSFLIP_PROF_HZ"); if (h && atoi(h) >= 10 && atoi(h) <= 10000) hz = atoi(h); }
    interval_ns = 1000000000L / hz;
    char outdir[256];
    const char *d = getenv("DSFLIP_PROF_OUT"), *l = getenv("DSFLIP_LOG");
    if (d && *d) snprintf(outdir, sizeof outdir, "%s", d);
    else if (l && strrchr(l, '/')) snprintf(outdir, sizeof outdir, "%.*s", (int)(strrchr(l, '/') - l), l);
    else strcpy(outdir, "/tmp");
    mkdir(outdir, 0755);
    snprintf(outpath, sizeof outpath, "%s/prof-%d.txt", outdir, prof_pid);
    tab = mmap(0, (size_t)TAB * sizeof *tab, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    agg = mmap(0, (size_t)AGG * sizeof *agg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tab == MAP_FAILED || agg == MAP_FAILED || pthread_key_create(&exit_key, thread_exit)) return;
    prctl(PR_GET_NAME, proc_comm);
    dl_iterate_phdr(find_jit, 0);
    dl_iterate_phdr(add_module, 0);       /* the objects' ranges; DraStic's symbols now, the others' by the reports */
    if (nmods) load_syms(&mods[0]);       /* so a report from a signal handler names DraStic's functions too */
    t_init = mono_ns();
    int tid = (int)syscall(SYS_gettid);
    my_slot = slot_new(0, tid, 0);                                    /* slot 1: the main thread, DraStic's emulation */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, 0);
    enabled = 1;
    sampling_on(tid);
    if (timer_fail) {                     /* no per-thread timers: one process-wide timer, with its misattribution */
        timer_fail = 0; use_itimer = 1;
        struct itimerval it = { { 0, interval_ns / 1000 }, { 0, interval_ns / 1000 } };
        setitimer(ITIMER_PROF, &it, 0);
    }
    atexit(at_exit);
    pthread_t th; if (!pthread_create(&th, 0, report_thread, 0)) pthread_setname_np(th, "dsf-prof");
    fprintf(stderr, "[prof] sampling at %d Hz of each thread's CPU time%s, report every %d s to %s (%d symbols of %s)\n", hz,
            use_itimer ? " (process-wide ITIMER_PROF)" : "", period, outpath, nmods ? mods[0].nsyms : 0, nmods ? mods[0].name : "?");
}
