// bananads-difftest: runs a ROM on two CPU back-ends (by default melonDS's interpreter, then its JIT) and
// says where they part. This is the method the DraStic JIT study used to find its JIT's bugs (ASR-by-register
// carry, QADD/QDADD/QDSUB, SMLAxy's Q flag, SMLALxy's halves, MSR cpsr_f and Q): a test ROM runs the same
// instructions both ways and the results are compared. It works for any ROM, with any JIT BananaDS gets.
//
//   bananads-difftest [options] rom.nds
//     --frames N            frames to run (default 600)
//     --region ADDR:LEN     compare only this main-RAM window (a test ROM's result block), e.g. 0x02300000:0x1000
//     --every               compare after every frame and stop at the first that differs (default: only at the end)
//     --a K=V / --b K=V     settings for run A / run B (default A: jit=off, B: jit=on)
//     --dump ADDR:LEN       print run A's bytes there at the end (to see what a test ROM wrote)
//     --input SPEC          scripted presses for both runs, as headless.input ("<frame>:<keys>[:<held>]",...)
//     --from N              with --every, compare from frame N on (hashing all of main RAM each frame is slow)
//     --regs                hash both CPUs' registers with main RAM (catches timing that main RAM doesn't show yet)
//     --timing              hash the geometry engine's timing state too (its cycle count and timestamp, GXSTAT, the
//                           FIFO's and pipe's levels, the ARM9's timestamp): a timing change that main RAM doesn't
//                           show (a game that waits for VBlank absorbs it) shows here
//     --fresh-save          delete the ROM's save (<save.dir>/<ROM name>.sav) before each run (a game that saves
//                           during the run, e.g. Mario Kart DS's first start, would else start run B from run A's)
//
// The two back-ends aren't cycle-exact with each other (melonDS's JIT counts cycles per block), so a game can
// drift apart in timing without a bug. Test ROMs that compute, write a result block and halt are the reliable
// input; --region picks that block. Exit code: 0 the same, 1 different, 2 usage or load error.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "Platform.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"
#include "config.h"
#include "emu.h"

namespace bananads {
extern int g_log_level;
}
using namespace bananads;

struct Snapshot {
    std::vector<uint8_t> ram;
    std::vector<uint32_t> screens;
    uint32_t regs[2][17];
};

struct Opts {
    std::string rom;
    long frames = 600;
    uint32_t addr = 0, len = 0;  // len 0 = all of main RAM
    bool every = false;
    uint32_t dump_addr = 0, dump_len = 0;
    std::vector<std::string> a{"jit=off"}, b{"jit=on"};
    std::string input;
    long from = 0;
    bool fresh = false;
    bool regs = false;
    bool timing = false;
};

// headless.input's format (src/video_headless.cpp): "<frame>:<keys>[:<frames held>]" separated by commas, keys joined
// by '+', held 6 frames unless given
struct Press { long at, len; uint32_t keys; };
static std::vector<Press> parse_input(const std::string& spec) {
    static const char* names[12] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
    std::vector<Press> out;
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(',', i);
        std::string item = spec.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? spec.size() : e + 1;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = item.find(':', c1 + 1);
        Press p{atol(item.c_str()), c2 == std::string::npos ? 6 : atol(item.c_str() + c2 + 1), 0};
        std::string keys = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        size_t k = 0;
        while (k <= keys.size()) {
            size_t plus = keys.find('+', k);
            std::string name = keys.substr(k, plus == std::string::npos ? std::string::npos : plus - k);
            for (int b = 0; b < 12; b++)
                if (name == names[b]) p.keys |= 1u << b;
            if (plus == std::string::npos) break;
            k = plus + 1;
        }
        out.push_back(p);
    }
    return out;
}

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static Config base_config(const std::vector<std::string>& sets) {
    Config c;
    c.set("wifi.dns", "off");
    c.set("resume.on_start", "off");
    c.set("save.dir", "/tmp/bananads-difftest");
    c.set("render.threaded", "off");  // the same pictures every run
    for (auto& s : sets) c.set_line(s);
    return c;
}

// Runs to `stop` frames (or the first frame whose hash differs from `ref`, with --every) and snapshots it.
static long run(const Opts& o, const std::vector<std::string>& sets, long stop, std::vector<uint64_t>* hashes,
                const std::vector<uint64_t>* ref, Snapshot* snap) {
    const Config cfg = base_config(sets);
    if (o.fresh) {
        std::string name = o.rom.substr(o.rom.find_last_of('/') + 1);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".nds") == 0) name.resize(name.size() - 4);
        unlink((cfg.str("save.dir", "/tmp/bananads-difftest") + "/" + name + ".sav").c_str());
    }
    Emu emu(cfg);
    if (!emu.load_rom(o.rom)) exit(2);
    const std::vector<Press> input = parse_input(o.input);
    long f = 0;
    for (; f < stop; f++) {
        if (!input.empty()) {
            uint32_t keys = 0;
            for (const Press& p : input)
                if (f >= p.at && f < p.at + p.len) keys |= p.keys;
            emu.set_keys(keys);
        }
        emu.run_frame();
        int16_t snd[4096 * 2];
        while (emu.audio(snd, 4096) > 0) {}
        if (o.every && f < o.from && f != stop - 1) {
            if (hashes) hashes->push_back(0);
        } else if (o.every || f == stop - 1) {
            const uint8_t* ram = emu.main_ram();
            uint32_t off = o.len ? (o.addr & (emu.main_ram_size() - 1)) : 0;
            uint32_t n = o.len ? o.len : emu.main_ram_size();
            uint64_t h = fnv(ram + off, n);
            if (o.regs) {
                uint32_t r[2][17];
                emu.cpu_regs(0, r[0]);
                emu.cpu_regs(1, r[1]);
                h ^= fnv((const uint8_t*)r, sizeof(r)) * 31;
            }
            if (o.timing) {
                const melonDS::NDS& nds = *emu.nds();
                const melonDS::GPU3D& g = nds.GPU.GPU3D;
                const uint64_t t[6] = {(uint64_t)(int64_t)g.CycleCount, g.Timestamp, g.GXStat,
                                       g.CmdFIFO.Level() | ((uint64_t)g.CmdPIPE.Level() << 32), nds.ARM9Timestamp,
                                       g.NumPolygons | ((uint64_t)g.NumVertices << 32)};
                h ^= fnv((const uint8_t*)t, sizeof(t)) * 131;
            }
            if (hashes) hashes->push_back(h);
            if (ref && (size_t)f < ref->size() && (*ref)[o.every ? f : 0] != h) {
                f++;
                break;
            }
        }
    }
    if (snap) {
        uint32_t off = o.len ? (o.addr & (emu.main_ram_size() - 1)) : 0;
        uint32_t n = o.len ? o.len : emu.main_ram_size();
        snap->ram.assign(emu.main_ram() + off, emu.main_ram() + off + n);
        snap->screens.assign(emu.top(), emu.top() + 256 * 192);
        snap->screens.insert(snap->screens.end(), emu.bottom(), emu.bottom() + 256 * 192);
        emu.cpu_regs(0, snap->regs[0]);
        emu.cpu_regs(1, snap->regs[1]);
    }
    return f;
}

static void report(const Opts& o, const Snapshot& A, const Snapshot& B, long frame) {
    printf("DIFFERENT after frame %ld\n", frame);
    int shown = 0;
    uint32_t base = o.len ? o.addr : 0x02000000;
    for (size_t i = 0; i < A.ram.size() && shown < 16; i++) {
        if (A.ram[i] == B.ram[i]) continue;
        size_t j = i;
        while (j < A.ram.size() && A.ram[j] != B.ram[j] && j - i < 16) j++;
        printf("  %08zx:", base + i);
        for (size_t k = i; k < j; k++) printf(" %02x/%02x", A.ram[k], B.ram[k]);
        printf("   (A/B)\n");
        shown++;
        i = j;
    }
    static const char* name[17] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8",
                                   "r9", "r10", "r11", "r12", "sp", "lr", "pc", "cpsr"};
    printf("  ARM9 pc: %08x / %08x, ARM7 pc: %08x / %08x\n", A.regs[0][15], B.regs[0][15], A.regs[1][15],
           B.regs[1][15]);
    for (int c = 0; c < 2; c++)
        for (int r = 0; r < 17; r++)
            if (r != 15 && A.regs[c][r] != B.regs[c][r])
                printf("  %s %s: %08x / %08x\n", c ? "ARM7" : "ARM9", name[r], A.regs[c][r], B.regs[c][r]);
    size_t px = 0;
    for (size_t i = 0; i < A.screens.size(); i++) px += A.screens[i] != B.screens[i];
    printf("  %zu of %zu pixels differ\n", px, A.screens.size());
}

int main(int argc, char** argv) {
    Opts o;
    bool a_set = false, b_set = false;
    for (int i = 1; i < argc; i++) {
        std::string s = argv[i];
        if (s == "--frames" && i + 1 < argc) o.frames = strtol(argv[++i], nullptr, 0);
        else if (s == "--every") o.every = true;
        else if (s == "--input" && i + 1 < argc) o.input = argv[++i];
        else if (s == "--from" && i + 1 < argc) o.from = strtol(argv[++i], nullptr, 0);
        else if (s == "--fresh-save") o.fresh = true;
        else if (s == "--regs") o.regs = true;
        else if (s == "--timing") o.timing = true;
        else if (s == "--region" && i + 1 < argc) {
            std::string r = argv[++i];
            size_t c = r.find(':');
            if (c == std::string::npos) return 2;
            o.addr = (uint32_t)strtoul(r.substr(0, c).c_str(), nullptr, 0);
            o.len = (uint32_t)strtoul(r.substr(c + 1).c_str(), nullptr, 0);
        } else if (s == "--dump" && i + 1 < argc) {
            std::string r = argv[++i];
            size_t c = r.find(':');
            if (c == std::string::npos) return 2;
            o.dump_addr = (uint32_t)strtoul(r.substr(0, c).c_str(), nullptr, 0);
            o.dump_len = (uint32_t)strtoul(r.substr(c + 1).c_str(), nullptr, 0);
        } else if (s == "--a" && i + 1 < argc) {
            if (!a_set) o.a.clear(), a_set = true;
            o.a.push_back(argv[++i]);
        } else if (s == "--b" && i + 1 < argc) {
            if (!b_set) o.b.clear(), b_set = true;
            o.b.push_back(argv[++i]);
        } else if (s[0] != '-') o.rom = s;
        else {
            fprintf(stderr, "usage: bananads-difftest [--frames N] [--region ADDR:LEN] [--every] [--from N] "
                            "[--input SPEC] [--fresh-save] [--regs] [--timing] [--a K=V]... [--b K=V]... rom.nds\n");
            return 2;
        }
    }
    if (o.rom.empty() || o.frames <= 0) return 2;
    g_log_level = 2;  // warnings and errors only: the report is the output

    std::vector<uint64_t> ha, hb;
    run(o, o.a, o.frames, &ha, nullptr, nullptr);
    long f = run(o, o.b, o.frames, &hb, o.every ? &ha : nullptr, nullptr);
    // With --every, run B stops at the first frame that differs (f < frames, or the last frame itself).
    bool differ = o.every ? hb.back() != ha[hb.size() - 1] : hb.back() != ha.back();
    if (o.dump_len) {
        Opts d = o;
        d.addr = o.dump_addr;
        d.len = o.dump_len;
        Snapshot S;
        run(d, o.a, o.frames, nullptr, nullptr, &S);
        printf("run A at %08x:", o.dump_addr);
        for (uint8_t b : S.ram) printf(" %02x", b);
        printf("\n");
    }
    if (!differ) {
        printf("SAME after %ld frames (%s)\n", o.frames, o.len ? "region" : "all of main RAM");
        return 0;
    }
    Snapshot A, B;
    run(o, o.a, f, nullptr, nullptr, &A);
    run(o, o.b, f, nullptr, nullptr, &B);
    report(o, A, B, f);
    return 1;
}
