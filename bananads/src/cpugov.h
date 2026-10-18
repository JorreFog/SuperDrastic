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
    std::atomic<bool> rebase{false};  // the clock changed: the lag from before is the old clock's, and already counted
    int scale = 1;
};

// The emulator thread's pacing at the DS's rate, which also counts GovFeed::late: each whole frame the emulator falls
// further behind the schedule while it runs (one or two in one frame; more at once is a stall, a stop or a
// savestate's write, not the clock). Too far behind (or back from a stop) it starts again from now rather than run
// to catch up; so it does after the governor changes the clock (GovFeed::rebase), or the backlog the old clock left
// would be counted against the new one.
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
};

void cpugov_start(const Config& cfg, GovFeed* feed);
void cpugov_boost(int ms);  // the top clock for ms from now: savestates (a stall that isn't the clock's fault)
void cpugov_stop();         // the limit from before the game, back

}  // namespace bananads
