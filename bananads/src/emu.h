// One emulated DS: melonDS's NDS plus what BananaDS adds around it (saves next to the game's name in the save
// folder, the microphone, Wi-Fi, savestates with a picture for the in-game menu).
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "config.h"
#include "mic.h"

namespace melonDS { class NDS; class Net; }

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

    // 256x192 XRGB8888 each, valid until the next run_frame().
    const uint32_t* top() const;
    const uint32_t* bottom() const;

    int audio(int16_t* stereo, int max_frames);  // what the SPU made since the last call

    bool save_state(int slot);  // slot 0 = the quit/resume state
    bool load_state(int slot);
    std::string state_path(int slot) const;

    Mic& mic() { return mic_; }
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

    Config cfg_;
    std::unique_ptr<melonDS::NDS> nds_;
    std::unique_ptr<melonDS::Net> net_;
    Mic mic_;
    std::string rom_path_, save_path_, game_;
    bool stop_ = false;
    bool local_ = false;
    std::unique_ptr<uint8_t[]> save_copy_;
    uint32_t save_len_ = 0;
    uint64_t save_dirty_us_ = 0;  // when the game last wrote its save; 0 = written out
};

}  // namespace bananads
