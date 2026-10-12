// The CPU governor (src/cpugov.cpp) against a simulated game on a made-up cpufreq folder (cpugov.sysfs): no handheld
// needed. The game has HeartGold's rhythm at 2x on an RG DS Plus: frames of 19 and 12 ms in turn at 1104 MHz, of which
// only part shrinks with the clock (12 of the 19 ms and 7 of the 12; the rest is memory), so 1104 MHz keeps up and
// 816 MHz doesn't. Time runs twice as fast as a DS's (frames of 8.36 ms, cpugov.window_ms=125) to keep it short.
//   bananads-cpugov-test <scratch dir> HeartGold.nds   (the last argument names the game, as bananads's does)
#include <pthread.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "config.h"
#include "cpugov.h"

using namespace bananads;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static void put(const std::string& path, const std::string& text) { std::ofstream(path) << text << "\n"; }
static std::string get(const std::string& path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}
static long khz_now(const std::string& pol) { return atol(get(pol + "scaling_max_freq").c_str()); }

static long long mono_ns() {  // (busy for that long: a thread's CPU clock would be a system call per read)
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

// the game: its frames on this thread, paced as the KMS frontend's emulator thread
static void game(const std::string& pol, GovFeed& feed, std::atomic<bool>& run) {
    pthread_setname_np(pthread_self(), "bds-emu");
    Pacer pace(feed, std::chrono::nanoseconds(16715180 / 2));
    bool heavy = true;
    long khz = 1992000;
    while (run) {
        if (long k = khz_now(pol); k > 0) khz = k;  // (0: read while the governor rewrote it)
        const double ms = (heavy ? 7 + 12 * 1104000.0 / khz : 5 + 7 * 1104000.0 / khz) / 2;
        const long long end = mono_ns() + (long long)(ms * 1e6);
        while (mono_ns() < end) {}
        feed.frames++;
        pace.wait();
        heavy = !heavy;
    }
}

// one game session of `seconds` from the top clock: the clock it ended at, and the frames late in its last `quiet` s
struct Result { long khz, late; };
static Result session(const std::string& dir, const std::string& pol, long min_khz, double seconds, double quiet) {
    put(pol + "scaling_max_freq", "1992000");
    Config cfg;
    cfg.set("cpugov.sysfs", pol);
    cfg.set("cpugov.window_ms", "125");
    cfg.set("cpugov.dir", dir + "/memory");
    cfg.set("cpugov.min_khz", std::to_string(min_khz));
    GovFeed feed;
    feed.scale = 2;
    std::atomic<bool> run{true};
    std::thread th([&] { game(pol, feed, run); });
    cpugov_start(cfg, &feed);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds - quiet));
    const long before = feed.late;
    std::this_thread::sleep_for(std::chrono::duration<double>(quiet));
    const Result r{khz_now(pol), feed.late - before};
    cpugov_stop();
    run = false;
    th.join();
    fprintf(stderr, "[test] min %ld MHz: at %ld MHz after %.1f s, %ld late in the last %.1f s\n", min_khz / 1000,
            r.khz / 1000, seconds, r.late, quiet);
    CHECK(khz_now(pol) == 1992000);  // put back on exit
    return r;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: bananads-cpugov-test <scratch dir> <game>.nds\n");
        return 2;
    }
    const std::string dir = argv[1], pol = dir + "/cpufreq/policy0/";
    mkdir(dir.c_str(), 0755);
    mkdir((dir + "/cpufreq").c_str(), 0755);
    mkdir(pol.c_str(), 0755);
    put(pol + "scaling_governor", "performance");
    put(pol + "scaling_available_frequencies", "408000 600000 816000 1104000 1416000 1608000 1800000 1992000 ");
    put(pol + "cpuinfo_max_freq", "1992000");
    remove((dir + "/memory/" + argv[argc - 1] + ".2x").c_str());

    // from the top clock down to 1104 MHz (four steps of a second each here) and no lower: the floor
    Result r = session(dir, pol, 1104000, 6.5, 2);
    CHECK(r.khz == 1104000);
    CHECK(r.late == 0);
    // with 816 MHz allowed: it gets there, is late, goes back up and bans 816 MHz, remembered for the game
    r = session(dir, pol, 816000, 10, 2);
    CHECK(r.khz == 1104000);
    CHECK(r.late == 0);
    const std::string mem = get(dir + "/memory/" + argv[argc - 1] + ".2x");
    CHECK(mem.find("816000 ") == 0);
    // the next session: 816 MHz still banned from the start, so it settles at 1104 MHz without being late there
    r = session(dir, pol, 816000, 6.5, 4.5);
    CHECK(r.khz == 1104000);
    CHECK(r.late == 0);

    if (fails) {
        fprintf(stderr, "%d governor checks failed\n", fails);
        return 1;
    }
    fprintf(stderr, "the governor settles at 1104 MHz and never on a clock that can't keep up\n");
    return 0;
}
