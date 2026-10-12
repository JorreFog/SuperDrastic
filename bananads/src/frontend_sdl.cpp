#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "Platform.h"
#include "frontend.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

static void on_audio(void* ud, Uint8* stream, int len) {
    Emu* emu = (Emu*)ud;
    int16_t* out = (int16_t*)stream;
    int frames = len / 4;
    int got = emu->audio(out, frames);
    // Short of samples (a slow frame): repeat the last one rather than click.
    for (int i = got; i < frames; i++) {
        out[i * 2] = got ? out[(got - 1) * 2] : 0;
        out[i * 2 + 1] = got ? out[(got - 1) * 2 + 1] : 0;
    }
}

static void on_mic(void* ud, Uint8* stream, int len) {
    ((Emu*)ud)->mic().push((const int16_t*)stream, len / 2, 44100);
}

// sdl.present=queue: the emulator runs on a thread of its own at the DS's rate and queues its finished frames; the
// main thread shows the oldest one at each vsync. A frame waits about one vsync, so a game whose frames take 19 and
// 12 ms in turn (HeartGold at 1104 MHz, its logic every other frame) still shows one new frame per vsync.
struct FrameQueue {
    std::mutex m;
    std::deque<std::vector<uint32_t>> ready;  // oldest first
    std::vector<std::vector<uint32_t>> spare;
    std::vector<uint32_t> shown;              // what the last vsync showed
    long repeated = 0, dropped = 0, pushed = 0;  // vsyncs without a new frame, frames never shown, frames made
    void push(const uint32_t* top, const uint32_t* bot, size_t n) {
        std::vector<uint32_t> f;
        {
            std::lock_guard<std::mutex> l(m);
            if (!spare.empty()) { f = std::move(spare.back()); spare.pop_back(); }
        }
        f.resize(2 * n);
        memcpy(f.data(), top, n * 4);
        memcpy(f.data() + n, bot, n * 4);
        std::lock_guard<std::mutex> l(m);
        ready.push_back(std::move(f));
        pushed++;
        // (the display a little faster than the DS: the queue drains and a frame repeats now and then; slower: it
        // grows, and the oldest frames go)
        while (ready.size() > 3) { spare.push_back(std::move(ready.front())); ready.pop_front(); dropped++; }
    }
    // the frame for this vsync: the oldest ready one, or null to show the last one again
    const std::vector<uint32_t>* next() {
        std::lock_guard<std::mutex> l(m);
        if (ready.empty()) {
            if (!shown.empty()) repeated++;
            return nullptr;
        }
        if (!shown.empty()) spare.push_back(std::move(shown));
        shown = std::move(ready.front());
        ready.pop_front();
        return &shown;
    }
};

static uint32_t key_bit(SDL_Keycode k) {
    switch (k) {
        case SDLK_x: return KeyA;
        case SDLK_z: return KeyB;
        case SDLK_s: return KeyX;
        case SDLK_a: return KeyY;
        case SDLK_q: return KeyL;
        case SDLK_w: return KeyR;
        case SDLK_RETURN: return KeyStart;
        case SDLK_RSHIFT: case SDLK_BACKSPACE: return KeySelect;
        case SDLK_UP: return KeyUp;
        case SDLK_DOWN: return KeyDown;
        case SDLK_LEFT: return KeyLeft;
        case SDLK_RIGHT: return KeyRight;
        default: return 0;
    }
}

int run_sdl(Emu& emu, const Config& cfg) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        Log(LogLevel::Error, "[sdl] %s\n", SDL_GetError());
        return 1;
    }
    int scale = (int)cfg.num("sdl.scale", 2);
    SDL_Window* win = SDL_CreateWindow("BananaDS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 256 * scale,
                                       384 * scale, SDL_WINDOW_RESIZABLE);
    // sdl.present=queue shows a frame per display refresh: by SDL's vsync (sdl.vsync=on), or by a clock at sdl.refresh Hz
    // (off, the default: under sway on the RG DS Plus SDL's vsync let only ~18 presents a second through, and a
    // Wayland compositor shows the newest buffer at its own refresh anyway). sdl.present=direct always uses vsync.
    const bool queued = cfg.str("sdl.present", "queue") != "direct";
    const bool vsync = !queued || cfg.flag("sdl.vsync", false);
    const double refresh = cfg.num("sdl.refresh", 60);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, vsync ? SDL_RENDERER_PRESENTVSYNC : 0);
    SDL_RendererInfo ri{};
    if (SDL_GetRendererInfo(ren, &ri) == 0)
        Log(LogLevel::Info, "[sdl] renderer %s, video driver %s\n", ri.name, SDL_GetCurrentVideoDriver());
    SDL_RenderSetLogicalSize(ren, 256, 384);
    const int rs = emu.scale();  // render.scale: the screens' pixels (the logical size stays the DS's, for touch)
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 256 * rs, 384 * rs);

    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = on_audio;
    want.userdata = &emu;
    SDL_AudioDeviceID out = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (out) SDL_PauseAudioDevice(out, 0);

    SDL_AudioDeviceID in = 0;
    if (cfg.str("mic.source", "device") == "device") {
        SDL_AudioSpec mw{}, mh{};
        mw.freq = 44100;
        mw.format = AUDIO_S16SYS;
        mw.channels = 1;
        mw.samples = 1024;
        mw.callback = on_mic;
        mw.userdata = &emu;
        in = SDL_OpenAudioDevice(nullptr, 1, &mw, &mh, 0);
        if (in) SDL_PauseAudioDevice(in, 0);
        else Log(LogLevel::Warn, "[sdl] no microphone: %s (F1 still blows)\n", SDL_GetError());
    }

    uint32_t keys = 0;
    bool touching = false, quit = false;
    auto next = std::chrono::steady_clock::now();
    const auto frame = std::chrono::nanoseconds(16715180);  // 1 / 59.8261 Hz

    // sdl.present: queue (the default: the emulator on its own thread, a frame shown one vsync after it is done) or
    // direct (a frame shown as soon as it is done: no added latency, but a frame slower than a vsync misses it)
    FrameQueue fq;
    auto tick = std::chrono::steady_clock::now();
    const auto period = std::chrono::nanoseconds((long long)(1e9 / (refresh > 1 ? refresh : 60)));
    std::atomic<bool> run{true};
    // the main thread's input and requests for the emulator's thread (applied before its next frame)
    std::atomic<uint32_t> in_keys{0};
    std::atomic<int> in_touch{-1};      // y << 16 | x, -1 released
    std::atomic<int> in_save{0}, in_load{0};
    std::thread emu_thread;
    if (queued) {
        emu_thread = std::thread([&] {
            auto due = std::chrono::steady_clock::now();
            int touched = -1;
            const size_t n = (size_t)256 * 192 * rs * rs;
            while (run.load(std::memory_order_relaxed) && !emu.stopped()) {
                if (int s = in_save.exchange(0)) emu.save_state(s);
                if (int s = in_load.exchange(0)) emu.load_state(s);
                emu.set_keys(in_keys.load(std::memory_order_relaxed));
                const int t = in_touch.load(std::memory_order_relaxed);
                if (t != touched) {
                    if (t < 0) emu.release_touch();
                    else emu.touch(t & 0xFFFF, t >> 16);
                    touched = t;
                }
                emu.run_frame();
                fq.push(emu.top(), emu.bottom(), n);
                due += frame;
                auto now = std::chrono::steady_clock::now();
                if (due > now) std::this_thread::sleep_until(due);
                else if (now - due > frame * 4) due = now;  // too far behind: don't run to catch up
            }
            run = false;
        });
    }

    while (!quit && !emu.stopped() && (!queued || run.load())) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = true;
            else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                bool down = e.type == SDL_KEYDOWN;
                SDL_Keycode k = e.key.keysym.sym;
                if (down && !e.key.repeat) {
                    if (k == SDLK_ESCAPE) quit = true;
                    else if (k == SDLK_F1) emu.mic().blow(1500);
                    else if (k >= SDLK_F5 && k <= SDLK_F7) {
                        if (queued) in_save = 1 + (k - SDLK_F5);
                        else emu.save_state(1 + (k - SDLK_F5));
                    } else if (k >= SDLK_F9 && k <= SDLK_F11) {
                        if (queued) in_load = 1 + (k - SDLK_F9);
                        else emu.load_state(1 + (k - SDLK_F9));
                    }
                }
                uint32_t b = key_bit(k);
                keys = down ? keys | b : keys & ~b;
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONUP) {
                int wx = e.type == SDL_MOUSEMOTION ? e.motion.x : e.button.x;
                int wy = e.type == SDL_MOUSEMOTION ? e.motion.y : e.button.y;
                float lx, ly;
                SDL_RenderWindowToLogical(ren, wx, wy, &lx, &ly);
                if (e.type == SDL_MOUSEBUTTONDOWN && ly >= 192) touching = true;
                if (e.type == SDL_MOUSEBUTTONUP) {
                    touching = false;
                    if (queued) in_touch = -1;
                    else emu.release_touch();
                }
                if (touching) {
                    const int tx = std::clamp((int)lx, 0, 255), ty = std::clamp((int)ly - 192, 0, 191);
                    if (queued) in_touch = ty << 16 | tx;
                    else emu.touch(tx, ty);
                }
            }
        }
        const int w = 256 * rs, h = 192 * rs;
        if (queued) {
            in_keys = keys;
            // (the texture keeps the last frame: a repeat uploads nothing)
            if (const std::vector<uint32_t>* f = fq.next())
                SDL_UpdateTexture(tex, nullptr, f->data(), w * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, nullptr);
            SDL_RenderPresent(ren);  // (vsync paces this thread, or the clock below)
            if (!vsync) {
                tick += period;
                const auto now = std::chrono::steady_clock::now();
                if (tick > now) std::this_thread::sleep_until(tick);
                else if (now - tick > period * 4) tick = now;
            }
            // sdl.stats: every this many vsyncs, the vsync and emulation rates, how many vsyncs showed no new frame and
            // how many frames were never shown
            static const long every = cfg.num("sdl.stats", 0);
            static long vsyncs = 0;
            static auto since = std::chrono::steady_clock::now();
            if (every > 0 && ++vsyncs % every == 0) {
                const auto now = std::chrono::steady_clock::now();
                long rep, drop, made;
                {
                    std::lock_guard<std::mutex> l(fq.m);
                    rep = fq.repeated; drop = fq.dropped; made = fq.pushed;
                    fq.repeated = fq.dropped = fq.pushed = 0;
                }
                const double sec = std::chrono::duration<double>(now - since).count();
                Log(LogLevel::Info, "[sdl] %ld vsyncs at %.2f Hz, %.2f frames/s emulated: %ld without a new frame, %ld dropped\n",
                    every, every / sec, made / sec, rep, drop);
                since = now;
            }
            continue;
        }
        emu.set_keys(keys);
        emu.run_frame();  // the audio callback reads the SPU meanwhile: melonDS locks its output buffer

        void* px;
        int pitch;
        SDL_LockTexture(tex, nullptr, &px, &pitch);
        for (int y = 0; y < 2 * h; y++)
            memcpy((uint8_t*)px + y * pitch, y < h ? emu.top() + y * w : emu.bottom() + (y - h) * w, w * 4);
        SDL_UnlockTexture(tex);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        next += frame;
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > frame * 4) next = now;  // too far behind: don't run to catch up
    }

    run = false;
    if (emu_thread.joinable()) emu_thread.join();
    if (cfg.flag("resume.on_quit", true)) emu.save_state(0);
    if (in) SDL_CloseAudioDevice(in);
    if (out) SDL_CloseAudioDevice(out);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

}  // namespace bananads
