// kms.pace_phase's arithmetic (src/pacephase.h): the move that has the next frame done 3 ms before a vblank
// (so a frame that fits runs after the GPU's pass that starts at the flip), nothing when it is already there, and
// the frame time it goes by (a 90th percentile that a start-up spike doesn't hold).
#include <cmath>
#include <cstdio>

#include "pacephase.h"

using namespace bananads;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

// where a frame due at `due` starts after the vblank grid at vbl (us in [0, per))
static double phase(long long due, long long vbl, double per) {
    double p = std::fmod((double)(due - vbl), per);
    return p < 0 ? p + per : p;
}

int main() {
    const double per = 16620;          // the RG DS Plus's 60.17 Hz
    const long long vbl = 12000000000; // (CLOCK_MONOTONIC us, as the flip events give it)
    double want = -1;
    // the measured case: dsreal's race, ~6 ms frames that started 16.17 ms after the vblank (in the GPU's 6.6 ms
    // pass after the next one): to start at 7.62 ms, after the pass, done at 13.62
    long long due = vbl + 3 * (long long)per + 16170;
    double m = pace_phase_move(due, vbl, per, 6000, &want);
    CHECK(std::fabs(want - 7620) < 1);
    CHECK(std::fabs(phase(due + std::llround(m), vbl, per) - 7620) < 1);
    CHECK(std::fabs(m) <= per / 2);   // (never more than half a refresh: one frame sooner or later, once)
    // moved once, then left alone
    CHECK(pace_phase_move(due + std::llround(m), vbl, per, 6000, &want) == 0);
    // within the slack (1 ms) either side: nothing; just over it: moved there
    CHECK(pace_phase_move(vbl + 8500, vbl, per, 6000, &want) == 0);
    CHECK(pace_phase_move(vbl + 6700, vbl, per, 6000, &want) == 0);
    CHECK(std::fabs(pace_phase_move(vbl + 8700, vbl, per, 6000, &want) + 1080) < 1);
    CHECK(std::fabs(pace_phase_move(vbl + 6500, vbl, per, 6000, &want) - 1120) < 1);
    // the vblank given may be after the due time (a later flip seen first): the same answer
    CHECK(std::fabs(phase(vbl + 2400 + std::llround(pace_phase_move(vbl + 2400, vbl + 5 * (long long)per, per, 6000, &want)), vbl, per) - 7620) < 1);
    // a heavy frame (12 ms): it can't miss the pass; started at 1.62 ms it is still done 3 ms before the vblank
    pace_phase_move(vbl, vbl, per, 12000, &want);
    CHECK(std::fabs(want - 1620) < 1);
    // a frame over a refresh minus the lead: the start wraps into the refresh before
    pace_phase_move(vbl, vbl, per, 15000, &want);
    CHECK(std::fabs(want - (per - 1380)) < 1);
    CHECK(want >= 0 && want < per);

    // the frame time: the 90th percentile of the last n, in us
    float ring[1024] = {};
    CHECK(pace_phase_frame(ring, 1024, 50, 120) == 0);   // (not enough yet)
    for (unsigned k = 0; k < 300; k++) ring[k] = (k < 10) ? 40.0f : (k % 20 == 0 ? 9.0f : 5.0f);   // start-up spikes
    CHECK(std::fabs(pace_phase_frame(ring, 1024, 300, 120) - 5000) < 1);   // (start-up gone; 1 in 20 at 9 ms: the 90th is 5)
    for (unsigned k = 300; k < 420; k++) ring[k] = (k % 4 == 0) ? 9.0f : 5.0f;  // a heavier stretch
    CHECK(std::fabs(pace_phase_frame(ring, 1024, 420, 120) - 9000) < 1);
    // across the ring's wrap
    for (unsigned k = 0; k < 1024; k++) ring[k] = 4.0f;
    for (unsigned k = 1024; k < 1100; k++) ring[k % 1024] = 6.0f;
    CHECK(std::fabs(pace_phase_frame(ring, 1024, 1100, 120) - 6000) < 1);
    if (fails) return 1;
    printf("pacephase: ok\n");
    return 0;
}
