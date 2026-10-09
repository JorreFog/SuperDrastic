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
 * RAST_SCHED=0: DraStic's fixed sets. RAST_SCHEDSTATS=1 (or N: every N frames, else 300): the threads' work per frame
 * from each bin's thread CPU time: the mean thread; the slowest thread with the fixed sets, with the queue's order on
 * a core per thread (and with the order from the previous frame's CPU times, and from this frame's own), with the
 * best fixed split by the previous frame's and this frame's times, and as the simulator's threads (which share loaded
 * host cores) claimed them; the bound max(mean thread, largest bin); the update's wall time; the queue's check.
 * RAST_SCHEDHASH=1: a hash of every rendered output frame, to compare runs. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ds3d.h"
#include "binsched.h"

#define U8(p, o) (*(const uint8_t *)((const uint8_t *)(p) + (o)))

int sched_mode = 1;                     /* RAST_SCHED: 0 DraStic's fixed bins, 1 claimed */
int sched_diff;                         /* RAST_SCHEDDIFF=1: claimed bins in RAST=diff too */
static int in_update;                   /* between sched_frame_begin and sched_frame_end: the queue is open */
static unsigned claimed;                /* atomic: this frame's claimed bins (bit b): a bin is claimed by setting it */
static uint8_t order[NBINS];            /* this frame's claim order: by t_last, longest first */
static uint64_t t_last[NBINS];          /* atomic: each bin's last render time (counter ticks) */
static unsigned long errs;

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
    const uint64_t *t; unsigned n, steps, cnt[NBINS]; uint64_t best, lb, load[NBINS]; uint8_t ord[NBINS], cur[NBINS], *own;
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
    unsigned n = st_n, m = __atomic_load_n(&claimed, __ATOMIC_RELAXED);
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
    /* the queue on n threads that start together, each on a core of its own: in the order it took (by the wall
     * ticks, which are noisy here: the simulator's threads share loaded host cores), by the previous frame's CPU
     * times, and by this frame's own (the best order); and the best fixed split by the previous frame's times and by
     * this frame's own (the best any assignment of whole bins does) */
    uint8_t o[NBINS];
    acc.queue += list_makespan(st_order, n);
    lpt_order(prev_cpu, o); acc.queue_cpu += list_makespan(o, n);
    lpt_order(st_cpu, o); acc.queue_now += list_makespan(o, n);
    partition(prev_cpu, n, o); acc.split_prev += split_makespan(o, n);
    partition(st_cpu, n, o); acc.split_now += split_makespan(o, n);
    memcpy(prev_cpu, st_cpu, sizeof prev_cpu);
    acc.frames++; acc.total += tot; acc.fixed += mf; acc.claimed += mc; acc.max_bin += mb; acc.wall += wall;
    acc.bound += tot / n > mb ? tot / n : mb;
    if (acc.frames % stats_every) return;
    double f = (double)acc.frames, mean = acc.total / f / n;
    fprintf(stderr, "[sched] %lu frames (%lu skipped), %u threads (first bins %03x), %s: per frame, thread CPU us: mean "
            "thread %.0f; slowest thread: fixed bins %.0f (%.3f of the mean), the queue on a core each %.0f (%.3f) [ordered "
            "by the previous frame's CPU times %.3f, by this frame's %.3f; the best fixed split by the previous frame's "
            "times %.3f, by this frame's %.3f], as claimed here %.0f (%.3f); bound max(mean, largest bin) %.3f; largest "
            "bin %.0f; update wall %.0f us; queue errors %lu\n",
            acc.frames, acc.skipped, n, st_firsts, st_dyn ? "claimed" : "fixed bins", mean / 1e3, acc.fixed / f / 1e3,
            acc.fixed / f / mean, acc.queue / f / 1e3, acc.queue / f / mean, acc.queue_cpu / f / mean,
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

/* ---- the queue ---- */
void sched_frame_begin(void) {
    if (stats) {
        memset(st_cpu, 0, sizeof st_cpu);
        st_wall0 = mono_ns();
        st_n = st_dyn = 0;
        for (unsigned b = 0; b < NBINS; b++) st_order[b] = (uint8_t)b;
    }
    __atomic_store_n(&claimed, 0, __ATOMIC_RELAXED);
    if (!sched_mode) return;
    uint64_t t[NBINS];
    for (unsigned b = 0; b < NBINS; b++) t[b] = __atomic_load_n(&t_last[b], __ATOMIC_RELAXED);
    lpt_order(t, order);
    if (stats) memcpy(st_order, order, NBINS);
    __atomic_store_n(&in_update, 1, __ATOMIC_RELEASE);  /* the helpers see it after DraStic's mutex hand-off */
}

void sched_frame_end(const uint8_t *sys) {
    int dyn = __atomic_load_n(&in_update, __ATOMIC_RELAXED);
    unsigned m = __atomic_load_n(&claimed, __ATOMIC_RELAXED);
    if (hash_frames && m) hash_frame(sys);
    /* the check: a frame that rendered (some bin claimed) claimed every bin (each once: the claims are its bits) */
    if (dyn && m && m != (1u << NBINS) - 1 && __atomic_fetch_add(&errs, 1, __ATOMIC_RELAXED) < 20)
        fprintf(stderr, "[sched] frame's bins claimed %03x: some bin not rendered\n", m);
    if (stats && m) stats_frame();
    __atomic_store_n(&in_update, 0, __ATOMIC_RELAXED);
}

void sched_start(sched_it *it, const uint8_t *ctx, int allow_dyn) {
    it->first = U8(ctx, CTX_FIRST_BIN); it->stride = U8(ctx, CTX_BIN_STRIDE);
    it->nb = it->stride ? NBINS / it->stride : 0;      /* DraStic's udiv: a stride of 0 renders nothing */
    it->k = 0; it->bin = -1;
    it->dyn = allow_dyn && it->stride && __atomic_load_n(&in_update, __ATOMIC_ACQUIRE);
    if (stats) {
        __atomic_store_n(&st_n, it->stride, __ATOMIC_RELAXED);
        if (it->dyn) __atomic_store_n(&st_dyn, 1, __ATOMIC_RELAXED);
        if (it->first < NBINS) __atomic_fetch_or(&st_firsts, 1u << it->first, __ATOMIC_RELAXED);
    }
}

int sched_next(sched_it *it) {
    uint64_t now = ticks();
    if (it->bin >= 0) {
        __atomic_store_n(&t_last[it->bin], now - it->t0, __ATOMIC_RELAXED);
        if (stats) st_cpu[it->bin] = cpu_ns() - it->c0;
    }
    int b = -1;
    if (it->dyn) {
        /* the first bin of the frame's order whose bit this thread sets (the bins before it.k are taken) */
        while (b < 0 && it->k < NBINS) {
            unsigned c = order[it->k++];
            if (!(__atomic_fetch_or(&claimed, 1u << c, __ATOMIC_RELAXED) & 1u << c)) b = (int)c;
        }
    } else {
        unsigned c = it->first + it->k * it->stride;
        if (it->k < it->nb && c < NBINS) { b = (int)c; __atomic_fetch_or(&claimed, 1u << c, __ATOMIC_RELAXED); }
        it->k++;
    }
    if (b >= 0) {
        if (stats) { st_thr[b] = (uint8_t)it->first; it->c0 = cpu_ns(); }
        it->t0 = now;
    }
    it->bin = b;
    return b;
}

void sched_init(void) {
    const char *e = getenv("DSFLIP_RAST_SCHED");
    if (!e) e = getenv("RAST_SCHED");
    if (e) sched_mode = atoi(e) != 0;
    sched_diff = getenv("RAST_SCHEDDIFF") && atoi(getenv("RAST_SCHEDDIFF"));
    stats = getenv("RAST_SCHEDSTATS") ? atoi(getenv("RAST_SCHEDSTATS")) : 0;
    if (stats > 1) stats_every = (unsigned)stats;
    hash_frames = getenv("RAST_SCHEDHASH") && atoi(getenv("RAST_SCHEDHASH"));
    for (unsigned b = 0; b < NBINS; b++) order[b] = (uint8_t)b;
}
