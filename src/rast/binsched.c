/* binsched.c: the frame's bins shared out among DraStic's render threads as they go, longest first.
 *
 * DraStic renders a hi-res frame on N threads (update_frame_3d_4x: it wakes N-1 helper threads one after the other,
 * then the calling thread, the emulation thread with threaded_3d=0, renders its own share and waits for each
 * helper's done flag; N = the stride byte of every context, 3 in the simulator). Each thread renders a fixed set of
 * bins: first, first + N, ... (first = the thread's index). The frame is done when the slowest thread is, and the 12
 * bins are far from equal (the sky, the floor, the bins a character stands in), so with the fixed sets the others
 * wait: with each bin's thread CPU time in the simulator, the slowest thread's work is 1.07 of the mean thread's on
 * the stress ROM and the fog scene and 1.19-1.21 on the field scene S7, at 2x and at 3x.
 *
 * Here the threads of a frame claim bins instead, until none is left: list scheduling (a thread that is free takes
 * the next bin), which ends within one bin of the ideal however the bins' costs fall and whenever each thread starts,
 * with the longest bins first (LPT) within 4/3 of the best split. A bin is claimed by setting its bit in the frame's
 * mask (one atomic or: exactly one thread gets it); the order is by each bin's time in the previous frame (the counter
 * ticks around its render and resolve, recorded by whichever thread rendered it): scenes move little from frame to
 * frame, and the measure is the device's own, whatever the scale, the shading mode or the thread's interruptions.
 * Each bin is still rendered whole into the claiming thread's own context and resolved into the output frame by that
 * thread (rast.c resolve_bin, comp_bin), as with the fixed sets: a bin's output depends only on the frame's data, not
 * on the thread or the context (the per-thread caches, fused.c's texture classifications, are filled from the same
 * data; every context line a bin reads is written for that bin first), so the output is the same bytes.
 * The simulator's figures (RAST_SCHEDSTATS; the bins' CPU times in the order the queue took them, replayed on a core
 * per thread): the slowest thread 1.19 -> 1.10 of the mean on S7 at 2x, 1.20 -> 1.12 at 3x, 1.07 -> 1.06 on S4 at
 * 3x. A fixed split of the bins that is the best one for the previous frame's times does worse (S7 at 3x: 1.16, with
 * the others stealing a set's last bins 1.13): the frames' bins vary more than the queue's order loses.
 *
 * The queue's frame: comp.c's wrapper on update_frame_3d_4x calls sched_frame_begin() before DraStic's function
 * (one thread, no render thread running: the previous update waited for all of them) and sched_frame_end() after
 * it (all render threads of the update have returned from hook_entry: DraStic waited for each one's done flag).
 * begin clears the mask and computes the order; the helper threads read them after the mutex hand-off that wakes
 * them, the calling thread is the one that wrote them. So every hook_entry call between begin and end belongs to that
 * update, however the threads interleave (a helper may start before or after the calling thread, a fast thread may
 * take every bin before a slow one starts: the slow one then finds none left and returns), and no state but the bins'
 * times reaches the next frame. end checks that a frame that rendered claimed all 12 bins. Calls outside a begin/end
 * pair (no wrapper: RAST_COMP < 2 or an unknown DraStic build) keep DraStic's fixed sets, as do RAST=diff unless
 * RAST_SCHEDDIFF=1 (DraStic's renderer's quirk pixels read scratch memory that earlier bins of the thread left, see
 * the README) and a stride of 0 (DraStic renders no bin then).
 *
 * RAST_WORKERS=k (DSFLIP_RAST_WORKERS; default 0, at most 4): k render threads of librast's own claim bins beside
 * DraStic's N, each into a context of its own, when threaded_3d is off (the emulation thread is then one of DraStic's
 * render threads, and DraStic's 3 leave a core of a four-core handheld to the rest; with threaded_3d the emulation
 * thread runs beside them). How they join a frame and how DraStic's threads wait for their bins: at wake_workers().
 * The simulator's figures (as above; the queue on a core per thread, against DraStic's fixed sets and mean thread):
 * one worker takes the slowest thread from 1.19 to 0.88 of DraStic's mean thread on S7 at 2x and 3x and from 1.07 to
 * 0.82 on L4 at 2x, -23 to -26%, if the fourth core is free: off until a device A/B shows it is (an audio or display
 * thread there would share it). RAST_WORKERS=-1: one worker, used while it pays (wk_ctl_frame: windows of frames
 * with and without it, by the update's wall time).
 *
 * RAST_SCHED=0: DraStic's fixed sets. RAST_SCHEDSTATS=1 (or N: every N frames, else 300): the threads' work per frame
 * from each bin's thread CPU time: the mean thread; the slowest thread with the fixed sets, with the queue's order on
 * a core per thread (and with the order from the previous frame's CPU times, and from this frame's own), with the
 * best fixed split by the previous frame's and this frame's times, and as the simulator's threads (which share loaded
 * host cores) claimed them; the bound max(mean thread, largest bin); the update's wall time; the queue's check.
 * RAST_SCHEDHASH=1: a hash of every rendered output frame, to compare runs. */
#define _GNU_SOURCE
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "ds3d.h"
#include "binsched.h"

#define U8(p, o) (*(const uint8_t *)((const uint8_t *)(p) + (o)))

int sched_mode = 1;                     /* RAST_SCHED: 0 DraStic's fixed bins, 1 claimed */
int sched_diff;                         /* RAST_SCHEDDIFF=1: claimed bins in RAST=diff too */
static int in_update;                   /* between sched_frame_begin and sched_frame_end: the queue is open */
/* atomic: the frame's generation (bits 32-63) and its claimed bins (bit b): a bin is claimed by setting its bit; a
 * worker (RAST_WORKERS) claims only while the generation is the one it was woken for */
static uint64_t claimed;
static unsigned qgen;                   /* the frame's generation (sched_frame_begin) */
static unsigned qdone;                  /* atomic: the frame's finished bins (waited for with RAST_WORKERS) */
#define MASK(w) ((unsigned)(w) & ((1u << NBINS) - 1))
static uint8_t order[NBINS];            /* this frame's claim order: by t_last, longest first */
static uint64_t t_last[NBINS];          /* atomic: each bin's last render time (counter ticks) */
static unsigned long errs;
static unsigned nworkers;               /* RAST_WORKERS (below) */
static int wk_auto, wk_frame;           /* RAST_WORKERS=-1: the workers when they pay (wk_ctl); this frame's choice */

static inline uint64_t ticks(void) {
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
}

/* the bins by the times t, longest first (ties: the lower bin first) */
static void lpt_order(const uint64_t *t, uint8_t *ord) {
    for (unsigned i = 0; i < NBINS; i++) {
        unsigned j = i;
        for (; j && t[ord[j - 1]] < t[i]; j--) ord[j] = ord[j - 1];
        ord[j] = (uint8_t)i;
    }
}

/* ---- RAST_SCHEDSTATS ---- */
static int stats;
static unsigned stats_every = 300;
static uint64_t cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}
static uint64_t st_cpu[NBINS];          /* this frame: each bin's thread CPU time (ns) */
static uint64_t prev_cpu[NBINS];        /* the previous frame's */
static uint8_t st_thr[NBINS];           /* the thread (its context's first bin) that rendered it */
static uint8_t st_order[NBINS];         /* this frame's claim order */
static unsigned st_n, st_dyn, st_firsts;    /* the stride (threads); some thread claimed; the threads' first bins */
static uint64_t st_wall0;
static struct {
    unsigned long frames, skipped;
    double total, fixed, claimed, queue, queue_cpu, queue_now, split_prev, split_now, bound, max_bin, wall;
} acc;

/* the bins in order ord[] on n threads that start together, a core each: each bin to the thread free first */
static double list_makespan(const uint8_t *ord, unsigned n) {
    double l[NBINS] = { 0 }, m = 0;
    for (unsigned i = 0; i < NBINS; i++) {
        unsigned t = 0;
        for (unsigned u = 1; u < n; u++) if (l[u] < l[t]) t = u;
        l[t] += (double)st_cpu[ord[i]];
    }
    for (unsigned t = 0; t < n; t++) if (l[t] > m) m = l[t];
    return m;
}

/* the bins split into n fixed sets with the smallest largest sum (t: the bins' times; own[b]: bin b's set): from
 * LPT's split, depth first over the bins longest first, each into a set (of the empty sets only the first: they are
 * interchangeable), cutting where a set would reach the best split's largest sum; at most PART_STEPS steps (the
 * optimum on every case a brute force checked, 2 to 4 sets) */
#define PART_STEPS 4096
static struct {
    const uint64_t *t; unsigned n, steps, cnt[NBINS]; uint64_t best, lb, load[NBINS];
    uint8_t ord[NBINS], cur[NBINS], *own;
} P;
static void part_dfs(unsigned i) {
    if (P.best <= P.lb || P.steps++ >= PART_STEPS) return;
    if (i == NBINS) {
        uint64_t m = 0;
        for (unsigned s = 0; s < P.n; s++) if (P.load[s] > m) m = P.load[s];
        if (m < P.best) { P.best = m; memcpy(P.own, P.cur, NBINS); }
        return;
    }
    unsigned b = P.ord[i];
    int empty = 0;
    for (unsigned s = 0; s < P.n; s++) {
        if (!P.cnt[s]) { if (empty) continue; empty = 1; }
        if (P.load[s] + P.t[b] >= P.best) continue;
        P.load[s] += P.t[b]; P.cnt[s]++; P.cur[b] = (uint8_t)s;
        part_dfs(i + 1);
        P.load[s] -= P.t[b]; P.cnt[s]--;
    }
}
static void partition(const uint64_t *t, unsigned n, uint8_t *own) {
    uint64_t tot = 0, mx = 0;
    memset(&P, 0, sizeof P);
    P.t = t; P.n = n; P.own = own;
    lpt_order(t, P.ord);
    for (unsigned i = 0; i < NBINS; i++) {                 /* LPT's split: each bin to the set with the least */
        unsigned b = P.ord[i], s = 0;
        for (unsigned u = 1; u < n; u++) if (P.load[u] < P.load[s]) s = u;
        P.load[s] += t[b]; own[b] = (uint8_t)s; tot += t[b]; if (t[b] > mx) mx = t[b];
    }
    for (unsigned s = 0; s < n; s++) if (P.load[s] > P.best) P.best = P.load[s];
    P.lb = (tot + n - 1) / n > mx ? (tot + n - 1) / n : mx;
    memset(P.load, 0, sizeof P.load);
    part_dfs(0);
}
static double split_makespan(const uint8_t *own, unsigned n) {      /* each set on its thread, this frame's times */
    double l[NBINS] = { 0 }, m = 0;
    for (unsigned b = 0; b < NBINS; b++) l[own[b] % NBINS] += (double)st_cpu[b];
    for (unsigned t = 0; t < n; t++) if (l[t] > m) m = l[t];
    return m;
}

static void stats_frame(void) {
    unsigned n = st_n, nq = n + (st_dyn && wk_frame ? nworkers : 0);
    unsigned m = MASK(__atomic_load_n(&claimed, __ATOMIC_RELAXED));
    double wall = (double)(mono_ns() - st_wall0);
    if (!n || n > NBINS || m != (1u << NBINS) - 1) { acc.skipped++; return; }
    double fixed[NBINS] = { 0 }, cl[NBINS] = { 0 }, tot = 0, mb = 0, mf = 0, mc = 0;
    for (unsigned b = 0; b < NBINS; b++) {
        double c = (double)st_cpu[b];
        tot += c; if (c > mb) mb = c;
        fixed[b % n] += c;                              /* DraStic's set of thread i: bins i, i + n, ... */
        cl[st_thr[b] % NBINS] += c;
    }
    for (unsigned t = 0; t < NBINS; t++) { if (fixed[t] > mf) mf = fixed[t]; if (cl[t] > mc) mc = cl[t]; }
    /* the queue on nq threads (the workers too) that start together, each on a core of its own: in the order it took
     * (by the wall ticks, which are noisy here: the simulator's threads share loaded host cores), by the previous
     * frame's CPU times, and by this frame's own (the best order); and the best fixed split by the previous frame's
     * times and by this frame's own (the best any assignment of whole bins does) */
    uint8_t o[NBINS];
    acc.queue += list_makespan(st_order, nq);
    lpt_order(prev_cpu, o); acc.queue_cpu += list_makespan(o, nq);
    lpt_order(st_cpu, o); acc.queue_now += list_makespan(o, nq);
    partition(prev_cpu, nq, o); acc.split_prev += split_makespan(o, nq);
    partition(st_cpu, nq, o); acc.split_now += split_makespan(o, nq);
    memcpy(prev_cpu, st_cpu, sizeof prev_cpu);
    acc.frames++; acc.total += tot; acc.fixed += mf; acc.claimed += mc; acc.max_bin += mb; acc.wall += wall;
    acc.bound += tot / nq > mb ? tot / nq : mb;
    if (acc.frames % stats_every) return;
    double f = (double)acc.frames, mean = acc.total / f / n;
    fprintf(stderr, "[sched] %lu frames (%lu skipped), %u threads + %u workers (first bins %03x), %s: per frame, "
            "thread CPU us (in brackets: of DraStic's threads' mean): mean thread %.0f; slowest thread: fixed bins "
            "%.0f (%.3f of the mean), the queue on a core each %.0f (%.3f) [ordered by the previous frame's CPU "
            "times %.3f, by this frame's %.3f; the best fixed split by the previous frame's times %.3f, by this "
            "frame's %.3f], as claimed here %.0f (%.3f); bound max(mean, largest bin) %.3f; largest bin %.0f; update "
            "wall %.0f us; queue errors %lu\n",
            acc.frames, acc.skipped, n, nq - n, st_firsts, st_dyn ? "claimed" : "fixed bins", mean / 1e3,
            acc.fixed / f / 1e3, acc.fixed / f / mean, acc.queue / f / 1e3, acc.queue / f / mean,
            acc.queue_cpu / f / mean,
            acc.queue_now / f / mean, acc.split_prev / f / mean, acc.split_now / f / mean, acc.claimed / f / 1e3,
            acc.claimed / f / mean, acc.bound / f / mean, acc.max_bin / f / 1e3, acc.wall / f / 1e3, errs);
}

/* RAST_SCHEDHASH=1: a hash of every rendered output frame after the update (the bins and DraStic's gap passes), to
 * compare runs with the fixed and the claimed bins (the emulation is the same frame for frame) */
static int hash_frames;
static void hash_frame(const uint8_t *sys) {
    static unsigned long n;
    const uint64_t *o = *(const uint64_t *const *)(sys + SYS_OUTPUT);
    uint64_t h = 14695981039346656037ull;
    for (unsigned i = 0; i < NBINS * BIN_BYTES / 8; i++) h = (h ^ o[i]) * 1099511628211ull;
    fprintf(stderr, "[sched] frame %lu hash %016llx\n", n++, (unsigned long long)h);
}

/* ---- RAST_WORKERS=-1: the workers when they pay ----
 * Windows of WK_WIN rendered frames with the workers and without, by turns, compare the update's mean wall time
 * (sched_frame_begin to sched_frame_end, counter ticks: DraStic's texture cache, binning, the render and the gap
 * passes); the faster one then runs for WK_KEEP windows before the next pair. A busy fourth core shows as slower
 * frames with the workers, a free one as faster. */
#define WK_WIN 30
#define WK_KEEP 8
static struct { uint64_t t0, sum[2]; unsigned n, win, keep; int on, best; unsigned long chosen[2]; } wk_ctl;
static void wk_ctl_frame(void) {
    wk_ctl.sum[wk_ctl.on] += ticks() - wk_ctl.t0;
    if (++wk_ctl.n < WK_WIN) return;
    wk_ctl.n = 0;
    if (wk_ctl.keep) { if (!--wk_ctl.keep) { wk_ctl.on = 1; wk_ctl.sum[0] = wk_ctl.sum[1] = 0; } return; }
    if (wk_ctl.on) { wk_ctl.on = 0; return; }                  /* the pair's first window was with them */
    wk_ctl.best = wk_ctl.on = wk_ctl.sum[1] < wk_ctl.sum[0];   /* both windows done: keep the faster */
    wk_ctl.chosen[wk_ctl.best]++;
    wk_ctl.keep = WK_KEEP;
    if (stats) fprintf(stderr, "[sched] workers: %s (mean update %llu ticks with them, %llu without; chosen %lu times, "
                       "not %lu)\n", wk_ctl.best ? "on" : "off", (unsigned long long)(wk_ctl.sum[1] / WK_WIN),
                       (unsigned long long)(wk_ctl.sum[0] / WK_WIN), wk_ctl.chosen[1], wk_ctl.chosen[0]);
}

/* ---- the queue ---- */
void sched_frame_begin(void) {
    if (stats) {
        memset(st_cpu, 0, sizeof st_cpu);
        st_wall0 = mono_ns();
        st_n = st_dyn = 0;
        for (unsigned b = 0; b < NBINS; b++) st_order[b] = (uint8_t)b;
    }
    qgen++;
    __atomic_store_n(&claimed, (uint64_t)qgen << 32, __ATOMIC_RELAXED);
    __atomic_store_n(&qdone, 0, __ATOMIC_RELAXED);
    if (!sched_mode) return;
    uint64_t t[NBINS];
    for (unsigned b = 0; b < NBINS; b++) t[b] = __atomic_load_n(&t_last[b], __ATOMIC_RELAXED);
    lpt_order(t, order);
    if (stats) memcpy(st_order, order, NBINS);
    wk_frame = nworkers && (!wk_auto || wk_ctl.on);
    wk_ctl.t0 = ticks();
    __atomic_store_n(&in_update, 1, __ATOMIC_RELEASE);  /* the helpers see it after DraStic's mutex hand-off */
}

void sched_frame_end(const uint8_t *sys) {
    int dyn = __atomic_load_n(&in_update, __ATOMIC_RELAXED);
    unsigned m = MASK(__atomic_load_n(&claimed, __ATOMIC_RELAXED));
    if (hash_frames && m) hash_frame(sys);
    /* the check: a frame that rendered (some bin claimed) claimed every bin (each once: the claims are its bits) */
    if (dyn && m && m != (1u << NBINS) - 1 && __atomic_fetch_add(&errs, 1, __ATOMIC_RELAXED) < 20)
        fprintf(stderr, "[sched] frame's bins claimed %03x: some bin not rendered\n", m);
    if (stats && m) stats_frame();
    if (wk_auto && dyn && m == (1u << NBINS) - 1) wk_ctl_frame();
    __atomic_store_n(&in_update, 0, __ATOMIC_RELAXED);
}

/* ---- RAST_WORKERS: render threads of our own beside DraStic's (see the top) ----
 * Each worker has a context of its own (its header copied from the waking thread's: sys, geom, the edge-marking
 * flag; its first bin DraStic's stride + its index) and sleeps until the first DraStic thread of a frame starts on
 * its bins (DraStic wakes its threads after it binned the frame's polygons) and wakes the workers with the frame's
 * generation. A worker claims by compare-and-swap only while the claim word holds that generation, so a worker that
 * wakes late takes nothing of a later frame; and DraStic's threads, once no bin is left to claim, wait until all 12
 * are finished (wait_done: qdone, counted by every thread after each bin; their acquire orders the workers' output
 * before DraStic's done flags and gap passes), so the frame is complete when DraStic's threads return, as before. A
 * worker touches its context only for bins it claimed, and the frame cannot end before those are finished, so the
 * next frame's header copy never meets a worker's render. */
static void (*worker_render)(uint8_t *ctx);
static pthread_mutex_t wk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wk_cond = PTHREAD_COND_INITIALIZER;
static unsigned wk_gen, wk_started;     /* under wk_lock (wk_gen also read atomically): the generation woken for */
static uint8_t *wk_ctx[4];
static __thread int wk_self;            /* this thread is a worker */
static __thread unsigned wk_self_gen;   /* the generation it was woken for */

static void *worker_main(void *arg) {
    unsigned w = (unsigned)(uintptr_t)arg, seen = 0;
    pthread_setname_np(pthread_self(), "rast-3dw");
    wk_self = 1;
    for (;;) {
        pthread_mutex_lock(&wk_lock);
        while (wk_gen == seen) pthread_cond_wait(&wk_cond, &wk_lock);
        seen = wk_self_gen = wk_gen;
        pthread_mutex_unlock(&wk_lock);
        worker_render(wk_ctx[w]);       /* rast.c: its bin loop, claiming (sched_start sees wk_self) */
    }
    return 0;
}

/* a DraStic thread that found no bin left: until the bins the workers still render are finished (a short spin,
 * then a futex wait on qdone, woken by each finished bin while someone waits; 1 ms timeouts as a safety net) */
static unsigned qwaiters;               /* atomic: threads in wait_done's futex wait */
static void wait_done(void) {
    unsigned d;
    for (unsigned spin = 0; (d = __atomic_load_n(&qdone, __ATOMIC_ACQUIRE)) < NBINS; spin++) {
        if (spin < 256) { __asm__ volatile("yield"); continue; }
        __atomic_fetch_add(&qwaiters, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&qdone, __ATOMIC_SEQ_CST) == d) {
            struct timespec ts = { 0, 1000000 };
            syscall(SYS_futex, &qdone, FUTEX_WAIT_PRIVATE, d, &ts, 0, 0);
        }
        __atomic_fetch_sub(&qwaiters, 1, __ATOMIC_SEQ_CST);
    }
}

/* with threaded_3d the emulation thread runs beside the render threads: no core is left for a worker */
static int threaded_3d(const uint8_t *ctx) {
    const uint8_t *sys = *(const uint8_t *const *)(ctx + CTX_SYS), *cfg = *(const uint8_t *const *)(sys + SYS_CFG);
    return *(const uint32_t *)(cfg + CFG_THREADED_3D) != 0;
}

/* the first DraStic thread of a frame that claims: the workers' contexts get its header, and they wake */
static void wake_workers(const uint8_t *ctx, unsigned gen) {
    pthread_mutex_lock(&wk_lock);
    if (wk_gen != gen) {
        unsigned n = 0;
        for (; n < nworkers; n++) {
            if (!wk_ctx[n] && !(wk_ctx[n] = aligned_alloc(64, CTX_SIZE))) break;
            if (n >= wk_started) memset(wk_ctx[n], 0, CTX_SIZE);
            memcpy(wk_ctx[n] + CTX_SYS, ctx + CTX_SYS, CTX_SIZE - CTX_SYS);
            wk_ctx[n][CTX_FIRST_BIN] = (uint8_t)(ctx[CTX_BIN_STRIDE] + n);
            if (n >= wk_started) {
                pthread_t th;
                if (pthread_create(&th, 0, worker_main, (void *)(uintptr_t)n)) break;
                pthread_detach(th);
                wk_started = n + 1;
            }
        }
        __atomic_store_n(&wk_gen, gen, __ATOMIC_RELAXED);
        pthread_cond_broadcast(&wk_cond);
    }
    pthread_mutex_unlock(&wk_lock);
}

void sched_start(sched_it *it, const uint8_t *ctx, int allow_dyn) {
    it->first = U8(ctx, CTX_FIRST_BIN); it->stride = U8(ctx, CTX_BIN_STRIDE);
    it->nb = it->stride ? NBINS / it->stride : 0;      /* DraStic's udiv: a stride of 0 renders nothing */
    it->k = 0; it->bin = -1;
    it->dyn = allow_dyn && it->stride && __atomic_load_n(&in_update, __ATOMIC_ACQUIRE);
    it->gen = wk_self ? wk_self_gen : qgen;
    if (wk_self) it->dyn = 2;
    else if (it->dyn && wk_frame && __atomic_load_n(&wk_gen, __ATOMIC_RELAXED) != it->gen && !threaded_3d(ctx))
        wake_workers(ctx, it->gen);
    if (stats) {
        if (!wk_self) __atomic_store_n(&st_n, it->stride, __ATOMIC_RELAXED);
        if (it->dyn) __atomic_store_n(&st_dyn, 1, __ATOMIC_RELAXED);
        if (it->first < NBINS) __atomic_fetch_or(&st_firsts, 1u << it->first, __ATOMIC_RELAXED);
    }
}

int sched_next(sched_it *it) {
    uint64_t now = ticks();
    if (it->bin >= 0) {
        __atomic_store_n(&t_last[it->bin], now - it->t0, __ATOMIC_RELAXED);
        if (stats) st_cpu[it->bin] = cpu_ns() - it->c0;
        __atomic_fetch_add(&qdone, 1, __ATOMIC_SEQ_CST);  /* (with wait_done's waiters count: no lost wake-up) */
        if (__atomic_load_n(&qwaiters, __ATOMIC_SEQ_CST))
            syscall(SYS_futex, &qdone, FUTEX_WAKE_PRIVATE, INT_MAX, 0, 0, 0);
    }
    int b = -1;
    if (it->dyn == 1) {
        /* the first bin of the frame's order whose bit this thread sets (the bins before it.k are taken) */
        while (b < 0 && it->k < NBINS) {
            unsigned c = order[it->k++];
            if (!(__atomic_fetch_or(&claimed, 1ull << c, __ATOMIC_RELAXED) & 1ull << c)) b = (int)c;
        }
        /* with workers (woken for this frame by its first thread, before any thread got here): return when every bin
         * is finished */
        if (b < 0 && nworkers && __atomic_load_n(&wk_gen, __ATOMIC_RELAXED) == it->gen) wait_done();
    } else if (it->dyn) {
        /* a worker: the same by compare-and-swap, while the frame is the one it was woken for */
        uint64_t w = __atomic_load_n(&claimed, __ATOMIC_ACQUIRE);
        while (b < 0 && it->k < NBINS && (unsigned)(w >> 32) == it->gen) {
            unsigned c = order[it->k];
            if (w & 1ull << c) { it->k++; continue; }
            if (__atomic_compare_exchange_n(&claimed, &w, w | 1ull << c, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                b = (int)c; it->k++;
            }
        }
    } else {
        unsigned c = it->first + it->k * it->stride;
        if (it->k < it->nb && c < NBINS) { b = (int)c; __atomic_fetch_or(&claimed, 1ull << c, __ATOMIC_RELAXED); }
        it->k++;
    }
    if (b >= 0) {
        if (stats) { st_thr[b] = (uint8_t)it->first; it->c0 = cpu_ns(); }
        it->t0 = now;
    }
    it->bin = b;
    return b;
}

void sched_init(void (*render)(uint8_t *ctx)) {
    const char *e = getenv("DSFLIP_RAST_SCHED");
    if (!e) e = getenv("RAST_SCHED");
    if (e) sched_mode = atoi(e) != 0;
    sched_diff = getenv("RAST_SCHEDDIFF") && atoi(getenv("RAST_SCHEDDIFF"));
    stats = getenv("RAST_SCHEDSTATS") ? atoi(getenv("RAST_SCHEDSTATS")) : 0;
    if (stats > 1) stats_every = (unsigned)stats;
    hash_frames = getenv("RAST_SCHEDHASH") && atoi(getenv("RAST_SCHEDHASH"));
    for (unsigned b = 0; b < NBINS; b++) order[b] = (uint8_t)b;
    worker_render = render;
    e = getenv("DSFLIP_RAST_WORKERS");
    if (!e) e = getenv("RAST_WORKERS");
    if (e && sched_mode && render) nworkers = atoi(e) < 0 ? 1 : atoi(e) > 4 ? 4 : (unsigned)atoi(e);
    wk_auto = e && atoi(e) < 0;
    wk_ctl.on = 1;
}
