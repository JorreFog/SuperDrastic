// BananaDS settings: bananads.conf (key = value, # comments), then BANANADS_<KEY> in the environment, then the
// command line (--set key=value). Keys are lower case with dots; the environment form is upper case with
// underscores (wifi.dns -> BANANADS_WIFI_DNS). docs/SETTINGS.md lists every key.
#pragma once
#include <map>
#include <string>

namespace bananads {

class Config {
public:
    bool load_file(const std::string& path);  // false if it can't be read (not an error: defaults apply)
    void set(const std::string& key, const std::string& value) { kv_[key] = value; }
    bool set_line(const std::string& line);   // "key=value"; false if malformed

    std::string str(const std::string& key, const std::string& def = "") const;
    long num(const std::string& key, long def) const;
    double real(const std::string& key, double def) const;
    bool flag(const std::string& key, bool def) const;  // 1/0, on/off, yes/no, true/false

    static std::string env_name(const std::string& key);

private:
    const std::string* find(const std::string& key) const;
    std::map<std::string, std::string> kv_;
    mutable std::string envbuf_;
};

}  // namespace bananads
