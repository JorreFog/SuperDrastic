// The screenshot hotkey (F + L2): both DS screens, the top one above, in one PNG (BananaOS 0.4.0).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bananads {

// top and bottom: w x h XRGB8888 each (the frame as emulated, at render.scale). Written on a thread of its own to
// <dir>/<game>-<yymmdd>-<hhmmss>.png (RetroArch's naming); done(ok, the file's name) is called from that thread
void screenshot_save(const std::string& dir, const std::string& game, std::vector<uint32_t> top,
                     std::vector<uint32_t> bottom, int w, int h, std::function<void(bool, const std::string&)> done);
// at quit: waits (up to ms) for the screenshots still being written, so none is cut short (a .tmp left behind) and
// none reports after the frontend and the log are gone; false if one was still going
bool screenshot_wait(int ms);

}  // namespace bananads
