// One emulated DS: melonDS's NDS plus what BananaDS adds around it (saves next to the game's name in the save
// folder, the microphone, Wi-Fi, savestates with a picture for the in-game menu).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ARCodeFile.h"
#include "config.h"
#include "mic.h"

namespace melonDS { class NDS; class Net; class BananaRenderer; }

namespace bananads {
bool rom_unmap(uint8_t* rom);  // a ROM Emu::load_rom mapped: unmapped, true (melonDS's Platform::ROMRelease)
}

namespace bananads {

// DS buttons as melonDS's key mask wants them (bit set = pressed; Emu inverts).
enum Key : uint32_t {
    KeyA = 1 << 0, KeyB = 1 << 1, KeySelect = 1 << 2, KeyStart = 1 << 3,
    KeyRight = 1 << 4, KeyLeft = 1 << 5, KeyUp = 1 << 6, KeyDown = 1 << 7,
    KeyR = 1 << 8, KeyL = 1 << 9, KeyX = 1 << 10, KeyY = 1 << 11,
};

class Emu {
public:
    explicit Emu(const Config& cfg);
    ~Emu();

    bool load_rom(const std::string& path);  // also loads its save; false with a logged reason
    void run_frame();                          // one DS frame (~1/59.83 s)
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
    // render.2d_lazy (the Banana 2D engine on its thread): the last run_frame()'s pictures may be finished by the 2D
    // thread after it returns. frame_fence() right after run_frame(), then wait_frame(fence) (any thread) before
    // reading the pictures drawn into draw_into's buffers; false: nothing to wait for
    bool set_lazy_2d(bool on);
    bool frame_fence(uint32_t& fence) const;
    void wait_frame(uint32_t fence) const;

    int audio(int16_t* stereo, int max_frames);  // what the SPU made since the last call
    void set_audio_rate(double hz);              // the SPU's output rate (48000: the DS's speed; kms.pace=display)
    int audio_backlog() const;                   // what the SPU made and audio() hasn't read yet, in stereo frames
    // the sound's speed against the game's (audio.servo): below 1 stretches it (more samples per emulated second);
    // any thread may ask, the emulator's thread applies it between frames (never inside the SPU's mix)
    void set_audio_skew(double s) { skew_req_.store(s, std::memory_order_relaxed); }

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

    std::atomic<double> skew_req_{1.0};
    double skew_cur_ = 1.0;
    Config cfg_;
    std::unique_ptr<melonDS::NDS> nds_;
    std::unique_ptr<melonDS::Net> net_;
    Mic mic_;
    std::string rom_path_, save_path_, game_;
    bool stop_ = false;
    bool local_ = false;
    int scale_ = 1;
    melonDS::BananaRenderer* banana_ = nullptr;  // the renderer, when it's the Banana 2D engine's
    std::unique_ptr<uint8_t[]> save_copy_;
    uint32_t save_len_ = 0;
    uint64_t save_dirty_us_ = 0;  // when the game last wrote its save; 0 = written out
    long slice_stats_ = 0;        // core.slice_stats: log the slice's statistics every this many frames
    long slice_frames_ = 0;
};

}  // namespace bananads
