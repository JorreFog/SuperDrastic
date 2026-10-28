#include "mic.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bananads {

void Mic::set_tuning(const MicTuning& t) {
    std::lock_guard<std::mutex> l(mu_);
    t_ = t;
}

void Mic::note_output(const int16_t* pcm, int n) {
    if (n <= 0) return;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += (double)pcm[i] * pcm[i];
    float rms = (float)(std::sqrt(sum / n) / 32768.0);
    // About the loudest of the last ~30 chunks: the speaker's sound reaches the mic a little later.
    float prev = out_level_.load(std::memory_order_relaxed);
    out_level_.store(std::max(rms, prev * 0.93f), std::memory_order_relaxed);
}

void Mic::put(int16_t s) {
    // Called with mu_ held. A full ring drops the oldest: the game must hear now, not a second ago.
    ring_[wr_] = s;
    wr_ = (wr_ + 1) % ring_.size();
    if (wr_ == rd_) rd_ = (rd_ + 1) % ring_.size();
}

void Mic::push(const int16_t* pcm, int n, int rate) {
    std::lock_guard<std::mutex> l(mu_);
    if (rate > 0) rate_ = rate;
    for (int i = 0; i < n; i++) {
        double x = pcm[i];
        double y = x - hp_x_ + 0.998 * hp_y_;
        hp_x_ = x;
        hp_y_ = y;
        blk_.push_back((int16_t)std::clamp(y, -32768.0, 32767.0));
        captured_++;
        if ((int)blk_.size() >= t_.block) block_done();
    }
}

void Mic::block_done() {
    double sum = 0;
    for (int16_t s : blk_) sum += (double)s * s;
    double level = std::sqrt(sum / blk_.size()) / 32768.0;
    double out = out_level_.load(std::memory_order_relaxed);

    bool gate = t_.threshold > 0;
    if (floor_n_ < 60) {
        floor_ = (floor_ * floor_n_ + level) / (floor_n_ + 1);
        floor_n_++;
    }
    // Learn the speaker-to-mic leak from what the mic hears above the floor while the game plays sound, whether
    // the gate is open or not (loud game sound opens it before the leak is known). Once it has 50 blocks, a block
    // far above the expected leak is someone at the mic, not the leak, and teaches it nothing.
    if (gate && floor_n_ >= 60 && out > 0.005 && t_.gate_k > 0) {
        double above0 = std::max(0.0, level - floor_);
        if (coup_n_ < 50 || above0 <= 1.5 * coup_ * out + 0.002) {
            coup_n_++;
            coup_ += (above0 / out - coup_) / std::min(coup_n_, 200);
            coup_ = std::min(coup_, t_.coupling_max);
        }
    }
    double bar = floor_ + t_.threshold + t_.gate_k * coup_ * out;

    if (!gate) {
        open_ = true;
    } else if (floor_n_ < 60) {
        open_ = false;  // still learning the room
    } else if (level > bar) {
        open_ = true;
        open_until_ = captured_ + (int64_t)t_.hold_ms * rate_ / 1000;
    } else if (open_ && (level > floor_ + 0.6 * (bar - floor_) || captured_ < open_until_)) {
        // still open: hysteresis, or the minimum hold
    } else {
        open_ = false;
    }

    if (gate && !open_ && floor_n_ >= 60) floor_ = 0.999 * floor_ + 0.001 * level;
    m_level_ = (float)level;
    m_bar_ = (float)bar;

    // Into the ring at the DS's rate (linear interpolation), silence while the gate is closed.
    double step = rate_ / kDSRate;
    for (size_t i = 0; i < blk_.size();) {
        double a = i == 0 ? last_ : blk_[i - 1];
        double s = a + (blk_[i] - a) * frac_;
        put(open_ ? (int16_t)s : 0);
        frac_ += step;
        while (frac_ >= 1.0 && i < blk_.size()) {
            frac_ -= 1.0;
            i++;
        }
    }
    last_ = blk_.back();
    blk_.clear();
}

int Mic::read(int16_t* out, int max) {
    int got = 0;
    {
        std::lock_guard<std::mutex> l(mu_);
        // At most kMaxLag samples between the mic and the game: older ones are dropped. The capture runs from the
        // start (the gate learns the room for 5 s) while the game reads nothing yet, and afterwards the game reads
        // as fast as the mic records, so that first pile stayed: the ring (1.37 s) filled, and every game heard
        // the mic ~1.3 s late (measured on the RG DS Plus in Phantom Hourglass; BananaOS #14's "delay")
        const size_t lag = (wr_ + ring_.size() - rd_) % ring_.size();
        if (lag > (size_t)kMaxLag) rd_ = (wr_ + ring_.size() - kMaxLag) % ring_.size();
        while (got < max && rd_ != wr_) {
            out[got++] = ring_[rd_];
            rd_ = (rd_ + 1) % ring_.size();
        }
    }
    // A blow from the menu: low-passed noise at about half of full scale, over what the mic has.
    int left = blow_left_.load();
    if (left > 0) {
        int n = std::max(got, std::min(max, left));
        for (int i = 0; i < n; i++) {
            noise_ = noise_ * 1664525u + 1013904223u;
            double w = (int32_t)noise_ / 2147483648.0;
            noise_lp_ += 0.3 * (w - noise_lp_);
            double s = (i < got ? out[i] : 0) + noise_lp_ * 20000.0;
            out[i] = (int16_t)std::clamp(s, -32768.0, 32767.0);
        }
        blow_left_.store(std::max(0, left - n));
        got = n;
    }
    return got;
}

Mic::Meter Mic::meter() const {
    meter_us_.store(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(mu_);
    return Meter{m_level_, (float)floor_, m_bar_, open_};
}

}  // namespace bananads
