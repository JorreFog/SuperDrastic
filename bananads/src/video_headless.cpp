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

static void dump_ppm(const std::string& path, const uint32_t* top, const uint32_t* bot) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n256 384\n255\n");
    std::vector<uint8_t> row(256 * 3);
    for (int y = 0; y < 384; y++) {
        const uint32_t* src = (y < 192 ? top + y * 256 : bot + (y - 192) * 256);
        for (int x = 0; x < 256; x++) {
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
    auto tm = t0;
    for (; done < frames && !emu.stopped(); done++) {
        if (done == from) tm = std::chrono::steady_clock::now();
        if (!input.empty()) {
            uint32_t keys = 0;
            for (const Press& p : input)
                if (done >= p.at && done < p.at + p.len) keys |= p.keys;
            emu.set_keys(keys);
        }
        emu.run_frame();
        while (emu.audio(snd.data(), 4096) > 0) {}  // drain it, as a sound device would
        if (done + 1 == reload && !(emu.save_state(3) && emu.load_state(3)))
            Log(LogLevel::Error, "[headless] headless.reload_at: the state didn't save and load back\n");
        if (!dir.empty() && every > 0 && (done + 1) % every == 0)
            dump_ppm(dir + "/frame" + std::to_string(done + 1) + ".ppm", emu.top(), emu.bottom());
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log(LogLevel::Info, "[headless] %ld frames in %.2f s: %.1f fps (%.0f%% of a DS)\n", done, s, done / s,
        done / s / 59.8261 * 100);
    if (from > 0 && done > from) {
        double sm = std::chrono::duration<double>(std::chrono::steady_clock::now() - tm).count();
        Log(LogLevel::Info, "[headless] frames %ld-%ld: %.1f fps (%.0f%% of a DS)\n", from, done, (done - from) / sm,
            (done - from) / sm / 59.8261 * 100);
    }
    return 0;
}

}  // namespace bananads
