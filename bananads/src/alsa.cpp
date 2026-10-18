// Sound out and the microphone in through ALSA, each on its own thread. Two lessons from SuperDrastic's audio.c:
// the output thread runs at real-time priority when it may (an even pump removes the stutter SDL's audio had),
// and the two PCMs are opened one after the other, never at once: opening both together crashed PipeWire's
// ALSA plugin on the RG DS (ROCKNIXDS #26).
#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "Platform.h"
#include "frontend.h"
#include "stretch.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

static std::mutex g_open_mu;

struct Alsa {
    Emu* emu;
    std::atomic<bool> run{true};
    std::atomic<bool> mute{false};  // alsa_mute: the game's sound faded to silence (the menu's sounds still play)
    std::thread out, in;
    std::string out_dev, in_dev;
    unsigned latency_us;
    bool rt;
    bool servo;        // audio.servo: the rate servo (below)
    bool stretch;      // audio.stretch: the servo stretches time (pitch kept), not the resampler's rate
    int servo_target;  // audio.servo_target: the SPU backlog it holds, in frames
    bool stats;        // audio.stats: a line every 10 s (short chunks, the servo's range)
    std::string dump;  // audio.dump: <path>.in.raw (the game's sound) and <path>.out.raw (what ALSA plays), 48 kHz s16 stereo
    // the in-game menu's sounds (alsa_ui_*), 48 kHz stereo, mixed over the game's output while one plays
    std::mutex ui_mu;
    std::vector<int16_t> ui[4];
    int ui_id = -1;
    size_t ui_pos = 0;
};

static snd_pcm_t* open_pcm(const std::string& dev, snd_pcm_stream_t dir, unsigned ch, unsigned rate, unsigned lat) {
    std::lock_guard<std::mutex> l(g_open_mu);
    snd_pcm_t* pcm = nullptr;
    int e = snd_pcm_open(&pcm, dev.c_str(), dir, 0);
    if (e < 0) {
        Log(LogLevel::Warn, "[alsa] %s (%s): %s\n", dev.c_str(), dir == SND_PCM_STREAM_PLAYBACK ? "out" : "mic",
            snd_strerror(e));
        return nullptr;
    }
    e = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16, SND_PCM_ACCESS_RW_INTERLEAVED, ch, rate, 1, lat);
    if (e < 0) {
        Log(LogLevel::Warn, "[alsa] %s: %s\n", dev.c_str(), snd_strerror(e));
        snd_pcm_close(pcm);
        return nullptr;
    }
    return pcm;
}

// The headphone jack (BananaOS #13): bananad renames a new /run/banana/asound.conf in when earphones go in or out,
// but alsa-lib read its configuration once, at the first open. When the file changes (another inode, or a newer
// time), the output is closed, the configuration dropped and "default" opened again: the sound follows the jack.
static const char* const JACK_CONF = "/run/banana/asound.conf";
static bool conf_id(struct stat& st) { return stat(JACK_CONF, &st) == 0; }
static bool conf_changed(const struct stat& was, bool had, struct stat& now) {
    const bool has = conf_id(now);
    if (has != had) return true;
    return has && (now.st_ino != was.st_ino || now.st_mtim.tv_sec != was.st_mtim.tv_sec ||
                   now.st_mtim.tv_nsec != was.st_mtim.tv_nsec);
}
static int64_t mono_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void out_thread(Alsa* a) {
    pthread_setname_np(pthread_self(), "bds-audio");
    if (a->rt) {
        sched_param sp{};
        sp.sched_priority = 10;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            Log(LogLevel::Info, "[alsa] no real-time priority for the sound thread\n");
    }
    struct stat conf{};
    bool had_conf = conf_id(conf);
    snd_pcm_t* pcm = open_pcm(a->out_dev, SND_PCM_STREAM_PLAYBACK, 2, 48000, a->latency_us);
    if (!pcm) return;
    const int chunk = 256;  // SuperDrastic's pump chunk
    std::vector<int16_t> buf(chunk * 2);
    int16_t last[2] = {0, 0};
    int since_check = 0;
    float gain = 1.f;
    // The rate servo (#12). The game makes 800 samples a frame; below full speed it makes fewer than ALSA plays, the
    // chunk runs short and the last sample is held: a crackle. The servo watches the SPU's backlog (averaged over two
    // frames' pulls, which takes out the frame-by-frame sawtooth) and stretches the sound to keep it at the target:
    // a skew below 1 makes more output samples per emulated one (the sound slows and lowers with the game, smoothly).
    // Proportional for short dips, integral for a game that runs slow for a while, and a slew limit so it glides.
    // It holds still while nothing comes (the menu, a load) and while muted (fast-forward). Only the sound changes,
    // the emulation never sees it.
    // Off, all of that is the resampler's and the pitch follows the speed (and the frame-by-frame jitter: a heavy
    // frame moved it by several percent). With audio.stretch (on) there are two calmer loops instead:
    //  - the resampler's rate, within +-0.5% (inaudible), slowly holds the backlog at its target: the drift between
    //    the game's clock and ALSA's, and nothing faster;
    //  - the tempo of a time stretch (src/stretch.cpp, pitch kept) is 1 while the game keeps up. Only when the
    //    backlog runs low does it follow the game's speed, measured from the samples the SPU made over 200 ms, and it
    //    goes back to 1 once the game is at full speed again. A slow game sounds slower, not lower, and a full-speed
    //    one plays untouched.
    double skew = 1.0, integ = 0.0, sent = 1.0, tempo = 1.0, rate = 1.0, speed = 1.0;
    const double skew_min = 0.75, integ_min = skew_min - 1.0;
    int64_t win_us = 0;
    long win_made = 0;
    int win_spu = 0;
    Stretch st;
    std::vector<int16_t> fresh(4096 * 2);
    FILE* dump_in = a->dump.empty() ? nullptr : fopen((a->dump + ".in.raw").c_str(), "wb");
    FILE* dump_out = a->dump.empty() ? nullptr : fopen((a->dump + ".out.raw").c_str(), "wb");
    long dumped = 0;
    int hist[6] = {0}, hn = 0, dry = 0;
    int64_t last_tick_us = 0, last_got_us = 0;
    long st_frames = 0, st_pulls = 0;
    long st_short = 0, st_held = 0;
    double st_min = 1.0, st_max = 1.0, st_sum = 0.0, st_back = 0.0;
    while (a->run) {
        if (++since_check >= 47) {  // ~250 ms of 256-frame chunks at 48 kHz
            since_check = 0;
            struct stat now{};
            if (conf_changed(conf, had_conf, now)) {
                conf = now;
                had_conf = conf_id(now);
                snd_pcm_drop(pcm);
                snd_pcm_close(pcm);
                {
                    std::lock_guard<std::mutex> l(g_open_mu);
                    snd_config_update_free_global();
                }
                pcm = open_pcm(a->out_dev, SND_PCM_STREAM_PLAYBACK, 2, 48000, a->latency_us);
                Log(LogLevel::Info, "[alsa] %s changed: %s opened again%s\n", JACK_CONF, a->out_dev.c_str(),
                    pcm ? "" : " (failed: no sound)");
                if (!pcm) return;
            }
        }
        int backlog = a->servo || a->stats ? a->emu->audio_backlog() : 0;
        const int spu_backlog = backlog;
        int got, pulled;
        if (a->stretch) {
            // only what the stretch needs comes out of the SPU, whose buffer holds the rest (and caps it)
            const int want = Stretch::kLookahead + chunk + Stretch::kHop - st.pending();
            pulled = want > 0 ? a->emu->audio(fresh.data(), std::min(want, (int)fresh.size() / 2)) : 0;
            if (pulled > 0) st.push(fresh.data(), pulled);
            if (dump_in && pulled > 0) fwrite(fresh.data(), 4, pulled, dump_in);
            backlog += st.pending() - Stretch::kLookahead;
            got = st.pull(buf.data(), chunk, tempo);
        } else {
            got = pulled = a->emu->audio(buf.data(), chunk);
            if (dump_in && got > 0) fwrite(buf.data(), 4, got, dump_in);
        }
        bool hold = true;
        if (a->servo) {
            const int64_t now = mono_us();
            dry = pulled > 0 || got > 0 ? 0 : (now - last_got_us > 40000 ? 9 : dry);
            if (pulled > 0 || got > 0) last_got_us = now;
            hold = dry > 8 || a->mute.load(std::memory_order_relaxed);
            // (sampled every 5 ms, a chunk's time: partial writes below make pulls more frequent while it's short)
            const bool tick = now - last_tick_us >= 5333;
            if (tick) { last_tick_us = now; hist[hn++ % 6] = backlog; }
            // the game's speed: what the SPU made (taken out, plus its buffer's growth) against what full speed makes
            if (hold) {
                win_us = 0;
            } else if (!win_us) {
                win_us = now, win_made = 0, win_spu = spu_backlog;
            } else {
                win_made += pulled;
                if (now - win_us >= 200000) {
                    const double made = (double)(win_made + spu_backlog - win_spu) * rate;
                    speed = 0.6 * speed + 0.4 * std::clamp(made / (48000e-6 * (double)(now - win_us)), 0.3, 1.5);
                    win_us = now, win_made = 0, win_spu = spu_backlog;
                }
            }
            if (tick && !hold && hn >= 6 && a->stretch) {
                double avg = 0;
                for (int v : hist) avg += v;
                avg /= 6;
                const double err = std::clamp((avg - a->servo_target) / a->servo_target, -1.0, 1.0);
                integ = std::clamp(integ + 0.00002 * err, -0.005, 0.005);
                rate = std::clamp(1.0 + integ + 0.002 * err, 0.995, 1.005);
                double want = 1.0;
                if ((tempo < 1.0 || avg < 0.5 * a->servo_target) && !(speed >= 0.985 && err > -0.2))
                    want = std::clamp(speed * (1.0 + 0.25 * err), 0.6, 1.0);
                tempo += std::clamp(want - tempo, -0.0015, 0.0015);
                if (want == 1.0 && tempo > 0.998) tempo = 1.0;
                skew = rate * tempo;
                if (std::fabs(rate - sent) >= 0.0002) {
                    sent = rate;
                    a->emu->set_audio_skew(rate);
                }
            } else if (tick && !hold && hn >= 6) {
                double avg = 0;
                for (int v : hist) avg += v;
                avg /= 6;
                const double err = std::clamp((avg - a->servo_target) / a->servo_target, -1.0, 1.0);
                integ = std::clamp(integ + 0.0006 * err, integ_min, 0.02);
                const double want = std::clamp(1.0 + integ + 0.15 * err, skew_min, 1.02);
                skew += std::clamp(want - skew, -0.004, 0.004);
                if (std::fabs(skew - sent) >= 0.0005) {
                    sent = skew;
                    a->emu->set_audio_skew(skew);
                }
            }
        }
        if (got > 0) {
            last[0] = buf[(got - 1) * 2];
            last[1] = buf[(got - 1) * 2 + 1];
        }
        // ALSA's writei blocks until there is room, which is what paces this loop. Short of samples, hold the
        // last one instead of a click. With the servo, while the game runs and ALSA still has more than ~10 ms
        // queued, a short chunk goes out as it is (nothing held) and the loop asks again a moment later: the
        // queue rides out the gap between two of the game's frames.
        int n = chunk;
        if (a->servo && !hold && got < chunk) {
            snd_pcm_sframes_t q = 0;
            if (snd_pcm_delay(pcm, &q) == 0 && q - (chunk - got) >= 512) {
                n = got;
                if (n == 0) {
                    usleep(1000);
                    continue;
                }
            }
        }
        for (int i = got; i < n; i++) {
            buf[i * 2] = last[0];
            buf[i * 2 + 1] = last[1];
        }
        if (a->stats) {
            st_pulls++;
            if (got < n) { st_short++; st_held += n - got; }
            st_frames += n;
            st_min = std::min(st_min, skew); st_max = std::max(st_max, skew); st_sum += skew; st_back += backlog;
            if (st_frames >= 480000) {  // 10 s
                Log(LogLevel::Info, "[alsa] 10 s: %ld short chunks (%.2f s held), skew %.3f-%.3f avg %.3f (%s), backlog avg %.0f\n",
                    st_short, st_held / 48000.0, st_min, st_max, st_sum / st_pulls,
                    a->stretch ? "pitch kept: +-0.5% rate, the rest tempo" : "the pitch follows it", st_back / st_pulls);
                st_frames = st_pulls = st_short = st_held = 0;
                st_min = st_max = skew; st_sum = st_back = 0;
            }
        }
        // muted (fast-forward): faded over ~5 ms each way, so neither end clicks
        const float target = a->mute.load(std::memory_order_relaxed) ? 0.f : 1.f;
        if (gain != target || target == 0.f)
            for (int i = 0; i < n; i++) {
                gain = target > gain ? std::min(target, gain + 1.f / 240) : std::max(target, gain - 1.f / 240);
                buf[i * 2] = (int16_t)(buf[i * 2] * gain);
                buf[i * 2 + 1] = (int16_t)(buf[i * 2 + 1] * gain);
            }
        {
            std::lock_guard<std::mutex> l(a->ui_mu);
            if (a->ui_id >= 0) {
                const std::vector<int16_t>& s = a->ui[a->ui_id];
                for (int i = 0; i < n * 2 && a->ui_pos < s.size(); i++, a->ui_pos++)
                    buf[i] = (int16_t)std::clamp((int)buf[i] + s[a->ui_pos], -32768, 32767);
                if (a->ui_pos >= s.size()) a->ui_id = -1;
            }
        }
        if (dump_out) {
            fwrite(buf.data(), 4, n, dump_out);
            if ((dumped += n) >= 48000 * 120) {  // two minutes
                fclose(dump_in), fclose(dump_out);
                dump_in = dump_out = nullptr;
            }
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf.data(), n);
        if (w < 0) snd_pcm_recover(pcm, (int)w, 1);
    }
    if (dump_in) fclose(dump_in), fclose(dump_out);
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
}

static void in_thread(Alsa* a) {
    pthread_setname_np(pthread_self(), "bds-mic");
    // After the output is open (see the top of this file).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    snd_pcm_t* pcm = open_pcm(a->in_dev, SND_PCM_STREAM_CAPTURE, 1, 44100, 100000);
    if (!pcm) {
        Log(LogLevel::Warn, "[mic] no microphone; the menu's Blow still works\n");
        return;
    }
    std::vector<int16_t> buf(1024);
    int silent_blocks = 0;
    bool told = false;
    while (a->run) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf.data(), buf.size());
        if (n < 0) {
            snd_pcm_recover(pcm, (int)n, 1);
            continue;
        }
        bool zero = true;
        for (snd_pcm_sframes_t i = 0; i < n && zero; i++) zero = buf[i] == 0;
        silent_blocks = zero ? silent_blocks + 1 : 0;
        if (silent_blocks == 215 && !told) {  // ~5 s of exact zeros: muted in the mixer, or the wrong device
            Log(LogLevel::Warn, "[mic] the microphone has given only silence for 5 s (mic.device=%s)\n",
                a->in_dev.c_str());
            told = true;
        }
        a->emu->mic().push(buf.data(), (int)n, 44100);
    }
    snd_pcm_close(pcm);
}

Alsa* alsa_start(Emu& emu, const Config& cfg) {
    Alsa* a = new Alsa;
    a->emu = &emu;
    a->out_dev = cfg.str("audio.out_device", "default");
    a->in_dev = cfg.str("mic.device", "default");
    a->latency_us = (unsigned)cfg.num("audio.latency_us", 30000);
    a->rt = cfg.flag("audio.realtime", true);
    a->servo = cfg.flag("audio.servo", true);
    a->stretch = a->servo && cfg.flag("audio.stretch", true);
    a->servo_target = std::max(256, (int)cfg.num("audio.servo_target", 768));
    a->stats = cfg.flag("audio.stats", false);
    a->dump = cfg.str("audio.dump", "");
    a->out = std::thread(out_thread, a);
    if (cfg.str("mic.source", "device") == "device") a->in = std::thread(in_thread, a);
    return a;
}

void alsa_mute(Alsa* a, bool on) {
    if (!a || a->mute.exchange(on) == on) return;
    Log(LogLevel::Info, "[alsa] game sound %s\n", on ? "faded out (fast-forward)" : "back");
}

void alsa_stop(Alsa* a) {
    if (!a) return;
    a->run = false;
    if (a->out.joinable()) a->out.join();
    if (a->in.joinable()) a->in.join();  // readi returns within one block (~23 ms)
    delete a;
}

void alsa_ui_load(Alsa* a, int id, const int16_t* pcm, int frames, int rate, int ch) {
    if (!a || id < 0 || id >= 4 || frames <= 0 || rate <= 0 || ch < 1) return;
    // to the output's 48 kHz stereo, linearly
    const size_t n = (size_t)((double)frames * 48000 / rate);
    std::vector<int16_t> out(n * 2);
    for (size_t i = 0; i < n; i++) {
        const double src = (double)i * rate / 48000;
        const int j = (int)src;
        const double f = src - j;
        for (int c = 0; c < 2; c++) {
            const int cc = ch > 1 ? c : 0;
            const int s0 = pcm[(size_t)std::min(j, frames - 1) * ch + cc], s1 = pcm[(size_t)std::min(j + 1, frames - 1) * ch + cc];
            out[i * 2 + c] = (int16_t)(s0 + (s1 - s0) * f);
        }
    }
    std::lock_guard<std::mutex> l(a->ui_mu);
    a->ui[id] = std::move(out);
}

void alsa_ui_play(Alsa* a, int id) {
    if (!a || id < 0 || id >= 4) return;
    std::lock_guard<std::mutex> l(a->ui_mu);
    a->ui_id = a->ui[id].empty() ? -1 : id;
    a->ui_pos = 0;
}

}  // namespace bananads
