/* DraStic's frame wait is (last + 14 ms - now) truncated to 32-bit microseconds. A wall-clock gap past
 * 2^31 us wraps that into a long positive sleep. clock_gap_apply() removes those gaps. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/clockgap.h"

static int fails;

static void expect(const char *what, long long got, long long want) {
    if (got == want) return;
    fprintf(stderr, "FAIL %s: got %lld want %lld\n", what, got, want);
    fails++;
}

/* the wait DraStic computes, in microseconds: a positive value is how long it sleeps */
static int32_t drastic_wait(long long last, long long now) {
    return (int32_t)(last + 14000 - now);
}

int main(void) {
    long long last = 0, cut = 0, gap = 0, shown;
    const long long t0 = 1700000000LL * 1000000;   /* a present-day gettimeofday, in us */

    shown = clock_gap_apply(t0, &last, &cut, &gap);
    expect("first sample is the raw time", shown, t0);
    expect("first sample is not a gap", gap, 0);

    /* a frame later: 16 ms stays, and the frame wait is one frame */
    shown = clock_gap_apply(t0 + 16000, &last, &cut, &gap);
    expect("a frame is kept", shown, t0 + 16000);
    expect("a frame is not a gap", gap, 0);
    expect("frame wait", drastic_wait(t0, shown), 14000 - 16000);

    /* exactly one second is still a real pause DraStic should see */
    shown = clock_gap_apply(t0 + 16000 + 1000000, &last, &cut, &gap);
    expect("one second is kept", shown, t0 + 16000 + 1000000);
    expect("one second is not a gap", gap, 0);

    /* 40 minutes asleep. Raw, the 32-bit wait wraps to about half an hour. Adjusted, one millisecond. */
    long long slept = t0 + 16000 + 1000000 + 40LL * 60 * 1000000;
    long long before = shown;
    int32_t raw_wait = drastic_wait(before, slept);
    if (raw_wait < 30 * 60 * 1000000) {
        fprintf(stderr, "FAIL raw 40 min wait should be a long sleep, got %d\n", raw_wait);
        fails++;
    }
    shown = clock_gap_apply(slept, &last, &cut, &gap);
    expect("40 min gap removed down to 1 ms", shown, before + 1000);
    expect("40 min gap reported", gap, slept - (t0 + 16000 + 1000000));
    expect("wait after the gap is 13 ms", drastic_wait(before, shown), 13000);

    /* the next frame still advances by a frame, not by the removed sleep */
    shown = clock_gap_apply(slept + 16000, &last, &cut, &gap);
    expect("frame after the sleep", shown, before + 1000 + 16000);
    expect("no second gap", gap, 0);

    /* a clock set backwards is also one millisecond, not a jump into the past */
    before = shown;
    shown = clock_gap_apply(slept + 16000 - 5LL * 60 * 1000000, &last, &cut, &gap);
    expect("backward step becomes 1 ms", shown, before + 1000);
    if (gap >= 0) { fprintf(stderr, "FAIL backward gap should be negative, got %lld\n", gap); fails++; }

    /* an hour asleep from a fresh clock: the measured case (MKDS frozen for minutes) */
    last = cut = 0;
    shown = clock_gap_apply(t0, &last, &cut, &gap);
    long long hour = t0 + 3600LL * 1000000;
    raw_wait = drastic_wait(shown, hour);
    if (raw_wait < 60 * 1000000) {
        fprintf(stderr, "FAIL raw 1 h wait should exceed a minute, got %d\n", raw_wait);
        fails++;
    }
    before = shown;
    shown = clock_gap_apply(hour, &last, &cut, &gap);
    expect("1 h gap removed down to 1 ms", shown, before + 1000);
    expect("wait after an hour is 13 ms", drastic_wait(before, shown), 13000);

    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("clock-gap: ok\n");
    return 0;
}
