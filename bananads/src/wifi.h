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
// Each handheld gets its own MAC (Nintendo's 00:09:BF prefix; choose_wifi_mac below, kept in wifi.mac_file): WFC
// servers tell players apart by MAC, and DraStic's firmware gives every device 00:01:02:03:04:05.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NetDriver.h"
#include "Platform.h"
#include "SPI_Firmware.h"

namespace bananads {

// "kaeru" etc. or a dotted address -> IPv4 in host order; 0 for "system"; -1 (all ones) if unknown.
uint32_t wfc_dns_address(const std::string& name);

// 00:09:BF:xx:yy:zz from the given machine id (a hex string); a fixed fallback when it is empty.
std::array<uint8_t, 6> wifi_mac_from_id(const std::string& machine_id);
std::array<uint8_t, 6> wifi_mac();  // from /etc/machine-id

// ---- the DS's MAC, one per handheld and kept for good ---------------------------------------------------------
// BananaOS has no /etc/machine-id, so wifi_mac() gave every handheld 00:09:BF:11:22:33 (two of them clash in
// local wireless). The MAC is chosen once and kept: wifi.mac (BANANADS_WIFI_MAC) if set; else the one saved in
// mac_file; else, with no saved MAC but WFC settings already saved (wfc_file), wifi_mac()'s as before (the
// settings' WFC ID was made with it); else one made from the eMMC's CID, wlan0's MAC or at random (00:09:BF:...),
// saved in mac_file. A saved MAC never changes. `sysroot` prefixes /sys for the tests ("" on the handheld).
struct WifiMac {
    std::array<uint8_t, 6> mac{};
    std::string source;
};
bool parse_mac(const std::string& s, std::array<uint8_t, 6>& out);  // "00:09:bf:12:34:56" (or with '-')
std::string format_mac(const std::array<uint8_t, 6>& m);
WifiMac choose_wifi_mac(const std::string& setting, const std::string& mac_file, const std::string& wfc_file,
                        const std::string& sysroot = "");
// the file replaced whole or not at all: path.tmp written, fsynced, renamed over path, the folder fsynced
bool write_file_durable(const std::string& path, const void* data, size_t len);

// ---- the Nintendo WFC settings, kept between sessions ---------------------------------------------------------
// The firmware is generated at every start, so the access points the game writes (with the Nintendo WFC ID:
// SPI_Firmware.h WifiAccessPoint::NintendoWFCID) were lost at quit and the game asked for a new WFC ID at the next
// connection. As melonDS's own frontend (EmuInstance.cpp, Platform.cpp WriteFirmware): the extended access points
// and the access points, side by side in the generated firmware, 3 x (512 + 256) bytes, the same wfcsettings.bin.
constexpr size_t kWfcSettingsSize = 3 * (512 + 256);
// the region as it is now in fw (false: not the generated firmware's layout)
bool wfc_settings_get(const melonDS::Firmware& fw, std::vector<uint8_t>& out);
// the file into fw's region (before UpdateChecksums). An access point whose checksum is wrong keeps fw's own; a
// file of the wrong size is set aside as <path>.bad. 1 loaded, 0 no file, -1 not usable
int wfc_settings_load(melonDS::Firmware& fw, const std::string& path);


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
