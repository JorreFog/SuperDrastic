#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Platform.h"
#include "frontend.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

static void dump_ppm(const std::string& path, const uint32_t* top, const uint32_t* bot, int s) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    const int w = 256 * s, h = 192 * s;
    fprintf(f, "P6\n%d %d\n255\n", w, 2 * h);
    std::vector<uint8_t> row(w * 3);
    for (int y = 0; y < 2 * h; y++) {
        const uint32_t* src = (y < h ? top + y * w : bot + (y - h) * w);
        for (int x = 0; x < w; x++) {
            uint32_t p = src[x];
            row[x * 3] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
}

// headless.input: scripted presses for a run without a person, "<frame>:<keys>[:<frames held>]" separated by commas,
// keys joined by '+' (a b select start right left up down r l x y), held 6 frames unless given. "600:start,
// 900:a:10,1200:up+a" presses Start at frame 600, A for 10 frames at 900, Up and A together at 1200.
struct Press { long at, len; uint32_t keys; };
static std::vector<Press> parse_input(const std::string& spec) {
    static const char* names[12] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
    std::vector<Press> out;
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(',', i);
        std::string item = spec.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? spec.size() : e + 1;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = item.find(':', c1 + 1);
        Press p{atol(item.c_str()), c2 == std::string::npos ? 6 : atol(item.c_str() + c2 + 1), 0};
        std::string keys = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        size_t k = 0;
        while (k <= keys.size()) {
            size_t plus = keys.find('+', k);
            std::string name = keys.substr(k, plus == std::string::npos ? std::string::npos : plus - k);
            for (int b = 0; b < 12; b++)
                if (name == names[b]) p.keys |= 1u << b;
            if (plus == std::string::npos) break;
            k = plus + 1;
        }
        out.push_back(p);
    }
    return out;
}

int run_headless(Emu& emu, const Config& cfg) {
    long frames = cfg.num("headless.frames", 600);
    const std::vector<Press> input = parse_input(cfg.str("headless.input", ""));
    std::string dir = cfg.str("headless.dump_dir", "");
    long every = cfg.num("headless.dump_every", 60);
    std::vector<int16_t> snd(4096 * 2);
    auto t0 = std::chrono::steady_clock::now();
    long done = 0;
    // headless.measure_from: also report the fps from this frame on (a game's scene after its loading)
    const long from = cfg.num("headless.measure_from", 0);
    // headless.reload_at: after this frame, save the state to slot 3 and load it straight back (tests the savestate
    // path: the run should go on as if nothing happened)
    const long reload = cfg.num("headless.reload_at", -1);
    // headless.audio_dump=<file>: the raw stereo s16 samples the SPU hands the frontend (SPU::ReadOutput), so two
    // builds' audio compares byte for byte with cmp
    const std::string adump = cfg.str("headless.audio_dump", "");
    FILE* af = adump.empty() ? nullptr : fopen(adump.c_str(), "wb");
    if (!adump.empty() && !af) Log(LogLevel::Error, "[headless] headless.audio_dump: can't open %s\n", adump.c_str());
    auto tm = t0;
    // headless.window: from headless.measure_from on, the fps of every this many frames too, and the slowest
    // window and frame at the end (how steady the speed is, not only its average)
    const long window = cfg.num("headless.window", 0);
    auto tw = t0, tf = t0;
    // headless.direct: every frame drawn straight into a ring of buffers of the frontend's, as frontend=kms does into
    // the scanout buffers (the pictures must be the same as without)
    std::vector<std::vector<uint32_t>> ring;
    if (cfg.flag("headless.direct", false)) {
        ring.assign(8, std::vector<uint32_t>((size_t)256 * 192 * emu.scale() * emu.scale(), 0xDEADBEEF));
        if (!emu.draw_into(ring[0].data(), ring[1].data())) {
            Log(LogLevel::Error, "[headless] headless.direct: this renderer can't (render.2d=banana can)\n");
            return 1;
        }
    }
    // render.2d_lazy (on): the 2D thread may finish a frame during the next one; top()/bottom() wait for it
    if (cfg.flag("render.2d_lazy", true)) emu.set_lazy_2d(true);
    double worst_w = 1e9, worst_f = 0;
    long worst_at = 0;
    for (; done < frames && !emu.stopped(); done++) {
        if (done == from) tm = tw = tf = std::chrono::steady_clock::now();
        if (!input.empty()) {
            uint32_t keys = 0;
            for (const Press& p : input)
                if (done >= p.at && done < p.at + p.len) keys |= p.keys;
            emu.set_keys(keys);
        }
        if (!ring.empty()) emu.draw_into(ring[done % 4 * 2].data(), ring[done % 4 * 2 + 1].data());
        emu.run_frame();
        for (int n; (n = emu.audio(snd.data(), 4096)) > 0;)  // drain it, as a sound device would
            if (af) fwrite(snd.data(), sizeof(int16_t) * 2, n, af);
        if (done + 1 == reload && !(emu.save_state(3) && emu.load_state(3)))
            Log(LogLevel::Error, "[headless] headless.reload_at: the state didn't save and load back\n");
        if (window > 0 && done >= from) {
            const auto now = std::chrono::steady_clock::now();
            const double f = std::chrono::duration<double>(now - tf).count();
            tf = now;
            if (f > worst_f) { worst_f = f; worst_at = done + 1; }
            if ((done + 1 - from) % window == 0) {
                const double w = window / std::chrono::duration<double>(now - tw).count();
                tw = now;
                worst_w = std::min(worst_w, w);
                Log(LogLevel::Info, "[headless] frames %ld-%ld: %.1f fps\n", done + 1 - window, done + 1, w);
            }
        }
        if (!dir.empty() && every > 0 && (done + 1) % every == 0)
            dump_ppm(dir + "/frame" + std::to_string(done + 1) + ".ppm", emu.top(), emu.bottom(), emu.scale());
    }
    if (af) fclose(af);
    if (!ring.empty()) emu.draw_into(nullptr, nullptr);  // (the ring goes with this function)
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log(LogLevel::Info, "[headless] %ld frames in %.2f s: %.1f fps (%.0f%% of a DS)\n", done, s, done / s,
        done / s / 59.8261 * 100);
    if (window > 0 && done > from)
        Log(LogLevel::Info, "[headless] slowest window %.1f fps, slowest frame %.1f ms (frame %ld)\n", worst_w,
            worst_f * 1000, worst_at);
    if (from > 0 && done > from) {
        double sm = std::chrono::duration<double>(std::chrono::steady_clock::now() - tm).count();
        Log(LogLevel::Info, "[headless] frames %ld-%ld: %.1f fps (%.0f%% of a DS)\n", from, done, (done - from) / sm,
            (done - from) / sm / 59.8261 * 100);
    }
    return 0;
}

}  // namespace bananads
