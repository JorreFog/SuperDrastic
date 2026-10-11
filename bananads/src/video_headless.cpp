#include <chrono>
#include <cstdio>
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

int run_headless(Emu& emu, const Config& cfg) {
    long frames = cfg.num("headless.frames", 600);
    std::string dir = cfg.str("headless.dump_dir", "");
    long every = cfg.num("headless.dump_every", 60);
    std::vector<int16_t> snd(4096 * 2);
    auto t0 = std::chrono::steady_clock::now();
    long done = 0;
    for (; done < frames && !emu.stopped(); done++) {
        emu.run_frame();
        while (emu.audio(snd.data(), 4096) > 0) {}  // drain it, as a sound device would
        if (!dir.empty() && every > 0 && (done + 1) % every == 0)
            dump_ppm(dir + "/frame" + std::to_string(done + 1) + ".ppm", emu.top(), emu.bottom());
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Log(LogLevel::Info, "[headless] %ld frames in %.2f s: %.1f fps (%.0f%% of a DS)\n", done, s, done / s,
        done / s / 59.8261 * 100);
    return 0;
}

}  // namespace bananads
