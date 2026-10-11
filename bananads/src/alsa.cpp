// Sound out and the microphone in through ALSA, each on its own thread. Two lessons from SuperDrastic's audio.c:
// the output thread runs at real-time priority when it may (an even pump removes the stutter SDL's audio had),
// and the two PCMs are opened one after the other, never at once: opening both together crashed PipeWire's
// ALSA plugin on the RG DS (ROCKNIXDS #26).
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

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
    std::thread out, in;
    std::string out_dev, in_dev;
    unsigned latency_us;
    bool rt;
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

static void out_thread(Alsa* a) {
    pthread_setname_np(pthread_self(), "bds-audio");
    if (a->rt) {
        sched_param sp{};
        sp.sched_priority = 10;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
            Log(LogLevel::Info, "[alsa] no real-time priority for the sound thread\n");
    }
    snd_pcm_t* pcm = open_pcm(a->out_dev, SND_PCM_STREAM_PLAYBACK, 2, 48000, a->latency_us);
    if (!pcm) return;
    const int chunk = 256;  // SuperDrastic's pump chunk
    std::vector<int16_t> buf(chunk * 2);
    int16_t last[2] = {0, 0};
    while (a->run) {
        int got = a->emu->audio(buf.data(), chunk);
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
    a->out = std::thread(out_thread, a);
    if (cfg.str("mic.source", "device") == "device") a->in = std::thread(in_thread, a);
    return a;
}

void alsa_stop(Alsa* a) {
    if (!a) return;
    a->run = false;
    if (a->out.joinable()) a->out.join();
    if (a->in.joinable()) a->in.join();  // readi returns within one block (~23 ms)
    delete a;
}

}  // namespace bananads
