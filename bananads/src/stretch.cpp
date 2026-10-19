#include "stretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bananads {

Stretch::Stretch() : win_(kWin) {
    for (int i = 0; i < kWin; i++) win_[i] = 0.5f - 0.5f * (float)std::cos(2.0 * M_PI * i / kWin);
    reset();
}

void Stretch::reset() {
    in_.clear();
    out_.clear();
    out_pos_ = 0;
    pos_ = 0;
    nat_ = 0;
    primed_ = false;
    std::memset(tail_, 0, sizeof tail_);
}

void Stretch::push(const int16_t* stereo, int frames) {
    const size_t n = in_.size();
    in_.resize(n + (size_t)frames * 2);
    for (int i = 0; i < frames * 2; i++) in_[n + i] = stereo[i];
}

int Stretch::pending() const {
    const int avail = (int)(in_.size() / 2);
    return std::max(0, avail - (int)pos_) + (int)((out_.size() - out_pos_) / 2);
}

// The start within +-kSeek of `around` whose first hop looks most like the natural continuation at `nat` (normalised
// cross-correlation of L+R): coarse over every second offset and sample, then the neighbours at full resolution.
int Stretch::seek(int around, int nat) const {
    const int avail = (int)(in_.size() / 2);
    const int lo = std::max(0, around - kSeek), hi = std::min(avail - kWin, around + kSeek);
    if (hi <= lo) return std::clamp(around, 0, std::max(0, avail - kWin));
    auto score = [&](int s, int step) {
        float c = 0, e = 1e-3f;
        for (int k = 0; k < kHop; k += step) {
            const float x = mono(s + k);
            c += x * mono(nat + k);
            e += x * x;
        }
        return c / std::sqrt(e);
    };
    int best = lo;
    float bs = -1e30f;
    for (int s = lo; s <= hi; s += 2)
        if (const float v = score(s, 2); v > bs) bs = v, best = s;
    const int c0 = best;
    bs = -1e30f;
    for (int s = std::max(lo, c0 - 1); s <= std::min(hi, c0 + 1); s++)
        if (const float v = score(s, 1); v > bs) bs = v, best = s;
    return best;
}

// One hop of output: the window at the chosen start, overlapped with the last one's tail.
bool Stretch::hop(double tempo) {
    const int avail = (int)(in_.size() / 2);
    const bool same = tempo == 1.0;
    int s;
    if (!primed_) {
        if (avail < kWin) return false;
        s = 0;
    } else if (same) {  // at full speed: the natural continuation, which plays the input back unchanged
        if (nat_ + kWin > avail) return false;
        s = nat_;
    } else {
        const int around = (int)std::lround(pos_);
        if (around + kSeek + kWin > avail || nat_ + kHop > avail) return false;
        s = seek(around, nat_);
    }
    const size_t o = out_.size();
    out_.resize(o + 2 * kHop);
    for (int i = 0; i < kHop; i++)
        for (int c = 0; c < 2; c++) {
            const float v = tail_[2 * i + c] + win_[i] * in_[2 * (s + i) + c];
            out_[o + 2 * i + c] = (int16_t)std::clamp(std::lround(v), -32768L, 32767L);
            tail_[2 * i + c] = win_[kHop + i] * in_[2 * (s + kHop + i) + c];
        }
    primed_ = true;
    nat_ = s + kHop;
    pos_ = same ? (double)nat_ : pos_ + kHop * tempo;
    // drop what no later window can reach
    const int keep = std::max(0, std::min((int)pos_ - kSeek, nat_));
    if (keep >= 4096) {
        in_.erase(in_.begin(), in_.begin() + 2 * (size_t)keep);
        pos_ -= keep;
        nat_ -= keep;
    }
    return true;
}

int Stretch::pull(int16_t* stereo, int max_frames, double tempo) {
    while ((int)((out_.size() - out_pos_) / 2) < max_frames && hop(tempo)) {}
    const int n = std::min(max_frames, (int)((out_.size() - out_pos_) / 2));
    std::memcpy(stereo, out_.data() + out_pos_, (size_t)n * 4);
    out_pos_ += (size_t)n * 2;
    if (out_pos_ == out_.size()) {
        out_.clear();
        out_pos_ = 0;
    }
    return n;
}

}  // namespace bananads
