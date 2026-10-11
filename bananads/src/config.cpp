#include "config.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

namespace bananads {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool Config::set_line(const std::string& raw) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#') return true;
    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    std::string v = trim(line.substr(eq + 1));
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    kv_[trim(line.substr(0, eq))] = v;
    return true;
}

bool Config::load_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) set_line(line);
    return true;
}

std::string Config::env_name(const std::string& key) {
    std::string e = "BANANADS_";
    for (char c : key) e += (c == '.' || c == '-') ? '_' : (char)toupper((unsigned char)c);
    return e;
}

const std::string* Config::find(const std::string& key) const {
    // The environment wins over the file, so a launcher can override one setting for one run.
    if (const char* e = getenv(env_name(key).c_str())) {
        envbuf_ = e;
        return &envbuf_;
    }
    auto it = kv_.find(key);
    return it == kv_.end() ? nullptr : &it->second;
}

std::string Config::str(const std::string& key, const std::string& def) const {
    const std::string* v = find(key);
    return v ? *v : def;
}

long Config::num(const std::string& key, long def) const {
    const std::string* v = find(key);
    if (!v || v->empty()) return def;
    char* end;
    long n = strtol(v->c_str(), &end, 0);
    return *end ? def : n;
}

double Config::real(const std::string& key, double def) const {
    const std::string* v = find(key);
    if (!v || v->empty()) return def;
    char* end;
    double d = strtod(v->c_str(), &end);
    return *end ? def : d;
}

bool Config::flag(const std::string& key, bool def) const {
    const std::string* v = find(key);
    if (!v) return def;
    std::string s;
    for (char c : *v) s += (char)tolower((unsigned char)c);
    if (s == "1" || s == "on" || s == "yes" || s == "true") return true;
    if (s == "0" || s == "off" || s == "no" || s == "false") return false;
    return def;
}

}  // namespace bananads
