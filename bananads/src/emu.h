// One emulated DS: melonDS's NDS plus what BananaDS adds around it (saves next to the game's name in the save
// folder, the microphone, Wi-Fi, savestates with a picture for the in-game menu).
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ARCodeFile.h"
#include "Args.h"
#include "NDSCart/CartCommon.h"
#include "config.h"
#include "mic.h"
#include "prof.h"

namespace melonDS { class NDS; class Net; class BananaRenderer; }

namespace bananads {
// with log.time: "[start] <what>" in the log, the start of a game timed step by step (platform.cpp)
void start_mark(const char* what);
bool rom_unmap(uint8_t* rom);  // a ROM Emu::load_rom mapped: unmapped, true (melonDS's Platform::ROMRelease)
}

namespace bananads {

// DS buttons as melonDS's key mask wants them (bit set = pressed; Emu inverts).
enum Key : uint32_t {
    KeyA = 1 << 0, KeyB = 1 << 1, KeySelect = 1 << 2, KeyStart = 1 << 3,
    KeyRight = 1 << 4, KeyLeft = 1 << 5, KeyUp = 1 << 6, KeyDown = 1 << 7,
    KeyR = 1 << 8, KeyL = 1 << 9, KeyX = 1 << 10, KeyY = 1 << 11,
};

// sched.emu_excl: the core the emulator's thread has to itself (-1: none); a thread it starts later moves itself off
// that core with sched_off_emu_core() (else it would inherit the emulator's one-core mask)
extern int g_emu_core;
void sched_off_emu_core();

class Emu {
public:
    explicit Emu(const Config& cfg);
    ~Emu();
    // the quit's part of the destructor: the save written out, the console stopped (once; the destructor's too)
    void shutdown();

    bool load_rom(const std::string& path);  // also loads its save; false with a logged reason
    bool is_dsi() const { return dsi_; }  // the game runs on a DSi (console = auto picks it for DSi-only games)
    // a message for the player, shown on the panels when the game starts (the frontend takes it once): what's missing
    void notice(const char* line1, const char* line2);
    bool take_notice(std::string& line1, std::string& line2);
    void run_frame();                          // one DS frame (~1/59.83 s)
    // the game's own frames: its 3D buffer swaps so far (a game that can't finish a frame in one DS frame swaps
    // every other one or less, as on a DS, while the emulator still runs 60 DS frames a second); any thread
    long swaps3d() const { return swaps3d_.load(std::memory_order_relaxed); }
    void set_keys(uint32_t pressed);
    void set_lid(bool closed);                 // closing the lid puts the game to sleep
    void touch(int x, int y);                  // bottom-screen pixels, 0..255 x 0..191
    void release_touch();

    // 256x192 XRGB8888 each (times scale() both ways), valid until the next run_frame().
    const uint32_t* top() const;
    const uint32_t* bottom() const;
    int scale() const { return scale_; }  // render.scale: 1, or 2 with Gengis Engine and the Banana 2D engine
    // The next frames drawn straight into these instead (frontend=kms: the scanout buffers), a screen at scale() each
    // with rows packed, only ever written; top() and bottom() then return them once the frame is finished. Set
    // between frames. False when the renderer can't (only the Banana 2D engine's can); null, null: back to its own.
    bool draw_into(uint32_t* top, uint32_t* bottom);
    // render.2d_out_skip: the frontend wrote into a buffer draw_into was given (overlay, copy, new buffer)
    void output_touched(const void* buf);
    // render.2d_lazy (the Banana 2D engine on its thread): the last run_frame()'s pictures may be finished by the 2D
    // thread after it returns. frame_fence() right after run_frame(), then wait_frame(fence) (any thread) before
    // reading the pictures drawn into draw_into's buffers; false: nothing to wait for
    bool set_lazy_2d(bool on);
    // kms.grid_cpu: the LCD grid table for the Banana 2D engine's 4x output (null: off); between frames, emulator's thread
    bool set_grid4(const unsigned char (*lut)[256]);
    bool frame_fence(uint32_t& fence) const;
    void wait_frame(uint32_t fence) const;

    int audio(int16_t* stereo, int max_frames);  // what the SPU made since the last call
    void set_audio_rate(double hz);              // the SPU's output rate (48000: the DS's speed; kms.pace=display)
    int audio_backlog() const;                   // what the SPU made and audio() hasn't read yet, in stereo frames
    // the sound's speed against the game's (audio.servo): below 1 stretches it (more samples per emulated second);
    // any thread may ask, the emulator's thread applies it between frames (never inside the SPU's mix)
    void set_audio_skew(double s) { skew_req_.store(s, std::memory_order_relaxed); }

    // Run-ahead (runahead = 1 or 2, experimental): right after run_frame(), the state kept in memory, n more frames
    // run with the same buttons (the last one's picture is the one shown: before_last() points draw_into at it,
    // after_last() may copy it), and the state put back, so the game answers a press n frames sooner than its own
    // lag. Those frames are undone: no sound, no save writes, no microphone or network traffic (hidden()), and the
    // JIT's blocks compiled meanwhile are dropped (melonDS patch 0091). Within ~2 s of a display capture the 3D frame
    // is drawn again from the state put back (the capture would otherwise carry the frame ahead's 3D into the game's
    // VRAM). False when it couldn't run: the picture is run_frame()'s then. runahead_ok(): whether it can (not with
    // local wireless, on a DSi, or with melonDS's own 3D renderer on its thread).
    bool runahead_ok() const;
    bool run_ahead(int n, const std::function<void()>& before_last, const std::function<void()>& after_last);
    bool hidden() const { return hidden_; }
    double runahead_save_ms() const { return ra_save_ms_; }  // the last snapshot's save and load, ms
    double runahead_load_ms() const { return ra_load_ms_; }
    size_t runahead_bytes() const { return ra_len_; }

    bool save_state(int slot);  // slot 0 = the quit/resume state
    bool load_state(int slot);
    void restart();  // the game from its start (the in-game menu's Restart game): a reset, booted straight into it
    std::string state_path(int slot) const;

    Mic& mic() { return mic_; }

    // For the tests and the differential harness (tools/difftest.cpp): the DS's state, read-only.
    const uint8_t* main_ram() const;      // 4 MB
    const uint8_t* dtcm() const;          // the ARM9's 16 KB data TCM (RetroAchievements reads it)
    const std::string& rom_path() const { return rom_path_; }
    const std::string& game() const { return game_; }
    void set_cheats(std::vector<melonDS::ARCode> codes);  // the Action Replay codes melonDS runs each frame (cheats.h)
    uint32_t main_ram_size() const;
    void cpu_regs(int cpu, uint32_t r[17]) const;  // cpu 0 = ARM9, 1 = ARM7; r[16] = CPSR
    const melonDS::NDS* nds() const { return nds_.get(); }  // (bananads-difftest's --timing reads its state)
    const Config& cfg() const { return cfg_; }

    // Platform callbacks (platform.cpp) reach the emulator through these.
    void write_save(const uint8_t* data, uint32_t len, uint32_t off, uint32_t n);
    melonDS::Net* net() { return net_.get(); }
    void stop_requested() { stop_ = true; }
    bool stopped() const { return stop_; }

    static Emu* from(void* userdata) { return static_cast<Emu*>(userdata); }

private:
    void setup_wifi();
    void setup_local();
    void flush_save(bool now);
    void setup_slice();
    void set_clock();
    std::unique_ptr<melonDS::NDS> make_dsi(melonDS::NDSArgs&& args);
    std::optional<melonDS::DSi_NAND::NANDImage> setup_nand(const std::array<uint8_t, 0x10000>& arm7i);
    void check_secure_area();
    void export_dsi_saves();
    uint64_t debug_pc_ = 0;
    const melonDS::NDSCart::CartCommon* dsi_cart_ = nullptr;  // (the game's, while make_dsi runs)
    bool dsi_nand_ = false;  // the NAND's working copy is in
    uint32_t dsi_title_cat_ = 0, dsi_title_id_ = 0;
    bool dsi_ = false;       // a DSi, not a DS
    bool dsi_bios_ = false;  // the DSi's own BIOS images, not the free ones
    bool key1_ = false;      // the free BIOS got the secure area's key from a DS bios7.bin
    bool secure_missing_ = false;
    std::vector<uint8_t> cia_tmd_;  // a .cia's title metadata (3DS format), for a NAND import
    std::mutex notice_mu_;
    std::string notice1_, notice2_;

    std::atomic<double> skew_req_{1.0};
    std::atomic<long> swaps3d_{0};
    bool lid_closed_ = false;  // the lid as set_lid last had it (emulator thread)
    uint32_t bank3d_ = 0;
    double skew_cur_ = 1.0;
    std::vector<int16_t> tap_;  // audio.tap: the mixer's 32768 Hz output (melonDS patch 0180), written at the quit
    std::string tap_path_;
    Config cfg_;
    std::unique_ptr<melonDS::NDS> nds_;
    std::unique_ptr<melonDS::Net> net_;
    Mic mic_;
    std::string rom_path_, save_path_, game_;
    bool stop_ = false;
    bool shut_ = false;  // shutdown() done
    bool local_ = false;
    int scale_ = 1;
    melonDS::BananaRenderer* banana_ = nullptr;  // the renderer, when it's the Banana 2D engine's
    std::unique_ptr<uint8_t[]> save_copy_;
    uint32_t save_len_ = 0;
    uint64_t save_dirty_us_ = 0;  // when the game last wrote its save; 0 = written out
    // the save's writer (#33): the file is written and fsynced on a thread of its own, not the emulator's, where a
    // slow SD card's fsync (tens to hundreds of ms) stopped the game each time it saved
    std::thread save_thr_;
    std::mutex save_mu_;
    std::condition_variable save_cv_;
    std::vector<uint8_t> save_pending_;
    bool save_have_ = false, save_stop_ = false;
    void save_writer();
    long slice_stats_ = 0;        // core.slice_stats: log the slice's statistics every this many frames
    long slice_frames_ = 0;
    std::unique_ptr<Profiler> prof_;  // prof.hz: the emulator's thread sampled (prof.cpp), off by default
    long frames_run_ = 0;
    // run-ahead's snapshot (run_ahead)
    std::vector<uint8_t> ra_buf_;
    size_t ra_len_ = 0;
    bool hidden_ = false, ra_broken_ = false;
    bool ra_soft_threaded_ = false;  // render.renderer=soft with render.threaded: no run-ahead (emu.cpp)
    uint32_t ra_captures_ = 0;   // GPU::CaptureFrames when last seen
    long ra_capture_at_ = -100000;  // frames_run_ then
    double ra_save_ms_ = 0, ra_load_ms_ = 0;
    void note_capture();
};

}  // namespace bananads
