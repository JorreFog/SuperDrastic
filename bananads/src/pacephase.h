// kms.pace_phase (frontend_kms.cpp): where in the refresh the emulator's next frame should start, and how far the
// pacer's schedule has to move to start it there.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace bananads {

// due: when the next frame is due; vbl: a vblank of the panel the times are measured against (both us, the same
// clock); per: the refresh; frame: a frame's time (us, its 90th percentile: pace_phase_frame). The frame should be
// done `lead` before a vblank: the GPU's pass (which starts at each flip) then never overlaps its end, and a frame
// that fits the refresh after the pass runs while the GPU is idle (the 2D thread's last lines get the lead).
// Returns how far to move the schedule (us, + = later; 0 when it is within `slack` of that) and where the frame
// should start after the vblank (*want, us in [0, per)).
inline double pace_phase_move(long long due, long long vbl, double per, double frame, double* want,
                              double lead = 3000.0, double slack = 1000.0) {
    double w = std::fmod(per - frame - lead, per);
    if (w < 0) w += per;
    if (want) *want = w;
    const double e = std::remainder((double)(due - vbl) - w, per);   // (-per/2, per/2]: + = it starts too late
    return std::fabs(e) > slack ? -e : 0.0;
}

// the 90th percentile of the last n of a ring of frame times (ms; ring[k % size] is the k-th, `count` written so
// far), in us; 0 when fewer than n are there. A start-up or loading spike moves it for no more than n frames
inline double pace_phase_frame(const float* ring, unsigned size, unsigned count, unsigned n) {
    if (count < n || n == 0 || n > size) return 0;
    std::vector<float> v(n);
    for (unsigned i = 0; i < n; i++) v[i] = ring[(count - n + i) % size];
    auto k = v.begin() + (n * 9) / 10;
    std::nth_element(v.begin(), k, v.end());
    return *k * 1000.0;
}

}  // namespace bananads
