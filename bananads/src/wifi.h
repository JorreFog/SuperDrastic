// Wi-Fi. melonDS emulates the DS's Wi-Fi hardware itself (Wifi.cpp, WifiAP.cpp), which is what DraStic never
// did: SuperDrastic's WFC work (1.6-prep, parked) found that DraStic's recompiled ARM7 writes Wi-Fi registers
// through store_io_register_arm7_* and never sends a packet. Here the game talks to melonDS's access point, and
// its packets go to the internet through libslirp (user-mode NAT, no root, no tun device).
//
// Nintendo WFC closed in 2014; games reach a revival server when their DNS answers Nintendo's names with it.
// wifi.dns picks that DNS (the same presets as ROCKNIXDS's nds.wfc_dns):
//   kaeru    178.62.43.212   Kaeru WFC
//   altwfc   172.104.88.237  AltWFC
//   wiilink  167.235.229.36  WiiLink WFC
//   a.b.c.d  any DNS server
//   system   the handheld's own resolver (melonDS's default: no WFC revival)
// WfcDns forwards the DS's DNS queries to that server and hands the answers back as if from slirp's DNS.
//
// Each handheld gets its own MAC (Nintendo's 00:09:BF prefix, the rest from /etc/machine-id): WFC servers tell
// players apart by MAC, and DraStic's firmware gives every device 00:01:02:03:04:05.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "NetDriver.h"
#include "Platform.h"

namespace bananads {

// "kaeru" etc. or a dotted address -> IPv4 in host order; 0 for "system"; -1 (all ones) if unknown.
uint32_t wfc_dns_address(const std::string& name);

// 00:09:BF:xx:yy:zz from the given machine id (a hex string); a fixed fallback when it is empty.
std::array<uint8_t, 6> wifi_mac_from_id(const std::string& machine_id);
std::array<uint8_t, 6> wifi_mac();  // from /etc/machine-id

// Wraps another NetDriver (slirp). DNS queries to slirp's DNS address go to `upstream` instead.
class WfcDns : public melonDS::NetDriver {
public:
    WfcDns(std::unique_ptr<melonDS::NetDriver> inner, uint32_t upstream,
           melonDS::Platform::SendPacketCallback to_ds);
    ~WfcDns() override;

    int SendPacket(melonDS::u8* data, int len) noexcept override;
    void RecvCheck() noexcept override;

    // Builds the Ethernet/IPv4/UDP frame slirp's DNS would have sent (public for the host tests).
    static int build_reply(uint8_t* out, int cap, const uint8_t ds_mac[6], uint32_t ds_ip, uint16_t ds_port,
                           const uint8_t* dns, int dnslen);
    // True if the frame is a DNS query to slirp's DNS address (10.64.0.2:53).
    static bool is_dns_query(const uint8_t* f, int len);

    static constexpr uint32_t kSlirpDNS = 0x0A400002;  // melonDS Net_Slirp.cpp kDNSIP
    static constexpr uint8_t kSlirpMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};  // its kServerMAC

private:
    struct Pending { uint8_t mac[6]; uint32_t ip; uint16_t port; };
    std::unique_ptr<melonDS::NetDriver> inner_;
    uint32_t upstream_;
    melonDS::Platform::SendPacketCallback to_ds_;
    int sock_ = -1;
    std::map<uint16_t, Pending> pending_;  // by DNS id
    uint16_t ipid_ = 1;
};

}  // namespace bananads
