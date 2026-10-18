// Cheats (Action Replay codes) for the in-game menu's Cheats page, run by melonDS's AREngine (BananaOS #14).
// A game's list comes from <dir>/<game>.mch (melonDS's own cheat file, which also keeps which codes are on), else
// from RetroArch's cheat database (<ra_dir>/<game's name> (<region>).cht, what RetroArch's Online Updater > Update
// Cheats downloads), copied into the .mch the first time a code is turned on or off.
#pragma once
#include <string>
#include <vector>
#include "ARCodeFile.h"

namespace bananads {

class Cheats {
public:
    // the game's list (game: the ROM's file name without its extension); 0 codes when there is none
    void load(const std::string& game, const std::string& dir, const std::string& ra_dir);
    int count() const { return (int)codes_.size(); }
    int enabled() const;
    const std::string& name(int i) const { return codes_[i].Name; }
    bool on(int i) const { return codes_[i].Enabled; }
    void set(int i, bool on);  // and saved in the .mch
    const std::string& source() const { return source_; }  // the file the list came from ("" none)
    std::vector<melonDS::ARCode> list() const { return codes_; }  // for melonDS's AREngine

private:
    bool save() const;
    std::vector<melonDS::ARCode> codes_;
    std::string mch_, source_;
};

}  // namespace bananads
