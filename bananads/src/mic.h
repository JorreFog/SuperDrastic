// The microphone. Unlike DraStic, melonDS takes real PCM, so the game hears the real mic: a blow, a voice, a
// clap. What SuperDrastic learned on the RG DS (ROCKNIXDS #26) is kept as the gate in front of it: the RG DS's
// mic sits next to its speakers, so the game's own sound would otherwise reach the game as "input".
//
//  - a DC-blocking high-pass (y = x - x[-1] + 0.998 y[-1], ~14 Hz);
//  - a noise floor, averaged over the first 60 blocks, then tracked slowly while the gate is closed;
//  - an echo gate: it learns how much of the speaker output leaks into the mic (the coupling) and wants the
//    level to clear floor + threshold + gate_k * coupling * output level;
//  - hysteresis: once open it stays open while the level is above 60% of the way from the floor to the bar,
//    and for hold_ms at least.
// While the gate is closed the game gets silence. threshold 0 turns the gate off (every sample passes).
// The leak is learned in the first ~50 blocks (~1 s) of game sound; until then loud game sound can get through.
//
// Threads: push() from the capture thread, note_output() from the audio thread, read() from the emulator.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace bananads {

struct MicTuning {
    double threshold = 0.05;     // mic.threshold: above the floor, full scale = 1 (Settings: high 0.02, medium 0.05, low 0.15)
    double gate_k = 3.0;         // mic.gate: 0 turns the echo gate off
    double coupling_max = 4.0;   // mic.coupling_max
    int hold_ms = 120;           // mic.hold_ms
    int block = 1024;            // capture block in samples at the capture rate
};

class Mic {
public:
    static constexpr double kDSRate = 47743.4659091;  // the rate melonDS's mic reads at
    static constexpr int kMaxLag = 4096;  // the most samples queued for the game (~86 ms: a capture block and some)

    explicit Mic(const MicTuning& t = MicTuning()) : t_(t), ring_(1 << 16) {}

    void set_tuning(const MicTuning& t);
    void push(const int16_t* pcm, int n, int rate);  // capture: any rate, mono
    void note_output(const int16_t* pcm, int n);      // what went to the speakers (stereo interleaved is fine)
    int read(int16_t* out, int max);                  // emulator: up to max samples at kDSRate
    int backlog() {  // samples queued for the game (its lag behind the mic, at kDSRate)
        std::lock_guard<std::mutex> l(mu_);
        return (int)((wr_ + ring_.size() - rd_) % ring_.size());
    }

    // The in-game menu's "Blow" (and a hotkey): the game hears a blow for ms milliseconds, mic or not.
    void blow(int ms) { blow_left_.store((int)(ms * kDSRate / 1000)); }

    // For the menu's meter: the last block's level, the bar it had to clear, whether the gate is open.
    struct Meter { float level, floor, bar; bool open; };
    Meter meter() const;

    bool active() const { return active_.load(); }
    // when the menu last read the meter (steady_clock microseconds): the capture runs while it is shown
    int64_t meter_us() const { return meter_us_.load(std::memory_order_relaxed); }
    void set_active(bool on) { active_.store(on); }  // the game started / stopped reading the mic

private:
    void block_done();
    void put(int16_t s);

    MicTuning t_;
    mutable std::mutex mu_;
    std::vector<int16_t> ring_;
    size_t rd_ = 0, wr_ = 0;

    // capture-rate state
    std::vector<int16_t> blk_;
    double hp_x_ = 0, hp_y_ = 0;
    double floor_ = 0;
    int floor_n_ = 0;
    double coup_ = 0;
    int coup_n_ = 0;
    bool open_ = false;
    int64_t open_until_ = 0;  // in captured samples
    int64_t captured_ = 0;
    int rate_ = 44100;
    double frac_ = 0;         // resampler phase
    int16_t last_ = 0;
    float m_level_ = 0, m_bar_ = 0;

    std::atomic<float> out_level_{0};
    std::atomic<int> blow_left_{0};
    std::atomic<bool> active_{false};
    mutable std::atomic<int64_t> meter_us_{0};
    uint32_t noise_ = 0x1234567;
    double noise_lp_ = 0;
};

}  // namespace bananads
