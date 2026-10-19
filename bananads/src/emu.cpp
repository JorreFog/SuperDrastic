#include "emu.h"
#ifdef BANANADS_GENGIS
#include "gengis_renderer.h"
#endif

#include <sys/stat.h>
#include <unistd.h>

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

Emu::~Emu() {
    if (prof_) prof_->finish();  // (its report: the emulator's thread is done by now)
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

    auto cart = NDSCart::ParseROM(std::move(romdata), (u32)romlen, this, std::move(cart_args));
    if (!cart) {
        if (romdata && rom_unmap(romdata.get())) (void)romdata.release();  // (rejected before melonDS took it)
        Log(LogLevel::Error, "[emu] %s isn't a DS ROM melonDS can load\n", path.c_str());
        return false;
    }

    NDSArgs args;  // FreeBIOS and a generated firmware: no BIOS or firmware files needed, as with DraStic
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
    // as the NDS is created; default on). Unset, the environment decides; a difftest sets it on both --a and --b.
    {
        const std::string v = cfg_.str("render.geom_thread", "");
        if (v == "on") setenv("MELONDS_GEOMTHREAD", "1", 1);
        else if (v == "off") setenv("MELONDS_GEOMTHREAD", "0", 1);
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

    setup_wifi();
    setup_local();

    nds_ = std::make_unique<NDS>(std::move(args), this);
    nds_->SetNDSCart(std::move(cart));
    nds_->Reset();
    nds_->SetupDirectBoot(game_ + ".nds");
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
#endif

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

void Emu::set_cheats(std::vector<melonDS::ARCode> codes) { nds_->AREngine.Cheats = std::move(codes); }
void Emu::set_keys(uint32_t pressed) { nds_->SetKeyMask(~pressed & 0xFFF); }
void Emu::set_lid(bool closed) { nds_->SetLidClosed(closed); }
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
    Log(LogLevel::Info, "[emu] state loaded: %s\n", p.c_str());
    return true;
}

}  // namespace bananads
