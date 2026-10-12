// The CPU governor (cpugov.cpp): the lowest clock that keeps the game at full speed, set through scaling_max_freq
// under the "performance" governor, remembered per game; the clock from before is put back on exit.
#pragma once
#include <atomic>

#include "config.h"

namespace bananads {

// What the frontend tells the governor: frames shown so far, frames it dropped, the heaviest frame's CPU time on the
// emulator's thread since the governor last took it, and the render scale (the memory is per 1x/2x).
struct GovFeed {
    std::atomic<long> frames{0}, drops{0};
    std::atomic<long long> work_max_ns{0};
    int scale = 1;
};

void cpugov_start(const Config& cfg, GovFeed* feed);
void cpugov_boost(int ms);  // the top clock for ms from now: savestates (a stall that isn't the clock's fault)
void cpugov_stop();         // the limit from before the game, back

}  // namespace bananads
