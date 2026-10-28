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
#include <poll.h>
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
std::atomic<int> g_audio_backlog{0}, g_audio_tempo{1000};

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
    int first_prime;   // the backlog the sound first waits for (kms.lowlat: twice the target, below)
    bool stats;        // audio.stats: a line every 10 s (short chunks, the servo's range)
    std::string dump;  // audio.dump: <path>.in.raw (the game's sound) and <path>.out.raw (what ALSA plays), 48 kHz s16 stereo
    bool mic_idle;     // mic.idle_stop: the capture stopped while nothing listens (below)
    bool close_child;  // audio.close_child: the output closed by a child process after the quit (below)
    snd_pcm_t* left = nullptr;  // ...the output left open for it by out_thread
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
    //  - the tempo of a time stretch (src/stretch.cpp, pitch kept) is 1 while the game keeps up. The buffer (60 ms
    //    by default) rides out a short stall untouched while the frontend's pacer catches the frames up (src/cpugov.h).
    //    Only when it is nearly empty does the tempo follow the game's speed, measured from the samples the SPU made
    //    over 200 ms, back to 1 once the game is at full speed and the buffer half full again: a game too slow even at
    //    the top clock sounds slower, not lower.
    double skew = 1.0, integ = 0.0, sent = 1.0, tempo = 1.0, rate = 1.0, speed = 1.0;
    const double skew_min = 0.75, integ_min = skew_min - 1.0;
    int64_t win_us = 0;
    long win_made = 0;
    int win_spu = 0;
    bool primed = false;  // the buffer filled to its target once since the sound started (or came back from a pause)
    bool ever_primed = false;
    Stretch st;
    std::vector<int16_t> fresh(4096 * 2);
    FILE* dump_in = a->dump.empty() ? nullptr : fopen((a->dump + ".in.raw").c_str(), "wb");
    FILE* dump_out = a->dump.empty() ? nullptr : fopen((a->dump + ".out.raw").c_str(), "wb");
    long dumped = 0;
    int hist[6] = {0}, hn = 0, dry = 0;
    int64_t last_tick_us = 0, last_got_us = 0;
    long st_frames = 0, st_pulls = 0;
    long st_short = 0, st_held = 0, st_pre = 0;  // (st_pre: short chunks while not primed: the silence before the sound starts)
    double st_min = 1.0, st_max = 1.0, st_sum = 0.0, st_back = 0.0;
    long st_xruns = 0, st_stretched = 0, st_bmin = 1 << 30;  // (audio.stats) ALSA errors, chunks at tempo < 1
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
            // everything the SPU made comes out (its ring holds only ~43 ms, less than the buffer's target), up to
            // four targets queued (one while muted: fast-forward comes back at the usual latency); beyond that the
            // SPU's ring drops the oldest
            const int keep = a->mute.load(std::memory_order_relaxed) ? 1 : 4;
            const int want = keep * a->servo_target + Stretch::kLookahead - st.pending();
            pulled = want > 0 ? a->emu->audio(fresh.data(), std::min(want, (int)fresh.size() / 2)) : 0;
            if (pulled > 0) st.push(fresh.data(), pulled);
            if (dump_in && pulled > 0) fwrite(fresh.data(), 4, pulled, dump_in);
            backlog += st.pending() - Stretch::kLookahead;
            // at the start and after a pause: silence until the buffer is full, then it plays
            if (!primed && a->servo && backlog >= (ever_primed ? a->servo_target : a->first_prime)) primed = ever_primed = true;
            got = primed || !a->servo ? st.pull(buf.data(), chunk, tempo) : 0;
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
            if (dry > 8) primed = false;
            // (sampled every 5 ms, a chunk's time: partial writes below make pulls more frequent while it's short)
            const bool tick = now - last_tick_us >= 5333;
            if (tick) { last_tick_us = now; hist[hn++ % 6] = backlog; g_audio_backlog.store(backlog, std::memory_order_relaxed); g_audio_tempo.store((int)std::lround(tempo * 1000), std::memory_order_relaxed); }
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
            if (tick && !hold && hn >= 6 && a->stretch && primed) {
                double avg = 0;
                for (int v : hist) avg += v;
                avg /= 6;
                const double err = std::clamp((avg - a->servo_target) / a->servo_target, -1.0, 1.0);
                integ = std::clamp(integ + 0.00002 * err, -0.005, 0.005);
                rate = std::clamp(1.0 + integ + 0.002 * err, 0.995, 1.005);
                double want = 1.0;
                if ((tempo < 1.0 || avg < 0.25 * a->servo_target) && !(speed >= 0.985 && err > -0.5))
                    want = std::clamp(speed * (1.0 + 0.25 * err), 0.6, 1.0);
                tempo += std::clamp(want - tempo, -0.0015, 0.0015);
                if (want == 1.0 && tempo > 0.998) tempo = 1.0;
                skew = rate * tempo;
                if (std::fabs(rate - sent) >= 0.0002) {
                    sent = rate;
                    a->emu->set_audio_skew(rate);
                }
            } else if (tick && !hold && hn >= 6 && !a->stretch) {
                // (the old servo: only with audio.stretch off. With it on and the buffer not primed yet, e.g. after a
                // state load's gap, this branch used to run and pull the SPU's resampler down to 0.75: the first
                // ~0.8 s played after a load came out up to 1.5 semitones low)
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
            if (got < n) { st_short++; st_held += n - got; if (!primed) st_pre++; }
            st_frames += n;
            st_min = std::min(st_min, skew); st_max = std::max(st_max, skew); st_sum += skew; st_back += backlog;
            if (primed && !hold) st_bmin = std::min(st_bmin, (long)backlog);
            if (tempo < 1.0) st_stretched++;
            if (st_frames >= 480000) {  // 10 s
                // (after the backlog: xruns = snd_pcm_writei errors (an underrun at the device), stretched = chunks
                // played at a tempo below 1, min = the lowest backlog while playing: the headroom left)
                Log(LogLevel::Info, "[alsa] 10 s: %ld short chunks (%.2f s held, %ld before the sound started), skew %.3f-%.3f avg %.4f (%s), backlog avg %.0f min %ld, xruns %ld, stretched %ld\n",
                    st_short, st_held / 48000.0, st_pre, st_min, st_max, st_sum / st_pulls,
                    a->stretch ? "pitch kept: +-0.5% rate, the rest tempo" : "the pitch follows it", st_back / st_pulls,
                    st_bmin == (1 << 30) ? -1L : st_bmin, st_xruns, st_stretched);
                st_frames = st_pulls = st_short = st_held = st_pre = 0;
                st_xruns = st_stretched = 0;
                st_bmin = 1 << 30;
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
        if (w < 0) {
            if (a->stats && st_xruns++ < 3) Log(LogLevel::Warn, "[alsa] write: %s\n", snd_strerror((int)w));
            snd_pcm_recover(pcm, (int)w, 1);
        }
    }
    if (dump_in) fclose(dump_in), fclose(dump_out);
    snd_pcm_drop(pcm);
    if (a->close_child) a->left = pcm;  // (closed by alsa_stop's child)
    else snd_pcm_close(pcm);
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
    // mic.idle_stop (on): the capture runs only while it is wanted: the first 5 s (the gate learns the room's noise
    // and the speakers' leak, as before), while the game listens (Mic_Start..Mic_Stop) and while the menu shows the
    // meter; otherwise it is stopped (snd_pcm_drop) and checked every 50 ms. The always-on capture cost ~4.7% of a
    // core on the RG DS Plus (alsa-lib's plug converting 48 kHz stereo to 44.1 kHz mono) for samples nothing read.
    // The gate's state and the samples queued for the game stay as they were across a stop.
    const int64_t opened = mono_us();
    bool running = true;
    while (a->run) {
        if (a->mic_idle) {
            const int64_t now = mono_us();
            const bool want = now - opened < 5000000 || a->emu->mic().active() || now - a->emu->mic().meter_us() < 2000000;
            if (!want) {
                if (running) {
                    snd_pcm_drop(pcm);
                    running = false;
                }
                usleep(50000);
                continue;
            }
            if (!running) {
                snd_pcm_prepare(pcm);  // (readi starts it again)
                running = true;
            }
        }
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
    // 60 ms with the stretch (a short stall plays through untouched), 16 ms for the old servo
    a->servo_target = std::max(256, (int)cfg.num("audio.servo_target", a->stretch ? 2880 : 768));
    // kms.lowlat: the sound starts once twice the target is made (60 ms more at the start, trimmed by the rate servo's
    // +0.5% over ~12 s). The start's frames run unevenly (the panels taken over, the shader made and checked at the
    // first frame: 20-33 ms frames on the RG DS Plus), and with low latency nothing runs ahead to cover them: the
    // backlog started at the target ran dry within half a second in every Mario Kart DS resume there, and the servo
    // stretched the tempo to 0.87-0.92 for ~1 s
    a->first_prime = cfg.str("kms.lowlat", "off") != "off" ? 2 * a->servo_target : a->servo_target;
    a->stats = cfg.flag("audio.stats", false);
    a->close_child = cfg.flag("audio.close_child", true);
    a->mic_idle = cfg.flag("mic.idle_stop", true);
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
    if (snd_pcm_t* pcm = a->left) {
        // audio.close_child (on): the last close of the output, where the driver stops the device, done by a child
        // that holds only that file and lives until BananaDS has gone, so the quit doesn't wait for it. On the RG DS
        // Plus the close took ~600 ms: the aw88166 amplifiers' stop (a "pa stop check fail" each) loads their DSP
        // firmware again over I2C. BananaDS closes only its own copy, so the menu comes back without that wait (a
        // sound it plays in those ~600 ms finds the device busy: it isn't heard)
        pollfd pfd{};
        const int fd = snd_pcm_poll_descriptors(pcm, &pfd, 1) == 1 ? pfd.fd : -1;
        const pid_t parent = getpid();
        const pid_t pid = fd >= 0 ? fork() : -1;
        if (pid == 0) {  // (only async-signal-safe calls: the parent has other threads)
            const int maxfd = (int)sysconf(_SC_OPEN_MAX);
            for (int i = 0; i < maxfd; i++)
                if (i != fd) close(i);
            while (getppid() == parent) usleep(20000);
            _exit(0);  // (the device's file closed here, its stop in this exit)
        }
        if (pid < 0) snd_pcm_close(pcm);
        // (pid > 0: BananaDS's copy closes as it exits; alsa-lib's state for it is left as it is)
    }
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
