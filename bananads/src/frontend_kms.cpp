// frontend=kms: the handheld path. Both DS screens straight onto the two panels through DRM/KMS (no display server,
// no compositor, no GPU), the pad and the bottom touchscreen from evdev, sound through ALSA. What BananaLinux's
// docs/EMULATORS.md asks of an emulator, modelled on SuperDrastic's dsflip.c (the same hardware):
//
// - DRM master on the card with two connected panels of kms.top's connector type (BANANADS_TOP: DSI-2 on the RG DS,
//   Virtual-2 in QEMU); kms.top is the top screen. The modes stay as they are (the kernel command line's, else the
//   panel's preferred one: the DTB carries a measured 60 Hz), and everything is given back on exit (closing the
//   device drops master; the menu modesets again).
// - Each frame is drawn into DRM dumb buffers, one per panel, and both go to the screen in ONE atomic commit
//   (DRM_MODE_ATOMIC_NONBLOCK | PAGE_FLIP_EVENT): the same emulated frame on both panels. The display plane scales
//   the 256x192 (512x384 at render.scale=2) buffer to the panel itself (VOP2 on the RG DS), and the Banana 2D
//   engine draws each line straight into it (Emu::draw_into: no copy, no GPU). Where the plane can't scale (QEMU's
//   virtio GPU), or with another renderer, the finished frame is copied in instead, scaled by a whole number on the
//   CPU and centred in the first case.
// - Frames queue (as sdl.present=queue): the emulator runs on its own thread at the DS's rate, and each finished
//   frame is committed as soon as the previous flip is done, so a game whose frames take 19 and 12 ms in turn
//   (HeartGold at 1104 MHz) still shows one new frame per refresh. Up to kms.queue frames wait; past that the oldest
//   is dropped (counted). The presenter (this thread) runs SCHED_FIFO (kms.rt), as SuperDrastic's does: its work per
//   wake-up is tiny, but a late one finds the last flip still pending and the frame waits a refresh.
// - kms.lowlat (experimental): no queue, each frame started so it is ready just before the vblank it aims at (see
//   Display's kms.lowlat members); with runahead, each frame also run ahead and undone (Emu::run_ahead).
// - Input: the pad (rocknix-singleadc-joypad, and any keyboard) read, never grabbed (bananad reads the same device
//   for the lid, power and volume keys, which are ignored here). The touchscreen is the device whose sysfs path
//   contains kms.touch (BANANADS_TOUCH: fe5e0000.i2c, the bottom panel's). F (BTN_MODE) + Start quits; F + R saves
//   slot 1, F + L loads it.
// - SIGTERM/SIGINT/SIGHUP quit the same way (the resume state written, then the save): BananaLinux's shutdown waits
//   5 s for that. SIGSTOP/SIGCONT around sleep only leave a gap: the pacing starts again from the time it wakes.
// - SIGUSR1 quits with the resume state written whatever resume.on_quit says (BananaOS's Power off after sleeping:
//   the game saved before the handheld powers itself off, loaded once at the next boot even with Quick resume off).
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <fstream>
#include <glob.h>
#include <iterator>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Platform.h"
#include "cpugov.h"
#include "frontend.h"
#ifdef BANANADS_GENGIS
#include "gengis_renderer.h"
#endif
#include "cheats.h"
#include "screenshot.h"
#include "menu.h"
#include "prof.h"
#include "ra.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_save{false};  // SIGUSR1: the quit writes the resume state, resume.on_quit or not
static std::atomic<bool> g_nosave{false};  // the menu's Quit without saving: no resume state (SIGUSR1's still is)
static std::atomic<int> g_ff{0};  // fast-forward: 0 off, else the speed (99: as fast as it goes)
static int g_wake = -1;  // eventfd: a signal or a new frame wakes the presenter

static void wake() {
    if (g_wake >= 0) {
        uint64_t one = 1;
        ssize_t r = write(g_wake, &one, sizeof one);
        (void)r;
    }
}
static void on_signal(int) {
    g_quit = true;
    wake();
}
static void on_save_quit(int) {
    g_save = true;
    g_quit = true;
    wake();
}

static long long now_us() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

// a priority-inheriting mutex (dsflip's): the presenter runs SCHED_FIFO, the emulator's thread doesn't, and on a
// busy core a plain one could leave the presenter waiting behind a preempted emulator thread
struct PiMutex {
    pthread_mutex_t m;
    PiMutex() {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&m, &a);
        pthread_mutexattr_destroy(&a);
    }
    ~PiMutex() { pthread_mutex_destroy(&m); }
    void lock() { pthread_mutex_lock(&m); }
    void unlock() { pthread_mutex_unlock(&m); }
};
using Lock = std::lock_guard<PiMutex>;

// ---- the display ----------------------------------------------------------------------------------------------

// SuperDrastic's shader pass (shader.c): GL on the presenter thread, dumb buffers in and out
extern "C" {
int shader_init(int fd, const char* name);
int shader_draw(uint32_t sh, int sw, int shh, int sp, uint64_t sgen, uint32_t dh, int dw, int dhh, int dp, uint64_t dgen, int finish);
int shader_viewport(int* v, int pw, int ph);
int shader_grid4_lut(unsigned char lut[2][256]);
extern int shader_fast;
int shader_fast_on(void);
extern int clear_first;
void dsflip_log(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Log(LogLevel::Info, "%s", b);
}
}

struct Buf {
    uint32_t fb = 0, handle = 0, pitch = 0, w = 0, h = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;
    bool r565 = false;   // DRM_FORMAT_RGB565 (kms.rgb565), else XRGB8888
};

// kms.rgb565: a row of a buffer's pixels as XRGB8888 (as the display expands them), and back
static void row_to32(const Buf& b, uint32_t y, uint32_t* dst, uint32_t w) {
    const uint8_t* p = b.map + (size_t)y * b.pitch;
    if (!b.r565) { memcpy(dst, p, (size_t)w * 4); return; }
    static thread_local std::vector<uint16_t> t;   // (one read of the slow buffer a row)
    t.resize(w);
    memcpy(t.data(), p, (size_t)w * 2);
    for (uint32_t x = 0; x < w; x++) dst[x] = Emu::px565(t[x]);
}

struct Panel {
    uint32_t conn = 0, crtc = 0, plane = 0, mode_blob = 0;
    int crtc_idx = -1;
    drmModeModeInfo mode{};
    uint32_t p_fb = 0, p_crtc = 0, p_sx = 0, p_sy = 0, p_sw = 0, p_sh = 0, p_cx = 0, p_cy = 0, p_cw = 0, p_ch = 0;
    std::string name;
};

enum SlotState { FREE, WRITING, QUEUED, COMMITTED, SCANOUT };

struct Display {
    static const int NB = 7;  // writing, up to 3 queued, drawn ahead through the shader, committed, on screen
    int fd = -1;
    Panel p[2];               // [0] = top, [1] = bottom
    // F + SELECT: the DS's top screen on the bottom panel and its bottom screen on the top one (the frames' buffers go to
    // the other planes at the commit; touch follows the DS's bottom screen, see the input loop)
    std::atomic<bool> swapped{false};
    int W = 0, H = 0;         // the panels' mode
    int fw = 0, fh = 0;       // a DS screen's size at render.scale
    bool hw = true;           // the plane scales the frame; else the CPU does, into panel-sized buffers
    bool direct = false;      // the emulator draws straight into the buffers (hw, rows packed, the Banana 2D engine)
    int k = 1, ox = 0, oy = 0;  // CPU scaling: the whole-number factor and where the picture sits
    Buf buf[2][NB];
    // kms.rgb565: each slot's RGB565 twins, made at the first frame drawn into them; is16[s]: slot s's frame is in them
    // (then shown as it is: no shader pass, as with the grid). rgb565: allowed (render.scale 4, drawn straight in, no
    // run-ahead); shader_live: the emulator's thread's view of shader_on (a shader: 32-bit frames)
    Buf buf16[2][NB];
    bool is16[NB] = {};
    bool rgb565 = false;
    std::atomic<bool> shader_live{false};
    const Buf* slot(int panel, int s) const { return is16[s] ? &buf16[panel][s] : &buf[panel][s]; }
    bool make16() {
        for (auto& pb : buf16)
            for (auto& b : pb)
                if (!b.map && (!mkbuf(b, fw, fh, true) || b.pitch != (uint32_t)fw * 2)) return false;
        return true;
    }
    uint32_t* map16(int panel, int s) { return (uint32_t*)buf16[panel][s].map; }
    Buf mbuf[2][2];           // the in-game menu's pictures: panel-sized, two a panel (one drawn while one shows)
    int mnext[2] = {0, 0};
    SlotState state[NB] = {};
    // render.2d_lazy: a slot's frame is finished once the 2D thread passes its fence (Emu::frame_fence), which the
    // presenter waits for before it reads or shows the slot (wait2d); the emulator's thread goes on meanwhile
    bool has_fence[NB] = {};
    uint32_t fence[NB] = {};
    std::function<void(uint32_t)> wait2d;
    std::function<void(const void*)> touched;   // render.2d_out_skip: we wrote into a buffer the emulator draws into
    std::deque<int> queue;    // frames waiting, oldest first
    // kms.shader: each frame drawn through a fragment shader (SuperDrastic's) into panel-sized buffers, shown 1:1
    std::string shader_name;
    bool shader_tried = false, shader_on = false;
    bool first_flip = false;  // (log.time's "[start] first flip")
    std::string shader_req;           // from the in-game menu (any thread): taken up by the presenter's next frame
    bool shader_req_set = false;
    Buf sbuf[2][3];
    int snext = 0;
    int committed = -1, scanout = -1, pending = 0;
    // what the panels show for the slots above: the slot's own buffers, or the shader's output drawn from them (the
    // in-game menu's background and the picture it puts back are these, so a shader's look stays while it is open)
    const Buf* committed_out[2] = {nullptr, nullptr};
    const Buf* scanout_out[2] = {nullptr, nullptr};
    PiMutex mu;
    double period_us = 16666.7;
    // kms.lowlat (experimental): each frame is aimed at a vblank: the emulator's thread starts it so that it is ready
    // just before that vblank's deadline (the buttons read as late as the panels allow). The time a frame takes is its
    // own work alone: the emulator's thread's (start -> published) and the presenter's (taken -> ready: the 2D thread's
    // last lines and the shader), each a decaying maximum. Never the time it waited for the panels: counted in, a frame
    // that waited made the next one start earlier, wait longer, and so on (measured on the RG DS Plus: 14 -> 26 ms in
    // Mario Kart DS, the deadlines it then skipped on purpose 100-200 repeats a minute). A frame that will miss its
    // vblank isn't held or replaced: it goes at once (it may still make it), and the next one aims at the vblank after
    // the one it makes (one repeat, no frame lost). Two frames may wait, so a game whose frames take longer than a
    // refresh starts the next one before the last is on screen: more latency while it needs it, not a stutter.
    // Below, with mu held unless said.
    bool lowlat = false;
    long long vbl_top = 0, vbl_bot = 0;  // the panels' last vblanks (flip timestamps, CLOCK_MONOTONIC us)
    double bot_off = 0;                  // the bottom panel's vblank after the top's, us in (-P/2, P/2]
    int nflips = 0;                      // flip pairs seen (the timing is trusted after a few)
    double margin_us = 1500, margin_base = 1500;  // the deadline: this long before the earlier panel's vblank (+300 a miss)
    long long start_us[NB] = {}, aim_us[NB] = {};  // when a slot's frame was started, and the deadline it aims at (0: none)
    long long take_us[NB] = {};          // when the presenter took the slot's frame off the queue
    std::atomic<long long> emu_us{0};    // the emulator's thread's time for a frame (us), a decaying maximum
    std::atomic<long long> pres_us{0};   // the presenter's: taken -> ready for the panels (us), a decaying maximum
    long long first_ev = 0;              // the first flip event of the commit in flight
    long long commit_us = 0;             // when the commit in flight was made
    long long commit_aim = 0;            // the deadline the commit in flight aimed at, if it went before it (0: none)
    long ll_late = 0, ll_miss = 0, ll_n = 0;  // statistics: frames after their deadline, commits that missed it, on time
    long ll_behind = 0;                  // ...frames shown after the vblank they aimed at (refreshes)
    // refreshes the frames on screen fell behind their aims since the emulator's thread last looked: it aims that much
    // later (the panels tell, at the flip; predicting it from the worst-case times shifted frames that were on time,
    // a repeat each: 280-340 a minute on the RG DS Plus)
    std::atomic<int> slip{0};
    // every slip counted so far, and its value when each slot's frame was aimed: the frames aimed before a slip was
    // known show late by that much too, and mustn't count it again (they did: each frame behind a late one moved the
    // aims on once more, 61 of 95 repeats in a minute on the RG DS)
    long slip_total = 0, slip_at[NB] = {};
    long long shown_vbl = 0;
    long long wake_at = 0;               // kms.lowlat: the presenter's next wake-up for a frame held to its window             // the top panel's vblank at which the last frame went on screen
    // (mu held) the vblank at which the newest frame not on screen yet will be: the frames waiting go one a refresh
    // after the last one shown, and none before the next vblank from now
    long long tail_vbl(long long now) const {
        int n = 0;
        for (int i = 0; i < NB; i++) n += state[i] == QUEUED || state[i] == COMMITTED;
        long long shown = shown_vbl;
        // between the pair's two flip events the committed frame is on screen already (at the first one's vblank):
        // counted as waiting, the next frame was aimed a refresh too late (a repeat each time, 16-27 a minute)
        if (pending == 1 && committed >= 0) { n--; shown = std::max(shown, first_ev ? first_ev : vbl_top); }
        // ...and so it is once the vblank after its commit has passed, before the flip events have come (they come
        // ~0.1-0.5 ms after the vblank): a frame published in that gap counted the one on screen as waiting and was
        // aimed a refresh later, a repeat and a frame of sound lost each time (RG DS, Mario Kart DS: 5 of 6 repeats
        // a minute, the sound's backlog run dry and stretched to 0.90). (A commit 1 ms before a vblank makes it.)
        else if (pending == 2 && committed >= 0 && commit_us && vbl_top) {
            const double k = std::ceil((double)(commit_us + 1000 - vbl_top) / period_us);   // (the top's grid; the
            const long long v = vbl_top + (long long)(std::max(1.0, k) * period_us + std::min(0.0, bot_off));  // earlier panel's)
            if (now >= v) { n--; shown = std::max(shown, v); }
        }
        if (n <= 0 || !shown) return 0;
        const double k = std::ceil((double)(now - vbl_top) / period_us);   // the next vblank after now
        const long long next_vbl = vbl_top + (long long)(std::max(1.0, k) * period_us);
        return std::max(shown + (long long)(n * period_us), next_vbl + (long long)((n - 1) * period_us));
    }
    double ll_lead_sum = 0;              // ...and how far before it the on-time ones were (us)
    bool vbl_known() const { return lowlat && vbl_top && nflips >= 30; }
    // the first deadline at or after t
    long long deadline_after(long long t) const {
        const double hi = period_us + std::min(0.0, bot_off) - margin_us;   // the phase after the top panel's vblank
        const double k = std::ceil(((double)(t - vbl_top) - hi) / period_us);
        return vbl_top + (long long)(k * period_us + hi);
    }
    static void decay_max(std::atomic<long long>& m, long long w) {   // (~6 s half-life at 60 frames a second: a heavy
        const long long d = m.load(std::memory_order_relaxed);         // stretch's frames come back within seconds)
        m.store(std::max(w, d - d / 512), std::memory_order_relaxed);
    }
    // more lead after a frame shown late: +2 ms each (at most 16: a heavy scene's spikes buy their own headroom), back
    // 0.25 ms after a clean 10 s
    std::atomic<long long> extra_us{0};
    // kms.lowlat_trace=<file>: each frame's times (us, CLOCK_MONOTONIC) in a ring, written at the quit: started,
    // published, aimed deadline, taken by the presenter, ready, committed, on screen (the pair's first flip event)
    struct Trace { long long start, pub, aim, take, ready, commit, flip, ftop, fbot; int ab, tempo, ra; };
    int ra_now = 0;                      // (the trace) the frame being published was run this many frames ahead
#ifdef BANANADS_ALSA
    static int trace_ab() { return g_audio_backlog.load(std::memory_order_relaxed); }
    static int trace_tempo() { return g_audio_tempo.load(std::memory_order_relaxed); }
#else
    static int trace_ab() { return 0; }
    static int trace_tempo() { return 1000; }
#endif
    std::vector<Trace> trace;
    unsigned long seq_next = 0, seq[NB] = {};
    Trace* tr(int s) { return trace.empty() ? nullptr : &trace[seq[s] % trace.size()]; }
    // statistics (kms.stats, the governor)
    std::atomic<long> made{0}, dropped{0}, shown{0};
    long repeats = 0;
    long long last_flip = 0;

    uint32_t prop(uint32_t obj, uint32_t type, const char* name) {
        drmModeObjectProperties* pr = drmModeObjectGetProperties(fd, obj, type);
        uint32_t id = 0;
        for (uint32_t i = 0; pr && i < pr->count_props && !id; i++) {
            drmModePropertyRes* q = drmModeGetProperty(fd, pr->props[i]);
            if (q && !strcmp(q->name, name)) id = q->prop_id;
            drmModeFreeProperty(q);
        }
        drmModeFreeObjectProperties(pr);
        return id;
    }
    uint64_t propval(uint32_t obj, uint32_t type, const char* name) {
        drmModeObjectProperties* pr = drmModeObjectGetProperties(fd, obj, type);
        uint64_t v = 0;
        for (uint32_t i = 0; pr && i < pr->count_props; i++) {
            drmModePropertyRes* q = drmModeGetProperty(fd, pr->props[i]);
            if (q && !strcmp(q->name, name)) v = pr->prop_values[i];
            drmModeFreeProperty(q);
        }
        drmModeFreeObjectProperties(pr);
        return v;
    }

    bool mkbuf(Buf& b, int w, int h, bool r565 = false) {
        drm_mode_create_dumb c{};
        c.width = w;
        c.height = h;
        c.bpp = r565 ? 16 : 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
        b.handle = c.handle;
        b.pitch = c.pitch;
        b.size = c.size;
        b.w = w;
        b.h = h;
        b.r565 = r565;
        uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
        if (drmModeAddFB2(fd, w, h, r565 ? DRM_FORMAT_RGB565 : DRM_FORMAT_XRGB8888, hs, ps, os, &b.fb, 0)) return false;
        drm_mode_map_dumb m{};
        m.handle = c.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return false;
        void* p = mmap(nullptr, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
        if (p == MAP_FAILED) return false;
        b.map = (uint8_t*)p;
        memset(b.map, 0, c.size);  // black (the borders of a CPU-scaled picture stay so)
        if (touched) touched(b.map);
        return true;
    }
    void freebuf(Buf& b) {
        if (b.map) munmap(b.map, b.size);
        if (b.fb) drmModeRmFB(fd, b.fb);
        if (b.handle) {
            drm_mode_destroy_dumb d{};
            d.handle = b.handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        b = Buf();
    }
    void freebufs() {
        for (auto& pb : buf)
            for (auto& b : pb) freebuf(b);
        for (auto& pb : buf16)
            for (auto& b : pb) freebuf(b);
        for (auto& pb : mbuf)
            for (auto& b : pb) freebuf(b);
    }

    // ---- the in-game menu: its pictures straight onto the panels, then the game's frame back ----
    bool menu_bufs() {
        for (auto& pb : mbuf)
            for (auto& b : pb)
                if (!b.map && !mkbuf(b, W, H)) return false;
        return true;
    }
    // (blocking commits, with no flip of the game's pending: wait_flips first)
    bool commit_now(int panel0, int panel1, const Buf* b0, const Buf* b1) {
        drmModeAtomicReq* r = drmModeAtomicAlloc();
        if (b0) add_plane(r, p[panel0], *b0);
        if (b1) add_plane(r, p[panel1], *b1);
        int ret = drmModeAtomicCommit(fd, r, 0, nullptr);
        drmModeAtomicFree(r);
        if (ret) Log(LogLevel::Warn, "[kms] menu commit failed: %s\n", strerror(errno));
        return ret == 0;
    }
    void show_menu(int panel, int k) { commit_now(panel, 0, &mbuf[panel][k], nullptr); }
    void show_game() {
        Lock l(mu);
        const int t = swapped ? 1 : 0;   // (what each panel showed; the game's buffers, as swapped, before a first commit)
        if (scanout >= 0) commit_now(0, 1, scanout_out[0] ? scanout_out[0] : slot(t, scanout), scanout_out[1] ? scanout_out[1] : slot(t ^ 1, scanout));
    }
    // the game's top screen as last shown (through the shader, when one is on: #49), at the panel's size (nearest)
    bool grab_top(uint32_t* dst, int pitch, int w, int h) {
        Lock l(mu);
        if (scanout < 0) return false;
        const Buf& b = scanout_out[0] ? *scanout_out[0] : *slot(0, scanout);
        if (!b.map || !b.w || !b.h) return false;
        const int sw = (int)b.w, sh = (int)b.h;
        std::vector<uint32_t> row(sw);
        for (int y = 0; y < h; y++) {
            const int sy = y * sh / h;
            row_to32(b, sy, row.data(), sw);  // (one read of the slow buffer a row)
            uint32_t* d = (uint32_t*)((uint8_t*)dst + (size_t)y * pitch);
            for (int x = 0; x < w; x++) d[x] = row[x * sw / w] & 0xFFFFFF;
        }
        return true;
    }

    void add_plane(drmModeAtomicReq* r, Panel& q, const Buf& b) {
        drmModeAtomicAddProperty(r, q.plane, q.p_fb, b.fb);
        drmModeAtomicAddProperty(r, q.plane, q.p_crtc, q.crtc);
        drmModeAtomicAddProperty(r, q.plane, q.p_sx, 0);
        drmModeAtomicAddProperty(r, q.plane, q.p_sy, 0);
        drmModeAtomicAddProperty(r, q.plane, q.p_sw, (uint64_t)b.w << 16);
        drmModeAtomicAddProperty(r, q.plane, q.p_sh, (uint64_t)b.h << 16);
        drmModeAtomicAddProperty(r, q.plane, q.p_cx, 0);
        drmModeAtomicAddProperty(r, q.plane, q.p_cy, 0);
        drmModeAtomicAddProperty(r, q.plane, q.p_cw, q.mode.hdisplay);
        drmModeAtomicAddProperty(r, q.plane, q.p_ch, q.mode.vdisplay);
    }

    // each panel lit in its mode on its CRTC (with ALLOW_MODESET: the menu may have switched them off on letting go)
    void add_modeset(drmModeAtomicReq* r) {
        for (auto& q : p) {
            drmModeAtomicAddProperty(r, q.conn, prop(q.conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), q.crtc);
            drmModeAtomicAddProperty(r, q.crtc, prop(q.crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), q.mode_blob);
            drmModeAtomicAddProperty(r, q.crtc, prop(q.crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
        }
    }

    // the device, the panels, the planes, the first (black) frame
    bool open(const std::string& top, int scale) {
        std::string type = top.substr(0, top.find('-'));
        for (int n = 0; n < 8 && fd < 0; n++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/dri/card%d", n);
            int f = ::open(path, O_RDWR | O_CLOEXEC);
            if (f < 0) continue;
            fd = f;
            if (!find_panels(top, type)) {
                ::close(f);
                fd = -1;
                p[0] = p[1] = Panel();
            }
        }
        if (fd < 0) {
            Log(LogLevel::Error, "[kms] no DRM device with two connected %s panels\n", type.c_str());
            return false;
        }
        // the menu may still be letting go of the device: up to 3 s for DRM master
        int m = -1;
        for (int i = 0; i < 300 && (m = drmSetMaster(fd)) != 0; i++) usleep(10000);
        if (m) Log(LogLevel::Warn, "[kms] not DRM master (%s); trying anyway\n", strerror(errno));
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
        if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
            Log(LogLevel::Error, "[kms] no atomic modesetting\n");
            return false;
        }
        drmModePlaneRes* pres = drmModeGetPlaneResources(fd);
        if (!pres) return false;
        for (int i = 0; i < 2; i++)
            for (uint32_t j = 0; j < pres->count_planes && !p[i].plane; j++) {
                drmModePlane* pl = drmModeGetPlane(fd, pres->planes[j]);
                if (pl && (pl->possible_crtcs & (1u << p[i].crtc_idx)) &&
                    propval(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type") == DRM_PLANE_TYPE_PRIMARY &&
                    (i == 0 || pl->plane_id != p[0].plane))
                    p[i].plane = pl->plane_id;
                drmModeFreePlane(pl);
            }
        if (!p[0].plane || !p[1].plane) {
            Log(LogLevel::Error, "[kms] no primary plane for both panels\n");
            drmModeFreePlaneResources(pres);
            return false;
        }
        for (auto& q : p) {
            const uint32_t o = DRM_MODE_OBJECT_PLANE;
            q.p_fb = prop(q.plane, o, "FB_ID");
            q.p_crtc = prop(q.plane, o, "CRTC_ID");
            q.p_sx = prop(q.plane, o, "SRC_X");
            q.p_sy = prop(q.plane, o, "SRC_Y");
            q.p_sw = prop(q.plane, o, "SRC_W");
            q.p_sh = prop(q.plane, o, "SRC_H");
            q.p_cx = prop(q.plane, o, "CRTC_X");
            q.p_cy = prop(q.plane, o, "CRTC_Y");
            q.p_cw = prop(q.plane, o, "CRTC_W");
            q.p_ch = prop(q.plane, o, "CRTC_H");
            drmModeCreatePropertyBlob(fd, &q.mode, sizeof q.mode, &q.mode_blob);
        }
        W = p[0].mode.hdisplay;
        H = p[0].mode.vdisplay;
        const drmModeModeInfo& md = p[0].mode;
        if (md.htotal && md.vtotal) period_us = 1e9 / ((double)md.clock * 1000.0 / (md.htotal * md.vtotal)) / 1000.0;
        fw = 256 * scale;
        fh = 192 * scale;

        // can the plane scale a DS-sized buffer to the panel? (VOP2 can; QEMU's virtio GPU can't)
        hw = mkbuf(buf[0][0], fw, fh) && mkbuf(buf[1][0], fw, fh);
        if (hw) {
            drmModeAtomicReq* r = drmModeAtomicAlloc();
            add_modeset(r);
            for (int i = 0; i < 2; i++) add_plane(r, p[i], buf[i][0]);
            hw = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
            drmModeAtomicFree(r);
        }
        freebufs();
        int bw = fw, bh = fh;
        if (!hw) {
            k = std::max(1, std::min(W / fw, H / fh));
            ox = (W - fw * k) / 2;
            oy = (H - fh * k) / 2;
            bw = W;
            bh = H;
        }
        for (int s = 0; s < NB; s++)
            if (!mkbuf(buf[0][s], bw, bh) || !mkbuf(buf[1][s], bw, bh)) {
                Log(LogLevel::Error, "[kms] can't make scanout buffers\n");
                drmModeFreePlaneResources(pres);
                return false;
            }

        // the first frame (black) on both panels in the mode they have, every other plane on their CRTCs off
        drmModeAtomicReq* r = drmModeAtomicAlloc();
        for (uint32_t j = 0; j < pres->count_planes; j++) {
            uint32_t id = pres->planes[j];
            if (id == p[0].plane || id == p[1].plane) continue;
            drmModeAtomicAddProperty(r, id, prop(id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
            drmModeAtomicAddProperty(r, id, prop(id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
        }
        drmModeFreePlaneResources(pres);
        add_modeset(r);
        for (int i = 0; i < 2; i++) add_plane(r, p[i], buf[i][0]);
        int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr);
        drmModeAtomicFree(r);
        if (ret) {
            Log(LogLevel::Error, "[kms] the first frame failed: %s\n", strerror(errno));
            return false;
        }
        scanout = 0;
        state[0] = SCANOUT;
        return true;
    }

    bool find_panels(const std::string& top, const std::string& type) {
        drmModeRes* res = drmModeGetResources(fd);
        if (!res) return false;
        struct Found { Panel q; std::vector<drmModeModeInfo> modes; bool top; };
        std::vector<Found> found;
        uint32_t used = 0;
        for (int i = 0; i < res->count_connectors && found.size() < 2; i++) {
            drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
            if (!c) continue;
            const char* tn = drmModeGetConnectorTypeName(c->connector_type);
            if (tn && type == tn && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
                Found f;
                // the kernel command line's mode (video=<connector>:<mode>) first, then the panel's preferred one
                f.q.mode = c->modes[0];
                for (int m = 0; m < c->count_modes; m++)
                    if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) { f.q.mode = c->modes[m]; break; }
                for (int m = 0; m < c->count_modes; m++)
                    if (c->modes[m].type & DRM_MODE_TYPE_USERDEF) { f.q.mode = c->modes[m]; break; }
                f.modes.assign(c->modes, c->modes + c->count_modes);
                for (int e = 0; e < c->count_encoders && !f.q.crtc; e++) {
                    drmModeEncoder* enc = drmModeGetEncoder(fd, c->encoders[e]);
                    if (!enc) continue;
                    for (int j = 0; j < res->count_crtcs && !f.q.crtc; j++)
                        if ((enc->possible_crtcs & (1u << j)) && !(used & (1u << j))) {
                            f.q.crtc = res->crtcs[j];
                            f.q.crtc_idx = j;
                            used |= 1u << j;
                        }
                    drmModeFreeEncoder(enc);
                }
                char name[32];
                snprintf(name, sizeof name, "%s-%u", tn, c->connector_type_id);
                f.q.name = name;
                f.q.conn = c->connector_id;
                f.top = top == name;
                if (f.q.crtc) found.push_back(f);
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        if (found.size() < 2) return false;
        // the top screen: the named connector, else the second one (as banana-ui)
        if (!found[0].top && !found[1].top) std::swap(found[0], found[1]);
        if (found[1].top) std::swap(found[0], found[1]);
        auto same = [&] {
            return found[0].q.mode.hdisplay == found[1].q.mode.hdisplay &&
                   found[0].q.mode.vdisplay == found[1].q.mode.vdisplay;
        };
        if (!same())
            for (auto& m : found[1].modes)
                if (m.hdisplay == found[0].q.mode.hdisplay && m.vdisplay == found[0].q.mode.vdisplay) {
                    found[1].q.mode = m;
                    break;
                }
        if (!same()) return false;
        p[0] = found[0].q;
        p[1] = found[1].q;
        return true;
    }

    void close() {
        cthread_stop();
        gpu_floor(false);
        if (fd < 0) return;
        freebufs();
        for (auto& q : p)
            if (q.mode_blob) drmModeDestroyPropertyBlob(fd, q.mode_blob);
        drmDropMaster(fd);
        ::close(fd);
        fd = -1;
    }

    // ---- the emulator's side: a slot to write the next frame into, then queue it ----
    int take() {
        Lock l(mu);
        for (int s = 0; s < NB; s++)
            if (state[s] == FREE) {
                state[s] = WRITING;
                return s;
            }
        // (can't happen: NB covers every state at once) the oldest waiting frame makes room
        int s = queue.front();
        queue.pop_front();
        dropped++;
        state[s] = WRITING;
        return s;
    }
    // the emulator draws a frame straight into slot s (Emu::draw_into), rows packed as it wants them
    bool packed() const {
        if (!hw) return false;
        for (auto& pb : buf)
            for (auto& b : pb)
                if (b.pitch != (uint32_t)fw * 4) return false;
        return true;
    }
    uint32_t* map(int panel, int s) { return (uint32_t*)buf[panel][s].map; }
    // the finished picture in slot s of a panel, for drawing over it (the achievements' pop-ups): where it starts,
    // its pitch in pixels and its size (the DS screen's at render.scale, or as the CPU scaled it into the panel)
    uint32_t* picture(int panel, int s, int* pitch, int* w, int* h) {
        Buf& b = buf[panel][s];
        *pitch = (int)(b.pitch / 4);
        if (hw) { *w = fw; *h = fh; return (uint32_t*)b.map; }
        *w = fw * k; *h = fh * k;
        return (uint32_t*)(b.map + (size_t)oy * b.pitch) + ox;
    }
    // or the finished frame is copied in
    void write(int s, const uint32_t* top, const uint32_t* bot) {
        const uint32_t* src[2] = {top, bot};
        for (int i = 0; i < 2; i++) {
            Buf& b = buf[i][s];
            if (touched) touched(b.map);
            if (hw) {
                for (int y = 0; y < fh; y++) memcpy(b.map + (size_t)y * b.pitch, src[i] + (size_t)y * fw, fw * 4);
                continue;
            }
            // whole-number nearest scaling, written once per output row (dumb buffers are slow to read back)
            static thread_local std::vector<uint32_t> row;
            row.resize((size_t)fw * k);
            for (int y = 0; y < fh; y++) {
                const uint32_t* in = src[i] + (size_t)y * fw;
                const uint32_t* out = in;
                if (k > 1) {
                    for (int x = 0; x < fw; x++)
                        for (int j = 0; j < k; j++) row[(size_t)x * k + j] = in[x];
                    out = row.data();
                }
                for (int j = 0; j < k; j++)
                    memcpy(b.map + (size_t)(oy + y * k + j) * b.pitch + (size_t)ox * 4, out, (size_t)fw * k * 4);
            }
        }
    }
    void publish(int s, int depth, bool fenced = false, uint32_t f = 0, long long start = 0, long long aim = 0,
                 long slip_seen = 0) {
        {
            Lock l(mu);
            slip_at[s] = slip_seen;
            start_us[s] = start;
            aim_us[s] = aim;
            take_us[s] = 0;
            if (start) {
                const long long t = now_us();
                if (t - start > 0 && t - start < 3 * (long long)period_us) decay_max(emu_us, t - start);
                seq[s] = seq_next++;
                if (Trace* x = tr(s)) *x = Trace{start, t, aim, 0, 0, 0, 0, 0, 0, trace_ab(), trace_tempo(), ra_now};
            }
            has_fence[s] = fenced;
            fence[s] = f;
            state[s] = QUEUED;
            queue.push_back(s);
            made++;
            while ((int)queue.size() > depth) {
                state[queue.front()] = FREE;
                queue.pop_front();
                dropped++;
            }
        }
        wake();
    }

    // ---- the presenter's side ----
    // The oldest waiting frame goes to both panels in one commit when no flip is pending. With a shader, the next
    // frame is drawn through it ahead, while the last commit's flip is still pending, and committed the moment that
    // flip completes. Drawn only after the flip (13.6 ms a frame for both of the RG DS Plus's 1024x768 panels with
    // Dot (MiZai, RG DS+)), the commit missed the next refresh every other frame: 40 frames shown a second of 60.
    // kms.grid_cpu: with ds-grid-2x at render.scale=4 (1:1), the grid applied by the Banana 2D engine as it writes the
    // frame (Emu::set_grid4, tables from shader_grid4_lut) instead of a GPU pass a frame: the same bytes on the panels
    // (the tables are the GPU's own output of the shader), no GPU pass, no Mesa work. grid_req: the emulator's thread's
    // next frame turns it on (1) or off (0); gridded[s]: slot s's frame has it (shown as it is)
    bool grid_cpu = false;
    std::atomic<int> grid_req{-1};
    unsigned char grid_lut[2][256] = {};
    bool gridded[NB] = {};
    int ready = -1;                                // a frame already drawn through the shader: its slot, or -1
    const Buf* ready_out[2] = {nullptr, nullptr};  // ...and the shader's output for each panel
    void shader_settings() {  // a change asked for in the in-game menu, and the program made at its first frame
        std::string want;
        bool asked = false;
        {
            Lock l(mu);
            if (shader_req_set) { want = shader_req; asked = true; shader_req_set = false; }
        }
        if (asked) {
            if (grid_cpu) grid_req = 0;   // (kms.grid_cpu: back to drawing frames without the grid)
            if (want == "none") {
                if (shader_on) gpu_floor(false);
                shader_on = false;
                shader_name.clear();
            } else if (want != shader_name || !shader_on) {
                shader_name = want;
                shader_tried = false;  // made again below, with the new program
            }
        }
        if (!shader_name.empty() && !shader_tried) {  // GL is made on this thread
            shader_tried = true;
            if (gpu_floor_low) { gpu_floor(false); gpu_floor_low = false; }   // (another shader: the 600 floor again)
            start_mark("shader: start");
            shader_on = shader_init(fd, shader_name.c_str()) != 0;
            for (int i = 0; i < 2 && shader_on; i++)
                for (auto& b : sbuf[i]) shader_on = shader_on && (b.fb || mkbuf(b, W, H));  // (made once)
            if (shader_on && gpu_dir.empty()) gpu_floor(true);
            Log(LogLevel::Info, "[kms] shader %s: %s\n", shader_name.c_str(), shader_on ? "on (panel-sized, 1:1)" : "unavailable");
            if (shader_on && grid_cpu && direct && fw == 1024 && fh == 768 && W == 1024 && H == 768 &&
                shader_name == "ds-grid-2x" && shader_grid4_lut(grid_lut)) {
                grid_req = 1;
                Log(LogLevel::Info, "[kms] grid: drawn by the 2D engine as it writes the 4x frame (kms.grid_cpu), no GPU pass\n");
            }
        }
        shader_live.store(shader_on, std::memory_order_relaxed);   // (kms.rgb565: 32-bit frames while a shader draws)
    }
    // the achievements' pop-ups and the volume pill (menu.c's bmenu_overlay) over the top panel's picture as it goes
    // to the screen: over the shader's output when there is one (drawn into the DS frame before the shader, they went
    // through it with the game: the grid over them, the DS's colours, scaled with the picture), else over the frame.
    // The shader's pass has finished here (shader_draw's glFinish with the second panel).
    void overlay(const Buf* b) {
        if (b && b->map && !b->r565) {
            bmenu_overlay((uint32_t*)b->map, (int)(b->pitch / 4), (int)b->w, (int)b->h);
            if (touched) touched(b->map);
        } else if (b && b->map && bmenu_overlay(nullptr, (int)b->w, (int)b->w, (int)b->h)) {
            // kms.rgb565: only when something is to be drawn, the rows it draws into as XRGB8888, drawn over, and
            // packed back (the 2D engine's packing: the top 5/6/5 bits)
            const int rows = bmenu_overlay_rows((int)b->w, (int)b->h);
            static std::vector<uint32_t> t;
            t.resize((size_t)b->w * rows);
            for (int y = 0; y < rows; y++) row_to32(*b, y, &t[(size_t)y * b->w], b->w);
            bmenu_overlay(t.data(), (int)b->w, (int)b->w, rows);
            for (int y = 0; y < rows; y++) {
                uint16_t* d = (uint16_t*)(b->map + (size_t)y * b->pitch);
                const uint32_t* q = &t[(size_t)y * b->w];
                for (uint32_t x = 0; x < b->w; x++)
                    d[x] = (uint16_t)(((q[x] >> 8) & 0xF800) | ((q[x] >> 5) & 0x07E0) | ((q[x] >> 3) & 0x001F));
            }
            if (touched) touched(b->map);
        }
    }
    // kms.commit_thread: the commits made blocking, on a thread of their own at SCHED_FIFO. A nonblocking commit
    // programs the display in a kernel worker (an ordinary-priority kworker): with every core busy at 4x (the Gengis
    // threads, the 2D thread) it ran late, and frames committed up to 5.5 ms before the top panel's vblank were shown
    // a refresh later (Mario Kart DS's demo race at 4x on the RG DS Plus: 78 of ~1370 frames). A blocking commit does
    // that work in the caller's own context (here: real-time), then waits for the flip
    bool cthread_on = false;
    std::thread cthread;
    std::mutex cm;
    std::condition_variable ccv;
    bool creq = false, cquit = false;
    const Buf* cout_[2] = {nullptr, nullptr};
    void committer() {
        sched_param sp{};
        sp.sched_priority = 6;   // (above the presenter's 5, below the sound's 10; it sleeps in the kernel almost always)
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        pthread_setname_np(pthread_self(), "bds-commit");
        for (;;) {
            const Buf* o[2];
            {
                std::unique_lock<std::mutex> l(cm);
                ccv.wait(l, [&] { return creq || cquit; });
                if (cquit) return;
                o[0] = cout_[0];
                o[1] = cout_[1];
                creq = false;
            }
            drmModeAtomicReq* r = drmModeAtomicAlloc();
            for (int i = 0; i < 2; i++) add_plane(r, p[i], *o[i]);
            const int ret = drmModeAtomicCommit(fd, r, DRM_MODE_PAGE_FLIP_EVENT, this);
            drmModeAtomicFree(r);
            if (ret) {   // (the frame back at the head of the queue, as a nonblocking commit's failure leaves it)
                Log(LogLevel::Warn, "[kms] commit failed: %s\n", strerror(errno));
                Lock l(mu);
                if (committed >= 0) {
                    state[committed] = QUEUED;
                    queue.push_front(committed);
                    committed = -1;
                }
                pending = 0;
                wake();
            }
        }
    }
    void cthread_start() {
        cthread_on = true;
        cthread = std::thread([this] { committer(); });
    }
    void cthread_stop() {
        if (!cthread.joinable()) return;
        {
            std::lock_guard<std::mutex> l(cm);
            cquit = true;
        }
        ccv.notify_all();
        cthread.join();
        cthread_on = false;
    }
    // slot s onto both panels, unless a flip is still pending; false when it wasn't committed
    bool commit(int s, const Buf* const out[2]) {
        Lock l(mu);
        if (pending) return false;
        if (cthread_on) {
            {
                std::lock_guard<std::mutex> c(cm);
                cout_[0] = out[0];
                cout_[1] = out[1];
                creq = true;
            }
            ccv.notify_one();
        } else {
            drmModeAtomicReq* r = drmModeAtomicAlloc();
            for (int i = 0; i < 2; i++) add_plane(r, p[i], *out[i]);
            int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, this);
            drmModeAtomicFree(r);
            if (ret) {
                if (errno != EBUSY) Log(LogLevel::Warn, "[kms] commit failed: %s\n", strerror(errno));
                return false;
            }
        }
        committed = s;
        committed_out[0] = out[0];
        committed_out[1] = out[1];
        pending = 2;
        const long long t = now_us();
        commit_us = t;
        commit_aim = t <= aim_us[s] ? aim_us[s] : 0;   // (one after its deadline is a late frame, not a missed commit)
        if (Trace* x = tr(s)) x->commit = t;
        return true;
    }
    // F + L2: the frame as it goes to the panels (its 3D in: right after the emulator's frame it may not be yet), before
    // the shader, to a PNG on a thread of its own (screenshot.h)
    std::atomic<bool> shot{false};
    std::string shot_dir, shot_game;
    void take_shot(const Buf& t, const Buf& b) {
        if (!t.map || !b.map || t.w != b.w || t.h != b.h) return;
        std::vector<uint32_t> px[2];
        const Buf* in[2] = {&t, &b};
        for (int i = 0; i < 2; i++) {
            px[i].resize((size_t)t.w * t.h);
            for (uint32_t y = 0; y < t.h; y++) row_to32(*in[i], y, &px[i][(size_t)y * t.w], t.w);
        }
        screenshot_save(shot_dir, shot_game, std::move(px[0]), std::move(px[1]), (int)t.w, (int)t.h, [](bool ok, const std::string& name) {
            Log(LogLevel::Info, "[shot] %s: %s\n", ok ? "saved" : "couldn't save", name.c_str());
            bmenu_popup(ok ? "Screenshot saved" : "Screenshot not saved", ok ? name.c_str() : "the card may be full", 2500);
        });
    }
    void commit_next() {
        shader_settings();
        if (ready >= 0) {  // drawn ahead: on screen as soon as the flip allows (tried again at the next wake-up)
            if (ll_early(ready)) return;   // (kms.lowlat: not before its vblank's window)
            if (!commit(ready, ready_out)) return;
            ready = -1;
        }
        int s;
        {
            Lock l(mu);
            // without a shader a frame is only taken when it can be committed at once; with one, it is drawn ahead
            if (queue.empty() || (pending && !(shader_on && !gridded[queue.front()] && !is16[queue.front()]))) return;
            // kms.lowlat: a frame goes no sooner than the vblank before the one it aims at has passed: shown a refresh
            // early (it was ready early), the next frame, on time for its own vblank, left that one empty (a repeat
            // after every early frame: the most of v4's 180-280 a minute on the RG DS). The presenter wakes then.
            // With a shader only the commit waits: the frame is drawn through it ahead (10-14 ms a frame on the RG DS
            // Plus: started at the window, it missed the deadline 17 times a minute)
            if (!(shader_on && !gridded[queue.front()] && !is16[queue.front()]) && ll_early(queue.front())) return;
            // the oldest frame, taken off the queue so the emulator can't recycle it while the shader draws it (outside
            // the lock: the emulator's thread never waits for the GPU)
            s = queue.front();
            queue.pop_front();
            state[s] = COMMITTED;
            if (lowlat) ll_take(s);
            else if (Trace* x = tr(s)) x->take = now_us();   // (kms.lowlat_trace without kms.lowlat too)
        }
        if (has_fence[s] && wait2d) wait2d(fence[s]);  // render.2d_lazy: its last lines drawn
        if (!lowlat) { Lock l(mu); if (Trace* x = tr(s)) x->ready = now_us(); }
        // (kms.grid_cpu: a frame with the grid in goes as it is; kms.rgb565: an RGB565 frame drawn before the shader
        // came on too)
        const bool pass = shader_on && !gridded[s] && !is16[s];
        if (!pass) ll_ready(s);
        if (shot.exchange(false)) take_shot(*slot(0, s), *slot(1, s));
        const Buf* out[2] = {slot(0, s), slot(1, s)};
        if (gridded[s] && !gpu_dir.empty()) gpu_floor(false);   // (no GPU pass: its clock floor put back)
        if (pass) {
            const long long t0 = now_us();
            for (int i = 0; i < 2; i++) {
                const Buf& a = buf[i][s];
                Buf& d = sbuf[i][snext];
                if (shader_draw(a.handle, a.w, a.h, a.pitch, a.fb, d.handle, d.w, d.h, d.pitch, d.fb, i == 1) == 0) out[i] = &d;
            }
            const long long dt = now_us() - t0;
            shader_us += dt;
            shader_n++;
            if (dt > shader_max_us) shader_max_us = dt;
            snext = (snext + 1) % 3;  // (one on screen, one committed, one drawn ahead)
            // kms.gpu_floor_mhz auto: with ds-grid-2x's fast twin the pass is ~5 ms a frame at 400 MHz, ~31% busy,
            // under panfrost's 45% up-threshold: the GPU stays at 400 instead of 600 (measured on the RG DS Plus)
            if (!gpu_floor_low && gpu_floor_mhz < 0 && shader_fast_on() && !gpu_dir.empty()) {
                gpu_floor_low = true;
                std::ofstream(gpu_dir + "/min_freq") << gpu_min_was;   // (put back first: the floor goes down)
                gpu_dir.clear();
                gpu_floor(true);
            }
            ll_ready(s);
            if (swapped) std::swap(out[0], out[1]);  // (F + SELECT: each DS screen on the other panel)
            overlay(out[0]);
            ready = s;
            ready_out[0] = out[0];
            ready_out[1] = out[1];
            if (!ll_early(s) && commit(s, out)) ready = -1;  // the flip had already come
            return;
        }
        if (swapped) std::swap(out[0], out[1]);
        overlay(out[0]);
        if (!commit(s, out)) {
            Lock l(mu);
            state[s] = QUEUED;  // back at the head of the queue: tried again at the next wake-up
            queue.push_front(s);
        }
    }
    // with a shader, the GPU's lowest clock raised (its on-demand governor ramps too slowly for a pass every
    // refresh: frames missed their vblank), and put back at the end
    std::string gpu_dir;
    std::string gpu_min_was;
    long gpu_floor_mhz = -1;   // kms.gpu_floor_mhz; -1 (auto): 600, or 400 once a fast twin draws
    bool gpu_floor_low = false;
    long long shader_us = 0, shader_max_us = 0;
    long shader_n = 0;
    void gpu_floor(bool on) {
        if (!on && !gpu_dir.empty() && !gpu_min_was.empty()) {
            std::ofstream(gpu_dir + "/min_freq") << gpu_min_was;
            gpu_dir.clear();
            return;
        }
        if (on) {
            glob_t g;
            if (glob("/sys/class/devfreq/*gpu*", 0, nullptr, &g) == 0 && g.gl_pathc) gpu_dir = g.gl_pathv[0];
            globfree(&g);
            if (gpu_dir.empty()) return;
            std::ifstream f(gpu_dir + "/min_freq");
            std::getline(f, gpu_min_was);
            std::ifstream a(gpu_dir + "/available_frequencies");
            std::vector<long> fr;
            for (long v; a >> v;) fr.push_back(v);
            std::sort(fr.begin(), fr.end());
            long want = 0;  // kms.gpu_floor_mhz (auto: 600, 400 with a fast twin), or the highest under it
            const long mhz = gpu_floor_mhz >= 0 ? gpu_floor_mhz : gpu_floor_low ? 400 : 600;
            for (long v : fr) if (v <= mhz * 1000000L) want = v;
            if (want) std::ofstream(gpu_dir + "/min_freq") << want;
            Log(LogLevel::Info, "[kms] GPU at least %ld MHz while the shader runs\n", want / 1000000);
        } else if (!gpu_dir.empty() && !gpu_min_was.empty()) {
            std::ofstream(gpu_dir + "/min_freq") << gpu_min_was;
        }
    }
    // kms.lowlat: is slot s's frame too early for the panels (before the vblank ahead of the one it aims at)? Then the
    // presenter wakes when its window opens
    bool ll_early(int s) {
        if (!lowlat || !vbl_known() || !aim_us[s]) return false;
        const long long earliest = aim_us[s] - (long long)period_us + (long long)margin_us + 500;
        if (now_us() >= earliest) return false;
        wake_at = earliest;
        return true;
    }
    // kms.lowlat (mu held): the presenter takes slot s's frame off the queue (to draw it through the shader, or to
    // commit it): how far before its deadline that is
    void ll_take(int s) {
        const long long now = now_us(), a = aim_us[s];
        take_us[s] = now;
        if (Trace* x = tr(s)) x->take = now;
        if (!a) return;                  // not aimed (the first frames, fast-forward)
        if (now <= a) { ll_lead_sum += (double)(a - now); ll_n++; }
        else ll_late++;
    }
    // kms.lowlat: slot s's frame is ready for the panels (its last lines drawn, through the shader): the presenter's
    // time for it (taken -> ready) goes into pres_us, once
    void ll_ready(int s) {
        if (!lowlat) return;
        Lock l(mu);
        if (!take_us[s]) return;
        const long long now = now_us(), w = now - take_us[s];
        if (w >= 0 && w < 2 * (long long)period_us) decay_max(pres_us, w);
        take_us[s] = 0;
        if (Trace* x = tr(s)) x->ready = now;
    }
    void flipped(long long t, long long kt = 0, unsigned crtc = 0) {
        Lock l(mu);
        if (lowlat && kt) {              // the panels' vblanks, for kms.lowlat's deadlines
            if (crtc == p[0].crtc) {
                if (vbl_top) {           // the refresh period, measured (the mode's is the nominal one)
                    const double iv = (double)(kt - vbl_top), k = std::floor(iv / period_us + 0.5);
                    if (k >= 1 && k <= 4 && std::fabs(iv / k - period_us) < 300) period_us += 0.02 * (iv / k - period_us);
                }
                vbl_top = kt;
            } else vbl_bot = kt;
            if (!first_ev || kt < first_ev) first_ev = kt;
            if (pending == 1 && vbl_top && vbl_bot) {   // (the pair's second event)
                const double d = std::remainder((double)(vbl_bot - vbl_top), period_us);
                bot_off = nflips < 10 ? d : bot_off + 0.05 * (d - bot_off);
                nflips++;
                if (commit_aim) {        // did the commit make the vblank its deadline was for?
                    if ((double)(first_ev - commit_aim) > margin_us + period_us / 2) {
                        ll_miss++;
                        margin_us = std::min(5000.0, margin_us + 300);
                    }
                    commit_aim = 0;
                }
                if (committed >= 0) {
                    shown_vbl = vbl_top;
                    if (Trace* x = tr(committed)) x->flip = first_ev;
                    if (const long long a = aim_us[committed]) {   // shown after the vblank it aimed at?
                        const double late = (double)(first_ev - a) - margin_us;
                        const long k = late > period_us / 2 ? std::lround(late / period_us) : 0;
                        const long more = std::min(3L, k - (slip_total - slip_at[committed]));   // (not known when aimed)
                        if (more > 0) {
                            slip += (int)more;
                            slip_total += more;
                            extra_us.store(std::min(16000LL, extra_us.load() + 2000));
                        }
                        if (k > 0) ll_behind++;
                    }
                }
                first_ev = 0;
            }
        }
        if (kt && committed >= 0)   // (kms.lowlat_trace: each panel's own flip)
            if (Trace* x = tr(committed)) (crtc == p[0].crtc ? x->ftop : x->fbot) = kt;
        if (--pending > 0) return;
        if (!lowlat && committed >= 0)
            if (Trace* x = tr(committed)) x->flip = kt ? std::min(kt, first_ev ? first_ev : kt) : t;
        if (!first_flip) { first_flip = true; start_mark("first flip"); }
        if (scanout >= 0) state[scanout] = FREE;
        scanout = committed;
        scanout_out[0] = committed_out[0];
        scanout_out[1] = committed_out[1];
        state[scanout] = SCANOUT;
        committed = -1;
        shown++;
        if (last_flip) {
            long gap = std::lround((t - last_flip) / period_us);
            if (gap > 1 && gap < 120) repeats += gap - 1;  // refreshes that showed no new frame (not a sleep)
        }
        last_flip = t;
    }
};

static void on_flip(int, unsigned, unsigned sec, unsigned usec, unsigned crtc, void* ud) {
    ((Display*)ud)->flipped(now_us(), sec * 1000000LL + usec, crtc);
}

// ---- input ----------------------------------------------------------------------------------------------------

struct InputDev {
    int fd = -1;
    bool touch = false, pointer = false;
    int panel = 1;          // a touchscreen's panel: 1 the bottom one (kms.touch), 0 the top one (the RG DS has one)
    int stick = 0;          // the left stick's half range (ABS_X/ABS_Y on a pad, not a touchscreen): 0 none
    int minx = 0, maxx = 0, miny = 0, maxy = 0, x = 0, y = 0;
    bool down = false, was = false;
};

static bool bit(const unsigned long* a, int n) { return a[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1; }

static uint32_t pad_bit(int code) {
    switch (code) {
        case BTN_EAST: case KEY_ENTER: return KeyA;          // the RG DS's A is east (Nintendo layout)
        case BTN_SOUTH: case KEY_BACKSPACE: return KeyB;
        case BTN_NORTH: case KEY_X: return KeyX;
        case BTN_WEST: case KEY_Y: return KeyY;
        case BTN_TL: case KEY_PAGEUP: return KeyL;
        case BTN_TR: case KEY_PAGEDOWN: return KeyR;
        case BTN_START: case KEY_S: return KeyStart;
        case BTN_SELECT: case KEY_TAB: return KeySelect;
        case BTN_DPAD_UP: case KEY_UP: return KeyUp;
        case BTN_DPAD_DOWN: case KEY_DOWN: return KeyDown;
        case BTN_DPAD_LEFT: case KEY_LEFT: return KeyLeft;
        case BTN_DPAD_RIGHT: case KEY_RIGHT: return KeyRight;
        default: return 0;
    }
}
static bool is_hotkey(int code) { return code == BTN_MODE || code == KEY_F1; }

static std::vector<InputDev> open_inputs(const std::string& touch_name) {
    std::vector<InputDev> devs;
    DIR* d = opendir("/dev/input");
    if (!d) return devs;
    while (dirent* e = readdir(d)) {
        if (strncmp(e->d_name, "event", 5)) continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {}, abs[ABS_MAX / (8 * sizeof(long)) + 1] = {},
                      sw[SW_MAX / (8 * sizeof(long)) + 1] = {};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw);
        InputDev dv;
        dv.fd = fd;
        bool mt = bit(abs, ABS_MT_POSITION_X) && bit(abs, ABS_MT_POSITION_Y);
        bool st = !mt && bit(abs, ABS_X) && bit(abs, ABS_Y) &&
                  (bit(keys, BTN_TOUCH) || (bit(keys, BTN_LEFT) && !bit(keys, BTN_SOUTH)));
        if (mt || st) {
            // the bottom panel's touchscreen is the DS's; the top one's (the RG DS's) only while the screens are swapped
            char link[300], real[PATH_MAX];
            snprintf(link, sizeof link, "/sys/class/input/%s/device", e->d_name);
            if (touch_name.empty() || !realpath(link, real)) {
                ::close(fd);
                continue;
            }
            dv.panel = strstr(real, touch_name.c_str()) ? 1 : 0;
            dv.touch = true;
            dv.pointer = st && !bit(keys, BTN_TOUCH);
            input_absinfo ax{}, ay{};
            ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ax);
            ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ay);
            dv.minx = ax.minimum;
            dv.maxx = ax.maximum;
            dv.miny = ay.minimum;
            dv.maxy = ay.maximum;
        } else {
            // the left stick (a pad's ABS_X/ABS_Y) plays the d-pad; the volume keys are only looked at so F + volume
            // (bananad's brightness) isn't taken for F alone, which opens the menu
            // (and the lid's hall sensor, SW_LID: the DS's lid; HOME, BTN_Z on an adc-keys device of its own: the menu)
            bool any = bit(abs, ABS_HAT0X) || bit(keys, KEY_VOLUMEUP) || bit(sw, SW_LID) || bit(keys, BTN_Z);
            if (bit(abs, ABS_X) && bit(abs, ABS_Y)) {
                input_absinfo ax{};
                ioctl(fd, EVIOCGABS(ABS_X), &ax);
                dv.stick = (ax.maximum - ax.minimum) / 2;
                any = any || dv.stick > 0;
            }
            for (int c = KEY_ESC; c < KEY_MAX && !any; c++)
                if (bit(keys, c) && (pad_bit(c) || is_hotkey(c))) any = true;
            if (!any) {
                ::close(fd);
                continue;
            }
        }
        Log(LogLevel::Info, "[kms] input %s: %s\n", e->d_name, dv.touch ? (dv.panel ? "the touchscreen" : "the top panel's touchscreen") : "buttons");
        devs.push_back(dv);
    }
    closedir(d);
    return devs;
}

// ---- F + SELECT: the DS's screens on the other panels, remembered for each game in <BANANADS_DATA>/swapped.txt (one
// game's file name a line) ----
static std::string swap_file() {
    const char* data = getenv("BANANADS_DATA");
    return std::string(data && *data ? data : "/data/bananads") + "/swapped.txt";
}
static std::string rom_base(const std::string& rom) { return rom.substr(rom.rfind('/') + 1); }
static bool swap_remembered(const std::string& rom) {
    std::ifstream f(swap_file());
    std::string ln, me = rom_base(rom);
    while (std::getline(f, ln)) if (!me.empty() && ln == me) return true;
    return false;
}
static void swap_toggle(Display& dp, const std::string& rom) {
    const bool on = !dp.swapped;
    dp.swapped = on;
    const std::string path = swap_file(), me = rom_base(rom);
    std::vector<std::string> keep;
    {
        std::ifstream f(path);
        std::string ln;
        while (std::getline(f, ln)) if (!ln.empty() && ln != me) keep.push_back(ln);
    }
    if (on && !me.empty()) keep.push_back(me);
    if (FILE* w = fopen((path + ".new").c_str(), "w")) {
        for (auto& k : keep) fprintf(w, "%s\n", k.c_str());
        fclose(w);
        rename((path + ".new").c_str(), path.c_str());
    }
    Log(LogLevel::Info, "[kms] F + SELECT: screens %s\n", on ? "swapped" : "back as they were");
    bmenu_popup(on ? "Screens swapped" : "Screens back", on ? "F + SELECT puts them back" : "F + SELECT swaps them again", 1800);
}

// ---- the in-game menu's host (menu.h): the panels, the input, the emulator, while the game is paused -----------

struct MenuHost {
    Display* dp = nullptr;
    Emu* emu = nullptr;
    std::vector<InputDev>* devs = nullptr;
    Alsa* snd = nullptr;
    std::string rom;
    std::deque<bmenu_ev> q;
    bool quit = false;
    bool nosave = false;  // the quit is Quit without saving
};

static int menu_key(int code) {
    switch (code) {
        case BTN_EAST: case KEY_ENTER: return BM_A;
        case BTN_SOUTH: case KEY_BACKSPACE: return BM_B;
        case BTN_WEST: case KEY_Y: return BM_Y;
        case BTN_NORTH: case KEY_X: return BM_X;
        case BTN_TL: case KEY_PAGEUP: return BM_L;
        case BTN_TR: case KEY_PAGEDOWN: return BM_R;
        case BTN_START: case KEY_S: return BM_START;
        case BTN_DPAD_UP: case KEY_UP: return BM_UP;
        case BTN_DPAD_DOWN: case KEY_DOWN: return BM_DOWN;
        case BTN_DPAD_LEFT: case KEY_LEFT: return BM_LEFT;
        case BTN_DPAD_RIGHT: case KEY_RIGHT: return BM_RIGHT;
        case BTN_THUMBL: case KEY_F2: case KEY_ESC: return BM_MENU;
        default: return -1;
    }
}
static bool is_menu_button(int code) { return code == BTN_THUMBL || code == KEY_F2; }
// the menu's Hotkeys page: the buttons the input loop below reads (keep the two together), and bananad's volume keys
// kms.input: scripted presses by emulated frame (a benchmark's input that is the same at every speed and scale, so
// two runs emulate the same frames: their sound compares bit for bit). headless.input's syntax: "<frame>:<keys>[:<frames
// held>]" separated by commas (or newlines), keys joined by '+' (a b select start right left up down r l x y), held
// 6 frames unless given; frame 0 = the first frame emulated (after the resume state's load). "save1".."save3" as a
// key saves the state to that slot at the frame's start. "@<file>" reads the list from a file. OR-ed with the pad's.
struct KmsPress { long at, len; uint32_t keys; int save; };
static std::vector<KmsPress> kms_parse_input(std::string spec) {
    static const char* names[12] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
    std::vector<KmsPress> out;
    if (!spec.empty() && spec[0] == '@') {
        std::ifstream f(spec.substr(1));
        if (!f) { Log(LogLevel::Error, "[kms] kms.input: can't read %s\n", spec.c_str() + 1); return out; }
        std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        spec = all;
    }
    for (char& c : spec) if (c == '\n' || c == '\r' || c == ' ') c = ',';
    size_t i = 0;
    while (i < spec.size()) {
        size_t e = spec.find(',', i);
        std::string item = spec.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? spec.size() : e + 1;
        size_t c1 = item.find(':');
        if (c1 == std::string::npos) continue;
        size_t c2 = item.find(':', c1 + 1);
        KmsPress p{atol(item.c_str()), c2 == std::string::npos ? 6 : atol(item.c_str() + c2 + 1), 0, 0};
        std::string keys = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        size_t k = 0;
        while (k <= keys.size()) {
            size_t plus = keys.find('+', k);
            std::string name = keys.substr(k, plus == std::string::npos ? std::string::npos : plus - k);
            for (int b = 0; b < 12; b++)
                if (name == names[b]) p.keys |= 1u << b;
            if (name.size() == 5 && name.compare(0, 4, "save") == 0 && name[4] >= '1' && name[4] <= '3') p.save = name[4] - '0';
            if (plus == std::string::npos) break;
            k = plus + 1;
        }
        out.push_back(p);
    }
    return out;
}

static bool g_quit_saves = true;  // resume.on_quit: F + START saves the spot
static int mh_hotkey(void*, int i, const char** keys, const char** what) {
    static const char* const k[][2] = {
        {"F / L3 / HOME", "This menu"}, {"F + R", "Save state (slot 1)"}, {"F + L", "Load state (slot 1)"},
        {"R2", "Fast-forward (held)"}, {"F + X", "Fast-forward on / off"}, {"F + L2", "Screenshot"},
        {"F + SELECT", "Swap the screens"}, {"F + START", nullptr}, {"VOL + / -", "Volume"}, {"F + VOL + / -", "Brightness"}};
    if (i < 0 || i >= (int)(sizeof k / sizeof k[0])) return 0;
    *keys = k[i][0];
    *what = k[i][1] ? k[i][1] : g_quit_saves ? "Save your spot and quit" : "Quit";
    return 1;
}

static int mh_canvas(void* c, int panel, uint32_t** px, int* pitch, int* w, int* h) {
    Display* dp = ((MenuHost*)c)->dp;
    if (panel < 0 || panel > 1 || !dp->menu_bufs()) return -1;
    const int k = dp->mnext[panel];
    *px = (uint32_t*)dp->mbuf[panel][k].map;
    *pitch = (int)dp->mbuf[panel][k].pitch;
    *w = dp->W;
    *h = dp->H;
    return k;
}
static void mh_show(void* c, int panel, int k) {
    Display* dp = ((MenuHost*)c)->dp;
    dp->show_menu(panel, k);
    dp->mnext[panel] = k ^ 1;  // the next picture goes into the other one
}
static int mh_grab_top(void* c, uint32_t* dst, int pitch, int w, int h) { return ((MenuHost*)c)->dp->grab_top(dst, pitch, w, h); }
static int mh_poll(void* c, bmenu_ev* out) {
    MenuHost* m = (MenuHost*)c;
    if (m->q.empty()) {
        if (g_quit) m->q.push_back({BM_EV_KEY, BM_START, 1, 0, 0});  // shutting down: the menu closes, the game saves
        for (auto& d : *m->devs) {
            input_event e[64];
            ssize_t n;
            while ((n = read(d.fd, e, sizeof e)) > 0)
                for (int j = 0; j < n / (int)sizeof(input_event); j++) {
                    const input_event& x = e[j];
                    if (d.touch) {
                        if (x.type == EV_ABS && (x.code == ABS_MT_POSITION_X || x.code == ABS_X)) d.x = x.value;
                        else if (x.type == EV_ABS && (x.code == ABS_MT_POSITION_Y || x.code == ABS_Y)) d.y = x.value;
                        else if (x.type == EV_ABS && x.code == ABS_MT_TRACKING_ID && x.value < 0) d.down = false;
                        else if (x.type == EV_KEY && (x.code == BTN_TOUCH || (d.pointer && x.code == BTN_LEFT)))
                            d.down = x.value != 0;
                        else if (x.type == EV_SYN && x.code == SYN_REPORT) {
                            if (d.down && !d.was && d.panel == 1) {  // (the menu is on the bottom panel)
                                const Display* dp = m->dp;
                                int px = d.maxx > d.minx ? (int)((double)(d.x - d.minx) * dp->W / (d.maxx - d.minx + 1)) : d.x;
                                int py = d.maxy > d.miny ? (int)((double)(d.y - d.miny) * dp->H / (d.maxy - d.miny + 1)) : d.y;
                                m->q.push_back({BM_EV_TOUCH, 0, 1, px, py});
                            }
                            d.was = d.down;
                        }
                        continue;
                    }
                    if (x.type == EV_ABS && (x.code == ABS_HAT0X || x.code == ABS_HAT0Y)) {
                        const int neg = x.code == ABS_HAT0X ? BM_LEFT : BM_UP, pos = x.code == ABS_HAT0X ? BM_RIGHT : BM_DOWN;
                        if (x.value) m->q.push_back({BM_EV_KEY, x.value < 0 ? neg : pos, 1, 0, 0});
                        else {
                            m->q.push_back({BM_EV_KEY, neg, 0, 0, 0});
                            m->q.push_back({BM_EV_KEY, pos, 0, 0, 0});
                        }
                        continue;
                    }
                    if (x.type != EV_KEY || x.value == 2) continue;
                    const int k = menu_key(x.code);
                    if (k >= 0) m->q.push_back({BM_EV_KEY, k, x.value ? 1 : 0, 0, 0});
                }
        }
    }
    if (m->q.empty()) return 0;
    *out = m->q.front();
    m->q.pop_front();
    return 1;
}
static int mh_battery(void*, int* charging) {
    int pct = -1;
    if (FILE* f = fopen("/sys/class/power_supply/battery/capacity", "r")) {
        if (fscanf(f, "%d", &pct) != 1) pct = -1;
        fclose(f);
    }
    char st[32] = "";
    if (FILE* f = fopen("/sys/class/power_supply/battery/status", "r")) {
        if (!fgets(st, sizeof st, f)) st[0] = 0;
        fclose(f);
    }
    *charging = !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4);
    // a charger plugged in counts as charging, as in BananaOS's menu: the rk817's gauge can say "Discharging" while it
    // charges (the menu's bolt was missing then, #35). Any supply that isn't the battery and is online, as banana-ui's
    // Library::power() looks: the charger isn't always called "charger"
    if (DIR* d = opendir("/sys/class/power_supply")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char p[300], t[32] = "";
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
            if (FILE* f = fopen(p, "r")) { if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f); }
            if (!strncmp(t, "Battery", 7)) continue;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            if (FILE* f = fopen(p, "r")) {
                int on = 0;
                if (fscanf(f, "%d", &on) == 1 && on) *charging = 1;
                fclose(f);
            }
        }
        closedir(d);
    }
    return pct;
}
static int mh_save(void* c, int slot) {
    cpugov_boost(3000);
    return ((MenuHost*)c)->emu->save_state(slot) ? 0 : -1;
}
static int mh_load(void* c, int slot) {
    cpugov_boost(3000);
    return ((MenuHost*)c)->emu->load_state(slot) ? 0 : -1;
}
static void mh_state_file(void* c, int slot, char* out, int n) {
    snprintf(out, n, "%s", ((MenuHost*)c)->emu->state_path(slot).c_str());
}
static void mh_quit(void* c) { ((MenuHost*)c)->quit = true; }
static void mh_quit_nosave(void* c) {
    ((MenuHost*)c)->quit = true;
    ((MenuHost*)c)->nosave = true;
}
static void mh_restart(void* c) {
    cpugov_boost(2000);
    ((MenuHost*)c)->emu->restart();
    ra_reset();
}
static void mh_blow(void* c, int ms) { ((MenuHost*)c)->emu->mic().blow(ms); }
static int mh_mic_meter(void* c, float* level, float* floor_, float* bar) {
    const Mic::Meter m = ((MenuHost*)c)->emu->mic().meter();
    *level = m.level;
    *floor_ = m.floor;
    *bar = m.bar > m.floor ? m.bar - m.floor : 0;
    return m.level > 0 || m.floor > 0 ? 1 : 0;
}
static void mh_sound_load(void* c, int id, const int16_t* pcm, int frames, int rate, int ch) {
#ifdef BANANADS_ALSA
    alsa_ui_load(((MenuHost*)c)->snd, id, pcm, frames, rate, ch);
#else
    (void)c, (void)id, (void)pcm, (void)frames, (void)rate, (void)ch;
#endif
}
static void mh_sound_play(void* c, int id) {
#ifdef BANANADS_ALSA
    alsa_ui_play(((MenuHost*)c)->snd, id);
#else
    (void)c, (void)id;
#endif
}
static void mh_boost(void*, int ms) { cpugov_boost(ms); }

// ---- the emulator's settings on the menu's Quick settings page ----
// a "key = value" (or "KEY=value") line in a settings file, replaced or added: the choice stays for the next game
static void set_kv(const std::string& path, const std::string& key, const std::string& value, const char* sep) {
    std::ifstream f(path);
    std::string ln, out;
    bool done = false;
    while (std::getline(f, ln)) {
        size_t eq = ln.find('=');
        std::string k = eq == std::string::npos ? "" : ln.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        size_t b = k.find_first_not_of(" \t");
        if (b != std::string::npos) k = k.substr(b);
        if (!ln.empty() && ln[0] != '#' && k == key) {
            if (done) continue;
            out += key + sep + value + "\n";
            done = true;
        } else out += ln + "\n";
    }
    if (!done) out += key + sep + value + "\n";
    std::ofstream(path) << out;
}
static const char* shader_label(const std::string& n) {
    static const std::pair<const char*, const char*> names[] = {
        {"none", "None"}, {"ds-crisp", "Crisp"}, {"ds-crisp-color", "Crisp + DS colours"}, {"ds-grid", "LCD grid"},
        {"ds-grid-color", "LCD grid + colours"}, {"ds-grid-2x", "LCD grid 2x"}, {"ds-fsr", "FSR"}, {"lcd1x-nds", "LCD (DS colours)"},
        {"lcd1x", "LCD 1x"}, {"lcd3x", "LCD 3x"}, {"zfast-lcd", "Fast LCD"}, {"nds-color", "DS colours"}, {"retro-v2", "Retro"},
        {"bevel", "Bevel"}, {"sharp-bilinear", "Sharp bilinear"}, {"sharp-bilinear-scanlines", "Sharp + scanlines"},
        {"zfast-crt", "Fast CRT"}, {"fxaa", "FXAA"}, {"ds-integer", "Integer"}, {"ds-integer-fill", "Integer fill"},
        {"nnddss-dot-mizai", "Dot (MiZai)"}, {"nnddss-dot-mizai-rgds", "Dot (MiZai, RG DS+)"}, {"nnddss-retro-tiles", "Retro tiles"},
        {"drastic-lcd1x", "DraStic LCD"}, {"drastic-lcd1x-nds-color", "DraStic LCD + colours"}, {"drastic-quilez", "DraStic Quilez"},
        {"drastic-sharp-bilinear", "DraStic sharp"}, {"drastic-sharp-bilinear-nds-color", "DraStic sharp + colours"},
        {"drastic-zfast-lcd", "DraStic fast LCD"}, {"drastic-zfast-lcd-nds-color", "DraStic fast LCD + colours"}};
    for (auto& p : names) if (n == p.first) return p.second;
    return n.c_str();
}
struct MenuOpts {
    std::vector<std::string> shaders;  // "none" first, then the shader folder's
    int shader = 0;
    bool edge = true, fog = true;
    bool fps = false;  // kms.fps: the FPS counter
    int ff_speed = 2;  // kms.ff_speed: fast-forward's speed when the hotkey turns it on (2, 3 or 99, Max)
};
static MenuOpts g_opts;
static void opts_init(const Config& cfg, Display& dp) {
    MenuOpts& o = g_opts;
    o.shaders = {"none"};
    const char* dir = getenv("DSFLIP_SHADER_DIR");
    std::string d = dir && *dir ? dir : "/usr/lib/superdrastic/shaders";
    glob_t g;
    if (glob((d + "/*.frag").c_str(), 0, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; i++) {
            std::string f = g.gl_pathv[i];
            f = f.substr(f.rfind('/') + 1);
            o.shaders.push_back(f.substr(0, f.size() - 5));
        }
    globfree(&g);
    o.shader = 0;
    for (size_t i = 0; i < o.shaders.size(); i++) if (o.shaders[i] == dp.shader_name) o.shader = (int)i;
    o.edge = cfg.flag("render.edge_marking", true);
    o.fog = cfg.flag("render.fog", true);
    o.fps = cfg.flag("kms.fps", false);
    const long ff = cfg.num("kms.ff_speed", 2);
    o.ff_speed = ff == 3 ? 3 : ff > 3 ? 99 : 2;
}
static const int ff_speeds[4] = {0, 2, 3, 99};  // the menu's Fast-forward values: Off, 2x, 3x, Max
static void save_opt(const char* key, const std::string& v) {
    const char* data = getenv("BANANADS_DATA");
    set_kv(std::string(data && *data ? data : "/data/bananads") + "/bananads.conf", key, v, " = ");
}
// F + X: fast-forward on at the speed last chosen in the menu, or off
static void ff_toggle() { g_ff = g_ff.load() ? 0 : g_opts.ff_speed; }
// the rows: Shader, 3D effects (edge marking and fog, one row since the Cheats row came), FPS counter, Fast-forward
static int mh_opt_count(void*) { return 4; }
static const char* mh_opt_label(void*, int i) {
    static const char* const l[4] = {"Shader", "3D effects", "FPS counter", "Fast-forward"};
    return i >= 0 && i < 4 ? l[i] : "?";
}
static int mh_opt_values(void*, int i) { return i == 0 ? (int)g_opts.shaders.size() : i == 2 ? 2 : 4; }
static const char* mh_opt_value_name(void*, int i, int v) {
    if (i == 0) return v >= 0 && v < (int)g_opts.shaders.size() ? shader_label(g_opts.shaders[v]) : "?";
    if (i == 1) return v == 0 ? "Edge + fog" : v == 1 ? "Edge only" : v == 2 ? "Fog only" : "Off";
    if (i == 3) return v == 1 ? "2x" : v == 2 ? "3x" : v == 3 ? "Max" : "Off";
    return v ? "On" : "Off";
}
static int mh_opt_get(void*, int i) {
    if (i == 3) {
        const int f = g_ff.load();
        return f == 2 ? 1 : f == 3 ? 2 : f ? 3 : 0;
    }
    if (i == 1) return g_opts.edge ? (g_opts.fog ? 0 : 1) : (g_opts.fog ? 2 : 3);
    return i == 0 ? g_opts.shader : g_opts.fps;
}
static void mh_opt_set(void* c, int i, int v) {
    MenuHost* h = (MenuHost*)c;
    if (i == 0) {
        if (v < 0 || v >= (int)g_opts.shaders.size()) return;
        g_opts.shader = v;
        {
            Lock l(h->dp->mu);
            h->dp->shader_req = g_opts.shaders[v];
            h->dp->shader_req_set = true;
        }
        // the same choice as Settings > Nintendo DS > Shader (both emulators read it from there)
        set_kv("/data/banana/superdrastic.conf", "DSFLIP_SHADER", g_opts.shaders[v], "=");
    } else if (i == 2) {
        g_opts.fps = v != 0;
        save_opt("kms.fps", v ? "on" : "off");
    } else if (i == 3) {
        if (v < 0 || v > 3) return;
        g_ff = ff_speeds[v];  // (on for this game only; the speed is kept for the hotkey)
        if (v) {
            g_opts.ff_speed = ff_speeds[v];
            save_opt("kms.ff_speed", v == 3 ? "max" : std::to_string(ff_speeds[v]));
        }
    } else if (i == 1) {
        if (v < 0 || v > 3) return;
        g_opts.edge = v == 0 || v == 1;
        g_opts.fog = v == 0 || v == 2;
#ifdef BANANADS_GENGIS
        GengisRenderer3D::SetEffects(g_opts.edge, g_opts.fog);
#endif
        save_opt("render.edge_marking", g_opts.edge ? "on" : "off");
        save_opt("render.fog", g_opts.fog ? "on" : "off");
    }
}
// the game's cheats (cheats.h): the menu changes them while the game is paused, then melonDS gets the new list
static Cheats g_cheats;
static Emu* g_cheat_emu;
static int mh_cheat_count(void*) { return g_cheats.count(); }
static const char* mh_cheat_name(void*, int i) { return i >= 0 && i < g_cheats.count() ? g_cheats.name(i).c_str() : ""; }
static int mh_cheat_get(void*, int i) { return i >= 0 && i < g_cheats.count() && g_cheats.on(i); }
static void mh_cheat_set(void*, int i, int on) {
    g_cheats.set(i, on != 0);
    if (g_cheat_emu) g_cheat_emu->set_cheats(g_cheats.list());
    Log(LogLevel::Info, "[cheats] %s: %s\n", mh_cheat_name(nullptr, i), on ? "on" : "off");
}
static void mh_log(void*, const char* line) { Log(LogLevel::Info, "%s", line); }

// ---- the frontend ---------------------------------------------------------------------------------------------

int run_kms(Emu& emu, const Config& cfg) {
    const char* et = getenv("BANANADS_TOP");
    const char* eh = getenv("BANANADS_TOUCH");
    const std::string top = cfg.str("kms.top", et && *et ? et : "DSI-2");
    const std::string touch = cfg.str("kms.touch", eh && *eh ? eh : "fe5e0000.i2c");
    // kms.lowlat (experimental, off by default): on = each frame aimed at a vblank, started as late as that allows (see
    // Display); runahead1 / runahead2 = that and run-ahead (Emu::run_ahead: the game answers a press one or two frames
    // sooner than its own lag), also the setting `runahead` = 1 or 2 alone. The queue holds two: the frames are timed
    // so one at most waits, but a frame started before the last is on screen (a game slower than a refresh) mustn't
    // replace it
    const std::string ll = cfg.str("kms.lowlat", "off");
    const bool lowlat = ll != "off";
    const int ra_n = (int)std::clamp(cfg.num("runahead", ll == "runahead1" ? 1 : ll == "runahead2" ? 2 : 0), 0L, 2L);
    const int depth = lowlat ? 3 : std::clamp((int)cfg.num("kms.queue", 2), 1, 3);
    const long every = cfg.num("kms.stats", 0);

    // the cheats (the card's .mch, else RetroArch's .cht for the game: a scan of the ~4200 files of the collection and
    // a parse) read on a thread of their own while the panels open: ~50 ms on the game's way to its first picture
    // otherwise. Taken (joined) where the menu gets them, before the emulator's thread starts.
    std::thread cheats_load([&emu, &cfg] {
        const char* data = getenv("BANANADS_DATA");
        g_cheats.load(emu.game(), cfg.str("cheats.dir", std::string(data && *data ? data : "/data/bananads") + "/cheats"),
                      {cfg.str("cheats.retroarch_dir", "/data/retroarch/retroarch/cheats/Nintendo - Nintendo DS"),
                       cfg.str("cheats.system_dir", "/usr/share/libretro/cheats/Nintendo - Nintendo DS")});
    });
    struct CheatsJoin {
        std::thread& t;
        ~CheatsJoin() { if (t.joinable()) t.join(); }  // (every return before the menu's: never a running thread left)
    } cheats_join{cheats_load};

    g_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGTERM, SIGINT, SIGHUP}) sigaction(s, &sa, nullptr);
    sa.sa_handler = on_save_quit;
    sigaction(SIGUSR1, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Display dp;
    if (!dp.open(top, emu.scale())) {
        dp.close();
        return 1;
    }
    if (lowlat) {
        dp.lowlat = true;
        if (long m = cfg.num("kms.lowlat_margin_us", 0); m > 0) dp.margin_us = dp.margin_base = (double)m;
        Log(LogLevel::Info, "[lowlat] on (experimental): each frame started to be ready %.1f ms before the vblank it "
            "aims at%s\n", dp.margin_us / 1000.0,
            ra_n ? (ra_n == 1 ? ", and run-ahead: 1 frame" : ", and run-ahead: 2 frames") : "");
    } else if (ra_n)
        Log(LogLevel::Info, "[runahead] %d frame%s (experimental)\n", ra_n, ra_n > 1 ? "s" : "");
    // (kms.lowlat_trace also without kms.lowlat: started, published, committed and on screen, to compare)
    if (!cfg.str("kms.lowlat_trace", "").empty()) dp.trace.resize(16384);   // (~4.5 minutes)
    // kms.shader: a fragment shader's name (SuperDrastic's: ds-grid, ds-crisp, ds-fsr... from DSFLIP_SHADER_DIR), or
    // none: the display plane scales the frames (the default)
    {
        const std::string sh = cfg.str("kms.shader", "none");
        if (sh != "none" && sh != "bilinear" && !sh.empty()) dp.shader_name = sh;
        // kms.shader_fast (on): a shader's faster built-in twin (ds-grid-2x at 4x), used once it drew a test picture
        // the same, byte for byte (shader.c)
        shader_fast = cfg.flag("kms.shader_fast", true) ? 1 : 0;
        dp.gpu_floor_mhz = cfg.num("kms.gpu_floor_mhz", -1);
        clear_first = cfg.flag("kms.shader_clear", true) ? 1 : 0;
        dp.grid_cpu = cfg.flag("kms.grid_cpu", true);
        dp.shader_live = !dp.shader_name.empty();
    }
    // kms.commit_thread (Display::committer): blocking commits on a real-time thread of their own. auto (the default):
    // at render.scale 3 and 4, where every core is busy (2x as before: nonblocking commits from this thread)
    const std::string ct = cfg.str("kms.commit_thread", "auto");
    if (ct == "on" || ct == "1" || (ct == "auto" && emu.scale() >= 3)) {
        dp.cthread_start();
        Log(LogLevel::Info, "[kms] commits on a thread of their own (blocking, SCHED_FIFO 6)\n");
    }
    dp.touched = [&emu](const void* p) { emu.output_touched(p); };
    dp.direct = dp.packed() && emu.draw_into(dp.map(0, 1), dp.map(1, 1));  // (each frame is given its own slot)
    // render.2d_lazy (on): drawing straight into the slots, the 2D thread may finish a frame while the emulator runs the
    // next one; the presenter waits for it instead (Display::wait2d)
    if (dp.direct && cfg.flag("render.2d_lazy", true) && emu.set_lazy_2d(true))
        dp.wait2d = [&emu](uint32_t f) { emu.wait_frame(f); };
    // kms.rgb565 (auto|on|off): at render.scale 4, drawn straight into the scanout buffers, the frames are RGB565
    // (half the bytes of XRGB8888 for the 2D engine to write and the display to read: 4x is memory-bound). Not with
    // a shader (its frames 32-bit as before, also when one is chosen in the menu: RGB565 again when it goes back to
    // none), not with run-ahead, not at other scales. auto (the default) and on are the same: where it can't, 32-bit
    {
        const std::string v = cfg.str("kms.rgb565", "auto");
        const bool want = v != "off" && v != "0";
        dp.rgb565 = want && dp.direct && emu.scale() == 4 && !ra_n;
        if (want && emu.scale() == 4)
            Log(LogLevel::Info, "[kms] rgb565: %s\n", dp.rgb565 ? "frames drawn as RGB565 (16-bit) while no shader is on" :
                !dp.direct ? "no (frames copied, not drawn straight in): 32-bit" : "no (run-ahead): 32-bit");
    }
    Log(LogLevel::Info, "[kms] %s top, %s bottom, %dx%d at %.3f Hz; %dx%d frames %s, scaled by %s\n",
        dp.p[0].name.c_str(), dp.p[1].name.c_str(), dp.W, dp.H, 1e6 / dp.period_us, dp.fw, dp.fh,
        dp.direct ? "drawn straight into the scanout buffers" : "copied into the scanout buffers",
        dp.hw ? "the display plane" : "the CPU");
    start_mark("display open");
    std::vector<InputDev> devs = open_inputs(touch);
    start_mark("inputs");

    // power.cpu_latency_us: hold the cores out of idle states slower to leave than this while the game runs (PM QoS,
    // /dev/cpu_dma_latency; the A55s' cpu-sleep takes 220 us to wake from). 0, the default: the kernel decides
    int qos = -1;
    if (long us = cfg.num("power.cpu_latency_us", 0); us > 0) {
        qos = ::open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        int32_t v = (int32_t)us;
        if (qos < 0 || ::write(qos, &v, sizeof v) != sizeof v) {
            Log(LogLevel::Warn, "[kms] power.cpu_latency_us: %s\n", strerror(errno));
            if (qos >= 0) ::close(qos);
            qos = -1;
        } else
            Log(LogLevel::Info, "[kms] cores kept out of idle states slower than %ld us\n", us);
    }

#ifdef BANANADS_ALSA
    Alsa* snd = alsa_start(emu, cfg);
    start_mark("alsa");
#endif
    dp.shot_dir = cfg.str("screenshot.dir", "/data/screenshots");  // F + L2's PNGs (RetroArch's go there too)
    dp.shot_game = emu.game();
    GovFeed feed;
    feed.scale = emu.scale();
    cpugov_start(cfg, &feed);
    start_mark("cpugov");

    // the presenter's (this thread's) requests for the emulator's thread, applied before its next frame
    std::atomic<uint32_t> in_keys{0};
    std::atomic<bool> in_lid{false};  // the handheld's lid shut: the DS's too (the emulator's thread passes it on)
    bool lid = false;
    std::atomic<int> in_touch{-1};  // y << 16 | x, -1 released
    std::atomic<int> in_save{0}, in_load{0};
    std::atomic<bool> run{true};
    // the in-game menu (kms.menu, on): L3 pauses the game at its next frame and opens it on both panels
    const bool menu_on = cfg.flag("kms.menu", true);
    std::mutex pm;
    std::condition_variable pcv;
    std::atomic<bool> pause_req{false};
    bool paused = false;
    MenuHost mh;
    mh.dp = &dp;
    mh.emu = &emu;
    mh.devs = &devs;
#ifdef BANANADS_ALSA
    mh.snd = snd;
#endif
    {
        char rom[4096] = "";
        if (FILE* f = fopen("/proc/self/cmdline", "rb")) {
            size_t n = fread(rom, 1, sizeof rom - 1, f);
            fclose(f);
            rom[n] = 0;
            for (size_t i = 0; i < n; i += strlen(rom + i) + 1)
                if (i && rom[i] && rom[i] != '-') mh.rom = rom + i;  // (the last argument: the game)
        }
        if (swap_remembered(mh.rom)) {
            dp.swapped = true;
            Log(LogLevel::Info, "[kms] screens swapped for this game (F + SELECT, remembered)\n");
        }
        bmenu_host h{};
        h.ctx = &mh;
        h.rom = mh.rom.c_str();
        h.resume_on = cfg.flag("resume.on_quit", true);
        h.canvas = mh_canvas;
        h.show = mh_show;
        h.grab_top = mh_grab_top;
        h.poll = mh_poll;
        h.battery = mh_battery;
        h.save = mh_save;
        h.load = mh_load;
        h.state_file = mh_state_file;
        h.quit = mh_quit;
        h.quit_nosave = mh_quit_nosave;
        h.hotkey = mh_hotkey;
        g_quit_saves = h.resume_on;
        h.restart = mh_restart;
        h.blow = mh_blow;
        h.mic_meter = mh_mic_meter;
        h.sound_load = mh_sound_load;
        h.sound_play = mh_sound_play;
        h.boost = mh_boost;
        h.log = mh_log;
        opts_init(cfg, dp);
        h.opt_count = mh_opt_count;
        h.opt_label = mh_opt_label;
        h.opt_values = mh_opt_values;
        h.opt_value_name = mh_opt_value_name;
        h.opt_get = mh_opt_get;
        h.opt_set = mh_opt_set;
        h.cheat_count = mh_cheat_count;
        h.cheat_name = mh_cheat_name;
        h.cheat_get = mh_cheat_get;
        h.cheat_set = mh_cheat_set;
        start_mark("menu options");
        bmenu_init(&h);
        start_mark("menu init");
        // cheats.dir (<BANANADS_DATA>/cheats): the games' .mch files; then RetroArch .cht files from cheats.retroarch_dir
        // (the card's RetroArch folder) and cheats.system_dir (the collection that comes with BananaOS)
        // (read on cheats_load's thread since run_kms began)
        cheats_load.join();
        g_cheat_emu = &emu;
        if (g_cheats.enabled()) emu.set_cheats(g_cheats.list());
        bmenu_session_start();
        {   // what the emulator found missing at the start (a DSi game's BIOS file...): on the top panel for a while
            std::string n1, n2;
            if (emu.take_notice(n1, n2)) bmenu_popup(n1.c_str(), n2.c_str(), 15000);
        }
        start_mark("cheats");
    }
    {  // RetroAchievements (ra.c): the account from BANANADS_RA_USER/PASSWORD, melonDS's memory read directly
        ra_host rh{};
        rh.ctx = &emu;
        rh.main_ram = [](void* c) { return ((Emu*)c)->main_ram(); };
        rh.dtcm = [](void* c) { return ((Emu*)c)->dtcm(); };
        rh.log = [](void*, const char* line) { Log(LogLevel::Info, "%s", line); };
        ra_init(&rh, emu.rom_path().c_str());
        start_mark("ra init");
    }

    if (cfg.str("prof.thread", "emu") == "present") prof_target_this_thread();   // (this thread presents)
    std::thread emu_thread([&] {
        pthread_setname_np(pthread_self(), "bds-emu");
        // sched.emu_nice: this thread's nice value (negative = ahead of the 2D thread, Gengis's workers and the rest
        // when the four cores are contended: it is the frame's critical path); sched.emu_cpu: pin it to one core.
        // Both 0/off by default: device A/Bs first (BananaLinux perf/bananads/session3)
        if (long n = cfg.num("sched.emu_nice", 0); n != 0) {
            if (setpriority(PRIO_PROCESS, 0, (int)n)) Log(LogLevel::Warn, "[kms] sched.emu_nice %ld: %s\n", n, strerror(errno));
            else Log(LogLevel::Info, "[kms] the emulator's thread at nice %ld\n", n);
        }
        // sched.emu_excl (on): the emulator's thread on a core of its own (sched.emu_cpu, else the last one), every other
        // thread of ours (the 2D thread, Gengis's, the presenter, sound, threads started later) off it. On the RG DS
        // Plus's four A55s, HeartGold at fast-forward: 113.7-114.3 emulated/s vs 110.8-110.9 (the emulator's thread 98.8%
        // busy instead of 96.6%: no more waiting behind a woken 2D/Gengis thread or the SCHED_FIFO presenter)
        const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        long c = cfg.num("sched.emu_cpu", -1);
        const bool excl = cfg.flag("sched.emu_excl", true) && ncpu >= 4;
        if (c < 0 && excl) c = ncpu - 1;
        if (c >= 0) {
            cpu_set_t set; CPU_ZERO(&set); CPU_SET((int)c, &set);
            if (sched_setaffinity(0, sizeof set, &set)) Log(LogLevel::Warn, "[kms] sched.emu_cpu %ld: %s\n", c, strerror(errno));
            else if (!excl) Log(LogLevel::Info, "[kms] the emulator's thread pinned to cpu%ld\n", c);
            else {
                g_emu_core = (int)c;
                const long self = (long)syscall(SYS_gettid);
                cpu_set_t rest; CPU_ZERO(&rest);
                for (int k = 0; k < std::min(CPU_SETSIZE, (int)sysconf(_SC_NPROCESSORS_CONF)); k++) if (k != c) CPU_SET(k, &rest);
                // sched.emu_share=1|2 (off: 0): Gengis's workers (2: and the 2D thread) keep every core; this thread
                // at SCHED_FIFO 1 takes its core back from them at once when it wakes
                const std::string sh = cfg.str("sched.emu_share", "off");
                g_emu_share = sh == "2" ? 2 : sh == "1" || sh == "on" ? 1 : sh == "auto" ? (emu.scale() >= 4 ? 1 : 0) : 0;
                if (g_emu_share) {
                    sched_param sp{};
                    sp.sched_priority = 1;
                    if (int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp)) {
                        Log(LogLevel::Warn, "[kms] sched.emu_share: no SCHED_FIFO: %s\n", strerror(e));
                        g_emu_share = 0;
                    }
                }
                cpu_set_t all; CPU_ZERO(&all);
                for (int k = 0; k < std::min(CPU_SETSIZE, (int)sysconf(_SC_NPROCESSORS_CONF)); k++) CPU_SET(k, &all);
                int n = 0, ns = 0;
                if (DIR* d = opendir("/proc/self/task")) {
                    while (dirent* e = readdir(d)) {
                        const long tid = atol(e->d_name);
                        if (tid <= 0 || tid == self) continue;
                        bool share = false;
                        if (g_emu_share) {
                            char path[64], comm[32] = {};
                            snprintf(path, sizeof path, "/proc/self/task/%ld/comm", tid);
                            if (FILE* f = fopen(path, "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
                            share = !strncmp(comm, "bds-gengis", 10) || (g_emu_share > 1 && !strncmp(comm, "bananads-2d", 11));
                        }
                        if (share ? sched_setaffinity((pid_t)tid, sizeof all, &all) == 0 && ++ns
                                  : sched_setaffinity((pid_t)tid, sizeof rest, &rest) == 0) n++;
                    }
                    closedir(d);
                }
                if (g_emu_share) Log(LogLevel::Info, "[kms] sched.emu_share=%d: %d threads may use cpu%ld too (this thread SCHED_FIFO 1)\n", g_emu_share, ns, c);
                Log(LogLevel::Info, "[kms] sched.emu_excl: the emulator's thread on cpu%ld, %d other threads off it\n", c, n);
            }
        }
        // kms.pace: display (the default) runs the DS at the panels' refresh (60.002 Hz on the RG DS, 0.3% faster than
        // its 59.826) so every refresh shows a new frame, with the SPU's output rate lowered as much so the sound
        // keeps 48000 samples a second; ds runs it at its own rate, and a frame shows twice every ~6 s
        // (kms.lowlat aims every frame at a vblank: the panels' rate whatever kms.pace says)
        const bool pace_display = (lowlat || cfg.str("kms.pace", "display") != "ds") && dp.period_us > 15000 && dp.period_us < 18000;
        const std::chrono::nanoseconds period(pace_display ? (long long)(dp.period_us * 1000.0) : 16715180LL);
        if (pace_display) emu.set_audio_rate(48000.0 * dp.period_us * 1000.0 / 16715180.0);
        Pacer pace(feed, period);
        int touched = -1, ff = 0;
        long emu_frames = 0;
        const std::vector<KmsPress> script = kms_parse_input(cfg.str("kms.input", ""));
        long script_end = 0;
        for (const KmsPress& p : script) script_end = std::max(script_end, p.at + p.len);
        if (!script.empty()) Log(LogLevel::Info, "[kms] kms.input: %zu presses, to frame %ld\n", script.size(), script_end);
        const bool ff_mute = cfg.flag("kms.ff_mute", true);
        // kms.lowlat: the deadline the next frame aims at (0: none yet), and kms.lowlat_pad_us on top of the frames'
        // measured times (decaying maxima, Display::emu_us and pres_us)
        long long aim = 0;
        long aim_slips = 0, aim_slips_next = 0;  // Display::slip_total as the aim was made
        const long long pad = std::max(0L, cfg.num("kms.lowlat_pad_us", 1000));
        // run-ahead: the real frame's picture goes here (direct drawing), the frame ahead's into the slot. It runs
        // only while it fits the refresh: the frame's own time (a decaying maximum, measured every frame, ahead or
        // not) times 1 + n, plus the snapshot's save and load, under 85% of a refresh. A frame that then takes more
        // than 90% of one, twice in four: off for a while (5, 10, 20, then 60 s; back to 5 after a clean minute).
        // Before: tried until 20 of the last 32 frames were over a refresh, each try a burst of ~20 repeats (Mario
        // Kart DS on the RG DS: 30-48 ms frames, 107 repeats a minute, the sound stretched down to 0.78)
        std::vector<uint32_t> ra_scratch[2];
        if (ra_n && dp.direct)
            for (auto& v : ra_scratch) v.assign((size_t)dp.fw * dp.fh, 0);
        uint32_t ra_over = 0;            // the last frames run ahead: over 90% of the refresh (a bit each)
        long long ra_off_until = 0, ra_backoff = 5000000, ra_clean_since = 0;
        long ra_frames = 0, ra_suspends = 0, ra_unfit = 0, ra_measured = 0;
        double ra_ms = 0, ra_snap_ms = 0;
        double ra_plain = 0;             // the frame's own time (ms, run_frame alone), a decaying maximum (~2 s)
        double ra_snap = 0;              // the snapshot's save + load (ms), a decaying maximum (0 until measured)
        bool ra_said_unfit = false;
        long long ra_t = now_us();
        bool grid_now = false;   // kms.grid_cpu: the 2D engine draws the grid into the frames
        bool asked16 = false, on16 = false;   // kms.rgb565: asked of the 2D engine, and in effect
        while (run.load(std::memory_order_relaxed) && !g_quit && !emu.stopped()) {
            if (pause_req.load(std::memory_order_acquire)) {  // the menu: at a frame's end, until it closes
                std::unique_lock<std::mutex> l(pm);
                paused = true;
                pcv.notify_all();
                pcv.wait(l, [&] { return !pause_req.load() || !run.load(); });
                paused = false;
                aim = 0;  // (kms.lowlat: aimed again from now, not from before the menu)
                continue;
            }
            // F + R / F + L: a pop-up once the state is written or read, as the menu's own say it
            if (int s = in_save.exchange(0)) {
                cpugov_boost(3000);
                char l1[64];
                if (emu.save_state(s)) snprintf(l1, sizeof l1, "Saved to slot %d", s);
                else snprintf(l1, sizeof l1, "Couldn't save");
                bmenu_popup(l1, bmenu_game(), 2500);
            }
            if (int s = in_load.exchange(0)) {
                cpugov_boost(3000);
                char l1[64], l2[64];
                if (emu.load_state(s)) {
                    snprintf(l1, sizeof l1, "Loaded slot %d", s);
                    bmenu_popup(l1, bmenu_game(), 2500);
                } else {
                    if (access(emu.state_path(s).c_str(), F_OK)) snprintf(l2, sizeof l2, "Nothing saved in slot %d", s);
                    else snprintf(l2, sizeof l2, "Slot %d couldn't be read", s);
                    bmenu_popup("Couldn't load", l2, 2500);
                }
            }
            if (const int f = g_ff.load(std::memory_order_relaxed); f != ff) {
                ff = f;
                pace.set_speed(ff ? ff : 1);  // (Max: 99 times as often, i.e. no wait at all in practice)
#ifdef BANANADS_ALSA
                alsa_mute(snd, ff && ff_mute);  // kms.ff_mute (on): no chopped sound while fast-forwarding
#endif
                Log(LogLevel::Info, "[kms] fast-forward %s\n", ff ? (ff == 99 ? "max" : ff == 2 ? "2x" : "3x") : "off");
            }
            if (!script.empty() && emu_frames <= script_end) {
                uint32_t keys = 0;
                for (const KmsPress& p : script) {
                    if (emu_frames >= p.at && emu_frames < p.at + p.len) keys |= p.keys;
                    if (p.save && emu_frames == p.at) {
                        const bool ok = emu.save_state(p.save);
                        Log(LogLevel::Info, "[kms] kms.input: frame %ld state %s slot %d\n", emu_frames, ok ? "saved to" : "NOT saved to", p.save);
                    }
                }
                emu.set_keys(in_keys.load(std::memory_order_relaxed) | keys);
                if (emu_frames == script_end) Log(LogLevel::Info, "[kms] kms.input: done at frame %ld\n", emu_frames);
            } else
                emu.set_keys(in_keys.load(std::memory_order_relaxed));
            if (const bool l = in_lid.load(std::memory_order_relaxed); l != lid) {
                lid = l;
                emu.set_lid(l);  // the game sees the DS close: most go to their sleep mode, some play a sound
                Log(LogLevel::Info, "[kms] lid %s\n", l ? "shut: the DS's lid closes" : "open: the DS's lid opens");
            }
            const int t = in_touch.load(std::memory_order_relaxed);
            if (t != touched) {
                if (t < 0) emu.release_touch();
                else emu.touch(t & 0xFFFF, t >> 16);
                touched = t;
            }
            if (const int g = dp.grid_req.exchange(-1); g >= 0) {
                grid_now = emu.set_grid4(g ? dp.grid_lut : nullptr) && g && melonDS::BananaGrid4On();
                Log(LogLevel::Info, "[kms] grid in the 2D engine: %s\n", grid_now ? "on (its NEON writers checked against the tables)" :
                    g ? "NOT on (check failed): the GPU pass draws it" : "off");
            }
            // kms.rgb565: the slot's 16-bit twins while no shader draws (the format switched between frames, as the grid)
            const bool want16 = dp.rgb565 && !dp.shader_live.load(std::memory_order_relaxed);
            if (want16 != asked16) {
                asked16 = want16;
                if (want16 && !dp.make16()) {
                    Log(LogLevel::Warn, "[kms] rgb565: couldn't make the 16-bit buffers: 32-bit\n");
                    dp.rgb565 = false;
                    asked16 = false;
                }
                on16 = emu.set_rgb565(asked16);
                Log(LogLevel::Info, "[kms] rgb565: %s\n", on16 ? "on" : asked16 ? "NOT on (the 2D engine can't here): 32-bit" : "off (a shader)");
            }
            const int s = dp.take();
            dp.gridded[s] = grid_now;
            dp.is16[s] = on16;
            const auto work0 = std::chrono::steady_clock::now();  // (cpugov.tail: the frame's work, to its publish)
            const long long t_start = now_us();
            bool ahead = ra_n && !ff && t_start >= ra_off_until && emu_frames > 60 && emu.runahead_ok();
            if (ahead && ra_plain * (1 + ra_n) + ra_snap > dp.period_us * 0.00085) {
                // (predicted not to fit: not tried; looked at again in 2 s)
                ahead = false;
                ra_off_until = t_start + 2000000;
                ra_unfit++;
                if (!ra_said_unfit) {
                    ra_said_unfit = true;
                    Log(LogLevel::Info, "[runahead] off here: a frame %.1f ms x %d + the snapshot %.1f ms is over 85%% of the refresh\n",
                        ra_plain, 1 + ra_n, ra_snap);
                }
            }
            if (dp.direct) {
                if (ahead && !ra_scratch[0].empty()) emu.draw_into(ra_scratch[0].data(), ra_scratch[1].data());
                else if (on16) emu.draw_into(dp.map16(0, s), dp.map16(1, s));
                else emu.draw_into(dp.map(0, s), dp.map(1, s));
            }
            if (!emu_frames++) start_mark("first frame: start");
            emu.run_frame();
            if (ra_n && emu_frames > 60) {   // (not the load's first frames; a stall isn't a frame's cost)
                const double w = (now_us() - t_start) / 1000.0;
                if (w < dp.period_us * 0.003) ra_plain = std::max(w, ra_plain * 0.995);
            }
            if (emu_frames == 1) start_mark("first frame: emulated");
            ra_frame();  // RetroAchievements: this frame's memory (its pop-ups are drawn by the presenter)
            bool shown_ahead = false;
            if (ahead) {
                // run-ahead: the frame(s) ahead with the same buttons, the last one's picture into the slot, undone
                shown_ahead = emu.run_ahead(ra_n, [&] { if (dp.direct) emu.draw_into(dp.map(0, s), dp.map(1, s)); },
                                            [&] { if (!dp.direct) dp.write(s, emu.top(), emu.bottom()); });
                if (!shown_ahead && dp.direct) {  // (it couldn't: the real frame's picture, from the scratch buffers)
                    emu.set_lazy_2d(false);
                    for (int i = 0; i < 2; i++) {
                        memcpy(dp.map(i, s), ra_scratch[i].data(), ra_scratch[i].size() * 4);
                        emu.output_touched(dp.map(i, s));
                    }
                    if (dp.wait2d) emu.set_lazy_2d(true);
                }
                const long long t_end = now_us();
                const bool over = t_end - t_start > (long long)(dp.period_us * 0.9);
                ra_over = ra_over << 1 | over;
                if (shown_ahead) {
                    const double sn = emu.runahead_save_ms() + emu.runahead_load_ms();
                    if (ra_measured++) ra_snap = std::max(sn, ra_snap * 0.995);   // (the first one allocates: 2-3x)
                    ra_frames++; ra_ms += (t_end - t_start) / 1000.0; ra_snap_ms += sn;
                }
                if (over) ra_clean_since = 0;
                else if (!ra_clean_since) ra_clean_since = t_end;
                else if (t_end - ra_clean_since > 60000000) ra_backoff = 5000000;
                if (__builtin_popcount(ra_over & 0xF) >= 2) {
                    ra_off_until = t_end + ra_backoff;
                    Log(LogLevel::Warn, "[runahead] doesn't fit the refresh here (%.1f ms a frame of %.1f): off for %lld s\n",
                        (t_end - t_start) / 1000.0, dp.period_us / 1000.0, ra_backoff / 1000000);
                    ra_backoff = std::min(60000000LL, ra_backoff * 2);
                    ra_over = 0;
                    ra_suspends++;
                    ra_said_unfit = false;
                } else if (t_end - ra_t >= 10000000 && ra_frames) {  // (a line every 10 s)
                    Log(LogLevel::Info, "[runahead] %ld frames run %d ahead in %.0f s, %.1f ms a frame (%.1f the snapshot), %ld times off, %ld frames not tried\n",
                        ra_frames, ra_n, (t_end - ra_t) / 1e6, ra_ms / ra_frames, ra_snap_ms / ra_frames, ra_suspends, ra_unfit);
                    ra_frames = 0; ra_ms = ra_snap_ms = 0; ra_t = t_end;
                }
            } else if (ra_n && ra_off_until && t_start >= ra_off_until) {
                ra_off_until = 0;
                cpugov_boost(3000);      // (back on: measured at the top clock first, the governor goes down from there)
            }
            if (!dp.direct) {
                if (!shown_ahead) dp.write(s, emu.top(), emu.bottom());
            }
            uint32_t fence = 0;
            const bool fenced = emu.frame_fence(fence);
            feed.frame_time(std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - work0).count());
            dp.ra_now = shown_ahead ? ra_n : 0;
            dp.publish(s, depth, fenced, fence, lowlat || !dp.trace.empty() ? t_start : 0, lowlat && !ff ? aim : 0, aim_slips);
            // kms.lowlat: the next frame aimed at the next deadline, started its measured time (the emulator's + the
            // presenter's, + kms.lowlat_pad_us; at most two refreshes) before it, or at once if that has passed. Never
            // a deadline skipped ahead of time: a frame shown late (one repeat) moves the aims after it a refresh on
            // (Display::slip)
            bool aimed = false;
            if (lowlat && !ff) {
                long long known = 0, next = 0, start = 0;
                {
                    Lock l(dp.mu);
                    if (dp.vbl_known()) {
                        known = 1;
                        const long long per = std::llround(dp.period_us), now = now_us();
                        const long long pres = dp.pres_us.load(std::memory_order_relaxed);
                        const long long est = std::min(dp.emu_us.load(std::memory_order_relaxed) + pres + pad +
                                                       dp.extra_us.load(std::memory_order_relaxed), 3 * per);
                        // the deadline after the last frame's, on the panels' grid (a chain of + period drifts off it
                        // over a minute), and not before the refresh after the one the newest waiting frame goes on
                        // screen at (from the queue as it is: a frame shown late moves the ones after it on by as much
                        // as it did, never more; counting each late frame's lateness overshot after long frames)
                        next = aim ? dp.deadline_after(aim + per / 2) : dp.deadline_after(now + est);
                        aim_slips_next = dp.slip_total;
                        dp.slip.store(0);
                        if (const long long tv = dp.tail_vbl(now)) {
                            const long long after = dp.deadline_after(tv + per / 2);
                            if (after > next) {
                                feed.late += std::min(2LL, (after - next + per / 2) / per);   // (the governor's signal)
                                next = after;
                            }
                        }
                        if (next < now) next = dp.deadline_after(now + est);   // (a stall: aimed afresh)
                        start = std::max(next - est, now);
                    }
                }
                if (known) {
                    aim = next;
                    aim_slips = aim_slips_next;
                    aimed = true;
                    pace.wait_until(std::chrono::steady_clock::time_point(std::chrono::microseconds(start)));
                } else aim = 0;
            } else aim = 0;
            if (!aimed) pace.wait();  // (and the governor's signal: the frames it fell behind)
        }
        run = false;
        pcv.notify_all();
        wake();  // the presenter
    });
    // kms.shader: its program made now, on this thread (GL's), while the emulator's thread runs the game's first frame.
    // Made at the first commit instead, it came after that frame: ~135 ms more before the first picture on an RG DS Plus
    // (log.time's "[start] shader: start" after "first frame: emulated")
    dp.shader_settings();

    // kms.rt: the presenter at SCHED_FIFO 5, below the sound thread (10), as dsflip's below its audio pump. Set after
    // the other threads are started (a new thread takes its creator's policy), and dropped again before the quit's
    // savestate
    const bool rt = cfg.flag("kms.rt", true);
    if (rt) {
        sched_param sp{};
        sp.sched_priority = 5;
        if (int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
            Log(LogLevel::Info, "[kms] no real-time priority for the presenter: %s\n", strerror(e));
    }

    drmEventContext ev{};
    ev.version = 3;
    ev.page_flip_handler2 = on_flip;  // one event per CRTC: a frame is on screen once both panels' have come
    uint32_t keys = 0;
    bool ff_held = false;  // fast-forward from R2 held down (off again when it's let go)
    bool hot = false, menu_down = false;
    bool hot_used = false;      // a button went down while F was held (F + something, not F alone)
    bool home_down = false, home_used = false;  // HOME held, and a key (a volume key: a modifier) pressed meanwhile
    uint32_t stick_keys = 0;    // the d-pad directions the left stick holds
    // the menu: the game paused at its next frame, the flips in flight done, then the menu until it closes; the
    // game's frame back on both panels, no button left held for the game, the game running again
    const long menu_test = cfg.num("kms.menu_test", 0);  // (tests: the menu opens 3 s in, on page menu_test - 1)
    const long long t_start = now_us();
    bool menu_tested = false;
    auto open_menu = [&](int start) {
        {
            std::unique_lock<std::mutex> l(pm);
            pause_req = true;
            if (!pcv.wait_for(l, std::chrono::seconds(2), [&] { return paused || !run.load(); }) || !run.load()) {
                pause_req = false;
                return;
            }
        }
        for (int i = 0; i < 10 && dp.pending; i++) {
            pollfd p{dp.fd, POLLIN, 0};
            if (poll(&p, 1, 50) > 0) drmHandleEvent(dp.fd, &ev);
        }
        mh.quit = false;
        mh.nosave = false;
        mh.q.clear();
        bmenu_run(start);
        dp.show_game();
        keys = 0;
        hot = false;
        home_down = false;
        menu_down = false;
        in_keys = 0;
        in_touch = -1;
        if (mh.quit) {
            if (mh.nosave) g_nosave = true;
            g_quit = true;
        }
        if (ff_held) {  // (R2's release went to the menu)
            ff_held = false;
            g_ff = 0;
        }
        {
            std::lock_guard<std::mutex> l(pm);
            pause_req = false;
        }
        pcv.notify_all();
    };
    std::vector<pollfd> pfd;
    long shown_mark = 0, made_mark = 0, drop_mark = 0, rep_mark = 0;
    long long since = now_us();
    long fps_mark = 0, swaps_mark = 0;
    long long fps_since = since;
    bmenu_fps(-1, 0);
    while (run.load()) {
        pfd.clear();
        pfd.push_back({dp.fd, POLLIN, 0});
        pfd.push_back({g_wake, POLLIN, 0});
        for (auto& d : devs) pfd.push_back({d.fd, POLLIN, 0});
        int timeout = 250;
        if (dp.wake_at) {                // (kms.lowlat: a frame held until its vblank's window opens)
            const long long w = dp.wake_at - now_us();
            timeout = w <= 0 ? 0 : (int)std::min(250LL, (w + 999) / 1000);
            dp.wake_at = 0;
        }
        if (poll(pfd.data(), pfd.size(), timeout) < 0 && errno != EINTR) break;
        if (g_quit) break;
        if (pfd[1].revents & POLLIN) {
            uint64_t v;
            ssize_t r = read(g_wake, &v, sizeof v);
            (void)r;
        }
        if (pfd[0].revents & POLLIN) drmHandleEvent(dp.fd, &ev);
        dp.commit_next();
        feed.frames = dp.made.load();
        feed.repeats = dp.repeats;  // (cpugov.repeats; flipped() counts them on this thread)

        for (size_t i = 2; i < 2 + devs.size(); i++) {
            if (!(pfd[i].revents & POLLIN)) continue;
            InputDev& d = devs[i - 2];
            input_event e[64];
            ssize_t n;
            while ((n = read(d.fd, e, sizeof e)) > 0) {
                for (int j = 0; j < n / (int)sizeof(input_event); j++) {
                    const input_event& x = e[j];
                    if (d.touch) {
                        if (x.type == EV_ABS && (x.code == ABS_MT_POSITION_X || x.code == ABS_X)) d.x = x.value;
                        else if (x.type == EV_ABS && (x.code == ABS_MT_POSITION_Y || x.code == ABS_Y)) d.y = x.value;
                        else if (x.type == EV_ABS && x.code == ABS_MT_TRACKING_ID && x.value < 0) d.down = false;
                        else if (x.type == EV_KEY && (x.code == BTN_TOUCH || (d.pointer && x.code == BTN_LEFT)))
                            d.down = x.value != 0;
                        else if (x.type == EV_SYN && x.code == SYN_REPORT) {
                            if (d.panel != (dp.swapped ? 0 : 1)) {  // not the panel the DS's bottom screen is on
                                if (d.was) in_touch = -1;          // (a press held across F + SELECT: let go)
                                d.was = false;
                                continue;
                            }
                            if (d.down) {
                                // the panel's pixel, then the DS's: where the bottom picture is on the panel
                                double px = d.maxx > d.minx ? (double)(d.x - d.minx) * dp.W / (d.maxx - d.minx + 1) : d.x;
                                double py = d.maxy > d.miny ? (double)(d.y - d.miny) * dp.H / (d.maxy - d.miny + 1) : d.y;
                                double rx = dp.hw ? 0 : dp.ox, ry = dp.hw ? 0 : dp.oy;
                                double rw = dp.hw ? dp.W : dp.fw * dp.k, rh = dp.hw ? dp.H : dp.fh * dp.k;
                                // a shader that draws the DS screen into part of the panel (Integer: 512x384 in the
                                // middle of the RG DS's 640x480) says where; before, touch went by the whole panel
                                // there, off by up to 64 px at the edges (#49). (shader_settings runs on this thread)
                                int v[4];
                                if (dp.shader_on && shader_viewport(v, dp.W, dp.H)) { rx = v[0]; ry = v[1]; rw = v[2]; rh = v[3]; }
                                int tx = std::clamp((int)((px - rx) * 256 / rw), 0, 255);
                                int ty = std::clamp((int)((py - ry) * 192 / rh), 0, 191);
                                in_touch = ty << 16 | tx;
                                if (!d.was) Log(LogLevel::Info, "[touch] down at %d,%d on the panel -> %d,%d on the DS\n", (int)px, (int)py, tx, ty);
                            } else if (d.was)
                                in_touch = -1;
                            d.was = d.down;
                        }
                        continue;
                    }
                    if (x.type == EV_ABS && d.stick > 0 && (x.code == ABS_X || x.code == ABS_Y)) {
                        // the left stick as the d-pad: past 60% of its throw (its own bits: the d-pad keeps its own)
                        const int t = d.stick * 3 / 5;
                        if (x.code == ABS_X)
                            stick_keys = (stick_keys & ~(KeyLeft | KeyRight)) | (x.value < -t ? KeyLeft : x.value > t ? KeyRight : 0);
                        else
                            stick_keys = (stick_keys & ~(KeyUp | KeyDown)) | (x.value < -t ? KeyUp : x.value > t ? KeyDown : 0);
                        continue;
                    }
                    if (x.type == EV_ABS && x.code == ABS_HAT0X)
                        keys = (keys & ~(KeyLeft | KeyRight)) | (x.value < 0 ? KeyLeft : x.value > 0 ? KeyRight : 0);
                    else if (x.type == EV_ABS && x.code == ABS_HAT0Y)
                        keys = (keys & ~(KeyUp | KeyDown)) | (x.value < 0 ? KeyUp : x.value > 0 ? KeyDown : 0);
                    // the lid (a shut lid when the game started counts from the next close: its first event is the open)
                    if (x.type == EV_SW && x.code == SW_LID) {
                        in_lid = x.value != 0;
                        continue;
                    }
                    if (x.type != EV_KEY || x.value == 2) continue;
                    if (home_down && x.value && x.code != BTN_Z) home_used = true;  // HOME + something: not HOME alone
                    if (x.code == BTN_Z) {  // HOME: the menu, on release (whatever F and L3 do), when let go on its own
                        if (x.value) { home_down = true; home_used = false; continue; }
                        if (home_down && !home_used && menu_on) open_menu(0);
                        home_down = false;
                        continue;
                    }
                    if (hot && x.value && !is_hotkey(x.code)) hot_used = true;  // F + something: not F alone
                    if (menu_on && is_menu_button(x.code)) {  // (opens on release: nothing of it reaches the game)
                        if (x.value) menu_down = true;
                        else if (menu_down) {
                            menu_down = false;
                            open_menu(0);
                        }
                        continue;
                    }
                    if (x.code == BTN_TL2) {  // F + L2: a screenshot (L2 alone: nothing, the DS has none)
                        if (hot && x.value) dp.shot = true;
                        continue;
                    }
                    if (x.code == BTN_TR2) {  // R2 held: fast-forward while it's down, at the F + X toggle's speed
                        if (x.value && !g_ff.load()) {
                            g_ff = g_opts.ff_speed;
                            ff_held = true;
                        } else if (!x.value && ff_held) {
                            g_ff = 0;
                            ff_held = false;
                        }
                        continue;
                    }
                    if (is_hotkey(x.code)) {
                        // F pressed and let go on its own opens the menu, as SuperDrastic's does; F + a button is a
                        // hotkey and opens nothing
                        if (x.value) { hot = true; hot_used = false; }
                        else {
                            hot = false;
                            if (!hot_used && menu_on) open_menu(0);
                        }
                        continue;
                    }
                    if (x.code == KEY_ESC && x.value) g_quit = true;
                    const uint32_t b = pad_bit(x.code);
                    if (hot && x.value && b) {  // F + a button: BananaDS's own, not the game's
                        if (b == KeyStart) g_quit = true;
                        else if (b == KeySelect) swap_toggle(dp, mh.rom);
                        else if (b == KeyR) in_save = 1;
                        else if (b == KeyL) in_load = 1;
                        else if (b == KeyX) ff_toggle();
                        continue;
                    }
                    keys = x.value ? keys | b : keys & ~b;
                }
            }
        }
        in_keys = hot ? 0 : keys | stick_keys;  // while F is held the game sees no buttons
        if (menu_test > 0 && !menu_tested && now_us() - t_start > 3000000) {
            menu_tested = true;
            open_menu((int)menu_test - 1);
        }
        if (g_quit) break;

        // the FPS counter (kms.fps, or the menu's; always while fast-forwarding): the frames emulated a second, twice
        // a second
        if (const long long t = now_us(); t - fps_since >= 500000) {
            const long m = dp.made.load(), g = emu.swaps3d();
            if (g_opts.fps || g_ff.load()) {
                bmenu_fps((int)((m - fps_mark) * 10000000LL / (t - fps_since)), g_ff.load());
                // the game's own frames (#37): with no 3D in this half second, the emulator's rate is the game's
                bmenu_fps_game(g > swaps_mark ? (int)((g - swaps_mark) * 10000000LL / (t - fps_since)) : -1);
            } else bmenu_fps(-1, 0);
            fps_mark = m;
            swaps_mark = g;
            fps_since = t;
        }

        // kms.lowlat: a line every 10 s
        if (lowlat) {
            static long long ll_since = now_us();
            if (const long long t = now_us(); t - ll_since >= 10000000) {
                Lock l(dp.mu);
                Log(LogLevel::Info, "[lowlat] %ld frames taken before their deadline (%.1f ms before on average), %ld "
                    "after it; %ld finished too late for their vblank, %ld commits missed theirs; a frame: emulator %.1f "
                    "ms + presenter %.1f ms + %.1f ms more lead, deadline %.1f ms before the vblank, bottom panel %+.1f ms\n", dp.ll_n,
                    dp.ll_n ? dp.ll_lead_sum / dp.ll_n / 1000.0 : 0.0, dp.ll_late, dp.ll_behind, dp.ll_miss,
                    dp.emu_us.load() / 1000.0, dp.pres_us.load() / 1000.0, dp.extra_us.load() / 1000.0, dp.margin_us / 1000.0, dp.bot_off / 1000.0);
                if (!dp.ll_miss && dp.margin_us > dp.margin_base) dp.margin_us = std::max(dp.margin_base, dp.margin_us - 300);
                if (!dp.ll_behind) dp.extra_us.store(std::max(0LL, dp.extra_us.load() - 250));
                dp.ll_n = dp.ll_late = dp.ll_miss = dp.ll_behind = 0;
                dp.ll_lead_sum = 0;
                ll_since = t;
            }
        }

        // kms.stats: every this many frames shown, the refresh rate of new frames, the frames emulated per second,
        // the refreshes that showed no new frame and the frames never shown
        const long s = dp.shown.load();
        if (every > 0 && s - shown_mark >= every) {
            const long long t = now_us();
            const double sec = (t - since) / 1e6;
            long rep;
            {
                Lock l(dp.mu);
                rep = dp.repeats;
            }
            Log(LogLevel::Info, "[kms] %ld frames shown in %.2f s (%.2f/s), %.2f emulated/s: %ld refreshes without a new frame, %ld dropped\n",
                s - shown_mark, sec, (s - shown_mark) / sec, (dp.made - made_mark) / sec, rep - rep_mark,
                dp.dropped - drop_mark);
            if (dp.shader_n)
                Log(LogLevel::Info, "[kms] shader: %.1f ms a frame on average, %.1f at most\n",
                    dp.shader_us / 1000.0 / dp.shader_n, dp.shader_max_us / 1000.0);
            dp.shader_us = dp.shader_max_us = 0;
            dp.shader_n = 0;
            shown_mark = s;
            made_mark = dp.made;
            drop_mark = dp.dropped;
            rep_mark = rep;
            since = t;
        }
    }
    const bool signalled = g_quit;
    run = false;
    if (emu_thread.joinable()) emu_thread.join();
    if (rt) {
        sched_param sp{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    Log(LogLevel::Info, "[kms] %s: %ld frames shown, %ld dropped\n", signalled ? "asked to quit" : "quit",
        dp.shown.load(), dp.dropped.load());

    // the resume state at the top clock (a save at 2x takes a visible moment at a low one), then the rest
    if (g_save || (cfg.flag("resume.on_quit", true) && !g_nosave)) {
        cpugov_boost(5000);
        emu.save_state(0);
    } else if (g_nosave)
        Log(LogLevel::Info, "[kms] quit without saving: the resume state left as it was\n");
    cpugov_stop();
#ifdef BANANADS_ALSA
    start_mark("quit: cpugov");
    alsa_stop(snd);
#endif
    start_mark("quit: alsa");
    if (!screenshot_wait(3000)) Log(LogLevel::Warn, "[shot] still being written at quit\n");
    start_mark("quit: screenshot");
    for (auto& d : devs) ::close(d.fd);
    if (qos >= 0) ::close(qos);
    emu.set_lazy_2d(false);  // render.2d_lazy: the last frame's lines drawn before its buffers go
    start_mark("quit: 2d");
    if (!dp.trace.empty()) {  // kms.lowlat_trace: the frames' times, oldest first
        if (FILE* f = fopen(cfg.str("kms.lowlat_trace", "").c_str(), "w")) {
            fprintf(f, "# seq start pub aim take ready commit flip ftop fbot (us, CLOCK_MONOTONIC; 0: not reached) audio_backlog tempo_x1000 ahead\n");
            const unsigned long n = dp.trace.size(), first = dp.seq_next > n ? dp.seq_next - n : 0;
            for (unsigned long q = first; q < dp.seq_next; q++) {
                const Display::Trace& x = dp.trace[q % n];
                fprintf(f, "%lu %lld %lld %lld %lld %lld %lld %lld %lld %lld %d %d %d\n", q, x.start, x.pub, x.aim, x.take, x.ready, x.commit, x.flip, x.ftop, x.fbot, x.ab, x.tempo, x.ra);
            }
            fclose(f);
        }
    }
    emu.draw_into(nullptr, nullptr);  // (the buffers go with the device)
    start_mark("quit: draw_into");
    dp.close();
    ::close(g_wake);
    start_mark("quit: display closed");
    g_wake = -1;
    return 0;
}

}  // namespace bananads
