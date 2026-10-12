// The lowest CPU clock that keeps the game at full speed: SuperDrastic's governor (src/cpugov.c, plan-1.4 section 7),
// fed by BananaDS's KMS frontend instead of DraStic's presents. Read SuperDrastic's file for the measurements behind
// every constant; in short:
//
// BananaLinux runs DS games under the "performance" governor (banana-perf), all four A55s at the top clock, on the
// understanding that the emulator lowers it per game (docs/EMULATORS.md). Every 250 ms this thread reads the CPU time
// of each of BananaDS's threads, the heaviest frame's CPU time on the emulator's thread, the frames shown and the
// frames the queue dropped. The busiest thread's load u at clock f predicts the clock that loads it to TARGET
// (f * u / TARGET), and the heaviest frame must fit PEAK_TARGET of a refresh the same way. Up at once when a window
// needs it (busy, a heavy frame, a drop, below full speed); down one step only after DOWN_AFTER windows in a row that
// all fit lower. A clock that dropped a CPU-bound frame is banned for 30 s (doubling per further drop, up to 10 min),
// remembered per game and scale in <cpugov.dir>/<rom>.<1x|2x>; two minutes at it or lower without a drop forgive one
// strike. A savestate boosts to the top clock while it is written (cpugov_boost).
//
// Settings: cpugov (on), cpugov.min_khz (1104000: 816 MHz dropped frames at 2x even in a still scene),
// cpugov.max_khz (the hardware's top), cpugov.max_soft (off: past max_khz while the game can't keep up),
// cpugov.memory (on), cpugov.dir ($BANANADS_DATA/cpugov), cpugov.log (off: a line per window).
#include "cpugov.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

#define POL "/sys/devices/system/cpu/cpufreq/policy0/"
static const int WINDOW_MS = 250;
static const double TARGET = 0.72, HIGH = 0.85, PEAK_TARGET = 0.80, PEAK_HIGH = 0.95, BLAME_PEAK = 0.80, IDLE_BUSY = 0.25;
static const int BAD_FOR_S = 30, BAD_MAX_S = 600, FORGIVE_MS = 120000, MAX_STRIKES = 6, FPS_WINDOWS = 4, DOWN_AFTER = 8;
static const int MAXT = 64, NF = 24;

static int freqs[NF], nf, cur, fmin_, fmax_, hwmax_, before_ = -1;
static long long bad_until[NF], now_ns;
static int strikes[NF], proof_ms[NF];
static bool verbose, soft_max, mem_on = true;
static std::atomic<long long> boost_until{0};
static std::atomic<bool> gov_on{false}, gov_run{false};
static std::thread gov_th;
static GovFeed* feed_;
static std::string mem_dir, mem_rom, mem_path;
static double last_peak;

static long long ban_ns(int k) {
    long long s = (long long)BAD_FOR_S << (k < 5 ? k : 5);
    return (s > BAD_MAX_S ? BAD_MAX_S : s) * 1000000000LL;
}
static int rd_int(const char* p) {
    FILE* f = fopen(p, "r");
    int v = 0;
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = 0;
        fclose(f);
    }
    return v;
}
static int wr_int(const char* p, int v) {
    FILE* f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "%d\n", v);
    return fclose(f);
}
static std::string rd_str(const char* p) {
    char b[256] = "";
    FILE* f = fopen(p, "r");
    if (f) {
        if (fgets(b, sizeof b, f)) b[strcspn(b, "\n")] = 0;
        fclose(f);
    }
    return b;
}

static void set_clock(int khz, const char* why, double u, double fps) {
    if (khz == cur) return;
    if (wr_int(POL "scaling_max_freq", khz)) {
        Log(LogLevel::Warn, "[cpugov] can't set %d kHz\n", khz);
        return;
    }
    Log(LogLevel::Info, "[cpugov] %d -> %d MHz (%s: busiest thread %.0f%%, heaviest frame %.0f%% of 16.7 ms, %.1f fps)\n",
        cur / 1000, khz / 1000, why, u * 100, last_peak * 100, fps);
    cur = khz;
}
static int fit_to(double khz, int top) {
    for (int i = 0; i < nf; i++)
        if (freqs[i] >= fmin_ && freqs[i] <= top && freqs[i] >= khz) return freqs[i];
    return top;
}
static int fit(double khz) { return fit_to(khz, fmax_); }
static int step_up(int khz) { return fit_to(khz + 1, soft_max ? hwmax_ : fmax_); }
static int idx(int khz) {
    for (int i = 0; i < nf; i++)
        if (freqs[i] == khz) return i;
    return -1;
}

// ---- the memory ----
static void mem_save() {
    if (mem_path.empty()) return;
    bool any = false;
    for (int i = 0; i < nf; i++) any |= strikes[i] != 0;
    if (!any) {
        unlink(mem_path.c_str());
        return;
    }
    std::string tmp = mem_path + ".new";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    for (int i = 0; i < nf; i++)
        if (strikes[i]) fprintf(f, "%d %d\n", freqs[i], strikes[i] < MAX_STRIKES ? strikes[i] : MAX_STRIKES);
    if (fclose(f) == 0) rename(tmp.c_str(), mem_path.c_str());
    else unlink(tmp.c_str());
}
static void mem_load(int scale) {
    for (int i = 0; i < nf; i++) strikes[i] = 0, bad_until[i] = 0, proof_ms[i] = 0;
    mem_path.clear();
    if (!mem_on || mem_rom.empty()) return;
    mkdir(mem_dir.c_str(), 0755);
    mem_path = mem_dir + "/" + mem_rom + (scale > 1 ? ".2x" : ".1x");
    std::string said;
    if (FILE* f = fopen(mem_path.c_str(), "r")) {
        int khz, k;
        while (fscanf(f, "%d %d", &khz, &k) == 2) {
            int i = idx(khz);
            if (i < 0 || k <= 0) continue;
            strikes[i] = k < MAX_STRIKES ? k : MAX_STRIKES;
            bad_until[i] = now_ns + ban_ns(strikes[i] - 1);
            char b[96];
            snprintf(b, sizeof b, ", %d MHz %d (not for %llds)", khz / 1000, strikes[i], ban_ns(strikes[i] - 1) / 1000000000LL);
            said += b;
        }
        fclose(f);
    }
    Log(LogLevel::Info, "[cpugov] remembered drops for %s%s\n", mem_path.c_str(), said.empty() ? ": none" : said.c_str());
}
// one step down, unless that clock dropped a frame recently
static int step_down(int khz) {
    int best = khz;
    for (int i = 0; i < nf; i++)
        if (freqs[i] < khz && freqs[i] >= fmin_ && (best == khz || freqs[i] > best)) best = freqs[i];
    int b = idx(best);
    return b >= 0 && bad_until[b] > now_ns ? khz : best;
}

static long long thread_ns(int tid) {
    timespec t;
    clockid_t c = ((~(clockid_t)tid) << 3) | 6;  // CPUCLOCK_PERTHREAD | CPUCLOCK_SCHED: any thread of ours
    if (clock_gettime(c, &t)) return -1;
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void gov_loop() {
    pthread_setname_np(pthread_self(), "bds-cpugov");
    int tids[MAXT];
    long long last[MAXT];
    int nt = 0;
    long drops_prev = feed_->drops;
    long long t_prev = 0, fps_t[FPS_WINDOWS] = {0};
    long fps_p[FPS_WINDOWS] = {0};
    int fi = 0, low = 0, rescan = 0, boosted = 0;
    double peaks[FPS_WINDOWS] = {0};
    {
        timespec n;
        clock_gettime(CLOCK_MONOTONIC, &n);
        now_ns = n.tv_sec * 1000000000LL + n.tv_nsec;
    }
    mem_load(feed_->scale);
    while (gov_run) {
        if (--rescan <= 0) {  // BananaDS's threads (not the governor or the sound's), once a second
            DIR* d = opendir("/proc/self/task");
            int n = 0, nt_[MAXT];
            long long nl[MAXT];
            while (dirent* e = d ? readdir(d) : nullptr) {
                if (n >= MAXT) break;
                int tid = atoi(e->d_name);
                if (tid <= 0) continue;
                char p[64];
                snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
                std::string comm = rd_str(p);
                if (comm == "bds-cpugov" || comm == "bds-audio" || comm == "bds-mic") continue;
                nt_[n] = tid;
                nl[n] = -1;
                for (int k = 0; k < nt; k++)
                    if (tids[k] == tid) nl[n] = last[k];
                n++;
            }
            if (d) closedir(d);
            memcpy(tids, nt_, sizeof(int) * n);
            memcpy(last, nl, sizeof(long long) * n);
            nt = n;
            rescan = 1000 / WINDOW_MS;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(WINDOW_MS));
        if (!gov_run) break;
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long t = now.tv_sec * 1000000000LL + now.tv_nsec, dt = t_prev ? t - t_prev : 0;
        now_ns = t;
        t_prev = t;
        double umax = 0, usum = 0;
        for (int k = 0; k < nt; k++) {
            long long ns = thread_ns(tids[k]);
            if (ns >= 0 && last[k] >= 0 && dt > 0) {
                double u = (double)(ns - last[k]) / dt;
                if (u > umax) umax = u;
                usum += u;
            }
            last[k] = ns;
        }
        long pres = feed_->frames;
        double fps = 60;
        if (fps_t[fi]) fps = (pres - fps_p[fi]) * 1e9 / (t - fps_t[fi]);
        fps_t[fi] = t;
        fps_p[fi] = pres;
        fi = (fi + 1) % FPS_WINDOWS;
        if (!dt) continue;
        if (boost_until > t || boosted) {  // a savestate: the top clock, nothing counts meanwhile
            if (boost_until > t) {
                if (!boosted) {
                    boosted = cur;
                    Log(LogLevel::Info, "[cpugov] savestate: %d MHz until it's done\n", hwmax_ / 1000);
                }
                cur = hwmax_;
            } else {
                Log(LogLevel::Info, "[cpugov] savestate done: back to %d MHz\n", boosted / 1000);
                if (wr_int(POL "scaling_max_freq", boosted) == 0) cur = boosted;
                boosted = 0;
                for (int k = 0; k < FPS_WINDOWS; k++) fps_t[k] = 0, peaks[k] = 0;
            }
            feed_->work_max_ns = 0;
            drops_prev = feed_->drops;
            low = 0;
            continue;
        }
        double peak = feed_->work_max_ns.exchange(0) / 16.67e6;
        last_peak = peak;
        peaks[fi] = peak;
        double peak1s = 0;
        for (int k = 0; k < FPS_WINDOWS; k++)
            if (peaks[k] > peak1s) peak1s = peaks[k];
        long drops = feed_->drops, dropped = drops - drops_prev;
        drops_prev = drops;
        double need = cur * umax / TARGET, needp = cur * peak / PEAK_TARGET;
        bool behind = fps < 58.5 && fps > 5 && (umax > 0.5 || usum > 1.0);
        int want = fit_to(need > needp ? need : needp, soft_max && behind ? hwmax_ : fmax_);
        int fits = soft_max ? fit_to(need > needp ? need : needp, hwmax_) : want;
        const char* why = nullptr;
        if (umax > HIGH) why = "busy";
        else if (peak > PEAK_HIGH) why = "heavy frame";
        else if (dropped && umax < IDLE_BUSY)
            Log(LogLevel::Info, "[cpugov] drop ignored: the game isn't running (busiest thread %.0f%%)\n", umax * 100);
        else if (dropped) {
            bool blame = peak1s >= BLAME_PEAK || umax > TARGET;
            why = blame ? "dropped" : "dropped, light frames";
            if (want <= cur) want = blame ? step_up(cur) : fit(cur + 1);
            int c = idx(cur);
            int top = soft_max ? hwmax_ : fmax_;
            if (c >= 0 && cur < top && blame) {
                bad_until[c] = t + ban_ns(strikes[c]);
                strikes[c]++;
                for (int i = 0; i < nf; i++)
                    if (freqs[i] <= cur) proof_ms[i] = 0;
                mem_save();
            } else if (c >= 0 && cur < top && bad_until[c] < t + ban_ns(0))
                bad_until[c] = t + ban_ns(0);
        } else if (behind) {
            why = "slow";
            if (want <= cur) want = step_up(cur);
            int c = idx(cur);
            if (c >= 0 && cur < (soft_max ? hwmax_ : fmax_)) {
                bad_until[c] = t + ban_ns(strikes[c]);
                strikes[c]++;
                mem_save();
            }
        }
        if (!dropped) {
            bool forgiven = false;
            for (int i = 0; i < nf; i++)
                if (strikes[i] && freqs[i] >= cur && (proof_ms[i] += dt / 1000000) >= FORGIVE_MS) {
                    strikes[i]--;
                    proof_ms[i] = 0;
                    forgiven = true;
                    Log(LogLevel::Info, "[cpugov] %d MHz forgiven a strike (%d left)\n", freqs[i] / 1000, strikes[i]);
                }
            if (forgiven) mem_save();
        }
        if (why && want > cur) {
            set_clock(want, why, umax, fps);
            low = 0;
        } else if (fits < cur && fps >= 59 && usum < 1.2 * cur / (double)step_down(cur)) {
            if (++low >= DOWN_AFTER) {
                set_clock(step_down(cur), "light", umax, fps);
                low = 0;
            }
        } else
            low = 0;
        if (verbose)
            Log(LogLevel::Info, "[cpugov] window: busiest %.0f%% peak frame %.0f%% fps %.1f drops %ld at %d MHz, fits %d\n",
                umax * 100, peak * 100, fps, dropped, cur / 1000, want / 1000);
    }
}

void cpugov_start(const Config& cfg, GovFeed* feed) {
    if (!cfg.flag("cpugov", true)) {
        Log(LogLevel::Info, "[cpugov] off (cpugov=off)\n");
        return;
    }
    verbose = cfg.flag("cpugov.log", false);
    mem_on = cfg.flag("cpugov.memory", true);
    std::string gov = rd_str(POL "scaling_governor");
    if (gov != "performance") {
        Log(LogLevel::Info, "[cpugov] off: the governor is %s, not performance\n", gov.empty() ? "unknown" : gov.c_str());
        return;
    }
    // one clock domain only (the RK3566/RK3568's four A55s)
    for (const char* p : {"policy1", "policy2", "policy4", "policy6"})
        if (access((std::string("/sys/devices/system/cpu/cpufreq/") + p).c_str(), F_OK) == 0) {
            Log(LogLevel::Info, "[cpugov] off: more than one CPU cluster\n");
            return;
        }
    std::string avail = rd_str(POL "scaling_available_frequencies");
    nf = 0;
    for (char* s = strtok(&avail[0], " "); s && nf < NF; s = strtok(nullptr, " ")) freqs[nf++] = atoi(s);
    for (int i = 1; i < nf; i++)
        for (int j = i; j > 0 && freqs[j] < freqs[j - 1]; j--) std::swap(freqs[j], freqs[j - 1]);
    cur = before_ = rd_int(POL "scaling_max_freq");
    hwmax_ = rd_int(POL "cpuinfo_max_freq");
    fmax_ = (int)cfg.num("cpugov.max_khz", hwmax_);
    fmin_ = (int)cfg.num("cpugov.min_khz", 1104000);
    if (nf < 2 || cur <= 0 || access(POL "scaling_max_freq", W_OK)) {
        Log(LogLevel::Info, "[cpugov] off: no writable cpufreq\n");
        before_ = -1;
        return;
    }
    if (hwmax_ < fmax_) hwmax_ = fmax_;
    soft_max = cfg.flag("cpugov.max_soft", false) && fmax_ < hwmax_;
    // the memory: per game (the ROM's file name) and scale
    const char* data = getenv("BANANADS_DATA");
    mem_dir = cfg.str("cpugov.dir", std::string(data && *data ? data : ".") + "/cpugov");
    {
        char cmd[4096];
        FILE* f = fopen("/proc/self/cmdline", "r");
        size_t n = f ? fread(cmd, 1, sizeof cmd - 1, f) : 0;
        if (f) fclose(f);
        cmd[n] = 0;
        int argc = 0;
        for (size_t i = 0; i < n; i += strlen(cmd + i) + 1)
            if (cmd[i] && argc++) {  // the last argument: the ROM
                const char* b = strrchr(cmd + i, '/');
                mem_rom = b ? b + 1 : cmd + i;
            }
    }
    Log(LogLevel::Info, "[cpugov] on: %d..%d MHz%s, target %.0f%% load of the busiest thread\n", fmin_ / 1000,
        fmax_ / 1000, soft_max ? " (soft: higher while the game can't keep up)" : "", TARGET * 100);
    if (!soft_max && cur > fmax_ && wr_int(POL "scaling_max_freq", fmax_) == 0) cur = fmax_;
    feed_ = feed;
    gov_on = true;
    gov_run = true;
    gov_th = std::thread(gov_loop);
}

void cpugov_boost(int ms) {
    if (!gov_on) return;
    timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    long long t = n.tv_sec * 1000000000LL + n.tv_nsec, until = t + ms * 1000000LL;
    if (until <= boost_until) return;
    bool fresh = boost_until <= t;
    boost_until = until;
    if (fresh) wr_int(POL "scaling_max_freq", hwmax_);  // at once, not at the next window
}

void cpugov_stop() {
    if (!gov_on) return;
    gov_run = false;
    if (gov_th.joinable()) gov_th.join();
    gov_on = false;
    if (before_ > 0 && wr_int(POL "scaling_max_freq", before_) == 0)
        Log(LogLevel::Info, "[cpugov] back to %d MHz\n", before_ / 1000);
}

}  // namespace bananads
