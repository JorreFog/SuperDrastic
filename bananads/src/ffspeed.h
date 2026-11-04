// kms.ff_speed: fast-forward's speed for its hotkeys (F + X, R2) from the setting's text: "2", "3", or "max" (what the
// in-game menu's Fast-forward row saves for Max) / any number over 3 = 99, as fast as it goes. Anything else: 2.
// (It was read with Config::num, which takes "max" for no number: Max came back as 2x at the next start.)
#pragma once
#include <cstdlib>
#include <string>

namespace bananads {
inline int ff_speed_of(const std::string& v) {
    if (v == "max") return 99;
    char* end = nullptr;
    const long n = v.empty() ? 2 : strtol(v.c_str(), &end, 10);
    if (v.empty() || *end) return 2;
    return n == 3 ? 3 : n > 3 ? 99 : 2;
}
}  // namespace bananads
