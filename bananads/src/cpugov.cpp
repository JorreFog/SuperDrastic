// The lowest CPU clock that keeps the game at full speed. The frame is SuperDrastic's governor (src/cpugov.c, plan-1.4
// section 7: the clock steps, the bans and their memory, the savestate boost), fed by BananaDS's KMS frontend; what it
// decides on is its own:
//
// BananaLinux runs DS games under the "performance" governor (banana-perf), all four A55s at the top clock, on the
// understanding that the emulator lowers it per game (docs/EMULATORS.md). It decides on what a clock costs where it
// is, not on a prediction: every 250 ms it reads the frames the emulator fell behind the DS's schedule (GovFeed::late,
// counted by the frontend's emulator thread: each whole frame once, stalls left out) and the frames emulated. Any
// late frame, or under 58.5 emulated frames a second over the last second while the game runs: one step up at once
// (two for three late frames or more), and the clock it was at is banned for 30 s (doubling per further strike, up
// to 10 min), remembered per game and scale in <cpugov.dir>/<rom>.<1x|2x>. Clean (no late frame, full speed) for
// DOWN_AFTER windows in a row: one step down, unless that clock is banned. Two minutes clean at a clock or lower
// forgive it one strike. A savestate boosts to the top clock while it is written (cpugov_boost).
//
// A prediction from the load (SuperDrastic's: the busiest thread's load and the heaviest frame scaled linearly to a
// lower clock, with headroom on top) held HeartGold at 1800 MHz although 1104 MHz keeps up: the emulator's time per
// frame doesn't shrink in proportion to the clock (its memory waits don't), and the frame queue makes a frame that
// takes longer than a refresh harmless as long as the next one is quicker. Only the frames actually lost say that.
//
// Settings: cpugov (on), cpugov.min_khz (1104000: 816 MHz dropped frames at 2x even in a still scene),
// cpugov.max_khz (the hardware's top), cpugov.max_soft (off: past max_khz while the game can't keep up),
// cpugov.memory (on), cpugov.dir ($BANANADS_DATA/cpugov), cpugov.log (off: a line per window).
#include "cpugov.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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

static std::string pol_dir = "/sys/devices/system/cpu/cpufreq/policy0/";  // cpugov.sysfs (the tests' own)
static std::string pol(const char* f) { return pol_dir + f; }
static int window_ms = 250;  // cpugov.window_ms (the tests')
static const double IDLE_BUSY = 0.25, SLOW_FPS = 58.5;
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

static std::string fps_str(double fps) {  // (not measured yet since the last change: -)
    char b[16] = "-";
    if (fps >= 0) snprintf(b, sizeof b, "%.1f", fps);
    return b;
}
static void set_clock(int khz, const char* why, long late, double u, double fps) {
    if (khz == cur) return;
    if (wr_int(pol("scaling_max_freq").c_str(), khz)) {
        Log(LogLevel::Warn, "[cpugov] can't set %d kHz\n", khz);
        return;
    }
    Log(LogLevel::Info, "[cpugov] %d -> %d MHz (%s: %ld frames late, busiest thread %.0f%%, %s fps)\n", cur / 1000,
        khz / 1000, why, late, u * 100, fps_str(fps).c_str());
    cur = khz;
}
static int fit_to(double khz, int top) {
    for (int i = 0; i < nf; i++)
        if (freqs[i] >= fmin_ && freqs[i] <= top && freqs[i] >= khz) return freqs[i];
    return top;
}
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
    Log(LogLevel::Info, "[cpugov] remembered strikes for %s%s\n", mem_path.c_str(), said.empty() ? ": none" : said.c_str());
}
// one step down, unless that clock was late recently
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
    long late_prev = feed_->late;
    long long t_prev = 0, fps_t[FPS_WINDOWS] = {0};
    long fps_p[FPS_WINDOWS] = {0};
    // the frame rate over the last FPS_WINDOWS windows (a second: over one window, a frame more or less is 4 fps),
    // unknown (-1) until there are that many since it started again (a new clock, a savestate)
    auto restart_fps = [&] {
        for (int k = 0; k < FPS_WINDOWS; k++) fps_t[k] = 0;
    };
    int fi = 0, low = 0, rescan = 0, boosted = 0;
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
            rescan = std::max(1, 1000 / window_ms);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
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
        double fps = -1;
        if (fps_t[fi] && t > fps_t[fi]) fps = (pres - fps_p[fi]) * 1e9 / (t - fps_t[fi]);
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
                if (wr_int(pol("scaling_max_freq").c_str(), boosted) == 0) cur = boosted;
                boosted = 0;
                restart_fps();
            }
            late_prev = feed_->late;
            low = 0;
            continue;
        }
        const long late_now = feed_->late, late = late_now - late_prev;
        late_prev = late_now;
        const bool running = umax >= IDLE_BUSY;
        const bool slow = fps >= 0 && fps < SLOW_FPS && fps > 5 && (umax > 0.5 || usum > 1.0);
        const int top = soft_max ? hwmax_ : fmax_;
        const char* why = nullptr;
        int want = cur;
        if (late && !running)
            Log(LogLevel::Info, "[cpugov] %ld late frames ignored: the game isn't running (busiest thread %.0f%%)\n", late,
                umax * 100);
        else if (late || slow) {
            why = late ? "late" : "slow";
            want = step_up(cur);
            if (late >= 3 || (fps >= 0 && fps < 50)) want = step_up(want);
            int c = idx(cur);
            if (c >= 0 && cur < top) {
                bad_until[c] = t + ban_ns(strikes[c]);
                strikes[c]++;
                for (int i = 0; i < nf; i++)
                    if (freqs[i] <= cur) proof_ms[i] = 0;
                mem_save();
            }
        }
        if (!why && !late) {
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
        const int was = cur;
        if (why) {
            set_clock(want, why, late, umax, fps);
            low = 0;
        } else if (!late && (fps < 0 || fps >= 59)) {  // clean: the next clock down once it has been for DOWN_AFTER windows
            if (++low >= DOWN_AFTER) {
                set_clock(step_down(cur), "clean", late, umax, fps);
                low = 0;
            }
        } else
            low = 0;
        if (cur != was) {  // what comes next is the new clock's: the backlog and the frame rate start again
            feed_->rebase = true;
            restart_fps();
        }
        if (verbose)
            Log(LogLevel::Info, "[cpugov] window: %ld late, busiest %.0f%%, %s fps at %d MHz\n", late, umax * 100,
                fps_str(fps).c_str(), cur / 1000);
    }
}

Pacer::Pacer(GovFeed& feed, std::chrono::nanoseconds period)
    : feed_(feed), period_(period), due_(std::chrono::steady_clock::now()) {}

void Pacer::wait() {
    due_ += period_;
    const auto now = std::chrono::steady_clock::now();
    if (feed_.rebase.exchange(false) && now > due_) {
        due_ = now;
        behind_ = 0;
    }
    const long b = now > due_ ? (long)((now - due_) / period_) : 0;
    if (b > behind_ && b - behind_ <= 2) feed_.late += b - behind_;
    behind_ = b;
    if (due_ > now) std::this_thread::sleep_until(due_);
    else if (now - due_ > period_ * 4) {  // too far behind (or back from a stop): don't run to catch up
        due_ = now;
        behind_ = 0;
    }
}

void cpugov_start(const Config& cfg, GovFeed* feed) {
    if (!cfg.flag("cpugov", true)) {
        Log(LogLevel::Info, "[cpugov] off (cpugov=off)\n");
        return;
    }
    verbose = cfg.flag("cpugov.log", false);
    pol_dir = cfg.str("cpugov.sysfs", pol_dir);
    window_ms = std::max(10, (int)cfg.num("cpugov.window_ms", 250));
    mem_on = cfg.flag("cpugov.memory", true);
    std::string gov = rd_str(pol("scaling_governor").c_str());
    if (gov != "performance") {
        Log(LogLevel::Info, "[cpugov] off: the governor is %s, not performance\n", gov.empty() ? "unknown" : gov.c_str());
        return;
    }
    // one clock domain only (the RK3566/RK3568's four A55s)
    for (const char* p : {"policy1", "policy2", "policy4", "policy6"})
        if (access((pol_dir + "../" + p).c_str(), F_OK) == 0) {
            Log(LogLevel::Info, "[cpugov] off: more than one CPU cluster\n");
            return;
        }
    std::string avail = rd_str(pol("scaling_available_frequencies").c_str());
    nf = 0;
    for (char* s = strtok(&avail[0], " "); s && nf < NF; s = strtok(nullptr, " ")) freqs[nf++] = atoi(s);
    for (int i = 1; i < nf; i++)
        for (int j = i; j > 0 && freqs[j] < freqs[j - 1]; j--) std::swap(freqs[j], freqs[j - 1]);
    cur = before_ = rd_int(pol("scaling_max_freq").c_str());
    hwmax_ = rd_int(pol("cpuinfo_max_freq").c_str());
    fmax_ = (int)cfg.num("cpugov.max_khz", hwmax_);
    fmin_ = (int)cfg.num("cpugov.min_khz", 1104000);
    if (nf < 2 || cur <= 0 || access(pol("scaling_max_freq").c_str(), W_OK)) {
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
    Log(LogLevel::Info, "[cpugov] on: %d..%d MHz%s, down a step after %d s without a late frame\n", fmin_ / 1000,
        fmax_ / 1000, soft_max ? " (soft: higher while the game can't keep up)" : "", DOWN_AFTER * window_ms / 1000);
    if (!soft_max && cur > fmax_ && wr_int(pol("scaling_max_freq").c_str(), fmax_) == 0) cur = fmax_;
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
    if (fresh) wr_int(pol("scaling_max_freq").c_str(), hwmax_);  // at once, not at the next window
}

void cpugov_stop() {
    if (!gov_on) return;
    gov_run = false;
    if (gov_th.joinable()) gov_th.join();
    gov_on = false;
    if (before_ > 0 && wr_int(pol("scaling_max_freq").c_str(), before_) == 0)
        Log(LogLevel::Info, "[cpugov] back to %d MHz\n", before_ / 1000);
}

}  // namespace bananads
