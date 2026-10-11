#include "emu.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#include "Args.h"
#include "GPU_Soft.h"
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
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
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

Emu::~Emu() {
    flush_save(true);
    if (nds_) nds_->Stop();
    if (net_) net_->UnregisterInstance(0);
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

bool Emu::load_rom(const std::string& path) {
    std::vector<uint8_t> rom;
    if (!read_file(path, rom) || rom.size() < 0x200) {
        Log(LogLevel::Error, "[emu] can't read the ROM %s\n", path.c_str());
        return false;
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

    auto romdata = std::make_unique<u8[]>(rom.size());
    memcpy(romdata.get(), rom.data(), rom.size());
    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)rom.size(), this, std::move(cart_args));
    if (!cart) {
        Log(LogLevel::Error, "[emu] %s isn't a DS ROM melonDS can load\n", path.c_str());
        return false;
    }

    NDSArgs args;  // FreeBIOS and a generated firmware: no BIOS or firmware files needed, as with DraStic
    args.OutputSampleRate = 48000.0;
    if (!cfg_.flag("jit", true)) args.JIT = std::nullopt;
#ifdef BANANADS_JIT
    else args.JIT->FastMemory = false;  // BananaJIT doesn't do fastmem yet (docs/JIT.md)
    // jit.fastloads=off: ARM9 loads through calls instead of inline (for bananads-difftest: both must agree)
    if (cfg_.flag("jit.fastloads", true)) unsetenv("BANANAJIT_NO_FASTLOADS");
    else setenv("BANANAJIT_NO_FASTLOADS", "1", 1);
    if (cfg_.flag("jit.faststores", true)) unsetenv("BANANAJIT_NO_FASTSTORES");
    else setenv("BANANAJIT_NO_FASTSTORES", "1", 1);
    if (cfg_.flag("jit.link", true)) unsetenv("BANANAJIT_NO_LINK");  // jit.link=off: back to the loop each block
    else setenv("BANANAJIT_NO_LINK", "1", 1);
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

    setup_wifi();
    setup_local();

    nds_ = std::make_unique<NDS>(std::move(args), this);
    nds_->SetNDSCart(std::move(cart));
    nds_->Reset();
    nds_->SetupDirectBoot(game_ + ".nds");

    RendererSettings rs{};
    rs.ScaleFactor = 1;
    rs.Threaded = cfg_.flag("render.threaded", true);
    nds_->SetRenderer(std::make_unique<SoftRenderer>(*nds_));
    nds_->GetRenderer().SetRenderSettings(rs);

    time_t t = time(nullptr);
    tm lt;
    localtime_r(&t, &lt);
    nds_->RTC.SetDateTime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);

    nds_->Start();
    Log(LogLevel::Info, "[emu] %s started (JIT %s, MAC %02x:%02x:%02x:%02x:%02x:%02x)\n", game_.c_str(),
        cfg_.flag("jit", true) ? "on" : "off", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

void Emu::run_frame() {
#ifdef BANANADS_LAN
    if (local_) MPInterface::Get().Process();
#endif
    nds_->RunFrame();
    flush_save(false);
}

void Emu::set_keys(uint32_t pressed) { nds_->SetKeyMask(~pressed & 0xFFF); }
void Emu::set_lid(bool closed) { nds_->SetLidClosed(closed); }
void Emu::touch(int x, int y) {
    nds_->TouchScreen((u16)std::clamp(x, 0, 255), (u16)std::clamp(y, 0, 191));
}
void Emu::release_touch() { nds_->ReleaseScreen(); }

const uint8_t* Emu::main_ram() const { return nds_->MainRAM; }
uint32_t Emu::main_ram_size() const { return nds_->MainRAMMask + 1; }
void Emu::cpu_regs(int cpu, uint32_t r[17]) const {
    const ARM& a = cpu == 0 ? (const ARM&)nds_->ARM9 : (const ARM&)nds_->ARM7;
    memcpy(r, a.R, 16 * 4);
    r[16] = a.CPSR;
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

int Emu::audio(int16_t* stereo, int max_frames) {
    int n = nds_->SPU.ReadOutput(stereo, max_frames);
    if (n > 0) mic_.note_output(stereo, n * 2);  // the echo gate wants to know what the speakers play
    return n;
}

void Emu::write_save(const uint8_t* data, uint32_t len, uint32_t, uint32_t) {
    // The game writes its save in many small pieces; it goes to disk a second after the last one.
    if (len != save_len_) {
        save_copy_ = std::make_unique<uint8_t[]>(len);
        save_len_ = len;
    }
    memcpy(save_copy_.get(), data, len);
    save_dirty_us_ = now_us();
}

void Emu::flush_save(bool now) {
    if (!save_dirty_us_ || (!now && now_us() - save_dirty_us_ < 1000000)) return;
    if (write_file_atomic(save_path_, save_copy_.get(), save_len_))
        Log(LogLevel::Info, "[emu] saved %s\n", save_path_.c_str());
    else
        Log(LogLevel::Error, "[emu] couldn't write %s: %s\n", save_path_.c_str(), strerror(errno));
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
    memcpy(pic.data(), top(), 256 * 192 * 4);
    memcpy(pic.data() + 256 * 192, bottom(), 256 * 192 * 4);
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
    Log(LogLevel::Info, "[emu] state loaded: %s\n", p.c_str());
    return true;
}

}  // namespace bananads
