#include "wifi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#include "Platform.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

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
