#include "emu.h"
#ifdef BANANADS_GENGIS
#include "gengis_renderer.h"
#endif

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <new>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <pthread.h>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <vector>

#include "Args.h"
#include "DSi.h"
#include "FreeBIOS.h"
#include "fatfs/ff.h"
#include "GPU_Soft.h"
#ifdef BANANADS_2D
#include "GPU_Banana.h"
#endif
#include "NDS.h"
#include "NDSCart.h"
#include "Net.h"
#include "Net_Slirp.h"
#include "Platform.h"
#include "Savestate.h"
#include "wifi.h"

#ifdef BANANADS_LAN
#include "LAN.h"
#include "MPInterface.h"
#endif

using namespace melonDS;
using Platform::Log;
using Platform::LogLevel;

namespace bananads {

static std::string basename_noext(const std::string& p) {
    size_t s = p.find_last_of('/');
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

static void mkdirs(const std::string& dir) {
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0755);
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    return n == 0 || (bool)f.read((char*)out.data(), n);  // in one read: a 128 MB ROM byte by byte took seconds
}

// Write next to it, then rename: a save is never half written, whatever happens to the power.
static bool write_file_atomic(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

static uint64_t now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

Emu::Emu(const Config& cfg) : cfg_(cfg) {
    MicTuning mt;
    mt.threshold = cfg.real("mic.threshold", mt.threshold);
    mt.gate_k = cfg.real("mic.gate", mt.gate_k);
    mt.coupling_max = cfg.real("mic.coupling_max", mt.coupling_max);
    mt.hold_ms = (int)cfg.num("mic.hold_ms", mt.hold_ms);
    mic_.set_tuning(mt);
}

Emu::~Emu() { shutdown(); }

void Emu::shutdown() {
    if (shut_) return;
    shut_ = true;
    if (prof_) prof_->finish();  // (its report: the emulator's thread is done by now)
    flush_save(true);
    start_mark("quit: save written");
    if (nds_) nds_->Stop();
    export_dsi_saves();
    if (net_) net_->UnregisterInstance(0);
    start_mark("quit: console stopped");
}

void Emu::setup_wifi() {
    std::string dns = cfg_.str("wifi.dns", "kaeru");
    if (dns == "off") return;
    uint32_t up = wfc_dns_address(dns);
    if (up == 0xFFFFFFFF) {
        Log(LogLevel::Warn, "[wifi] wifi.dns=%s isn't a preset or an address; using the system resolver\n",
            dns.c_str());
        up = 0;
    }
    net_ = std::make_unique<Net>();
    net_->RegisterInstance(0);
    Net* net = net_.get();
    auto to_ds = [net](const u8* data, int len) { net->RXEnqueue(data, len); };
    auto slirp = std::make_unique<Net_Slirp>(to_ds);
    net_->SetDriver(std::make_unique<WfcDns>(std::move(slirp), up, to_ds));
    Log(LogLevel::Info, "[wifi] on: internet through slirp, DNS %s\n", up ? dns.c_str() : "the system's");
}

// Local wireless with another handheld (Pokémon trades and battles, Mario Kart's local races, PictoChat):
// wifi.local = host on one, join:<the host's address> on the other. Both on the same network.
void Emu::setup_local() {
    std::string mode = cfg_.str("wifi.local", "off");
    if (mode == "off") return;
#ifdef BANANADS_LAN
    MPInterface::Set(MPInterface_LAN);
    LAN& lan = static_cast<LAN&>(MPInterface::Get());
    std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    bool ok = false;
    if (mode == "host") ok = lan.StartHost(nick.c_str(), (int)cfg_.num("wifi.local_players", 4));
    else if (mode.rfind("join:", 0) == 0) ok = lan.StartClient(nick.c_str(), mode.substr(5).c_str());
    Log(ok ? LogLevel::Info : LogLevel::Error, "[wifi] local wireless %s: %s\n", mode.c_str(), ok ? "ready" : "failed");
    local_ = ok;
#else
    Log(LogLevel::Warn, "[wifi] wifi.local=%s, but this build has no local wireless (BANANADS_LAN)\n", mode.c_str());
#endif
}

// the DS's clock: now, or rtc.fixed
void Emu::set_clock() {
    time_t t = time(nullptr);
    tm lt;
    localtime_r(&t, &lt);
    // rtc.fixed ("YYYY-MM-DD HH:MM:SS"): the DS clock starts there instead of now, so that two runs of a game that
    // reads the clock (to seed its random numbers, say) go the same way; for tests
    int fy, fmo, fd, fh, fmi, fs;
    if (sscanf(cfg_.str("rtc.fixed", "").c_str(), "%d-%d-%d%*c%d:%d:%d", &fy, &fmo, &fd, &fh, &fmi, &fs) == 6)
        nds_->RTC.SetDateTime(fy, fmo, fd, fh, fmi, fs);
    else
        nds_->RTC.SetDateTime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}

// ---- DSi games (DSiWare, DSi-only cartridges) ----
// A .cia is how the 3DS's eShop shipped DSiWare: certificates, a ticket, the title's metadata (TMD) and the content,
// the DSi ROM itself. A content the TMD marks encrypted (type bit 0) needs the 3DS's common key to read: not here.
struct CiaContent {
    size_t off = 0, len = 0;
    bool encrypted = false;
    const uint8_t* tmd = nullptr;  // the TMD's signed part (signature type, signature, header, records)
    size_t tmd_len = 0;
};
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static size_t sig_len(uint32_t type) {  // a signed block's signature and padding after its 4-byte type
    switch (type) {
    case 0x10000: case 0x10003: return 0x200 + 0x3C;
    case 0x10001: case 0x10004: return 0x100 + 0x3C;
    case 0x10002: case 0x10005: return 0x3C + 0x40;
    default: return 0;
    }
}
static bool cia_parse(const uint8_t* d, size_t n, CiaContent& c) {
    if (n < 0x2040 || le32(d) != 0x2020 || (d[4] | d[5]) != 0) return false;
    auto al = [](uint64_t x) { return (x + 63) & ~(uint64_t)63; };
    const uint64_t certs = le32(d + 8), tik = le32(d + 12), tmd = le32(d + 16), content = (uint64_t)le32(d + 24) |
        (uint64_t)le32(d + 28) << 32;
    const uint64_t o_tik = al(0x2020 + certs), o_tmd = al(o_tik + tik), o_con = al(o_tmd + tmd);
    if (o_con + content > n || tmd < 0x200) return false;
    const uint8_t* t = d + o_tmd;
    const size_t sl = sig_len(be32(t));
    if (!sl || 4 + sl + 0xC4 > tmd) return false;
    const uint8_t* h = t + 4 + sl;  // the TMD's header
    const unsigned count = (unsigned)h[0x9E] << 8 | h[0x9F];
    const uint8_t* rec = h + 0xC4 + 64 * 0x24;  // (3DS TMD: 64 content info records, then the chunk records)
    if (!count || rec + 0x30 > t + tmd) return false;
    c.off = o_con;
    c.len = (size_t)be32(rec + 12);  // (size is 64-bit; a DSi ROM is far below 4 GB)
    c.encrypted = (rec[7] & 1) != 0;
    c.tmd = t;
    c.tmd_len = tmd;
    return c.len >= 0x200 && c.off + c.len <= n && be32(rec + 8) == 0;
}

// the DSi's files, from the folder `bios` (BananaOS: /data/bios), named as melonDS's libretro core names them
static std::string bios_file(const Config& cfg, const char* key, const char* name) {
    std::string v = cfg.str(key, "");
    if (!v.empty()) return v;
    return cfg.str("bios", "/data/bios") + "/" + name;
}
template <size_t N> static bool read_exact(const std::string& path, std::array<u8, N>& out) {
    std::vector<uint8_t> b;
    if (!read_file(path, b) || b.size() != N) return false;
    memcpy(out.data(), b.data(), N);
    return true;
}


// The DSi: its own BIOS images (dsi_bios9.bin and dsi_bios7.bin, 64 KB each) when both are in the BIOS folder, else
// melonDS's free DSi BIOS. Booted straight into the game (direct boot), without the DSi's menu, so no DSi firmware or
// NAND is needed to start one; a DSiWare title's own saves (public.sav, private.sav) live on the NAND, which comes
// from the user's dsi_nand.bin when there is one (setup_nand).
std::unique_ptr<NDS> Emu::make_dsi(NDSArgs&& args) {
    const std::string b9 = bios_file(cfg_, "dsi.bios9", "dsi_bios9.bin");
    const std::string b7 = bios_file(cfg_, "dsi.bios7", "dsi_bios7.bin");
    auto a9 = std::make_unique<DSiBIOSImage>();
    auto a7 = std::make_unique<DSiBIOSImage>();
    dsi_bios_ = read_exact(b9, *a9) && read_exact(b7, *a7);
    if (dsi_bios_) {
        Log(LogLevel::Info, "[dsi] DSi BIOS: %s, %s\n", b9.c_str(), b7.c_str());
    } else {
        *a9 = FreeBIOSGetTwlArm9();
        *a7 = FreeBIOSGetTwlArm7();
        // A ROM's secure area (its first 2 KB of ARM9 code) as a DSi reads it from the card or the NAND is encrypted
        // (KEY1); the key's table is in the DSi's ARM9 BIOS at 0x99A0 and in the DS's ARM7 BIOS at 0x30, the same
        // 0x1048 bytes. The free BIOS has neither: a DS ARM7 BIOS (bios7.bin) lends its table to the free one.
        std::array<u8, 0x4000> ds7{};
        const std::string d7 = bios_file(cfg_, "bios7", "bios7.bin");
        if (read_exact(d7, ds7)) {
            memcpy(a9->data() + 0x99A0, ds7.data() + 0x30, 0x1048);
            key1_ = true;
            Log(LogLevel::Info, "[dsi] free DSi BIOS, the secure area's key from %s\n", d7.c_str());
        } else {
            Log(LogLevel::Info, "[dsi] free DSi BIOS (no %s and %s; no %s for the secure area's key)\n", b9.c_str(),
                b7.c_str(), d7.c_str());
        }
    }
    DSiArgs d{std::move(args), std::move(a9), std::move(a7), std::nullopt, std::nullopt, false};
    d.NANDImage = setup_nand(*d.ARM7iBIOS);
    return std::make_unique<DSi>(std::move(d), this);
}

// DSiWare keeps its saves on the DSi's NAND (title/<category>/<id>/data/public.sav, private.sav), so with the DSi's
// own BIOS (the NAND's keys come from it) and the user's dsi_nand.bin, BananaDS runs the game on a working copy of
// that NAND (never on the user's file: <data>/dsi/nand.bin, copied once), with the title installed into it, and the
// saves as files beside the other saves (<game>.public.sav, <game>.private.sav): put in at the start when newer than
// the copy, written out at quit.
static void nand_ids(const u8* rom, u32& cat, u32& id) {
    id = le32(rom + 0x230);
    cat = le32(rom + 0x234);
}

// a DSi TMD (melonDS's DSi_TMD::TitleMetadata) for the title: from the .cia's (3DS layout: the same signature and
// header, then 64 content info records and 0x30-byte content records) when there is one, else from the ROM's header
static void make_tmd(DSi_TMD::TitleMetadata& t, const std::vector<uint8_t>& cia, const u8* rom, size_t romlen) {
    memset(&t, 0, sizeof t);
    t.SignatureType = 0x04000100;  // (0x10004 big-endian, as on the NAND)
    u32 cat, id;
    nand_ids(rom, cat, id);
    u32 content_id = 0, version = (u32)rom[0x1E];
    if (cia.size() >= 4 + 0x13C + 0xC4 + 0x900 + 0x30 && sig_len(be32(cia.data())) == 0x13C) {
        memcpy(&t, cia.data(), std::min(sizeof t - sizeof t.Contents, cia.size()));
        const uint8_t* rec = cia.data() + 4 + 0x13C + 0xC4 + 0x900;
        content_id = be32(rec);
        version = (u32)cia[4 + 0x13C + 0x9C] << 8 | cia[4 + 0x13C + 0x9D];
    }
    const u8 tid[8] = {(u8)(cat >> 24), (u8)(cat >> 16), (u8)(cat >> 8), (u8)cat,
                       (u8)(id >> 24), (u8)(id >> 16), (u8)(id >> 8), (u8)id};
    memcpy(t.TitleId, tid, 8);  // the DSi's title id (a .cia carries the 3DS's 00048004 for it)
    const u32 pub = le32(rom + 0x238), prv = le32(rom + 0x23C);
    const u8 sz[8] = {(u8)(pub >> 24), (u8)(pub >> 16), (u8)(pub >> 8), (u8)pub,
                      (u8)(prv >> 24), (u8)(prv >> 16), (u8)(prv >> 8), (u8)prv};
    memcpy(t.PublicSaveSize, sz, 4);
    memcpy(t.PrivateSaveSize, sz + 4, 4);
    t.TitleVersion = (u16)((version >> 8 & 0xFF) | (version & 0xFF) << 8);
    t.NumberOfContents = 0x0100;  // 1, big-endian
    const u8 cid[4] = {(u8)(content_id >> 24), (u8)(content_id >> 16), (u8)(content_id >> 8), (u8)content_id};
    memcpy(t.Contents.ContentId, cid, 4);
    t.Contents.ContentType[1] = 1;
    const uint64_t n = romlen;
    for (int i = 0; i < 8; i++) t.Contents.ContentSize[i] = (u8)(n >> (56 - 8 * i));
}

static time_t mtime_of(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 ? st.st_mtime : 0;
}

// The DSi's settings as its menu keeps them (shared1/TWLCFG0.dat; at 0x02000400 for a game a DSi starts): English
// (or firmware.language), the nickname, the touch screen's calibration as melonDS writes it
static DSi_NAND::DSiFirmwareSystemSettings dsi_settings(const Config& cfg) {
    DSi_NAND::DSiFirmwareSystemSettings t{};
    t.Version = 1;
    t.ConfigFlags = 0x0100000F;  // (the settings were made, the EULA agreed to, wireless on)
    t.CountryCode = 49;          // (the USA; only the menu reads it)
    long lang = cfg.num("firmware.language", 1);
    if (lang < 0 || lang > 5) lang = 1;
    t.Language = static_cast<Firmware::Language>(lang);
    t.RTCYear = 26;
    t.EULAVersion = 1;
    t.SystemMenuFreeTitleSlots = 39;
    t.TouchCalibrationADC1 = {0, 0};
    t.TouchCalibrationPixel1 = {0, 0};
    t.TouchCalibrationADC2 = {255 << 4, 191 << 4};
    t.TouchCalibrationPixel2 = {255, 191};
    t.FavoriteColor = 6;
    t.BirthdayMonth = 1;
    t.BirthdayDay = 1;
    const std::string nick = cfg.str("firmware.nickname", "BananaDS");
    for (size_t i = 0; i < nick.size() && i < 10; i++) t.Nickname[i] = (char16_t)(unsigned char)nick[i];
    t.UpdateHash();
    return t;
}

// BananaDS's own NAND, for DSiWare saves when the user has no dsi_nand.bin: a blank one made here (no file from a
// DSi in it): melonDS's NAND crypto with an invented console id and eMMC CID (the FAT's key comes from those, not from
// a BIOS), an MBR and one FAT16 partition (fatfs' f_mkfs, through melonDS's NAND layer), the folders a DSi has, and
// the settings files. Titles are installed into it as with the user's.
static bool make_nand(const std::string& path, const u8* es_keyY, const Config& cfg) {
    const long mb = std::clamp(cfg.num("dsi.nand_mb", 64), 16L, 240L);
    const off_t size = (off_t)mb << 20;
    {
        FILE* f = fopen((path + ".tmp").c_str(), "wb");
        if (!f) return false;
        u8 foot[0x40] = {};
        memcpy(foot, "DSi eMMC CID/CPU", 16);
        static const u8 cid[16] = {0x42, 0x41, 0x4E, 0x41, 0x4E, 0x41, 0x44, 0x53, 0x01, 0x00, 0x00, 0x00, 0x15, 0x01, 0x00, 0x00};
        memcpy(foot + 16, cid, 16);
        const uint64_t console_id = 0x08A15E0042414E41ull;  // (an invented one, in a DSi's range)
        memcpy(foot + 32, &console_id, 8);
        const bool ok = ftruncate(fileno(f), size) == 0 && fseeko(f, size, SEEK_SET) == 0 && fwrite(foot, 1, sizeof foot, f) == sizeof foot;
        if (fclose(f) != 0 || !ok) {
            unlink((path + ".tmp").c_str());
            return false;
        }
    }
    bool ok = false;
    if (Platform::FileHandle* fh = Platform::OpenFile(path + ".tmp", Platform::FileMode::ReadWriteExisting)) {
        DSi_NAND::NANDImage img(fh, es_keyY);
        if (img) {
            DSi_NAND::NANDMount m(img);
            FF_MKFS_PARM opt{FM_FAT, 2, 0, 0, 0};
            std::vector<u8> work(0x8000);
            FRESULT r = f_mkfs("0:", &opt, work.data(), (UINT)work.size());
            ok = r == FR_OK;
            if (!ok) Log(LogLevel::Error, "[dsi] the NAND's format failed (%d)\n", (int)r);
            for (const char* d : {"0:/sys", "0:/title", "0:/ticket", "0:/shared1", "0:/shared2", "0:/tmp", "0:/import",
                                  "0:/progress", "0:/tmp/es", "0:/sys/log"})
                if (ok) f_mkdir(d);
            if (ok) {
                const auto t = dsi_settings(cfg);
                std::vector<u8> file(0x4000, 0xFF);
                memcpy(file.data(), t.Bytes, sizeof t.Bytes);
                ok = m.ImportFile("0:/shared1/TWLCFG0.dat", file.data(), file.size()) &&
                     m.ImportFile("0:/shared1/TWLCFG1.dat", file.data(), file.size());
                std::vector<u8> hw(0x4000, 0);  // (HWINFO_S.dat: a header and the region; melonDS copies a part)
                m.ImportFile("0:/sys/HWINFO_S.dat", hw.data(), hw.size());
                DSi_NAND::DSiFirmwareSystemSettings back{};
                ok = ok && m.ReadUserData(back) && back.Language == t.Language;
            }
        }
    }
    if (ok) ok = rename((path + ".tmp").c_str(), path.c_str()) == 0;
    else unlink((path + ".tmp").c_str());
    Log(ok ? LogLevel::Info : LogLevel::Error, "[dsi] BananaDS's own NAND (%ld MB) at %s: %s\n", mb, path.c_str(),
        ok ? "made" : "couldn't be made");
    return ok;
}

std::optional<DSi_NAND::NANDImage> Emu::setup_nand(const std::array<uint8_t, 0x10000>& arm7i) {
    const std::string user = bios_file(cfg_, "dsi.nand", "dsi_nand.bin");
    struct stat st{};
    const bool have_user = stat(user.c_str(), &st) == 0;
    const char* data = getenv("BANANADS_DATA");
    const std::string dir = std::string(data && *data ? data : ".") + "/dsi";
    // the user's NAND: never run on it, on a working copy (made once); none: BananaDS's own (dsi.own_nand=off: none)
    const std::string copy = have_user ? cfg_.str("dsi.nand_copy", dir + "/nand.bin") : dir + "/nand-bananads.bin";
    mkdirs(copy.substr(0, copy.find_last_of('/')));
    const bool fresh = mtime_of(copy) == 0;
    if (fresh && have_user) {
        std::vector<uint8_t> b;
        if (!read_file(user, b) || !write_file_atomic(copy, b.data(), b.size())) {
            Log(LogLevel::Error, "[dsi] couldn't copy %s to %s\n", user.c_str(), copy.c_str());
            return std::nullopt;
        }
        Log(LogLevel::Info, "[dsi] working copy of the NAND: %s\n", copy.c_str());
    } else if (fresh) {
        if (!cfg_.flag("dsi.own_nand", true)) {
            Log(LogLevel::Info, "[dsi] no DSi NAND (%s) and dsi.own_nand=off: DSiWare saves won't be kept\n", user.c_str());
            return std::nullopt;
        }
        if (!make_nand(copy, &arm7i[0x8308], cfg_)) return std::nullopt;
    }
    Platform::FileHandle* f = Platform::OpenFile(copy, Platform::FileMode::ReadWriteExisting);
    if (!f) return std::nullopt;
    DSi_NAND::NANDImage img(f, &arm7i[0x8308]);
    if (!img) {
        Log(LogLevel::Error, "[dsi] %s isn't a DSi NAND melonDS can read (no nocash footer)\n", copy.c_str());
        notice("dsi_nand.bin can't be read", "it needs its nocash footer");
        return std::nullopt;
    }
    if (dsi_cart_) {
        const u8* rom = dsi_cart_->GetROM();
        u32 cat, id;
        nand_ids(rom, cat, id);
        if (cat == 0x00030004 || cat == 0x00030005 || cat == 0x00030015 || cat == 0x00030017) {  // a NAND title
            DSi_NAND::NANDMount m(img);
            if (!m) {
                Log(LogLevel::Error, "[dsi] the NAND's file system doesn't mount\n");
                return std::nullopt;
            }
            if (!m.TitleExists(cat, id)) {
                DSi_TMD::TitleMetadata tmd;
                const size_t len = (size_t)le32(rom + 0x210) ? std::min((size_t)le32(rom + 0x210), (size_t)dsi_cart_->GetROMLength())
                                                            : (size_t)dsi_cart_->GetROMLength();
                make_tmd(tmd, cia_tmd_, rom, len);
                const bool ok = m.ImportTitle(rom, len, tmd, false);
                Log(ok ? LogLevel::Info : LogLevel::Error, "[dsi] title %08x/%08x into the NAND: %s\n", cat, id,
                    ok ? "installed" : "failed");
            }
            dsi_title_cat_ = cat;
            dsi_title_id_ = id;
            const std::string base = save_path_.substr(0, save_path_.size() - 4);
            const struct { int type; const char* ext; } saves[] = {{DSi_NAND::TitleData_PublicSav, ".public.sav"},
                                                                {DSi_NAND::TitleData_PrivateSav, ".private.sav"}};
            for (auto& sv : saves) {
                const std::string p = base + sv.ext;
                if ((fresh || mtime_of(p) > mtime_of(copy)) && m.ImportTitleData(cat, id, sv.type, p.c_str()))
                    Log(LogLevel::Info, "[dsi] save %s into the NAND\n", p.c_str());
            }
        }
    }
    dsi_nand_ = true;
    return img;
}

// at quit: the DSiWare title's saves out of the NAND, beside the other saves
void Emu::export_dsi_saves() {
    if (!dsi_nand_ || !dsi_title_id_ || !nds_ || nds_->ConsoleType != 1) return;
    DSi_NAND::NANDImage* img = static_cast<DSi&>(*nds_).SDMMC.GetNAND();
    if (!img || !*img) return;
    DSi_NAND::NANDMount m(*img);
    if (!m) return;
    const std::string base = save_path_.substr(0, save_path_.size() - 4);
    if (m.ExportTitleData(dsi_title_cat_, dsi_title_id_, DSi_NAND::TitleData_PublicSav, (base + ".public.sav").c_str()))
        Log(LogLevel::Info, "[dsi] save %s.public.sav written\n", base.c_str());
    m.ExportTitleData(dsi_title_cat_, dsi_title_id_, DSi_NAND::TitleData_PrivateSav, (base + ".private.sav").c_str());
}

// After the direct boot: was the secure area decrypted? melonDS fills it with 0xE7FFDEFF (undefined instructions)
// when it wasn't, and the game would stop at its first call into it: say what is missing, on the panels too.
void Emu::check_secure_area() {
    NDSCart::CartCommon* cart = nds_->NDSCartSlot.GetCart();
    if (!cart) return;
    const NDSHeader& h = cart->GetHeader();
    if (h.ARM9ROMOffset < 0x4000 || h.ARM9ROMOffset >= 0x8000) return;
    const u32 a = h.ARM9RAMAddress;
    if (nds_->ARM9Read32(a + 8) != 0xE7FFDEFF || nds_->ARM9Read32(a + 0x400) != 0xE7FFDEFF) return;
    secure_missing_ = true;
    const std::string dir = cfg_.str("bios", "/data/bios");
    Log(LogLevel::Error, "[dsi] this game's secure area is encrypted and BananaDS has no key for it: put a DS ARM7 BIOS "
        "(bios7.bin, 16 KB) or the DSi's BIOS (dsi_bios9.bin + dsi_bios7.bin, 64 KB each) in %s/. The game will likely "
        "stop at a black screen without it\n", dir.c_str());
    notice("DSi game: needs a BIOS file", ("put bios7.bin in " + dir).c_str());
}

void Emu::notice(const char* line1, const char* line2) {
    std::lock_guard<std::mutex> l(notice_mu_);
    notice1_ = line1;
    notice2_ = line2 ? line2 : "";
}

bool Emu::take_notice(std::string& line1, std::string& line2) {
    std::lock_guard<std::mutex> l(notice_mu_);
    if (notice1_.empty()) return false;
    line1.swap(notice1_);
    line2.swap(notice2_);
    notice1_.clear();
    notice2_.clear();
    return true;
}

void Emu::restart() {
    nds_->Reset();
    nds_->SetupDirectBoot(game_ + ".nds");
    set_clock();
    nds_->Start();
    Log(LogLevel::Info, "[emu] %s restarted\n", game_.c_str());
}

// ---- the ROM, mapped (BananaOS 0.4.0 #11) ----
// A ROM read into memory is all anonymous RAM: 256 MB for Golden Sun: Dark Dawn on a 981 MB handheld without swap,
// which the kernel's OOM killer ended. Mapped from its file (MAP_PRIVATE: a page the game writes becomes the game's
// own), a ROM page is page cache the kernel can drop and read again; the mapping is padded to a power of two with
// zero pages, as melonDS pads a ROM (so it keeps this buffer instead of copying it). A thread at the lowest priority
// reads the file once, so the game seldom waits on the card for a page.
static std::mutex g_maps_mu;
static std::vector<std::pair<uint8_t*, size_t>> g_maps;  // each mapping: where, how long

static uint8_t* map_rom(const std::string& path, size_t& file_len, size_t& map_len) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0x200 || st.st_size > (off_t)512 * 1024 * 1024) {
        close(fd);
        return nullptr;
    }
    file_len = (size_t)st.st_size;
    map_len = 0x1000;
    while (map_len < file_len) map_len <<= 1;
    void* base = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return nullptr;
    }
    if (mmap(base, file_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
        munmap(base, map_len);
        close(fd);
        return nullptr;
    }
    close(fd);
    {
        std::lock_guard<std::mutex> l(g_maps_mu);
        g_maps.emplace_back((uint8_t*)base, map_len);
    }
    std::thread([path] {  // the file into the page cache, not into this process: the mapping's pages come cheap
        pthread_setname_np(pthread_self(), "bds-romread");
        setpriority(PRIO_PROCESS, 0, 19);
        int f = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (f < 0) return;
        std::vector<char> buf(1 << 20);
        while (read(f, buf.data(), buf.size()) > 0) {}
        close(f);
    }).detach();
    return (uint8_t*)base;
}

// Mapped only from the system's own card (/data's filesystem): a ROM on a card that can be pulled out while the game
// runs (the RG DS Plus's second slot) is read into memory, or the next page it needs would kill the emulator (SIGBUS).
// Where there is no /data (not BananaOS), every ROM is mapped.
static bool mappable(const std::string& path) {
    struct stat data{}, rom{};
    if (stat("/data", &data) != 0) return true;
    return stat(path.c_str(), &rom) == 0 && rom.st_dev == data.st_dev;
}

bool rom_unmap(uint8_t* p) {
    std::lock_guard<std::mutex> l(g_maps_mu);
    for (size_t i = 0; i < g_maps.size(); i++)
        if (g_maps[i].first == p) {
            munmap(p, g_maps[i].second);
            g_maps.erase(g_maps.begin() + i);
            return true;
        }
    return false;
}


bool Emu::load_rom(const std::string& path) {
    std::unique_ptr<u8[]> romdata;
    size_t romlen = 0, file_len = 0;
    if (cfg_.flag("rom.map", true) && mappable(path))
        if (uint8_t* m = map_rom(path, file_len, romlen)) {
            romdata.reset(m);  // (melonDS's ~CartCommon gives it back through Platform::ROMRelease: rom_unmap)
            Log(LogLevel::Info, "[emu] ROM mapped (%zu MB), read in the background\n", file_len >> 20);
        }
    if (!romdata) {  // rom.map=off, a removable card, or the mapping failed: read into memory, straight into the buffer
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        const std::streamoff n = f ? (std::streamoff)f.tellg() : -1;
        if (n >= 0x200) {
            romlen = (size_t)n;
            romdata = std::unique_ptr<u8[]>(new (std::nothrow) u8[romlen]);  // (not zeroed: every byte is read)
            f.seekg(0);
            if (!romdata || !f.read((char*)romdata.get(), n)) romdata.reset();
        }
    }
    if (!romdata) {
        Log(LogLevel::Error, "[emu] can't read the ROM %s\n", path.c_str());
        return false;
    }
    {   // a .cia (DSiWare from the 3DS's eShop): its content is the DSi ROM, taken out into a buffer of its own
        CiaContent cia;
        const size_t have = file_len ? file_len : romlen;
        if (cia_parse(romdata.get(), have, cia)) {
            if (cia.encrypted) {
                notice("This .cia is encrypted", "decrypt it on a 3DS (or a PC tool) first");
                Log(LogLevel::Error, "[dsi] %s: the .cia's content is encrypted (it needs the 3DS's keys, which BananaDS "
                    "doesn't have): decrypt the .cia first, or use the title's .nds/.dsi\n", path.c_str());
                if (rom_unmap(romdata.get())) (void)romdata.release();
                return false;
            }
            size_t len = 0x1000;
            while (len < cia.len) len <<= 1;  // (a power of two, zero-padded: melonDS keeps the buffer as it is)
            std::unique_ptr<u8[]> rom(new (std::nothrow) u8[len]());
            if (!rom) return false;
            memcpy(rom.get(), romdata.get() + cia.off, cia.len);
            cia_tmd_.assign(cia.tmd, cia.tmd + cia.tmd_len);
            if (rom_unmap(romdata.get())) (void)romdata.release();
            romdata = std::move(rom);
            romlen = len;
            Log(LogLevel::Info, "[dsi] .cia: content of %zu bytes at 0x%zx, not encrypted\n", cia.len, cia.off);
        }
    }
    // the console: console = auto (the default: a DSi for a game only a DSi plays, unit code 3 - DSiWare and DSi-only
    // cartridges; a DS for the rest, DSi-enhanced games included, as they play on a DS), ds or dsi
    {
        const u8 unit = romdata[0x12];
        const std::string want = cfg_.str("console", "auto");
        dsi_ = want == "dsi" || (want != "ds" && unit == 3);
        if (dsi_) Log(LogLevel::Info, "[dsi] DSi mode (console=%s, unit code %u)\n", want.c_str(), unit);
    }
    rom_path_ = path;
    game_ = basename_noext(path);

    std::string savedir = cfg_.str("save.dir", "");
    if (savedir.empty()) {
        size_t s = path.find_last_of('/');
        savedir = s == std::string::npos ? "." : path.substr(0, s);
    }
    if (cfg_.flag("save.per_game_folder", false)) savedir += "/" + game_;  // ROCKNIXDS #42/#56
    mkdirs(savedir);
    save_path_ = savedir + "/" + game_ + ".sav";

    std::vector<uint8_t> sav;
    NDSCart::NDSCartArgs cart_args;
    if (read_file(save_path_, sav) && !sav.empty()) {
        cart_args.SRAM = std::make_unique<u8[]>(sav.size());
        memcpy(cart_args.SRAM.get(), sav.data(), sav.size());
        cart_args.SRAMLength = (u32)sav.size();
        Log(LogLevel::Info, "[emu] save %s (%zu bytes)\n", save_path_.c_str(), sav.size());
    }

    start_mark("save read");
    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)romlen, this, std::move(cart_args));
    if (!cart) {
        if (romdata && rom_unmap(romdata.get())) (void)romdata.release();  // (rejected before melonDS took it)
        Log(LogLevel::Error, "[emu] %s isn't a DS ROM melonDS can load\n", path.c_str());
        return false;
    }

    NDSArgs args;  // FreeBIOS and a generated firmware: no BIOS or firmware files needed, as with DraStic
    if (dsi_) args.Firmware = Firmware(1);  // (a DSi's generated firmware)
    args.OutputSampleRate = 48000.0;
    if (!cfg_.flag("jit", true)) args.JIT = std::nullopt;
#ifdef BANANADS_JIT
    // jit.timing: block (the default) is melonDS's JIT's level of timing, with fast memory and per-block cycles;
    // exact is the interpreter's timing, cycle for cycle (the jit.* gates below compare in that mode)
    const bool exact = cfg_.str("jit.timing", "block") == "exact";
    if (args.JIT) args.JIT->FastMemory = !exact;
    if (exact) setenv("BANANAJIT_EXACT", "1", 1);
    else unsetenv("BANANAJIT_EXACT");
    // jit.fastloads=off: ARM9 loads through calls instead of inline (for bananads-difftest: both must agree)
    if (cfg_.flag("jit.fastloads", true)) unsetenv("BANANAJIT_NO_FASTLOADS");
    else setenv("BANANAJIT_NO_FASTLOADS", "1", 1);
    if (cfg_.flag("jit.faststores", true)) unsetenv("BANANAJIT_NO_FASTSTORES");
    else setenv("BANANAJIT_NO_FASTSTORES", "1", 1);
    if (cfg_.flag("jit.fastmem7", true)) unsetenv("BANANAJIT_NO_FASTMEM7");
    else setenv("BANANAJIT_NO_FASTMEM7", "1", 1);
    if (cfg_.flag("jit.link", true)) unsetenv("BANANAJIT_NO_LINK");  // jit.link=off: back to the loop each block
    else setenv("BANANAJIT_NO_LINK", "1", 1);
    // jit.mul=off: multiply and register-shift through the interpreter (for bananads-difftest: both must agree)
    if (cfg_.flag("jit.mul", true)) unsetenv("BANANAJIT_NO_MUL");
    else setenv("BANANAJIT_NO_MUL", "1", 1);
    // jit.sys=off: MRS/MSR (CPSR forms) and CLZ through the interpreter (for bananads-difftest)
    if (cfg_.flag("jit.sys", true)) unsetenv("BANANAJIT_NO_SYS");
    else setenv("BANANAJIT_NO_SYS", "1", 1);
    // jit.jumps=off: jumps to a register (BX/BLX, LDM/POP with the PC; block timing) through the interpreter
    if (cfg_.flag("jit.jumps", true)) unsetenv("BANANAJIT_NO_JUMPS");
    else setenv("BANANAJIT_NO_JUMPS", "1", 1);
    // jit.ldm=off: block transfers (LDM/STM/PUSH/POP) through the interpreter (for bananads-difftest)
    if (cfg_.flag("jit.ldm", true)) unsetenv("BANANAJIT_NO_LDM");
    else setenv("BANANAJIT_NO_LDM", "1", 1);
#endif

    auto& hdr = args.Firmware.GetHeader();
    auto mac = wifi_mac();
    memcpy(hdr.MacAddr.data(), mac.data(), 6);
    auto& user = args.Firmware.GetEffectiveUserData();
    std::string nick = cfg_.str("firmware.nickname", "BananaDS");
    user.NameLength = (u16)std::min<size_t>(nick.size(), 10);
    for (size_t i = 0; i < user.NameLength; i++) user.Nickname[i] = (char16_t)(unsigned char)nick[i];
    long lang = cfg_.num("firmware.language", -1);  // 0 ja, 1 en, 2 fr, 3 de, 4 it, 5 es
    if (lang >= 0 && lang <= 5) user.Settings = (u16)((user.Settings & ~7) | lang);
    args.Firmware.UpdateChecksums();

    // jit.iofast=on|off sets MELONDS_IOFAST (melonDS patch 0005: the JIT slow paths' direct routes for main RAM,
    // the GX FIFO and IE/IF/IME/DISPSTAT/VCOUNT/KEYINPUT), which the NDS reads as it's created; unset, the
    // environment decides (default on; MELONDS_IOFAST_ROUTES=mask picks routes). For A/B runs, bananads-difftest
    // --a jit.iofast=on --b jit.iofast=off.
    {
        const std::string iofast = cfg_.str("jit.iofast", "");
        if (iofast == "on") setenv("MELONDS_IOFAST", "1", 1);
        else if (iofast == "off") setenv("MELONDS_IOFAST", "0", 1);
    }
    // jit.inval16=on|off sets MELONDS_JIT_INVAL16 (melonDS patch 0016: a write drops only the compiled blocks in its
    // 16 bytes, not every block of its 512; read at the first invalidation); unset, on
    {
        const std::string v = cfg_.str("jit.inval16", "");
        if (v == "on") setenv("MELONDS_JIT_INVAL16", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_INVAL16", "0", 1);
    }
    // core.dma_block=on|off sets MELONDS_DMABLOCK (melonDS patch 0018: DMA from main RAM into a VRAM page with one
    // bank copied in blocks, its timing unit by unit as before; exact; read at the first such DMA); unset, on
    {
        const std::string v = cfg_.str("core.dma_block", "");
        if (v == "on") setenv("MELONDS_DMABLOCK", "1", 1);
        else if (v == "off") setenv("MELONDS_DMABLOCK", "0", 1);
    }
    // jit.hotsmc=on|off sets MELONDS_JIT_HOTSMC (melonDS patch 0017: blocks stay out of code the game keeps rewriting,
    // so a rewrite recompiles a few instructions, not every block that had followed a branch into them); unset, on
    {
        const std::string v = cfg_.str("jit.hotsmc", "");
        if (v == "on") setenv("MELONDS_JIT_HOTSMC", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HOTSMC", "0", 1);
    }
    // jit.hotidx=on|off sets MELONDS_JIT_HOTIDX (melonDS patch 0019: ranges with hot self-modifying code keep an
    // index of their blocks' masks, so a write there doesn't scan every block); unset, on
    {
        const std::string v = cfg_.str("jit.hotidx", "");
        if (v == "on") setenv("MELONDS_JIT_HOTIDX", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HOTIDX", "0", 1);
    }
    // jit.codegen=new|old sets MELONDS_JIT_OLDCODEGEN (melonDS patch 0007: the AArch64 JIT's code generation
    // improvements, each also off by its own MELONDS_JIT_NO* variable), read as the JIT compiler is created; unset,
    // the environment decides (default new). For A/B runs, bananads-difftest --a jit.codegen=old --b jit.codegen=new.
    {
        const std::string codegen = cfg_.str("jit.codegen", "");
        if (codegen == "new") setenv("MELONDS_JIT_OLDCODEGEN", "0", 1);
        else if (codegen == "old") setenv("MELONDS_JIT_OLDCODEGEN", "1", 1);
    }

    // jit.slice_chain=on|off sets MELONDS_JIT_SLICECHAIN (melonDS patch 0030: when the ARM9's slice ends inside a
    // block chain and the slice loop would only keep time, the exit stub does that and the chain goes on; exact),
    // read as the JIT compiler is created; unset, the environment decides (default on; off with jit.codegen=old)
    {
        const std::string v = cfg_.str("jit.slice_chain", "");
        if (v == "on") setenv("MELONDS_JIT_SLICECHAIN", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_SLICECHAIN", "0", 1);
    }

    // jit.entry_x26=on|off: off sets MELONDS_JIT_NOENTRYX26=1, on unsets it (melonDS patch 0040: ARM_Dispatch sets the
    // fast memory base in x26, so blocks have no entry prologue; read as the JIT compiler is created; default on)
    {
        const std::string v = cfg_.str("jit.entry_x26", "");
        if (v == "on") unsetenv("MELONDS_JIT_NOENTRYX26");
        else if (v == "off") setenv("MELONDS_JIT_NOENTRYX26", "1", 1);
    }

    // jit.dyncold=on|off: off sets MELONDS_JIT_NODYNCOLD=1, on unsets it (melonDS patch 0041: a return's per-site
    // cached exit keeps its miss way in the JIT's far region; default on)
    {
        const std::string v = cfg_.str("jit.dyncold", "");
        if (v == "on") unsetenv("MELONDS_JIT_NODYNCOLD");
        else if (v == "off") setenv("MELONDS_JIT_NODYNCOLD", "1", 1);
    }

    // jit.flagstub=on|off: off sets MELONDS_JIT_NOFLAGSTUB=1, on unsets it (melonDS patch 0042: an exit's stub puts the
    // pending guest flags in RCPSR instead of three instructions in every block). Default off: no check sees a
    // mistake in ExitCheckPCF's flags (the test ROMs nor Mario Kart DS's race start, docs/PERF.md)
    {
        const std::string v = cfg_.str("jit.flagstub", "off");
        if (v == "on") unsetenv("MELONDS_JIT_NOFLAGSTUB");
        else if (v == "off") setenv("MELONDS_JIT_NOFLAGSTUB", "1", 1);
    }

    // jit.hugepages=on|off sets MELONDS_JIT_HUGEPAGE (melonDS patch 0085: the JIT's code memory aligned to 2 MB and
    // madvise(MADV_HUGEPAGE)'d, for the A55's TLBs; decided as the first JIT starts; unset, on). Emulation doesn't
    // change. The JIT's first reset logs "[jit] code memory at ..., N kB on huge pages"
    {
        const std::string v = cfg_.str("jit.hugepages", "");
        if (v == "on") setenv("MELONDS_JIT_HUGEPAGE", "1", 1);
        else if (v == "off") setenv("MELONDS_JIT_HUGEPAGE", "0", 1);
    }

    // jit.cp15nop=on|off: off sets MELONDS_JIT_NOCP15NOP=1, on unsets it (melonDS patch 0086: an ARM9 MCR to a CP15
    // cache or write buffer operation, which CP15Write ignores, compiled to its cycles instead of the interpreter's
    // call; user mode keeps the call). Exact; read as the JIT compiler is created
    {
        const std::string v = cfg_.str("jit.cp15nop", "");
        if (v == "on") unsetenv("MELONDS_JIT_NOCP15NOP");
        else if (v == "off") setenv("MELONDS_JIT_NOCP15NOP", "1", 1);
    }

    // jit.fastclz=on|off: off sets MELONDS_JIT_SOFTCLZ=1, on unsets it (melonDS patch 0088: the AArch64 emitter's
    // CountLeadingZeros by CLZ instead of a bit-by-bit loop; the JIT compiles faster, the code it emits is the same).
    // Read at the emitter's first use: one setting per process
    {
        const std::string v = cfg_.str("jit.fastclz", "");
        if (v == "on") unsetenv("MELONDS_JIT_SOFTCLZ");
        else if (v == "off") setenv("MELONDS_JIT_SOFTCLZ", "1", 1);
    }

    // jit.vramfast=on|off, jit.iodirect=on|off and render.vramtrack_skip=on|off set bits 1, 2 and 4 of
    // MELONDS_MEMFAST (melonDS patch 0010, read at the JIT's reset; default all on, exact): VRAM with one bank at
    // the 16 KB straight to the bank, OAM and palette straight to the GPU; the divider, square root,
    // IPCSYNC/IPCFIFOCNT and IME/IE/IF straight to their state, other IO registers without the ARM9Read*/ARM7Read*
    // dispatch; VRAM tracking's per-line DeriveState returns at once when nothing was remapped and its banks have
    // no dirty bit. jit.wram7fast=on|off sets bit 8 (melonDS patch 0072): the ARM7's slow loads and stores to its
    // own WRAM (0x03800000) straight to it, the JIT's check then the store, a block's region checked once. Unset,
    // the environment decides; as with jit.iofast, a difftest sets them on both --a and --b.
    {
        const char* cur = getenv("MELONDS_MEMFAST");
        unsigned long mask = cur ? strtoul(cur, nullptr, 0) & 15 : 15;
        const unsigned long before = mask;
        const std::string vram = cfg_.str("jit.vramfast", ""), io = cfg_.str("jit.iodirect", "");
        const std::string track = cfg_.str("render.vramtrack_skip", ""), wram7 = cfg_.str("jit.wram7fast", "");
        if (vram == "on") mask |= 1; else if (vram == "off") mask &= ~1ul;
        if (io == "on") mask |= 2; else if (io == "off") mask &= ~2ul;
        if (track == "on") mask |= 4; else if (track == "off") mask &= ~4ul;
        if (wram7 == "on") mask |= 8; else if (wram7 == "off") mask &= ~8ul;
        if (mask != before || (!cur && (!vram.empty() || !io.empty() || !track.empty() || !wram7.empty())))
            setenv("MELONDS_MEMFAST", std::to_string(mask).c_str(), 1);
    }

    // render.geom_batch=on|off sets MELONDS_GEOMBATCH (melonDS patch 0050: with render.geom_fast, the GX FIFO's DMA
    // and IRQ checks once per run of the geometry engine while no write is stalled, instead of after every refill
    // of its pipe; exact; read as the NDS is created; default on). Unset, the environment decides; a difftest sets
    // it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_batch", "");
        if (v == "on") setenv("MELONDS_GEOMBATCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMBATCH", "0", 1);
    }
    // render.gx_dmafast=on|off sets MELONDS_GXDMAFAST (melonDS patch 0060: the GX FIFO DMA's block path unpacks its
    // words into the FIFO with the command state and the FIFO's level in registers; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmafast", "");
        if (v == "on") setenv("MELONDS_GXDMAFAST", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMAFAST", "0", 1);
    }
    // render.geom_fifo=on|off sets MELONDS_GEOMFIFO (melonDS patch 0061: with render.geom_batch and no stalled write,
    // the geometry engine's command loop reads its pipe and the GX FIFO as one queue and puts back both rings' state
    // after the loop; exact; read as the NDS is created; default on). Unset, the environment decides; a difftest
    // sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_fifo", "");
        if (v == "on") setenv("MELONDS_GEOMFIFO", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMFIFO", "0", 1);
    }
    // render.geom_queue=on|off sets MELONDS_GEOMQUEUE (melonDS patch 0080: render.geom_fifo's loop reads its queue run
    // by run with a pointer and works out the pipe's refills after the loop from the number of entries read; exact;
    // read as the NDS is created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_queue", "");
        if (v == "on") setenv("MELONDS_GEOMQUEUE", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMQUEUE", "0", 1);
    }
    // render.geom_switch=on|off sets MELONDS_GEOMSWITCH (melonDS patch 0081: render.geom_queue's loop dispatches every
    // command with one switch on it, a full vertex's two entries with a path of their own; exact; read as the NDS is
    // created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_switch", "");
        if (v == "on") setenv("MELONDS_GEOMSWITCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMSWITCH", "0", 1);
    }
    // render.geom_thread=on|off sets MELONDS_GEOMTHREAD (melonDS patch 0083: a stored polygon's build on a worker
    // thread, every timing decision kept on the emulator thread; drained at VBlank, savestates and reset; exact; read
    // as the NDS is created). Default OFF: on the RG DS Plus (four A55s shared by the emulator's thread, the 2D thread
    // and three Gengis workers) the worker cost Mario Kart DS's race start ~35 frames and ~50 repeated refreshes a
    // run on the panels (2026-10-19, perf/bananads/session3/DEVICE-RESULTS.md); it only pays with a core free.
    // Unset, the environment decides (a difftest sets it on both --a and --b), else off.
    {
        const std::string v = cfg_.str("render.geom_thread", "");
        if (v == "on") setenv("MELONDS_GEOMTHREAD", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMTHREAD", "0", 1);
        else setenv("MELONDS_GEOMTHREAD", "0", 0);
    }
    // render.gx_batch=on|off sets MELONDS_GXBATCH (melonDS patch 0063, session 2's 0021: the JIT's STMs into the GX
    // FIFO hand their words to the geometry engine in one call, through 0060's loop; exact; read at the JIT's reset;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_batch", "");
        if (v == "on") setenv("MELONDS_GXBATCH", "1", 1);
        else if (v == "off") setenv("MELONDS_GXBATCH", "0", 1);
    }
    // render.gx_str=on|off sets MELONDS_GXSTR (melonDS patch 0064, session 2's 0022: the JIT's STRs to the GX FIFO
    // port and command ports with fewer layers, and a FIFO-port word that is one of its command's parameters before
    // the last straight into the FIFO; exact; read at the JIT's reset and as the NDS is created; default on). Unset,
    // the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_str", "");
        if (v == "on") setenv("MELONDS_GXSTR", "1", 1);
        else if (v == "off") setenv("MELONDS_GXSTR", "0", 1);
    }
    // render.geom_sort=on|off sets MELONDS_GEOMSORT (melonDS patch 0065: VBlank's polygon Y-sort as a stable counting
    // sort for sets of 64 polygons or more, the order std::stable_sort gives; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_sort", "");
        if (v == "on") setenv("MELONDS_GEOMSORT", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMSORT", "0", 1);
    }
    // render.gx_dmarun=on|off sets MELONDS_GXDMARUN (melonDS patch 0066: DMA::Run9 takes the GX FIFO DMA's block path
    // in a function of its own when nothing else in Run9 would run for that call; exact; read as the NDS is created;
    // default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmarun", "");
        if (v == "on") setenv("MELONDS_GXDMARUN", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMARUN", "0", 1);
    }
    // render.gx_dmatime=on|off sets MELONDS_GXDMATIME (melonDS patch 0082: render.gx_dmarun's burst timing takes 4
    // units at a time while none of their table entries ends the table and their sum stays below the target; exact;
    // read as the NDS is created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.gx_dmatime", "");
        if (v == "on") setenv("MELONDS_GXDMATIME", "1", 1);
        else if (v == "off") setenv("MELONDS_GXDMATIME", "0", 1);
    }

    // render.geom_fast=on|off sets MELONDS_GEOMFAST (melonDS patch 0011: the 3D geometry engine's fast paths, exact;
    // read as the NDS is created; default on). Unset, the environment decides; as with jit.iofast, a difftest sets it
    // on both --a and --b.
    {
        const std::string geom = cfg_.str("render.geom_fast", "");
        if (geom == "on") setenv("MELONDS_GEOMFAST", "1", 1);
        else if (geom == "off") setenv("MELONDS_GEOMFAST", "0", 1);
    }

    start_mark("cart parsed, settings");
    setup_wifi();
    setup_local();
    start_mark("wifi");

    if (dsi_) {
        dsi_cart_ = cart.get();
        nds_ = make_dsi(std::move(args));
        dsi_cart_ = nullptr;
    }
    else nds_ = std::make_unique<NDS>(std::move(args), this);
    start_mark("NDS created");
    nds_->SetNDSCart(std::move(cart));
    start_mark("cart in");
    nds_->Reset();
    start_mark("reset");
    nds_->SetupDirectBoot(game_ + ".nds");
    start_mark("direct boot");
    debug_pc_ = (uint64_t)std::max(0L, cfg_.num("debug.pc", 0));
    if (dsi_) {
        if (!dsi_nand_) {  // no NAND for melonDS to take the DSi's settings from: the ones a DSi would hand the game
            const auto t = dsi_settings(cfg_);
            for (u32 i = 0; i < 0x128; i += 4) nds_->ARM9Write32(0x02000400 + i, le32(t.Bytes + 0x88 + i));
        }
        check_secure_area();
    }
    setup_slice();
    // core.fast_gxdma / core.fast_spu: exact shortcuts in melonDS's DMA and SPU (patches/melonds/0004), off by
    // default until measured on the handheld
    nds_->FastGXDMA = cfg_.flag("core.fast_gxdma", false);
    nds_->FastSPU = cfg_.flag("core.fast_spu", false);
    // core.fast_mix: the SPU runs only the channels whose output can change, sums with NEON, and sound DMA/capture
    // access main RAM directly (patches/melonds/0013; exact, default on)
    nds_->SPU.SetFastMix(cfg_.flag("core.fast_mix", true));
    // core.fast_samples: with core.fast_mix, a due PCM16/ADPCM channel's samples taken with its state in registers and
    // ADPCM's step from tables (patches/melonds/0070; exact)
    nds_->SPU.SetFastSamples(cfg_.flag("core.fast_samples", true));
    // core.fast_dma: 32-bit DMA from main RAM to the GX FIFO or within main RAM without the bus dispatch, and the
    // ARM9's MRAM burst timing looked up inline (patches/melonds/0013; exact, default on)
    nds_->SetFastDMA(cfg_.flag("core.fast_dma", true));
    // core.fast_sched: the scheduler keeps its next event instead of walking its list every slice (patch 0006; exact,
    // on by default since core.fast_loop needs it)
    nds_->SetFastSched(cfg_.flag("core.fast_sched", true));
    // core.fast_timers: a CPU's timers run only when one of them overflows; core.fast_loop: RunFrame's slice loop for
    // the JIT with the common cases inline, used with core.fast_sched (patch 0009; both exact, default on)
    nds_->SetFastTimers(cfg_.flag("core.fast_timers", true));
    nds_->SetFastLoop(cfg_.flag("core.fast_loop", true));
    // core.halt_skip: with both CPUs halted and nothing to wake them, the slice loop takes the times straight to the
    // next event or timer instead of slice by slice (melonDS patch 0031; exact, default on; with core.fast_loop)
    nds_->SetHaltSkip(cfg_.flag("core.halt_skip", true));
    // jit.region_pair: a CPU's PC leaving its code region checks the region it last left (ARM9: ITCM or main RAM; ARM7:
    // BIOS, main RAM or WRAM) before setting one up again (melonDS patch 0071; exact, default on; with core.fast_loop)
    nds_->SetRegionPair(cfg_.flag("jit.region_pair", true));
    // jit.region_trio (melonDS patch 0087, with jit.region_pair): two code regions kept for the swap back, not one,
    // the ARM9's BIOS and the ARM7's shared WRAM among them (an IRQ's round main RAM -> BIOS -> ITCM and back, the
    // ARM7's sound driver in shared WRAM). Exact. off: jit.region_pair's own way
    nds_->SetRegionTrio(cfg_.flag("jit.region_trio", true));

    RendererSettings rs{};
    rs.ScaleFactor = 1;
    rs.Threaded = cfg_.flag("render.threaded", true);
    // render.2d: banana (the default: the Banana 2D engine, gpu2d/) or melonds (melonDS's 2D renderer)
#ifdef BANANADS_2D
    const bool banana2d = cfg_.str("render.2d", "banana") == "banana";
    // render.2d_fast: the Banana 2D engine's NEON paths (on, the default) or the portable code (off); same pixels
    melonDS::Banana2DFast = cfg_.flag("render.2d_fast", true);
    // render.3d_share: at 2x the 2D thread reads Gengis's rows of the 3D frame instead of a 4 KB copy a line (on)
    melonDS::Banana3DShare = cfg_.flag("render.3d_share", true);
    // render.3d_wait2d: with render.3d_share, the 2D thread waits for the 3D rows, not the emulator's thread (on)
    melonDS::Banana3DWait2D = cfg_.flag("render.3d_wait2d", true);
    // render.cap_unit_a: unit A goes onto the emulator's thread for display capture only when the capture reads unit A's
    // 2D line (source A = the graphics screen); a capture of the 3D layer or of VRAM/main memory alone leaves it on the 2D thread (off by default: no game measured that captures that way)
    melonDS::BananaCapNeedA = cfg_.flag("render.cap_unit_a", false);
    // render.2d_hires_shown: at 2x, unit A's quarters 1-3 composited only on lines whose display mode shows unit A (on)
    melonDS::BananaHiresShown = cfg_.flag("render.2d_hires_shown", true);
#else
    const bool banana2d = false;
#endif
    // render.scale: 2 renders the 3D layer at 2x and gives 512x384 screens (Gengis Engine with the Banana 2D engine;
    // otherwise 1x)
    const int scale = cfg_.num("render.scale", 1) == 2 ? 2 : 1;
    scale_ = 1;
#ifdef BANANADS_GENGIS
    // render.renderer: gengis (the default: Gengis Engine, SuperDrastic's rasterizer, on render.gengis_threads
    // threads) or soft (melonDS's software renderer, render.threaded)
    if (cfg_.str("render.renderer", "gengis") == "gengis") {
        const int threads = (int)cfg_.num("render.gengis_threads", 3);
        const bool defer = cfg_.flag("render.gengis_defer", false);
        GengisRenderer3D::Help = (int)cfg_.num("render.gengis_help", 1);
        GengisRenderer3D::Nice = (int)cfg_.num("sched.gengis_nice", 0);
        // render.edge_marking, render.fog: off leaves the effect out even where the game turns it on (outlines
        // round 3D models; distance fog)
        GengisRenderer3D::SetEffects(cfg_.flag("render.edge_marking", true), cfg_.flag("render.fog", true));
#ifdef BANANADS_2D
        if (banana2d) {
            nds_->SetRenderer(std::make_unique<GengisBananaRenderer>(*nds_, threads, defer, scale));
            scale_ = scale;
        } else
#endif
            nds_->SetRenderer(std::make_unique<GengisSoftRenderer>(*nds_, threads, defer));
    } else
#endif
    {
#ifdef BANANADS_2D
        if (banana2d)
            nds_->SetRenderer(std::make_unique<BananaRenderer>(*nds_));
        else
#endif
            nds_->SetRenderer(std::make_unique<SoftRenderer>(*nds_));
        nds_->GetRenderer().SetRenderSettings(rs);
        // melonDS's renderer on its own thread draws the frame again after any load (PostSavestate), with semaphores
        // that a snapshot every frame got out of step (its pictures, and display captures, went wrong): run-ahead
        // stays off with it
        ra_soft_threaded_ = rs.Threaded;
    }
#ifdef BANANADS_2D
    // render.2d_thread: on draws the Banana 2D engine's lines on a thread of its own (gpu2d/GPU_Banana.h), the same
    // pictures (the default at render.scale=2); off (the default at 1x) draws them on the emulator's thread
    // (on by default at 2x, where unit A composites four times: HeartGold at 1104 MHz 39.8 -> 49.6 fps)
    if (banana2d) banana_ = &static_cast<BananaRenderer&>(nds_->GetRenderer());
    // render.2d_ring: split (the default) puts the job ring's counters the two threads write for every job (Head;
    // Tail and the copy log's read position) on cache lines of their own, and the emulator's thread reads the 2D
    // thread's only when the ring or the log looks full (exact: the same jobs); shared is the old layout
    if (banana2d) banana_->SetSplitRing(cfg_.str("render.2d_ring", "split") != "shared");
    // render.2d_test_delay (tests only, default 0): the 2D thread sleeps this many microseconds after each job, so that
    // it falls behind and the job ring and the copy log fill: the checks then reach their full paths
    if (banana2d) banana_->SetTestDelay((u32)cfg_.num("render.2d_test_delay", 0));
    if (banana2d && cfg_.flag("render.2d_thread", scale_ == 2)) banana_->SetThreaded2D(true);
    // render.2d_kick: how many jobs (a frame has 383: 192 lines, 191 sprite lines) go to the 2D thread before it is
    // woken; a wake of the sleeping thread is a futex call on the emulator's thread (16 was the old fixed batch)
    if (banana2d) banana_->SetKickBatch((u32)cfg_.num("render.2d_kick", 64));
    // sched.2d_nice: the 2D thread's nice value (0: the default; negative puts it ahead of Gengis's workers when the cores
    // are contended). sched.emu_nice/sched.emu_cpu (the emulator's thread) and sched.gengis_nice are set beside it
    if (banana2d) banana_->SetNice((int)cfg_.num("sched.2d_nice", 0));
    // render.2d_fence_wait: the waits for the 2D thread spin on sched_yield (spin, the old way) or block on a condition
    // variable (block). Default block since 2026-10-19: on the RG DS Plus the four A55s are shared by five threads, and the
    // emulator's thread spent ~5% of Mario Kart DS's race start spinning (a prof.hz profile on the panels)
    if (banana2d) banana_->SetFenceWaitBlocking(cfg_.str("render.2d_fence_wait", "block") != "spin");
#endif

    start_mark("renderer");
    set_clock();

    if (scale != scale_)
        Log(LogLevel::Warn, "[emu] render.scale=2 needs render.renderer=gengis and render.2d=banana: rendering at 1x\n");

    nds_->Start();
    prof_ = Profiler::create(cfg_);
    Log(LogLevel::Info, "[emu] %s started (JIT %s, MAC %02x:%02x:%02x:%02x:%02x:%02x)\n", game_.c_str(),
        cfg_.flag("jit", true) ? "on" : "off", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

// core.slice: the scheduler slice, the most system cycles (33.5 MHz) the ARM9 and then the ARM7 run before they
// are interleaved again (patches/melonds/0003). 64 is melonDS's. Longer is faster and coarser: each CPU sees the
// other's writes up to a slice late. auto (or any core.slice_idle) is adaptive: core.slice_idle cycles while
// neither CPU has touched the IPC registers or WRAMCNT for core.slice_hold cycles, core.slice (64 for auto) near them.
void Emu::setup_slice() {
    // (64, melonDS's, by default: auto was the default at render.scale=2 (HeartGold at 1104 MHz 60.2 -> 64.2 fps),
    // but Golden Sun: Dark Dawn's 3D field was never drawn with it: the game's own timing goes wrong (BananaOS 0.4.0))
    const std::string s = cfg_.str("core.slice", "64");
    const bool adaptive = s == "auto";
    const long slice = adaptive ? 64 : cfg_.num("core.slice", 64);
    long idle = cfg_.num("core.slice_idle", adaptive ? 512 : slice);
    long hold = cfg_.num("core.slice_hold", 4096);
    slice_stats_ = cfg_.num("core.slice_stats", 0);
    auto ok = [](long v) { return v >= 1 && v <= 16384; };
    if (!ok(slice) || !ok(idle) || hold < 0 || hold > (1 << 24)) {
        Log(LogLevel::Warn, "[emu] core.slice=%s / slice_idle=%ld / slice_hold=%ld out of range; using 64\n", s.c_str(),
            idle, hold);
        nds_->SetSlice(64, 64, 0);
        return;
    }
    nds_->SetSlice((s32)slice, (s32)idle, (u32)hold);
    if (slice != 64 || idle != slice)
        Log(LogLevel::Info, "[emu] scheduler slice %ld, %ld while %ld cycles away from IPC\n", slice, idle, hold);
}

void Emu::run_frame() {
#ifdef BANANADS_LAN
    if (local_) MPInterface::Get().Process();
#endif
    if (const double sk = skew_req_.load(std::memory_order_relaxed); sk != skew_cur_) {
        skew_cur_ = sk;
        nds_->SPU.SetOutputSkew(sk);
    }
    if (prof_) prof_->frame(frames_run_);  // prof.hz: on the emulator's thread, whichever runs the frames
    frames_run_++;
    nds_->RunFrame();
    if (debug_pc_ && frames_run_ % debug_pc_ == 0) {  // debug.pc=N: where both CPUs are, every N frames
        Log(LogLevel::Info, "[debug] frame %llu: ARM9 pc %08x lr %08x cpsr %08x halted %d | ARM7 pc %08x lr %08x "
            "cpsr %08x halted %d | POWCNT1 %04x IE %08x IF %08x\n", (unsigned long long)frames_run_,
            nds_->ARM9.R[15], nds_->ARM9.R[14], nds_->ARM9.CPSR, nds_->ARM9.Halted, nds_->ARM7.R[15],
            nds_->ARM7.R[14], nds_->ARM7.CPSR, nds_->ARM7.Halted, nds_->PowerControl9, nds_->IE[0], nds_->IF[0]);
        // debug.dump = "c:addr:len,...": memory as CPU c (7 or 9) reads it, once, at the third report
        if (frames_run_ == 3 * (long)debug_pc_) {
            std::string spec = cfg_.str("debug.dump", "");
            size_t at = 0;
            while (at < spec.size()) {
                size_t e = spec.find(',', at);
                if (e == std::string::npos) e = spec.size();
                unsigned c = 0, a = 0, n = 0;
                char tok[16] = {};
                if (sscanf(spec.substr(at, e - at).c_str(), "%u:%15[^:]:%x", &c, tok, &n) == 3) {
                    a = !strcmp(tok, "sp") ? (c == 7 ? nds_->ARM7.R[13] : nds_->ARM9.R[13]) : (unsigned)strtoul(tok, nullptr, 16);
                    a &= ~3u;
                }
                if (n)
                    for (unsigned i = 0; i < n; i += 16) {
                        u32 w[4];
                        for (int k = 0; k < 4; k++)
                            w[k] = c == 7 ? nds_->ARM7Read32(a + i + 4 * k) : nds_->ARM9Read32(a + i + 4 * k);
                        Log(LogLevel::Info, "[dump%u] %08x: %08x %08x %08x %08x\n", c, a + i, w[0], w[1], w[2], w[3]);
                    }
                at = e + 1;
            }
            Log(LogLevel::Info, "[debug] ARM9 r0-r7 %08x %08x %08x %08x %08x %08x %08x %08x sp %08x\n", nds_->ARM9.R[0],
                nds_->ARM9.R[1], nds_->ARM9.R[2], nds_->ARM9.R[3], nds_->ARM9.R[4], nds_->ARM9.R[5], nds_->ARM9.R[6],
                nds_->ARM9.R[7], nds_->ARM9.R[13]);
            Log(LogLevel::Info, "[debug] ARM7 r0-r7 %08x %08x %08x %08x %08x %08x %08x %08x\n", nds_->ARM7.R[0],
                nds_->ARM7.R[1], nds_->ARM7.R[2], nds_->ARM7.R[3], nds_->ARM7.R[4], nds_->ARM7.R[5], nds_->ARM7.R[6],
                nds_->ARM7.R[7]);
        }
    }
    if (const uint32_t b = nds_->GPU.GPU3D.CurRAMBank; b != bank3d_) {  // the 3D swapped at this VBlank
        bank3d_ = b;
        swaps3d_.fetch_add(1, std::memory_order_relaxed);
    }
    note_capture();
    flush_save(false);
    if (slice_stats_ > 0 && ++slice_frames_ % slice_stats_ == 0) {
        // the share of emulated time within core.slice_hold cycles of an IPC access (where the short slice runs)
        const double near = 100.0 * (double)nds_->SliceNearIPCCycles / (560190.0 * (double)slice_stats_);
        Log(LogLevel::Info, "[emu] slice: frames %ld-%ld, %.0f IPC accesses a frame, %.1f%% of the time near one\n",
            slice_frames_ - slice_stats_ + 1, slice_frames_, (double)nds_->SliceIPCAccesses / (double)slice_stats_,
            near);
        nds_->SliceIPCAccesses = 0;
        nds_->SliceNearIPCCycles = 0;
    }
}

// ---- run-ahead ----
void Emu::note_capture() {
    if (const uint32_t c = nds_->GPU.CaptureFrames; c != ra_captures_) {
        ra_captures_ = c;
        ra_capture_at_ = frames_run_;
    }
}

bool Emu::runahead_ok() const { return !ra_broken_ && !ra_soft_threaded_ && nds_ && !local_ && nds_->ConsoleType == 0; }

bool Emu::run_ahead(int n, const std::function<void()>& before_last, const std::function<void()>& after_last) {
    if (n < 1 || !runahead_ok()) return false;
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    nds_->LeanSavestate = true;
    bool ok;
    if (ra_buf_.empty()) {  // the first time: a snapshot that grows as it must, then a buffer with room to spare
        Savestate st(8 << 20);
        nds_->DoSavestate(&st);
        st.Finish();
        ok = !st.Error;
        if (ok) {
            ra_len_ = st.Length();
            ra_buf_.resize(ra_len_ + ra_len_ / 4 + (1 << 20));
            memcpy(ra_buf_.data(), st.Buffer(), ra_len_);
            Log(LogLevel::Info, "[runahead] %d frame%s ahead; the snapshot is %.1f MB\n", n, n > 1 ? "s" : "",
                ra_len_ / 1048576.0);
        }
    } else {
        Savestate st(ra_buf_.data(), (u32)ra_buf_.size(), true);
        nds_->DoSavestate(&st);
        st.Finish();
        ok = !st.Error;
        ra_len_ = st.Length();
        if (!ok) ra_buf_.clear();  // (it didn't fit: made again, bigger, next time)
    }
    const auto t1 = clk::now();
    if (!ok) {
        nds_->LeanSavestate = false;
        Log(LogLevel::Warn, "[runahead] the snapshot failed; this frame isn't run ahead\n");
        return false;
    }
#ifdef JIT_ENABLED
    nds_->JIT.TrackNewBlocks(true);
#endif
    nds_->SPU.DiscardOutput = true;
    hidden_ = true;
    for (int i = 0; i < n; i++) {
        if (i == n - 1 && before_last) before_last();
        nds_->RunFrame();
        note_capture();
    }
    if (after_last) after_last();
    const auto t2 = clk::now();
    Savestate ld(ra_buf_.data(), (u32)ra_len_, false);
    ok = !ld.Error && nds_->DoSavestate(&ld);
    hidden_ = false;
    nds_->SPU.DiscardOutput = false;
    nds_->LeanSavestate = false;
    if (ok && frames_run_ - ra_capture_at_ <= 120) {
        // a game that captures the display (into VRAM, which is the game's state): the 3D frame the real next frame
        // shows and captures is drawn again from the state put back, not left as the frame ahead drew it
        nds_->GPU.GetRenderer().Start3DRendering();
    }
    if (!ok) {
        // (the game goes on from the frame ahead: a skip of n frames, once; run-ahead off from here)
        ra_broken_ = true;
#ifdef JIT_ENABLED
        nds_->JIT.Reset();
#endif
        Log(LogLevel::Error, "[runahead] the snapshot couldn't be loaded back: run-ahead off\n");
    }
    ra_save_ms_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    ra_load_ms_ = std::chrono::duration<double, std::milli>(clk::now() - t2).count();
    return true;
}

void Emu::set_cheats(std::vector<melonDS::ARCode> codes) { nds_->AREngine.Cheats = std::move(codes); }
void Emu::set_keys(uint32_t pressed) { nds_->SetKeyMask(~pressed & 0xFFF); }
void Emu::set_lid(bool closed) { lid_closed_ = closed; nds_->SetLidClosed(closed); }
void Emu::touch(int x, int y) {
    nds_->TouchScreen((u16)std::clamp(x, 0, 255), (u16)std::clamp(y, 0, 191));
}
void Emu::release_touch() { nds_->ReleaseScreen(); }

const uint8_t* Emu::main_ram() const { return nds_->MainRAM; }
const uint8_t* Emu::dtcm() const { return nds_->ARM9.DTCM; }
uint32_t Emu::main_ram_size() const { return nds_->MainRAMMask + 1; }
void Emu::cpu_regs(int cpu, uint32_t r[17]) const {
    const ARM& a = cpu == 0 ? (const ARM&)nds_->ARM9 : (const ARM&)nds_->ARM7;
    memcpy(r, a.R, 16 * 4);
    r[16] = a.CPSR;
}

bool Emu::set_lazy_2d(bool on) {
#ifdef BANANADS_2D
    if (!banana_) return false;
    banana_->SetLazyFrames(on);
    return true;
#else
    (void)on;
    return false;
#endif
}
bool Emu::frame_fence(uint32_t& fence) const {
#ifdef BANANADS_2D
    return banana_ && banana_->FrameFence(fence);
#else
    (void)fence;
    return false;
#endif
}
void Emu::wait_frame(uint32_t fence) const {
#ifdef BANANADS_2D
    if (banana_) banana_->WaitFence(fence);
#else
    (void)fence;
#endif
}

bool Emu::draw_into(uint32_t* top, uint32_t* bottom) {
#ifdef BANANADS_2D
    if (!banana_) return false;
    banana_->SetOutput(top, bottom);
    return true;
#else
    (void)top, (void)bottom;
    return false;
#endif
}

const uint32_t* Emu::top() const {
    void *t = nullptr, *b = nullptr;
    nds_->GPU.GetFramebuffers(&t, &b);
    return (const uint32_t*)t;
}

const uint32_t* Emu::bottom() const {
    void *t = nullptr, *b = nullptr;
    nds_->GPU.GetFramebuffers(&t, &b);
    return (const uint32_t*)b;
}

void Emu::set_audio_rate(double hz) {
    if (nds_) nds_->SPU.SetOutputSampleRate(hz);
}

int Emu::audio_backlog() const { return nds_->SPU.GetOutputSize(); }

int Emu::audio(int16_t* stereo, int max_frames) {
    int n = nds_->SPU.ReadOutput(stereo, max_frames);
    if (n > 0) mic_.note_output(stereo, n * 2);  // the echo gate wants to know what the speakers play
    return n;
}

void Emu::write_save(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n) {
    if (hidden_) return;  // a frame run ahead (run_ahead): undone, and written again when it runs for real
    // The game writes its save in many small pieces; it goes to disk a second after the last one. Only the piece
    // that changed is copied (#33): copying the whole save at every page cost Nintendogs' 256 KB save a 256 KB copy
    // per page written, frames of the emulator's thread over a whole save
    if (len != save_len_ || !save_copy_) {
        save_copy_ = std::make_unique<uint8_t[]>(len);
        save_len_ = len;
        memcpy(save_copy_.get(), data, len);
    } else if (n > 0 && off < len && n <= len - off) {
        memcpy(save_copy_.get() + off, data + off, n);
    } else {
        memcpy(save_copy_.get(), data, len);
    }
    save_dirty_us_ = now_us();
}

void Emu::save_writer() {
    for (;;) {
        std::vector<uint8_t> buf;
        {
            std::unique_lock<std::mutex> l(save_mu_);
            save_cv_.wait(l, [this] { return save_have_ || save_stop_; });
            if (!save_have_) return;  // stopping, and nothing left to write
            buf.swap(save_pending_);
            save_have_ = false;
        }
        const uint64_t t0 = now_us();
        if (write_file_atomic(save_path_, buf.data(), buf.size()))
            Log(LogLevel::Info, "[emu] saved %s (%.0f ms, beside the game)\n", save_path_.c_str(), (now_us() - t0) / 1000.0);
        else
            Log(LogLevel::Error, "[emu] couldn't write %s: %s\n", save_path_.c_str(), strerror(errno));
    }
}

// a second after the game's last write to its save, the save handed to the writer's thread; now (at quit): the
// writer finishes what it has, then the latest is written here
void Emu::flush_save(bool now) {
    if (now) {
        if (save_thr_.joinable()) {
            {
                std::lock_guard<std::mutex> l(save_mu_);
                save_stop_ = true;
            }
            save_cv_.notify_one();
            save_thr_.join();
        }
        if (!save_dirty_us_) return;
        if (write_file_atomic(save_path_, save_copy_.get(), save_len_))
            Log(LogLevel::Info, "[emu] saved %s\n", save_path_.c_str());
        else
            Log(LogLevel::Error, "[emu] couldn't write %s: %s\n", save_path_.c_str(), strerror(errno));
        save_dirty_us_ = 0;
        return;
    }
    if (!save_dirty_us_ || now_us() - save_dirty_us_ < 1000000) return;
    {
        std::lock_guard<std::mutex> l(save_mu_);
        save_pending_.assign(save_copy_.get(), save_copy_.get() + save_len_);
        save_have_ = true;
    }
    if (!save_thr_.joinable() && !save_stop_) {
        save_thr_ = std::thread([this] {
            pthread_setname_np(pthread_self(), "bds-save");
            save_writer();
        });
    }
    save_cv_.notify_one();
    save_dirty_us_ = 0;
}

std::string Emu::state_path(int slot) const {
    std::string dir = cfg_.str("state.dir", "");
    if (dir.empty()) {
        size_t s = save_path_.find_last_of('/');
        dir = s == std::string::npos ? "." : save_path_.substr(0, s);
    }
    return dir + "/" + game_ + (slot == 0 ? ".resume" : "." + std::to_string(slot)) + ".mst";
}

bool Emu::save_state(int slot) {
    Savestate st;
    if (st.Error) return false;
    nds_->DoSavestate(&st);
    if (st.Error) return false;
    std::string p = state_path(slot);
    mkdirs(p.substr(0, p.find_last_of('/')));
    if (!write_file_atomic(p, st.Buffer(), st.Length())) return false;
    // Both screens beside it, for the in-game menu's slot pictures (256x384 XRGB8888, top over bottom).
    std::vector<uint32_t> pic(256 * 384);
    const int s = scale_;  // (at 2x every other pixel of every other row)
    for (int y = 0; y < 192; y++)
        for (int x = 0; x < 256; x++) {
            pic[y * 256 + x] = top()[(y * 256 * s + x) * s];
            pic[(192 + y) * 256 + x] = bottom()[(y * 256 * s + x) * s];
        }
    write_file_atomic(p + ".pic", pic.data(), pic.size() * 4);
    Log(LogLevel::Info, "[emu] state saved: %s\n", p.c_str());
    return true;
}

bool Emu::load_state(int slot) {
    std::vector<uint8_t> buf;
    std::string p = state_path(slot);
    if (!read_file(p, buf) || buf.empty()) return false;
    Savestate st(buf.data(), (u32)buf.size(), false);
    if (st.Error || !nds_->DoSavestate(&st)) {
        Log(LogLevel::Error, "[emu] %s couldn't be loaded\n", p.c_str());
        return false;
    }
    // the lid as it is now: melonDS keeps it outside its savestates (KeyInput), and a state saved with the lid shut
    // (asleep on the charger, then a resume save at shutdown) has the game in its sleep mode, waiting for the
    // lid-open interrupt that nothing would send again. Open, this raises that interrupt; a game that isn't asleep
    // has it off.
    nds_->SetLidClosed(lid_closed_);
    Log(LogLevel::Info, "[emu] state loaded: %s\n", p.c_str());
    return true;
}

}  // namespace bananads
