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

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "Platform.h"
#include "frontend.h"

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
    int servo_target;  // audio.servo_target: the SPU backlog it holds, in frames
    bool stats;        // audio.stats: a line every 10 s (short chunks, the servo's range)
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
    // Proportional for short dips, integral for a game that runs slow for a while, and a slew limit so the pitch
    // glides. It holds still while nothing comes (the menu, a load) and while muted (fast-forward). Only the sound
    // changes: the skew is the resampler's, the emulation never sees it.
    double skew = 1.0, integ = 0.0, sent = 1.0;
    int hist[6] = {0}, hn = 0, dry = 0;
    long st_chunks = 0, st_short = 0, st_held = 0;
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
        const int backlog = a->servo || a->stats ? a->emu->audio_backlog() : 0;
        int got = a->emu->audio(buf.data(), chunk);
        if (a->servo) {
            dry = got > 0 ? 0 : dry + 1;
            const bool hold = dry > 8 || a->mute.load(std::memory_order_relaxed);
            hist[hn++ % 6] = backlog;
            if (!hold && hn >= 6) {
                double avg = 0;
                for (int v : hist) avg += v;
                avg /= 6;
                const double err = std::clamp((avg - a->servo_target) / a->servo_target, -1.0, 1.0);
                integ = std::clamp(integ + 0.0006 * err, -0.25, 0.02);
                const double want = std::clamp(1.0 + integ + 0.15 * err, 0.75, 1.02);
                skew += std::clamp(want - skew, -0.004, 0.004);
                if (std::fabs(skew - sent) >= 0.0005) {
                    sent = skew;
                    a->emu->set_audio_skew(skew);
                }
            }
        }
        if (a->stats) {
            st_chunks++;
            if (got < chunk) { st_short++; st_held += chunk - got; }
            st_min = std::min(st_min, skew); st_max = std::max(st_max, skew); st_sum += skew; st_back += backlog;
            if (st_chunks == 1875) {  // 10 s
                Log(LogLevel::Info, "[alsa] 10 s: %ld short chunks (%.2f s held), skew %.3f-%.3f avg %.3f, backlog avg %.0f\n",
                    st_short, st_held / 48000.0, st_min, st_max, st_sum / st_chunks, st_back / st_chunks);
                st_chunks = st_short = st_held = 0;
                st_min = st_max = skew; st_sum = st_back = 0;
            }
        }
        if (got > 0) {
            last[0] = buf[(got - 1) * 2];
            last[1] = buf[(got - 1) * 2 + 1];
        }
        // ALSA's writei blocks until there is room, which is what paces this loop. Short of samples, hold the
        // last one instead of a click.
        for (int i = got; i < chunk; i++) {
            buf[i * 2] = last[0];
            buf[i * 2 + 1] = last[1];
        }
        // muted (fast-forward): faded over ~5 ms each way, so neither end clicks
        const float target = a->mute.load(std::memory_order_relaxed) ? 0.f : 1.f;
        if (gain != target || target == 0.f)
            for (int i = 0; i < chunk; i++) {
                gain = target > gain ? std::min(target, gain + 1.f / 240) : std::max(target, gain - 1.f / 240);
                buf[i * 2] = (int16_t)(buf[i * 2] * gain);
                buf[i * 2 + 1] = (int16_t)(buf[i * 2 + 1] * gain);
            }
        {
            std::lock_guard<std::mutex> l(a->ui_mu);
            if (a->ui_id >= 0) {
                const std::vector<int16_t>& s = a->ui[a->ui_id];
                for (int i = 0; i < chunk * 2 && a->ui_pos < s.size(); i++, a->ui_pos++)
                    buf[i] = (int16_t)std::clamp((int)buf[i] + s[a->ui_pos], -32768, 32767);
                if (a->ui_pos >= s.size()) a->ui_id = -1;
            }
        }
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf.data(), chunk);
        if (w < 0) snd_pcm_recover(pcm, (int)w, 1);
    }
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
    a->servo_target = std::max(256, (int)cfg.num("audio.servo_target", 768));
    a->stats = cfg.flag("audio.stats", false);
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
