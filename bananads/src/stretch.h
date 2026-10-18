// Time stretch without a change of pitch (WSOLA), for the sound thread's rate servo (src/alsa.cpp). A game running
// below full speed makes fewer samples than ALSA plays; stretching them by resampling (the servo's old way) lowers
// the pitch with the speed. This repeats or skips short stretches of the waveform instead, each joined where it
// lines up best with what was played, so the sound keeps its pitch and only its tempo follows the game.
#pragma once
#include <cstdint>
#include <vector>

namespace bananads {

class Stretch {
public:
    // 48 kHz stereo: windows of 10.7 ms, a hop of 5.3 ms, joins searched within +-4 ms
    static constexpr int kHop = 256, kWin = 2 * kHop, kSeek = 192;
    // the input it keeps ahead of what it has played, at a tempo of 1 (the servo counts it as latency)
    static constexpr int kLookahead = kWin + kSeek;

    Stretch();
    void push(const int16_t* stereo, int frames);
    // up to max_frames of output; tempo = input frames used per output frame (below 1 stretches, 1 passes through)
    int pull(int16_t* stereo, int max_frames, double tempo);
    // input not yet played, in input frames (what is queued in the window and still to come)
    int pending() const;
    void reset();

private:
    bool hop(double tempo);
    int seek(int around, int nat) const;
    float mono(int f) const { return in_[2 * f] + in_[2 * f + 1]; }

    std::vector<float> in_;     // interleaved stereo; frame 0 is the oldest kept
    std::vector<float> win_;    // periodic Hann, kWin long: two halves kHop apart sum to 1
    float tail_[2 * kHop];      // the second half of the last window, to overlap the next one
    std::vector<int16_t> out_;  // made and not yet pulled
    size_t out_pos_ = 0;
    double pos_ = 0;            // where the next window would start at the requested tempo
    int nat_ = 0;               // where the last window's natural continuation starts
    bool primed_ = false;
};

}  // namespace bananads
