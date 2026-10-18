// Cheats for the in-game menu (cheats.h).
#include "cheats.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

// a name for matching: lower case letters and digits only, without the (region) and [flags] parts
static std::string plain(const std::string& n) {
    std::string out;
    int depth = 0;
    for (char ch : n) {
        if (ch == '(' || ch == '[') depth++;
        else if ((ch == ')' || ch == ']') && depth > 0) depth--;
        else if (!depth && isalnum((unsigned char)ch)) out += (char)tolower((unsigned char)ch);
    }
    return out;
}

// RetroArch's .cht: cheatN_desc = "...", cheatN_code = "XXXXXXXX+YYYYYYYY+..." (or with spaces), cheatN_enable
static std::vector<melonDS::ARCode> read_cht(const std::string& path) {
    struct Entry { std::string desc, code; bool on = false; };
    std::map<int, Entry> e;
    std::ifstream f(path);
    std::string ln;
    while (std::getline(f, ln)) {
        int n;
        char field[16];
        if (sscanf(ln.c_str(), "cheat%d_%15[a-z]", &n, field) != 2) continue;
        size_t q0 = ln.find('"'), q1 = ln.rfind('"');
        std::string v = q0 != std::string::npos && q1 > q0 ? ln.substr(q0 + 1, q1 - q0 - 1) : "";
        if (q0 == std::string::npos) {  // (an unquoted value: after the '=')
            size_t eq = ln.find('=');
            v = eq == std::string::npos ? "" : ln.substr(eq + 1);
        }
        std::string fld = field;
        if (fld == "desc") e[n].desc = v;
        else if (fld == "code") e[n].code = v;
        else if (fld == "enable") e[n].on = v.find("true") != std::string::npos;
    }
    std::vector<melonDS::ARCode> codes;
    std::string section;  // an entry without a code is a heading ("Miscellaneous Codes"): it names the codes after it
    for (auto& [n, x] : e) {
        std::vector<uint32_t> words;
        std::string tok;
        for (size_t i = 0; i <= x.code.size(); i++) {
            const char ch = i < x.code.size() ? x.code[i] : ' ';
            if (isxdigit((unsigned char)ch)) tok += ch;
            else {
                if (tok.size() == 8) words.push_back((uint32_t)strtoul(tok.c_str(), nullptr, 16));
                tok.clear();
            }
        }
        if (words.empty() || words.size() % 2) {
            if (!x.desc.empty()) section = x.desc;
            continue;
        }
        std::string name = x.desc.empty() ? "Code " + std::to_string(n) : x.desc;
        if (!section.empty() && name.find(':') == std::string::npos && x.desc.size() < 40 && section.size() < 40)
            name = section + ": " + name;
        if (name.size() > 120) name.resize(120);
        codes.push_back(melonDS::ARCode{nullptr, name, "", x.on, words});
    }
    return codes;
}

// RetroArch's file for the game: the same name, else the same name without regions (the USA's first, then Europe's)
static std::string find_cht(const std::string& game, const std::string& ra_dir) {
    DIR* d = opendir(ra_dir.c_str());
    if (!d) return "";
    const std::string want = plain(game);
    std::string best;
    int best_rank = 99;
    while (dirent* de = readdir(d)) {
        std::string f = de->d_name;
        if (f.size() < 5 || f.compare(f.size() - 4, 4, ".cht")) continue;
        const std::string base = f.substr(0, f.size() - 4);
        int rank = 99;
        if (base == game) rank = 0;
        else if (plain(base) == want)
            rank = base.find("(USA") != std::string::npos ? 1 : base.find("(Europe") != std::string::npos ? 2 : 3;
        if (rank < best_rank || (rank == best_rank && f < best)) {
            best_rank = rank;
            best = f;
        }
    }
    closedir(d);
    return best_rank < 99 ? ra_dir + "/" + best : "";
}

void Cheats::load(const std::string& game, const std::string& dir, const std::vector<std::string>& cht_dirs) {
    codes_.clear();
    source_.clear();
    mch_ = dir + "/" + game + ".mch";
    if (access(mch_.c_str(), R_OK) == 0) {
        melonDS::ARCodeFile f(mch_);
        if (!f.Error) {
            codes_ = f.GetCodes();
            source_ = mch_;
        } else Log(LogLevel::Warn, "[cheats] %s couldn't be read (melonDS's format?)\n", mch_.c_str());
    }
    for (const auto& d : cht_dirs) {  // (the card's first: a file of the user's wins over the installed one)
        if (!source_.empty()) break;
        const std::string cht = find_cht(game, d);
        if (!cht.empty()) {
            codes_ = read_cht(cht);
            source_ = cht;
        }
    }
    if (!source_.empty())
        Log(LogLevel::Info, "[cheats] %d codes (%d on) from %s\n", count(), enabled(), source_.c_str());
}

int Cheats::enabled() const {
    return (int)std::count_if(codes_.begin(), codes_.end(), [](const melonDS::ARCode& c) { return c.Enabled; });
}

void Cheats::set(int i, bool on) {
    if (i < 0 || i >= count()) return;
    codes_[i].Enabled = on;
    if (!save()) Log(LogLevel::Warn, "[cheats] couldn't write %s\n", mch_.c_str());
}

// melonDS's format (its ARCodeFile reads it back): one category, then each code with its words in pairs
bool Cheats::save() const {
    const size_t slash = mch_.find_last_of('/');
    if (slash != std::string::npos) mkdir(mch_.substr(0, slash).c_str(), 0755);
    const std::string tmp = mch_ + ".new";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    fprintf(f, "CAT Cheats\n");
    for (const auto& c : codes_) {
        std::string n = c.Name;
        std::replace(n.begin(), n.end(), '\n', ' ');
        fprintf(f, "CODE %d %s\n", c.Enabled ? 1 : 0, n.c_str());
        for (size_t w = 0; w + 1 < c.Code.size(); w += 2) fprintf(f, "%08X %08X\n", c.Code[w], c.Code[w + 1]);
    }
    const bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    return ok && rename(tmp.c_str(), mch_.c_str()) == 0;
}

}  // namespace bananads
