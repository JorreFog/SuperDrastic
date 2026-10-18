// The CPU governor (cpugov.cpp): the lowest clock that keeps the game at full speed, set through scaling_max_freq
// under the "performance" governor, remembered per game; the clock from before is put back on exit.
#pragma once
#include <atomic>
#include <chrono>

#include "config.h"

namespace bananads {

// What the frontend tells the governor: frames emulated so far, the frames the emulator fell behind the DS's schedule
// while running (each whole frame once: what a too-low clock costs, measured at the clock it's at), and the render
// scale (the memory is per 1x/2x).
struct GovFeed {
    std::atomic<long> frames{0}, late{0};
    // refreshes the panels showed no new frame for (the KMS presenter's count): a frame can be on time for the DS's
    // schedule and still miss its refresh, e.g. Golden Sun: Dark Dawn's 26-29 ms town frames after 11 ms ones
    std::atomic<long> repeats{0};
    // the emulator's work per frame (ms, run to publish), the last NFT frames: the governor steps down only while
    // the slowest of them would still fit at the lower clock (cpugov.tail)
    static const int NFT = 1024;
    float frame_ms[NFT] = {};
    std::atomic<unsigned> nframe_ms{0};
    void frame_time(float ms) {
        const unsigned n = nframe_ms.load(std::memory_order_relaxed);
        frame_ms[n % NFT] = ms;
        nframe_ms.store(n + 1, std::memory_order_release);
    }
    std::atomic<bool> rebase{false};  // the clock changed: the lag from before is the old clock's, and already counted
    std::atomic<bool> catching{false};  // the pacer is catching frames up (busy by design: not a sign of the limit)
    int scale = 1;
};

// The emulator thread's pacing at the DS's rate, which also counts GovFeed::late: each whole frame the emulator falls
// further behind the schedule while it runs (one or two in one frame; more at once is a stall, a stop or a
// savestate's write, not the clock). Up to CATCH_UP frames behind it runs the frames it owes back to back, so the
// emulated time (and the sound, which can only be as fast as it) gets back in step and the sound buffer refills;
// further behind (or back from a stop) it starts again from now. After the governor changes the clock
// (GovFeed::rebase) it still catches up, but the lag from before isn't counted against the new clock: until it has
// caught up, only falling two frames further behind than it was counts.
class Pacer {
public:
    explicit Pacer(GovFeed& feed, std::chrono::nanoseconds period = std::chrono::nanoseconds(16715180));  // 59.8261 Hz
    void wait();  // after each frame: counts it, then sleeps until the next one is due
    void set_speed(int times);  // fast-forward: the frames due this many times as often (1: the normal rate)

private:
    GovFeed& feed_;
    std::chrono::nanoseconds base_, period_;
    std::chrono::steady_clock::time_point due_;
    long behind_ = 0;  // whole frames behind after the last frame
    long peak_ = 0;          // catching up after a clock change: the most it has been behind since
    bool catching_ = false;
    static const int CATCH_UP = 8;  // 134 ms: a clock's step up after a late frame, with the sound buffer's 60 ms
};

void cpugov_start(const Config& cfg, GovFeed* feed);
void cpugov_boost(int ms);  // the top clock for ms from now: savestates (a stall that isn't the clock's fault)
void cpugov_stop();         // the limit from before the game, back

}  // namespace bananads
