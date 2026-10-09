// cpugov.c: the lowest CPU clock that keeps DraStic at full speed (plan-1.4 section 7).
//
// ROCKNIX runs DS games with the CPU governor "performance": all four cores at 1992 MHz for the whole session. Most
// of that is heat. Measured (HeartGold at 2x, walking, 60 s each): the busiest DraStic thread is 46% busy at 1992
// MHz and 66% at 1104, still 60 fps with no clear extra drops, while the battery current fell by ~100 mA and the SoC ran
// 5 C cooler. schedutil doesn't get there (average 1771 MHz: DraStic's several busy threads keep it high).
//
// So this thread sets the clock itself, from what the game needs: every 250 ms it reads the CPU time of each of
// DraStic's threads (per-thread CPU clocks, ns precision), the heaviest single frame's CPU time on DraStic's main
// thread (measured at each present), the frames presented and the frames the queue dropped. The busiest thread's
// load u at the current clock f predicts the clock that would load it to TARGET: f * u / TARGET (work scales with
// 1/f; memory-bound parts scale less, which errs on the fast side); the heaviest frame must fit PEAK_TARGET of a
// refresh the same way. The average alone was not enough: at 1104 MHz HeartGold averaged 66% yet single frames ran
// over 16.7 ms, DraStic caught up in a burst and the queue dropped frames (0.2-0.26/s instead of ~0.05). Up at once
// when a window needs it (busy, a heavy frame, a drop, or below full speed); down one step only after DOWN_AFTER
// windows in a row that would all fit lower. The clock is set through scaling_max_freq under the "performance" governor (this kernel has no
// "userspace" governor); session.sh saves the limit before the game and restore.sh puts it back.
//
//
// A clock that dropped a frame isn't stepped down to again for a while (30 s, doubling with each further drop, up to
// 10 min), and that is remembered per game, shader and resolution: without it every session found the same clocks
// again by dropping frames at each of them, most of them in its first minute (1.4: one or two such drops in the first
// minute of a 90 s HeartGold run). A session starts with those clocks banned for as long as their strikes say, and
// two minutes at a clock or lower without a drop forgive it one strike. <data>/cpugov/<rom>.<shader>.<1x|2x> holds
// "<kHz> <strikes>" per clock that has any; <data> is DSFLIP_DATA, else libdsflip's folder.
//
// Only a drop that looks CPU-bound earns a strike: a frame in the last second took BLAME_PEAK of a refresh, or the
// busiest thread was over TARGET. Most drops in real play are not that: they come in waves as DraStic's presents drift
// through the refresh cycle (~50 ppm, one cycle every 4-9 min), at every clock including the top, with the heaviest
// frame at 55-65% (2 h of real play, 2026-09-29: 62 of 70 strikes were such drops, and the strikes they left on 1800 MHz
// held the clock at 1992 for 75% of a Black 2 session while it dropped more there than anywhere). A light drop still
// steps up once and keeps the clock it happened at away for BAD_FOR_S, but it isn't remembered or escalated. A drop
// while the game isn't running (DraStic's menu, quitting: busiest thread under IDLE_BUSY) is ignored.
//
// Savestates: DraStic saves (and loads) on its main thread, compressing the whole machine state in one go, and at a
// low clock that stalls the game for a long, visible moment (at 2x a save at 1104 MHz; ROCKNIX's "performance" runs it
// at 1992). A save or load press, or DraStic opening a savestate file, boosts the clock to the hardware's top for a few
// seconds (cpugov_boost); the windows meanwhile don't count (the stall isn't a drop at any clock) and the clock from
// before comes back after it.
//
// DSFLIP_CPU_MAX can be a soft bound (DSFLIP_CPU_MAX_SOFT=1): above it only while the game is below full speed with
// real work going on, or drops CPU-bound frames, at it. A hard bound made heavy 3D games at 2x run at 45-55 fps all
// session long at a power profile's cap (players' logs, 1.5: Call of Duty and Final Fantasy - The 4 Heroes of Light
// below full speed in 74% and 38% of their play at 1416-1608 MHz). A hard bound is applied from the start.
//
// DSFLIP_CPUGOV=0: off (the clock stays as ROCKNIX set it). DSFLIP_CPU_MIN / DSFLIP_CPU_MAX (kHz): bounds.
// DSFLIP_CPUGOV_MEMORY=0: start every session knowing nothing. DSFLIP_CPUGOV_LOG=1: one log line per decision window
// instead of per change.
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void dsflip_log(const char *fmt, ...);
extern volatile int dsflip_presents;        /* dsflip.c: SDL_RenderPresent calls, monotonic */
extern volatile long long dsflip_frame_work_max;   /* dsflip.c: heaviest frame's main-thread CPU ns since taken */
extern volatile int dsflip_queue_drops;     /* dsflip.c: frames dropped because DraStic fell behind and caught up */
extern volatile int dsflip_screen_w;        /* dsflip.c: DraStic's screen width, 256 (1x) or 512 (2x); 0 until it has one */

#define POL "/sys/devices/system/cpu/cpufreq/policy0/"
#define WINDOW_MS 250
#define TARGET 0.72                          /* load the busiest thread may reach at the chosen clock */
#define HIGH 0.85                            /* above this at the current clock: go up now */
#define PEAK_TARGET 0.80                     /* the heaviest frame in a window may take this much of 16.7 ms */
#define PEAK_HIGH 0.95                       /* a frame this close to late: go up now */
#define BAD_FOR_S 30                         /* a clock that dropped a frame isn't tried again for this long, doubled
                                               for each further time it drops one (up to BAD_MAX_S) */
#define BAD_MAX_S 600
#define FORGIVE_MS 120000                    /* this long at a clock or lower without a drop forgives it one strike */
#define MAX_STRIKES 6                        /* more wouldn't ban it longer (30 s << 5 is past BAD_MAX_S already) */
#define BLAME_PEAK 0.80                      /* a drop is held against the clock only after a frame this heavy (last 1 s)... */
#define IDLE_BUSY 0.25                       /* ...and ignored with the busiest thread below this (menu, quitting) */
#define FPS_WINDOWS 4                        /* the frame rate is judged over 1 s: 250 ms holds only ~15 frames */
#define DOWN_AFTER 8                         /* windows (2 s) in a row that fit a lower clock before stepping down */
#define MAXT 64

static int freqs[24], nf, cur, fmin_, fmax_;
static long long bad_until[24];              /* per clock: don't step down to it before this (CLOCK_MONOTONIC ns) */
static int strikes[24];                      /* per clock: how often it dropped frames (remembered, less forgiven) */
static int proof_ms[24];                     /* per clock: time at it or lower without a drop, towards a forgiveness */
static long long now_ns;
static int verbose;
static int hwmax_;                           /* the hardware's top clock (savestate boosts, a soft DSFLIP_CPU_MAX) */
static int soft_max;                         /* DSFLIP_CPU_MAX_SOFT=1: fmax_ may be passed when the game needs it */
static volatile long long boost_until;       /* CLOCK_MONOTONIC ns: boosted until then (cpugov_boost) */
static volatile int gov_on;

/* how long a clock's (k+1)th drop keeps it banned */
static long long ban_ns(int k) {
    long long s = (long long)BAD_FOR_S << (k < 5 ? k : 5);
    return (s > BAD_MAX_S ? BAD_MAX_S : s) * 1000000000LL;
}

static int rd_int(const char *p) { FILE *f = fopen(p, "r"); int v = 0; if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); } return v; }
static int wr_int(const char *p, int v) { FILE *f = fopen(p, "w"); if (!f) return -1; fprintf(f, "%d\n", v); return fclose(f); }
static void rd_str(const char *p, char *out, size_t n) {
    FILE *f = fopen(p, "r"); out[0] = 0;
    if (f) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}

static double last_peak;
static void set_clock(int khz, const char *why, double u, double fps) {
    if (khz == cur) return;
    /* raising: max first is enough (performance follows it); lowering: the same single write */
    if (wr_int(POL "scaling_max_freq", khz)) { dsflip_log("[cpugov] can't set %d kHz\n", khz); return; }
    dsflip_log("[cpugov] %d -> %d MHz (%s: busiest thread %.0f%%, heaviest frame %.0f%% of 16.7 ms, %.1f fps)\n",
               cur / 1000, khz / 1000, why, u * 100, last_peak * 100, fps);
    cur = khz;
}

/* the lowest available clock >= khz, within the bounds (up to top) */
static int fit_to(double khz, int top) {
    for (int i = 0; i < nf; i++) if (freqs[i] >= fmin_ && freqs[i] <= top && freqs[i] >= khz) return freqs[i];
    return top;
}
static int fit(double khz) { return fit_to(khz, fmax_); }
/* one step up for a game that can't keep up: past a soft DSFLIP_CPU_MAX too */
static int step_up(int khz) { return fit_to(khz + 1, soft_max ? hwmax_ : fmax_); }
static int idx(int khz) { for (int i = 0; i < nf; i++) if (freqs[i] == khz) return i; return -1; }

/* ---- the memory ---- */
static int mem_on = 1, mem_w;                /* mem_w: the resolution the memory in use is for */
static char mem_path[768];                   /* "": nothing to remember by (no ROM on DraStic's command line) */
static void mem_save(void) {
    if (!mem_path[0]) return;
    char tmp[800]; snprintf(tmp, sizeof tmp, "%s.new", mem_path);
    int any = 0;
    for (int i = 0; i < nf; i++) any |= strikes[i];
    if (!any) { unlink(mem_path); return; }
    FILE *f = fopen(tmp, "w"); if (!f) return;
    for (int i = 0; i < nf; i++) if (strikes[i]) fprintf(f, "%d %d\n", freqs[i], strikes[i] < MAX_STRIKES ? strikes[i] : MAX_STRIKES);
    if (fclose(f) == 0) rename(tmp, mem_path); else unlink(tmp);
}
/* what's known for this game with this shader at resolution w (DraStic's screen width); starts the bans */
static void mem_load(int w) {
    mem_w = w; mem_path[0] = 0;
    for (int i = 0; i < nf; i++) { strikes[i] = 0; bad_until[i] = 0; proof_ms[i] = 0; }
    char cmd[4096], rom[256] = ""; int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "r"); size_t n = f ? fread(cmd, 1, sizeof cmd - 1, f) : 0; if (f) fclose(f);
    cmd[n] = 0;
    for (size_t i = 0; i < n; i += strlen(cmd + i) + 1) if (cmd[i] && argc++) {     /* the last argument: the ROM */
        const char *b = strrchr(cmd + i, '/'); snprintf(rom, sizeof rom, "%s", b ? b + 1 : cmd + i);
    }
    if (!rom[0]) { dsflip_log("[cpugov] no ROM on the command line: nothing remembered\n"); return; }
    const char *sh = getenv("DSFLIP_SHADER"); if (!sh || !*sh) sh = getenv("DSHOOK_SHADER"); if (!sh || !*sh) sh = "none";
    const char *sb = strrchr(sh, '/'); if (sb) sh = sb + 1;
    const char *data = getenv("DSFLIP_DATA"); if (!data || !*data) data = "/storage/.config/drastic/dsflip";
    char dir[512]; snprintf(dir, sizeof dir, "%s/cpugov", data); mkdir(dir, 0755);
    snprintf(mem_path, sizeof mem_path, "%s/%s.%s.%s", dir, rom, sh, w > 256 ? "2x" : "1x");
    char said[256] = ""; size_t m = 0; int khz, k;
    if ((f = fopen(mem_path, "r"))) {
        while (fscanf(f, "%d %d", &khz, &k) == 2) {
            int i = idx(khz); if (i < 0 || k <= 0) continue;
            strikes[i] = k < MAX_STRIKES ? k : MAX_STRIKES;
            bad_until[i] = now_ns + ban_ns(strikes[i] - 1);
            if (m < sizeof said) m += snprintf(said + m, sizeof said - m, ", %d MHz %d (not for %llds)", khz / 1000,
                                                strikes[i], ban_ns(strikes[i] - 1) / 1000000000LL);
        }
        fclose(f);
    }
    dsflip_log("[cpugov] remembered drops for %s%s\n", mem_path + strlen(dir) + 1, m ? said : ": none");
}
/* one step down, unless that clock dropped a frame recently: stepping down to it again, dropping, and going back up
 * was where most of the remaining drops came from (measured: ds-crisp 0.13/s bouncing 1416 <-> 1608) */
static int step_down(int khz) {
    int best = khz;
    for (int i = 0; i < nf; i++) if (freqs[i] < khz && freqs[i] >= fmin_ && (best == khz || freqs[i] > best)) best = freqs[i];
    int b = idx(best);
    return b >= 0 && bad_until[b] > now_ns ? khz : best;
}

static long long thread_ns(int tid) {
    struct timespec t;
    clockid_t c = ((~(clockid_t)tid) << 3) | 6;         /* CPUCLOCK_PERTHREAD | CPUCLOCK_SCHED: any thread of ours */
    if (clock_gettime(c, &t)) return -1;
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void *gov_thread(void *a) {
    (void)a;
    int tids[MAXT]; long long last[MAXT]; int nt = 0;
    struct timespec w = { 0, WINDOW_MS * 1000000L };
    int drops_prev = dsflip_queue_drops;
    long long t_prev = 0, fps_t[FPS_WINDOWS] = { 0 }; int fps_p[FPS_WINDOWS] = { 0 }, fi = 0, low = 0, rescan = 0;
    double peaks[FPS_WINDOWS] = { 0 };                  /* the heaviest frame of each of the last windows (1 s) */
    int w_cand = 0, w_stable = 0;                        /* DraStic's screen width, and for how many windows */
    int boosted = 0;                                     /* during a savestate boost: the clock to go back to */
    for (;;) {
        if (--rescan <= 0) {                             /* DraStic's threads (not ours: dsf-*), once a second */
            DIR *d = opendir("/proc/self/task"); struct dirent *e; int n = 0; int nt_[MAXT]; long long nl[MAXT];
            while (d && (e = readdir(d)) && n < MAXT) {
                int tid = atoi(e->d_name); if (tid <= 0) continue;
                char p[64], comm[32]; snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid); rd_str(p, comm, sizeof comm);
                if (!strncmp(comm, "dsf-", 4) || !strncmp(comm, "data-loop", 9) || !strncmp(comm, "pw-", 3)) continue;
                nt_[n] = tid; nl[n] = -1;
                for (int k = 0; k < nt; k++) if (tids[k] == tid) nl[n] = last[k];
                n++;
            }
            if (d) closedir(d);
            memcpy(tids, nt_, sizeof(int) * n); memcpy(last, nl, sizeof(long long) * n); nt = n; rescan = 1000 / WINDOW_MS;
        }
        nanosleep(&w, 0);
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long long t = now.tv_sec * 1000000000LL + now.tv_nsec, dt = t_prev ? t - t_prev : 0;
        now_ns = t;
        t_prev = t;
        double umax = 0, usum = 0;                       /* the busiest DraStic thread, and all of them together */
        for (int k = 0; k < nt; k++) {
            long long ns = thread_ns(tids[k]);
            if (ns >= 0 && last[k] >= 0 && dt > 0) { double u = (double)(ns - last[k]) / dt; if (u > umax) umax = u; usum += u; }
            last[k] = ns;
        }
        /* presents over the last FPS_WINDOWS windows: one present of jitter in 250 ms alone reads as 56 or 64 fps */
        int pres = dsflip_presents; double fps = 60;
        if (fps_t[fi]) fps = (pres - fps_p[fi]) * 1e9 / (t - fps_t[fi]);
        fps_t[fi] = t; fps_p[fi] = pres; fi = (fi + 1) % FPS_WINDOWS;
        if (!dt) continue;
        if (boost_until > t || boosted) {               /* a savestate: the top clock, nothing counts meanwhile */
            if (boost_until > t) {
                if (!boosted) { boosted = cur; dsflip_log("[cpugov] boost (a savestate, the menu, the volume keys): %d MHz until it's done\n", hwmax_ / 1000); }
                cur = hwmax_;
            } else {
                dsflip_log("[cpugov] boost done: back to %d MHz\n", boosted / 1000);
                if (wr_int(POL "scaling_max_freq", boosted) == 0) cur = boosted;
                boosted = 0;
                for (int k = 0; k < FPS_WINDOWS; k++) { fps_t[k] = 0; peaks[k] = 0; }   /* the stall isn't this clock's */
            }
            __atomic_exchange_n(&dsflip_frame_work_max, 0, __ATOMIC_RELAXED);
            drops_prev = dsflip_queue_drops; low = 0;
            continue;
        }
        /* the memory for the resolution DraStic settled on (it starts at 1x and switches to 2x a moment later; the
         * player can switch in its menu): once it has held for 1 s. No stepping down before that. */
        { int w = dsflip_screen_w; if (w != w_cand) { w_cand = w; w_stable = 0; } else w_stable++;
          if (mem_on && w && w_stable >= 1000 / WINDOW_MS && w != mem_w) mem_load(w); }
        /* the heaviest frame decides as much as the average: a single frame over 16.7 ms makes DraStic late, it
         * catches up with a burst, and the queue drops a frame (measured: all extra drops at low clocks were those) */
        double peak = __atomic_exchange_n(&dsflip_frame_work_max, 0, __ATOMIC_RELAXED) / 16.67e6;
        last_peak = peak;
        /* a late frame makes DraStic catch up in the next window, so the drop can land a window after it */
        peaks[fi] = peak; double peak1s = 0; for (int k = 0; k < FPS_WINDOWS; k++) if (peaks[k] > peak1s) peak1s = peaks[k];
        int drops = dsflip_queue_drops, dropped = drops - drops_prev; drops_prev = drops;
        double need = cur * umax / TARGET, needp = cur * peak / PEAK_TARGET;
        /* below full speed with real work going on (all of DraStic's threads count: see "slow" below) */
        int behind = fps < 58.5 && fps > 5 && (umax > 0.5 || usum > 1.0);
        int want = fit_to(need > needp ? need : needp, soft_max && behind ? hwmax_ : fmax_);
        /* stepping down: above a soft bound, only when a lower clock fits the model too (not merely the bound) */
        int fits = soft_max ? fit_to(need > needp ? need : needp, hwmax_) : want;
        const char *why = 0;
        if (umax > HIGH) why = "busy";
        else if (peak > PEAK_HIGH) why = "heavy frame";
        else if (dropped && umax < IDLE_BUSY) {
            dsflip_log("[cpugov] drop ignored: the game isn't running (busiest thread %.0f%%)\n", umax * 100);
        }
        else if (dropped) {
            /* up one step either way (at 816 MHz frames were dropped with the heaviest at only ~40% of a refresh, in a
             * still HeartGold dialog; the floor is 1104 now), but only a CPU-bound drop is a strike (see the top) */
            int blame = peak1s >= BLAME_PEAK || umax > TARGET;
            why = blame ? "dropped" : "dropped, light frames"; if (want <= cur) want = blame ? step_up(cur) : fit(cur + 1);
            int c = idx(cur);
            int top = soft_max ? hwmax_ : fmax_;         /* (at the top there's nothing to avoid: not held against it) */
            if (c >= 0 && cur < top && blame) {
                bad_until[c] = t + ban_ns(strikes[c]); strikes[c]++;
                for (int i = 0; i < nf; i++) if (freqs[i] <= cur) proof_ms[i] = 0;   /* and lower won't do better */
                mem_save();
            } else if (c >= 0 && cur < top && bad_until[c] < t + ban_ns(0)) bad_until[c] = t + ban_ns(0);
        }
        /* below full speed with real work going on: the busiest thread alone is not the measure when a game's load
         * is in DraStic's 3D helper threads (Dragon Quest Monsters: main 38%, three helpers 35% each, presents at
         * 52-58/s and the clock stepped down to 1104 as "light"), so all of DraStic's threads together count too */
        else if (behind) {
            why = "slow"; if (want <= cur) want = step_up(cur);                              /* +1 step... */
            int c = idx(cur);                                                                /* ...and the clock that was */
            if (c >= 0 && cur < (soft_max ? hwmax_ : fmax_)) { bad_until[c] = t + ban_ns(strikes[c]); strikes[c]++; mem_save(); }   /* too slow is held
                against it like a drop: stepping back down to it after 15 s "light" and up again made a 1104/1416 see-saw */
        }
        if (!dropped) {                                  /* a window without a drop: time towards forgiveness */
            int forgiven = 0;
            for (int i = 0; i < nf; i++) if (strikes[i] && freqs[i] >= cur && (proof_ms[i] += dt / 1000000) >= FORGIVE_MS) {
                strikes[i]--; proof_ms[i] = 0; forgiven = 1;
                dsflip_log("[cpugov] %d MHz forgiven a strike (%d left): %d s at it or lower without a drop\n",
                           freqs[i] / 1000, strikes[i], FORGIVE_MS / 1000);
            }
            if (forgiven) mem_save();
        }
        if (why && want > cur) { set_clock(want, why, umax, fps); low = 0; }
        else if (fits < cur && fps >= 59 && usum < 1.2 * cur / (double)step_down(cur) && (!mem_on || mem_w)) {   /* never while below full speed, nor
                 when all of DraStic's threads together would exceed 1.2 cores at the lower clock */
            if (++low >= DOWN_AFTER) { set_clock(step_down(cur), "light", umax, fps); low = 0; } }
        else low = 0;
        if (verbose) dsflip_log("[cpugov] window: busiest %.0f%% peak frame %.0f%% fps %.1f drops %d at %d MHz, fits %d\n",
                                umax * 100, peak * 100, fps, dropped, cur / 1000, want / 1000);
    }
    return 0;
}

void cpugov_start(void) {
    const char *e = getenv("DSFLIP_CPUGOV");
    if (e && *e == '0') { dsflip_log("[cpugov] off (DSFLIP_CPUGOV=0)\n"); return; }
    verbose = getenv("DSFLIP_CPUGOV_LOG") != 0;
    { const char *m = getenv("DSFLIP_CPUGOV_MEMORY"); mem_on = !(m && *m == '0'); }
    char gov[32], avail[256];
    rd_str(POL "scaling_governor", gov, sizeof gov);
    if (strcmp(gov, "performance")) { dsflip_log("[cpugov] off: governor is %s, not performance\n", gov); return; }
    /* one clock domain only (the RK3566/RK3568's four A55s): with several clusters DraStic's threads may run on one
     * this doesn't manage, and the load/clock model below would be wrong */
    if (access("/sys/devices/system/cpu/cpufreq/policy1", F_OK) == 0 || access("/sys/devices/system/cpu/cpufreq/policy2", F_OK) == 0 ||
        access("/sys/devices/system/cpu/cpufreq/policy4", F_OK) == 0 || access("/sys/devices/system/cpu/cpufreq/policy6", F_OK) == 0) {
        dsflip_log("[cpugov] off: more than one CPU cluster\n"); return;
    }
    rd_str(POL "scaling_available_frequencies", avail, sizeof avail);
    for (char *s = strtok(avail, " "); s && nf < 24; s = strtok(0, " ")) freqs[nf++] = atoi(s);
    for (int i = 1; i < nf; i++) for (int j = i; j > 0 && freqs[j] < freqs[j - 1]; j--) { int x = freqs[j]; freqs[j] = freqs[j - 1]; freqs[j - 1] = x; }
    cur = rd_int(POL "scaling_max_freq");
    /* the hardware's top, not the current limit: a session that crashed before restore.sh must not cap this one */
    fmax_ = getenv("DSFLIP_CPU_MAX") ? atoi(getenv("DSFLIP_CPU_MAX")) : rd_int(POL "cpuinfo_max_freq");
    /* 816 MHz dropped frames at 2x even in a still scene; the step to 1104 saves little */
    fmin_ = getenv("DSFLIP_CPU_MIN") ? atoi(getenv("DSFLIP_CPU_MIN")) : 1104000;
    if (nf < 2 || cur <= 0 || access(POL "scaling_max_freq", W_OK)) { dsflip_log("[cpugov] off: no writable cpufreq\n"); return; }
    hwmax_ = rd_int(POL "cpuinfo_max_freq"); if (hwmax_ < fmax_) hwmax_ = fmax_;
    { const char *sm = getenv("DSFLIP_CPU_MAX_SOFT"); soft_max = sm && *sm == '1' && fmax_ < hwmax_; }
    dsflip_log("[cpugov] on: %d..%d MHz%s, target %.0f%% load of the busiest DraStic thread\n", fmin_ / 1000, fmax_ / 1000,
               soft_max ? " (soft: higher while the game can't keep up)" : "", TARGET * 100);
    /* a hard bound holds from the start: the session begins at the clock the menu left (often the top) */
    if (!soft_max && cur > fmax_ && wr_int(POL "scaling_max_freq", fmax_) == 0) cur = fmax_;
    gov_on = 1;
    pthread_t th;
    if (!pthread_create(&th, 0, gov_thread, 0)) pthread_setname_np(th, "dsf-cpugov");
}

/* DraStic is about to save or load a state (resume.c), the menu is in use (menu.c), the volume keys are (volume.c):
 * the top clock now, for ms from now (extends a boost) */
void cpugov_boost(int ms) {
    if (!gov_on) return;
    struct timespec n; clock_gettime(CLOCK_MONOTONIC, &n);
    long long t = n.tv_sec * 1000000000LL + n.tv_nsec, until = t + ms * 1000000LL;
    if (until <= boost_until) return;
    int fresh = boost_until <= t;
    boost_until = until;
    if (fresh) wr_int(POL "scaling_max_freq", hwmax_);  /* at once, not at the governor's next window */
}
