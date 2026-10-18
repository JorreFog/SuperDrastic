// The screenshot hotkey's PNG (screenshot.h).
#include "screenshot.h"
#include <atomic>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <thread>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"

namespace bananads {

static std::atomic<int> g_pending{0};  // screenshots being written (screenshot_wait)

void screenshot_save(const std::string& dir, const std::string& game, std::vector<uint32_t> top,
                     std::vector<uint32_t> bottom, int w, int h, std::function<void(bool, const std::string&)> done) {
    g_pending.fetch_add(1);
    std::thread([=, top = std::move(top), bottom = std::move(bottom)] {
        setpriority(PRIO_PROCESS, 0, 10);  // (the game's threads first)
        std::vector<uint8_t> rgb((size_t)w * h * 2 * 3);
        for (int s = 0; s < 2; s++) {
            const std::vector<uint32_t>& src = s ? bottom : top;
            uint8_t* o = rgb.data() + (size_t)s * w * h * 3;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                const uint32_t p = src[i];
                o[i * 3] = (uint8_t)(p >> 16);
                o[i * 3 + 1] = (uint8_t)(p >> 8);
                o[i * 3 + 2] = (uint8_t)p;
            }
        }
        mkdir(dir.c_str(), 0755);
        char when[32];
        const time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(when, sizeof when, "%y%m%d-%H%M%S", &lt);  // (named as RetroArch names its own, in the same folder)
        const std::string name = game + "-" + when + ".png", path = dir + "/" + name, tmp = path + ".tmp";
        const bool ok = stbi_write_png(tmp.c_str(), w, h * 2, 3, rgb.data(), w * 3) != 0 && rename(tmp.c_str(), path.c_str()) == 0;
        if (!ok) remove(tmp.c_str());
        done(ok, name);
        g_pending.fetch_sub(1);
    }).detach();
}

bool screenshot_wait(int ms) {
    for (int i = 0; i < ms / 10 && g_pending.load() > 0; i++) usleep(10000);
    return g_pending.load() == 0;
}

}  // namespace bananads
