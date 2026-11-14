/* DraStic paces frames with gettimeofday and waits "last + 14 ms - now" in 32-bit microseconds
 * (dsflip.c). A wall-clock jump longer than 2^31 us (~35.8 min) — the handheld slept, the process was
 * SIGSTOP'd, or the clock was set — wraps that difference into a large positive delay, so the next
 * wait sleeps for up to another 36 minutes (white panels, the game frozen).
 *
 * clock_gap_apply() is one raw sample in microseconds. *last and *cut are the hook's state, both 0
 * before the first sample. A jump of more than a second, or a step backwards, is taken out of the
 * time DraStic sees: the returned sample is 1 ms after the previous one. *gap_out, when not null,
 * receives the raw jump (0 when the sample is kept). */
#ifndef DSFLIP_CLOCKGAP_H
#define DSFLIP_CLOCKGAP_H
static inline long long clock_gap_apply(long long t, long long *last, long long *cut, long long *gap_out) {
    long long gap = 0;
    if (*last && (t - *last > 1000000 || t < *last)) { gap = t - *last; *cut += gap - 1000; }
    *last = t;
    if (gap_out) *gap_out = gap;
    return t - *cut;
}
#endif
