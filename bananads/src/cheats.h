// Cheats (Action Replay codes) for the in-game menu's Cheats page, run by melonDS's AREngine (BananaOS #14).
// A game's list comes from <dir>/<game>.mch (melonDS's own cheat file, which also keeps which codes are on), else
// from a RetroArch cheat file (<game's name> (<region>).cht) in the first of cht_dirs that has one: the card's
// RetroArch folder, then the collection BananaOS installs (libretro-database's DS cheats); copied into the .mch the
// first time a code is turned on or off.
#pragma once
#include <string>
#include <vector>
#include "ARCodeFile.h"

namespace bananads {

class Cheats {
public:
    // the game's list (game: the ROM's file name without its extension); 0 codes when there is none
    void load(const std::string& game, const std::string& dir, const std::vector<std::string>& cht_dirs);
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
