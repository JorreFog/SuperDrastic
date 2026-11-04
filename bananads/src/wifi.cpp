#include "wifi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;
using melonDS::u8;
using melonDS::u16;
using melonDS::u32;

namespace bananads {

uint32_t wfc_dns_address(const std::string& name) {
    if (name.empty() || name == "system") return 0;
    if (name == "kaeru") return 0xB23E2BD4;    // 178.62.43.212
    if (name == "altwfc") return 0xAC6858ED;   // 172.104.88.237
    if (name == "wiilink") return 0xA7EBE524;  // 167.235.229.36
    in_addr a;
    if (inet_pton(AF_INET, name.c_str(), &a) == 1) return ntohl(a.s_addr);
    return 0xFFFFFFFF;
}

std::array<uint8_t, 6> wifi_mac_from_id(const std::string& id) {
    // FNV-1a of the id: stable per device, different between devices.
    uint32_t h = 2166136261u;
    for (char c : id) h = (h ^ (uint8_t)c) * 16777619u;
    if (id.empty()) h = 0x112233;
    return {0x00, 0x09, 0xBF, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
}

std::array<uint8_t, 6> wifi_mac() {
    std::ifstream f("/etc/machine-id");
    std::string id;
    std::getline(f, id);
    return wifi_mac_from_id(id);
}

// ---- the DS's MAC --------------------------------------------------------------------------------------------

bool parse_mac(const std::string& s, std::array<uint8_t, 6>& out) {
    std::array<uint8_t, 6> m{};
    size_t p = 0;
    for (int i = 0; i < 6; i++) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
        if (i && p < s.size() && (s[p] == ':' || s[p] == '-')) p++;
        unsigned v = 0;
        int n = 0;
        while (n < 2 && p < s.size() && isxdigit((unsigned char)s[p])) {
            v = v * 16 + (unsigned)(isdigit((unsigned char)s[p]) ? s[p] - '0' : (tolower((unsigned char)s[p]) - 'a' + 10));
            p++;
            n++;
        }
        if (n != 2) return false;
        m[i] = (uint8_t)v;
    }
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    if (p != s.size()) return false;
    if (m[0] & 1) return false;  // a multicast address can't be a station's
    bool zero = true, ones = true;
    for (uint8_t b : m) { zero &= b == 0; ones &= b == 0xFF; }
    if (zero || ones) return false;
    out = m;
    return true;
}

std::string format_mac(const std::array<uint8_t, 6>& m) {
    char b[24];
    snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static std::string first_line(const std::string& path) {
    std::ifstream f(path);
    std::string l;
    if (f) std::getline(f, l);
    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
    return l;
}

static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

bool write_file_durable(const std::string& path, const void* data, size_t len) {
    const std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0) {
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += (size_t)n;
    }
    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    size_t s = path.find_last_of('/');
    const std::string dir = s == std::string::npos ? "." : s == 0 ? "/" : path.substr(0, s);
    int d = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) {
        fsync(d);
        close(d);
    }
    return true;
}

// the eMMC's CID (a card that's type MMC: the handheld's own memory, not the SD card), "" if none
static std::string emmc_cid(const std::string& root) {
    const std::string dir = root + "/sys/bus/mmc/devices";
    DIR* d = opendir(dir.c_str());
    if (!d) return "";
    std::string best;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string base = dir + "/" + e->d_name;
        if (first_line(base + "/type") != "MMC") continue;
        std::string cid = first_line(base + "/cid");
        if (cid.size() >= 16 && (best.empty() || cid < best)) best = cid;  // (the same one every time if two)
    }
    closedir(d);
    return best;
}

static std::string wlan_mac(const std::string& root) {
    std::string a = first_line(root + "/sys/class/net/wlan0/address");
    std::array<uint8_t, 6> m;
    return parse_mac(a, m) ? format_mac(m) : "";
}

static std::array<uint8_t, 6> mac_from_seed(const std::string& seed) {
    uint32_t h = 2166136261u;
    for (char c : seed) h = (h ^ (uint8_t)c) * 16777619u;
    h = (h ^ (h >> 24)) & 0xFFFFFF;
    // not the address every BananaOS handheld had before (00:09:BF:11:22:33), nor all zeros or all ones
    if (h == 0x112233 || h == 0 || h == 0xFFFFFF) h ^= 0x5A5A5A;
    return {0x00, 0x09, 0xBF, (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h};
}

WifiMac choose_wifi_mac(const std::string& setting, const std::string& mac_file, const std::string& wfc_file,
                        const std::string& sysroot) {
    WifiMac r;
    std::array<uint8_t, 6> saved{};
    bool have_saved = false;
    if (!mac_file.empty() && exists(mac_file)) {
        const std::string l = first_line(mac_file);
        if (parse_mac(l, saved)) have_saved = true;
        else {
            Log(LogLevel::Warn, "[wifi] %s doesn't hold a MAC (\"%s\"): set aside as %s.bad\n", mac_file.c_str(),
                l.c_str(), mac_file.c_str());
            rename(mac_file.c_str(), (mac_file + ".bad").c_str());
        }
    }
    if (!setting.empty()) {
        if (parse_mac(setting, r.mac)) {
            r.source = "the wifi.mac setting";
            if (have_saved && saved != r.mac)
                Log(LogLevel::Warn, "[wifi] wifi.mac=%s isn't the saved MAC %s: a game's WFC ID made with the other "
                    "one may be refused\n", format_mac(r.mac).c_str(), format_mac(saved).c_str());
            return r;  // (not saved: without the setting the saved one comes back)
        }
        Log(LogLevel::Warn, "[wifi] wifi.mac=%s isn't a MAC (xx:xx:xx:xx:xx:xx, not multicast): ignored\n",
            setting.c_str());
    }
    if (have_saved) {
        r.mac = saved;
        r.source = "saved in " + mac_file;
        return r;
    }
    std::string how;
    if (!wfc_file.empty() && exists(wfc_file)) {
        // WFC settings saved without a MAC beside them: their WFC ID was made with the old MAC; keep it
        r.mac = wifi_mac();
        how = "the MAC before (" + wfc_file + " was there already)";
    } else if (std::string cid = emmc_cid(sysroot); !cid.empty()) {
        r.mac = mac_from_seed("bananads-mac/emmc-cid/" + cid);
        how = "made from the eMMC's CID";
    } else if (std::string w = wlan_mac(sysroot); !w.empty()) {
        r.mac = mac_from_seed("bananads-mac/wlan0/" + w);
        how = "made from wlan0's MAC";
    } else {
        uint8_t b[3] = {0, 0, 0};
        if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b) {
            uint32_t t = (uint32_t)time(nullptr) ^ (uint32_t)getpid() * 2654435761u;
            b[0] = (uint8_t)(t >> 16); b[1] = (uint8_t)(t >> 8); b[2] = (uint8_t)t;
        }
        char seed[32];
        snprintf(seed, sizeof seed, "bananads-mac/random/%02x%02x%02x", b[0], b[1], b[2]);
        r.mac = mac_from_seed(seed);
        how = "random";
    }
    if (mac_file.empty()) {
        r.source = how + ", not kept (no file)";
        return r;
    }
    std::string text = format_mac(r.mac) + "\n# the DS's MAC, BananaDS's (" + how + "); it stays this one\n";
    if (write_file_durable(mac_file, text.data(), text.size())) r.source = how + ", saved in " + mac_file;
    else {
        Log(LogLevel::Warn, "[wifi] couldn't save the MAC in %s (%s)\n", mac_file.c_str(), strerror(errno));
        r.source = how + ", not saved";
    }
    return r;
}

// ---- the Nintendo WFC settings --------------------------------------------------------------------------------

static bool wfc_layout_ok(const melonDS::Firmware& fw) {
    const u32 eap = fw.GetExtendedAccessPointOffset(), ap = fw.GetWifiAccessPointOffset();
    return eap + 3 * sizeof(melonDS::Firmware::ExtendedWifiAccessPoint) == ap &&
           ap + 3 * sizeof(melonDS::Firmware::WifiAccessPoint) <= fw.Length() &&
           eap + kWfcSettingsSize <= fw.Length();
}

bool wfc_settings_get(const melonDS::Firmware& fw, std::vector<uint8_t>& out) {
    if (!fw.Buffer() || !wfc_layout_ok(fw)) return false;
    const u8* p = fw.GetExtendedAccessPointPosition();
    out.assign(p, p + kWfcSettingsSize);
    return true;
}

int wfc_settings_load(melonDS::Firmware& fw, const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::vector<uint8_t> b(kWfcSettingsSize + 1);
    const size_t n = fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (n != kWfcSettingsSize) {
        Log(LogLevel::Warn, "[wifi] %s is %zu%s bytes, not %zu: set aside as %s.bad, the WFC settings start empty\n",
            path.c_str(), n, n > kWfcSettingsSize ? "+" : "", kWfcSettingsSize, path.c_str());
        rename(path.c_str(), (path + ".bad").c_str());
        return -1;
    }
    if (!fw.Buffer() || !wfc_layout_ok(fw)) {
        Log(LogLevel::Warn, "[wifi] the firmware's access points aren't where %s expects them: not loaded\n",
            path.c_str());
        return -1;
    }
    constexpr size_t kExt = 3 * sizeof(melonDS::Firmware::ExtendedWifiAccessPoint);
    constexpr size_t kAP = sizeof(melonDS::Firmware::WifiAccessPoint);
    u8* dst = fw.GetExtendedAccessPointPosition();
    int good = 0;
    for (int i = 0; i < 3; i++) {
        const u8* ap = b.data() + kExt + i * kAP;
        const u16 sum = (u16)(ap[0xFE] | ap[0xFF] << 8);
        if (melonDS::CRC16(ap, 0xFE, 0x0000) != sum) {
            Log(LogLevel::Warn, "[wifi] %s: access point %d's checksum is wrong: it starts empty\n", path.c_str(), i + 1);
            continue;
        }
        memcpy(dst + kExt + i * kAP, ap, kAP);
        good++;
    }
    memcpy(dst, b.data(), kExt);  // the DSi's extended ones, as melonDS keeps them (a DS doesn't read them)
    return good ? 1 : -1;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, (uint16_t)v); }

bool WfcDns::is_dns_query(const uint8_t* f, int len) {
    if (len < 14 + 20 + 8 + 12) return false;
    if (rd16(f + 12) != 0x0800) return false;            // IPv4
    const uint8_t* ip = f + 14;
    int ihl = (ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || len < 14 + ihl + 8 + 12) return false;
    if (ip[9] != 17) return false;                        // UDP
    if (rd32(ip + 16) != kSlirpDNS) return false;
    const uint8_t* udp = ip + ihl;
    return rd16(udp + 2) == 53;
}

int WfcDns::build_reply(uint8_t* out, int cap, const uint8_t ds_mac[6], uint32_t ds_ip, uint16_t ds_port,
                        const uint8_t* dns, int dnslen) {
    int total = 14 + 20 + 8 + dnslen;
    if (dnslen <= 0 || total > cap) return 0;
    memcpy(out, ds_mac, 6);
    memcpy(out + 6, kSlirpMAC, 6);
    wr16(out + 12, 0x0800);
    uint8_t* ip = out + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    wr16(ip + 2, (uint16_t)(20 + 8 + dnslen));
    ip[8] = 64;  // TTL
    ip[9] = 17;
    wr32(ip + 12, kSlirpDNS);
    wr32(ip + 16, ds_ip);
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += rd16(ip + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    wr16(ip + 10, (uint16_t)~sum);
    uint8_t* udp = ip + 20;
    wr16(udp, 53);
    wr16(udp + 2, ds_port);
    wr16(udp + 4, (uint16_t)(8 + dnslen));
    wr16(udp + 6, 0);  // no UDP checksum (allowed on IPv4)
    memcpy(udp + 8, dns, dnslen);
    return total;
}

WfcDns::WfcDns(std::unique_ptr<melonDS::NetDriver> inner, uint32_t upstream,
               melonDS::Platform::SendPacketCallback to_ds)
    : inner_(std::move(inner)), upstream_(upstream), to_ds_(std::move(to_ds)) {
    sock_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (sock_ < 0) Log(LogLevel::Error, "[wifi] DNS socket: %s; using the system resolver\n", strerror(errno));
}

WfcDns::~WfcDns() {
    if (sock_ >= 0) close(sock_);
}

int WfcDns::SendPacket(melonDS::u8* data, int len) noexcept {
    if (sock_ < 0 || upstream_ == 0 || !is_dns_query(data, len)) return inner_->SendPacket(data, len);
    const uint8_t* ip = data + 14;
    int ihl = (ip[0] & 15) * 4;
    const uint8_t* udp = ip + ihl;
    const uint8_t* dns = udp + 8;
    int dnslen = std::min<int>(rd16(udp + 4) - 8, len - 14 - ihl - 8);
    if (dnslen < 12) return len;
    Pending p;
    memcpy(p.mac, data + 6, 6);
    p.ip = rd32(ip + 12);
    p.port = rd16(udp);
    pending_[rd16(dns)] = p;
    if (pending_.size() > 64) pending_.erase(pending_.begin());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(upstream_);
    if (sendto(sock_, dns, dnslen, 0, (sockaddr*)&to, sizeof to) < 0)
        Log(LogLevel::Warn, "[wifi] DNS query to the WFC server: %s\n", strerror(errno));
    return len;
}

void WfcDns::RecvCheck() noexcept {
    inner_->RecvCheck();
    if (sock_ < 0) return;
    uint8_t buf[1500], frame[1600];
    for (;;) {
        sockaddr_in from{};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(sock_, buf, sizeof buf, 0, (sockaddr*)&from, &fl);
        if (n < 12) break;
        if (ntohl(from.sin_addr.s_addr) != upstream_) continue;  // only the server we asked
        auto it = pending_.find(rd16(buf));
        if (it == pending_.end()) continue;
        int fl2 = build_reply(frame, sizeof frame, it->second.mac, it->second.ip, it->second.port, buf, (int)n);
        pending_.erase(it);
        if (fl2) to_ds_(frame, fl2);
    }
}

}  // namespace bananads
