// The sound thread's time stretch (src/stretch.cpp): at a tempo of 1 it plays the input back unchanged; below 1 the
// sound gets longer but keeps its pitch (a 440 Hz tone stays 440 Hz), with no jumps where the windows join.
#include <cmath>
#include <cstdio>
#include <vector>

#include "stretch.h"

using namespace bananads;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static std::vector<int16_t> tone(int frames, double hz, double amp, double hz2 = 0) {
    std::vector<int16_t> v((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        double x = amp * std::sin(2 * M_PI * hz * i / 48000);
        if (hz2 > 0) x += amp * 0.5 * std::sin(2 * M_PI * hz2 * i / 48000 + 1.0);
        v[2 * i] = (int16_t)std::lround(x);
        v[2 * i + 1] = (int16_t)std::lround(x * 0.8);
    }
    return v;
}

// fed like the sound thread: 800 frames a game frame in, 256 a chunk out, at the given tempo
static std::vector<int16_t> run(Stretch& st, const std::vector<int16_t>& in, double (*tempo)(size_t out_frames)) {
    std::vector<int16_t> out;
    int16_t buf[512];
    const int frames = (int)(in.size() / 2);
    for (int f = 0; f < frames; f += 800) {
        st.push(in.data() + 2 * f, std::min(800, frames - f));
        for (int n; (n = st.pull(buf, 256, tempo(out.size() / 2))) > 0;) out.insert(out.end(), buf, buf + 2 * n);
    }
    return out;
}

static double freq(const std::vector<int16_t>& v, size_t from, size_t to) {
    int cross = 0;
    size_t first = 0, last = 0;
    for (size_t i = from + 1; i < to; i++)
        if (v[2 * (i - 1)] < 0 && v[2 * i] >= 0) {
            if (!cross) first = i;
            last = i;
            cross++;
        }
    return cross > 1 ? 48000.0 * (cross - 1) / (double)(last - first) : 0;
}

static int max_step(const std::vector<int16_t>& v, size_t from) {
    int m = 0;
    for (size_t i = from + 1; i < v.size() / 2; i++) m = std::max(m, std::abs(v[2 * i] - v[2 * (i - 1)]));
    return m;
}

int main() {
    const int frames = 96000;  // 2 s
    {  // tempo 1: unchanged after the first hop (the fade-in)
        Stretch st;
        const auto in = tone(frames, 440, 9000, 1210);
        const auto out = run(st, in, [](size_t) { return 1.0; });
        CHECK(out.size() / 2 + Stretch::kLookahead >= (size_t)frames);
        int diff = 0;
        for (size_t i = 2 * Stretch::kHop; i < out.size(); i++) diff = std::max(diff, std::abs(out[i] - in[i]));
        printf("tempo 1: %zu of %d frames out, max difference %d\n", out.size() / 2, frames, diff);
        CHECK(diff <= 1);
    }
    {  // tempo 0.75: 4/3 as long, same pitch, smooth
        Stretch st;
        const auto in = tone(frames, 440, 9000);
        const auto out = run(st, in, [](size_t) { return 0.75; });
        const double len = (double)(out.size() / 2) / frames, f = freq(out, 2000, out.size() / 2);
        const int step = max_step(out, Stretch::kWin);
        printf("tempo 0.75: length x%.3f, %.1f Hz, largest step %d (a 440 Hz tone's own: %d)\n", len, f, step,
               (int)(9000 * 2 * M_PI * 440 / 48000) + 1);
        CHECK(std::fabs(len - 1 / 0.75) < 0.03);
        CHECK(std::fabs(f - 440) < 2.5);
        CHECK(step <= 600);
    }
    {  // the servo moving the tempo: 1, then 0.8, then back to 1
        Stretch st;
        const auto in = tone(frames, 523.25, 9000);
        const auto out = run(st, in, [](size_t o) { return o < 30000 ? 1.0 : o < 70000 ? 0.8 : 1.0; });
        const double f1 = freq(out, 2000, 29000), f2 = freq(out, 31000, 69000), f3 = freq(out, 71000, out.size() / 2);
        const int step = max_step(out, Stretch::kWin);
        printf("tempo 1/0.8/1: %.1f %.1f %.1f Hz, largest step %d\n", f1, f2, f3, step);
        CHECK(std::fabs(f1 - 523.25) < 2.5 && std::fabs(f2 - 523.25) < 2.5 && std::fabs(f3 - 523.25) < 2.5);
        CHECK(step <= 720);
    }
    if (fails) fprintf(stderr, "%d failed\n", fails);
    else printf("stretch: all passed\n");
    return fails ? 1 : 0;
}
