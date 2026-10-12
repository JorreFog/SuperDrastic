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
// - Input: the pad (rocknix-singleadc-joypad, and any keyboard) read, never grabbed (bananad reads the same device
//   for the lid, power and volume keys, which are ignored here). The touchscreen is the device whose sysfs path
//   contains kms.touch (BANANADS_TOUCH: fe5e0000.i2c, the bottom panel's). F (BTN_MODE) + Start quits; F + R saves
//   slot 1, F + L loads it.
// - SIGTERM/SIGINT/SIGHUP quit the same way (the resume state written, then the save): BananaLinux's shutdown waits
//   5 s for that. SIGSTOP/SIGCONT around sleep only leave a gap: the pacing starts again from the time it wakes.
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <fstream>
#include <glob.h>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
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
#include "menu.h"
#include "ra.h"

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

static std::atomic<bool> g_quit{false};
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
};

struct Panel {
    uint32_t conn = 0, crtc = 0, plane = 0, mode_blob = 0;
    int crtc_idx = -1;
    drmModeModeInfo mode{};
    uint32_t p_fb = 0, p_crtc = 0, p_sx = 0, p_sy = 0, p_sw = 0, p_sh = 0, p_cx = 0, p_cy = 0, p_cw = 0, p_ch = 0;
    std::string name;
};

enum SlotState { FREE, WRITING, QUEUED, COMMITTED, SCANOUT };

struct Display {
    static const int NB = 6;  // writing, up to 3 queued, committed, on screen
    int fd = -1;
    Panel p[2];               // [0] = top, [1] = bottom
    int W = 0, H = 0;         // the panels' mode
    int fw = 0, fh = 0;       // a DS screen's size at render.scale
    bool hw = true;           // the plane scales the frame; else the CPU does, into panel-sized buffers
    bool direct = false;      // the emulator draws straight into the buffers (hw, rows packed, the Banana 2D engine)
    int k = 1, ox = 0, oy = 0;  // CPU scaling: the whole-number factor and where the picture sits
    Buf buf[2][NB];
    Buf mbuf[2][2];           // the in-game menu's pictures: panel-sized, two a panel (one drawn while one shows)
    int mnext[2] = {0, 0};
    SlotState state[NB] = {};
    std::deque<int> queue;    // frames waiting, oldest first
    // kms.shader: each frame drawn through a fragment shader (SuperDrastic's) into panel-sized buffers, shown 1:1
    std::string shader_name;
    bool shader_tried = false, shader_on = false;
    std::string shader_req;           // from the in-game menu (any thread): taken up by the presenter's next frame
    bool shader_req_set = false;
    Buf sbuf[2][3];
    int snext = 0;
    int committed = -1, scanout = -1, pending = 0;
    PiMutex mu;
    double period_us = 16666.7;
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

    bool mkbuf(Buf& b, int w, int h) {
        drm_mode_create_dumb c{};
        c.width = w;
        c.height = h;
        c.bpp = 32;
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return false;
        b.handle = c.handle;
        b.pitch = c.pitch;
        b.size = c.size;
        b.w = w;
        b.h = h;
        uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
        if (drmModeAddFB2(fd, w, h, DRM_FORMAT_XRGB8888, hs, ps, os, &b.fb, 0)) return false;
        drm_mode_map_dumb m{};
        m.handle = c.handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return false;
        void* p = mmap(nullptr, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
        if (p == MAP_FAILED) return false;
        b.map = (uint8_t*)p;
        memset(b.map, 0, c.size);  // black (the borders of a CPU-scaled picture stay so)
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
        if (scanout >= 0) commit_now(0, 1, &buf[0][scanout], &buf[1][scanout]);
    }
    // the game's top screen as last shown, at the panel's size (nearest)
    bool grab_top(uint32_t* dst, int pitch, int w, int h) {
        Lock l(mu);
        if (scanout < 0) return false;
        const Buf& b = buf[0][scanout];
        const int sw = hw ? fw : W, sh = hw ? fh : H;
        std::vector<uint32_t> row(sw);
        for (int y = 0; y < h; y++) {
            const int sy = y * sh / h;
            memcpy(row.data(), b.map + (size_t)sy * b.pitch, sw * 4);  // (one read of the slow buffer a row)
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
    void publish(int s, int depth) {
        {
            Lock l(mu);
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
    // with no flip pending, the oldest waiting frame goes to both panels in one commit
    void commit_next() {
        int s;
        {
            Lock l(mu);
            if (pending || queue.empty()) return;
            // the oldest frame, taken off the queue so the emulator can't recycle it while the shader draws it (outside
            // the lock: the emulator's thread never waits for the GPU)
            s = queue.front();
            queue.pop_front();
            state[s] = COMMITTED;
        }
        const Buf* out[2] = {&buf[0][s], &buf[1][s]};
        {
            std::string want;
            bool asked = false;
            {
                Lock l(mu);
                if (shader_req_set) { want = shader_req; asked = true; shader_req_set = false; }
            }
            if (asked) {
                if (want == "none") {
                    if (shader_on) gpu_floor(false);
                    shader_on = false;
                    shader_name.clear();
                } else if (want != shader_name || !shader_on) {
                    shader_name = want;
                    shader_tried = false;  // made again below, with the new program
                }
            }
        }
        if (!shader_name.empty() && !shader_tried) {  // GL is made on this thread, at its first frame
            shader_tried = true;
            shader_on = shader_init(fd, shader_name.c_str()) != 0;
            for (int i = 0; i < 2 && shader_on; i++)
                for (auto& b : sbuf[i]) shader_on = shader_on && (b.fb || mkbuf(b, W, H));  // (made once)
            if (shader_on && gpu_dir.empty()) gpu_floor(true);
            Log(LogLevel::Info, "[kms] shader %s: %s\n", shader_name.c_str(), shader_on ? "on (panel-sized, 1:1)" : "unavailable");
        }
        if (shader_on) {
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
            snext = (snext + 1) % 3;  // (one on screen, one committed, one drawn)
        }
        Lock l(mu);
        drmModeAtomicReq* r = drmModeAtomicAlloc();
        for (int i = 0; i < 2; i++) add_plane(r, p[i], *out[i]);
        int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, this);
        drmModeAtomicFree(r);
        if (ret) {
            if (errno != EBUSY) Log(LogLevel::Warn, "[kms] commit failed: %s\n", strerror(errno));
            state[s] = QUEUED;  // back at the head of the queue: tried again at the next wake-up
            queue.push_front(s);
            return;
        }
        committed = s;
        pending = 2;
    }
    // with a shader, the GPU's lowest clock raised (its on-demand governor ramps too slowly for a pass every
    // refresh: frames missed their vblank), and put back at the end
    std::string gpu_dir;
    std::string gpu_min_was;
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
            long want = 0;  // 600 MHz, or the highest under it
            for (long v : fr) if (v <= 600000000) want = v;
            if (want) std::ofstream(gpu_dir + "/min_freq") << want;
            Log(LogLevel::Info, "[kms] GPU at least %ld MHz while the shader runs\n", want / 1000000);
        } else if (!gpu_dir.empty() && !gpu_min_was.empty()) {
            std::ofstream(gpu_dir + "/min_freq") << gpu_min_was;
        }
    }
    void flipped(long long t) {
        Lock l(mu);
        if (--pending > 0) return;
        if (scanout >= 0) state[scanout] = FREE;
        scanout = committed;
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

static void on_flip(int, unsigned, unsigned, unsigned, unsigned, void* ud) { ((Display*)ud)->flipped(now_us()); }

// ---- input ----------------------------------------------------------------------------------------------------

struct InputDev {
    int fd = -1;
    bool touch = false, pointer = false;
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
        unsigned long keys[KEY_MAX / (8 * sizeof(long)) + 1] = {}, abs[ABS_MAX / (8 * sizeof(long)) + 1] = {};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        InputDev dv;
        dv.fd = fd;
        bool mt = bit(abs, ABS_MT_POSITION_X) && bit(abs, ABS_MT_POSITION_Y);
        bool st = !mt && bit(abs, ABS_X) && bit(abs, ABS_Y) &&
                  (bit(keys, BTN_TOUCH) || (bit(keys, BTN_LEFT) && !bit(keys, BTN_SOUTH)));
        if (mt || st) {
            // only the bottom panel's touchscreen (the top one's isn't the DS's)
            char link[300], real[PATH_MAX];
            snprintf(link, sizeof link, "/sys/class/input/%s/device", e->d_name);
            if (touch_name.empty() || !realpath(link, real) || !strstr(real, touch_name.c_str())) {
                ::close(fd);
                continue;
            }
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
            bool any = bit(abs, ABS_HAT0X);
            for (int c = KEY_ESC; c < KEY_MAX && !any; c++)
                if (bit(keys, c) && (pad_bit(c) || is_hotkey(c))) any = true;
            if (!any) {
                ::close(fd);
                continue;
            }
        }
        Log(LogLevel::Info, "[kms] input %s: %s\n", e->d_name, dv.touch ? "the touchscreen" : "buttons");
        devs.push_back(dv);
    }
    closedir(d);
    return devs;
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
};

static int menu_key(int code) {
    switch (code) {
        case BTN_EAST: case KEY_ENTER: return BM_A;
        case BTN_SOUTH: case KEY_BACKSPACE: return BM_B;
        case BTN_WEST: case KEY_Y: return BM_Y;
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
                            if (d.down && !d.was) {
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
}
static int mh_opt_count(void*) { return 3; }
static const char* mh_opt_label(void*, int i) { return i == 0 ? "Shader" : i == 1 ? "Edge marking" : "Fog"; }
static int mh_opt_values(void*, int i) { return i == 0 ? (int)g_opts.shaders.size() : 2; }
static const char* mh_opt_value_name(void*, int i, int v) {
    if (i == 0) return v >= 0 && v < (int)g_opts.shaders.size() ? shader_label(g_opts.shaders[v]) : "?";
    return v ? "On" : "Off";
}
static int mh_opt_get(void*, int i) { return i == 0 ? g_opts.shader : i == 1 ? g_opts.edge : g_opts.fog; }
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
    } else {
        (i == 1 ? g_opts.edge : g_opts.fog) = v != 0;
#ifdef BANANADS_GENGIS
        GengisRenderer3D::SetEffects(g_opts.edge, g_opts.fog);
#endif
        const char* data = getenv("BANANADS_DATA");
        set_kv(std::string(data && *data ? data : "/data/bananads") + "/bananads.conf", i == 1 ? "render.edge_marking" : "render.fog",
               v ? "on" : "off", " = ");
    }
}
static void mh_log(void*, const char* line) { Log(LogLevel::Info, "%s", line); }

// ---- the frontend ---------------------------------------------------------------------------------------------

int run_kms(Emu& emu, const Config& cfg) {
    const char* et = getenv("BANANADS_TOP");
    const char* eh = getenv("BANANADS_TOUCH");
    const std::string top = cfg.str("kms.top", et && *et ? et : "DSI-2");
    const std::string touch = cfg.str("kms.touch", eh && *eh ? eh : "fe5e0000.i2c");
    const int depth = std::clamp((int)cfg.num("kms.queue", 2), 1, 3);
    const long every = cfg.num("kms.stats", 0);

    g_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    for (int s : {SIGTERM, SIGINT, SIGHUP}) sigaction(s, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    Display dp;
    if (!dp.open(top, emu.scale())) {
        dp.close();
        return 1;
    }
    // kms.shader: a fragment shader's name (SuperDrastic's: ds-grid, ds-crisp, ds-fsr... from DSFLIP_SHADER_DIR), or
    // none: the display plane scales the frames (the default)
    {
        const std::string sh = cfg.str("kms.shader", "none");
        if (sh != "none" && sh != "bilinear" && !sh.empty()) dp.shader_name = sh;
    }
    dp.direct = dp.packed() && emu.draw_into(dp.map(0, 1), dp.map(1, 1));  // (each frame is given its own slot)
    Log(LogLevel::Info, "[kms] %s top, %s bottom, %dx%d at %.3f Hz; %dx%d frames %s, scaled by %s\n",
        dp.p[0].name.c_str(), dp.p[1].name.c_str(), dp.W, dp.H, 1e6 / dp.period_us, dp.fw, dp.fh,
        dp.direct ? "drawn straight into the scanout buffers" : "copied into the scanout buffers",
        dp.hw ? "the display plane" : "the CPU");
    std::vector<InputDev> devs = open_inputs(touch);

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
#endif
    GovFeed feed;
    feed.scale = emu.scale();
    cpugov_start(cfg, &feed);

    // the presenter's (this thread's) requests for the emulator's thread, applied before its next frame
    std::atomic<uint32_t> in_keys{0};
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
        bmenu_init(&h);
        bmenu_session_start();
    }
    {  // RetroAchievements (ra.c): the account from BANANADS_RA_USER/PASSWORD, melonDS's memory read directly
        ra_host rh{};
        rh.ctx = &emu;
        rh.main_ram = [](void* c) { return ((Emu*)c)->main_ram(); };
        rh.dtcm = [](void* c) { return ((Emu*)c)->dtcm(); };
        rh.log = [](void*, const char* line) { Log(LogLevel::Info, "%s", line); };
        ra_init(&rh, emu.rom_path().c_str());
    }

    std::thread emu_thread([&] {
        pthread_setname_np(pthread_self(), "bds-emu");
        // kms.pace: display (the default) runs the DS at the panels' refresh (60.002 Hz on the RG DS, 0.3% faster than
        // its 59.826) so every refresh shows a new frame, with the SPU's output rate lowered as much so the sound
        // keeps 48000 samples a second; ds runs it at its own rate, and a frame shows twice every ~6 s
        const bool pace_display = cfg.str("kms.pace", "display") != "ds" && dp.period_us > 15000 && dp.period_us < 18000;
        const std::chrono::nanoseconds period(pace_display ? (long long)(dp.period_us * 1000.0) : 16715180LL);
        if (pace_display) emu.set_audio_rate(48000.0 * dp.period_us * 1000.0 / 16715180.0);
        Pacer pace(feed, period);
        int touched = -1;
        while (run.load(std::memory_order_relaxed) && !g_quit && !emu.stopped()) {
            if (pause_req.load(std::memory_order_acquire)) {  // the menu: at a frame's end, until it closes
                std::unique_lock<std::mutex> l(pm);
                paused = true;
                pcv.notify_all();
                pcv.wait(l, [&] { return !pause_req.load() || !run.load(); });
                paused = false;
                continue;
            }
            if (int s = in_save.exchange(0)) {
                cpugov_boost(3000);
                emu.save_state(s);
            }
            if (int s = in_load.exchange(0)) {
                cpugov_boost(3000);
                emu.load_state(s);
            }
            emu.set_keys(in_keys.load(std::memory_order_relaxed));
            const int t = in_touch.load(std::memory_order_relaxed);
            if (t != touched) {
                if (t < 0) emu.release_touch();
                else emu.touch(t & 0xFFFF, t >> 16);
                touched = t;
            }
            const int s = dp.take();
            if (dp.direct) emu.draw_into(dp.map(0, s), dp.map(1, s));
            emu.run_frame();
            if (!dp.direct) dp.write(s, emu.top(), emu.bottom());
            ra_frame();  // RetroAchievements: this frame's memory, then any pop-up over the top screen
            {
                int pitch, w, h;
                uint32_t* top = dp.picture(0, s, &pitch, &w, &h);
                bmenu_overlay(top, pitch, w, h);
            }
            dp.publish(s, depth);
            pace.wait();  // (and the governor's signal: the frames it fell behind)
        }
        run = false;
        pcv.notify_all();
        wake();  // the presenter
    });

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
    bool hot = false, menu_down = false;
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
        mh.q.clear();
        bmenu_run(start);
        dp.show_game();
        keys = 0;
        hot = false;
        menu_down = false;
        in_keys = 0;
        in_touch = -1;
        if (mh.quit) g_quit = true;
        {
            std::lock_guard<std::mutex> l(pm);
            pause_req = false;
        }
        pcv.notify_all();
    };
    std::vector<pollfd> pfd;
    long shown_mark = 0, made_mark = 0, drop_mark = 0, rep_mark = 0;
    long long since = now_us();
    while (run.load()) {
        pfd.clear();
        pfd.push_back({dp.fd, POLLIN, 0});
        pfd.push_back({g_wake, POLLIN, 0});
        for (auto& d : devs) pfd.push_back({d.fd, POLLIN, 0});
        if (poll(pfd.data(), pfd.size(), 250) < 0 && errno != EINTR) break;
        if (g_quit) break;
        if (pfd[1].revents & POLLIN) {
            uint64_t v;
            ssize_t r = read(g_wake, &v, sizeof v);
            (void)r;
        }
        if (pfd[0].revents & POLLIN) drmHandleEvent(dp.fd, &ev);
        dp.commit_next();
        feed.frames = dp.made.load();

        for (size_t i = 2; i < pfd.size(); i++) {
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
                            if (d.down) {
                                // the panel's pixel, then the DS's: where the bottom picture is on the panel
                                double px = d.maxx > d.minx ? (double)(d.x - d.minx) * dp.W / (d.maxx - d.minx + 1) : d.x;
                                double py = d.maxy > d.miny ? (double)(d.y - d.miny) * dp.H / (d.maxy - d.miny + 1) : d.y;
                                double rx = dp.hw ? 0 : dp.ox, ry = dp.hw ? 0 : dp.oy;
                                double rw = dp.hw ? dp.W : dp.fw * dp.k, rh = dp.hw ? dp.H : dp.fh * dp.k;
                                int tx = std::clamp((int)((px - rx) * 256 / rw), 0, 255);
                                int ty = std::clamp((int)((py - ry) * 192 / rh), 0, 191);
                                in_touch = ty << 16 | tx;
                            } else if (d.was)
                                in_touch = -1;
                            d.was = d.down;
                        }
                        continue;
                    }
                    if (x.type == EV_ABS && x.code == ABS_HAT0X)
                        keys = (keys & ~(KeyLeft | KeyRight)) | (x.value < 0 ? KeyLeft : x.value > 0 ? KeyRight : 0);
                    else if (x.type == EV_ABS && x.code == ABS_HAT0Y)
                        keys = (keys & ~(KeyUp | KeyDown)) | (x.value < 0 ? KeyUp : x.value > 0 ? KeyDown : 0);
                    if (x.type != EV_KEY || x.value == 2) continue;
                    if (menu_on && is_menu_button(x.code)) {  // (opens on release: nothing of it reaches the game)
                        if (x.value) menu_down = true;
                        else if (menu_down) {
                            menu_down = false;
                            open_menu(0);
                        }
                        continue;
                    }
                    if (is_hotkey(x.code)) {
                        hot = x.value != 0;
                        continue;
                    }
                    if (x.code == KEY_ESC && x.value) g_quit = true;
                    const uint32_t b = pad_bit(x.code);
                    if (hot && x.value && b) {  // F + a button: BananaDS's own, not the game's
                        if (b == KeyStart) g_quit = true;
                        else if (b == KeyR) in_save = 1;
                        else if (b == KeyL) in_load = 1;
                        continue;
                    }
                    keys = x.value ? keys | b : keys & ~b;
                }
            }
        }
        in_keys = hot ? 0 : keys;  // while F is held the game sees no buttons
        if (menu_test > 0 && !menu_tested && now_us() - t_start > 3000000) {
            menu_tested = true;
            open_menu((int)menu_test - 1);
        }
        if (g_quit) break;

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
    if (cfg.flag("resume.on_quit", true)) {
        cpugov_boost(5000);
        emu.save_state(0);
    }
    cpugov_stop();
#ifdef BANANADS_ALSA
    alsa_stop(snd);
#endif
    for (auto& d : devs) ::close(d.fd);
    if (qos >= 0) ::close(qos);
    emu.draw_into(nullptr, nullptr);  // (the buffers go with the device)
    dp.close();
    ::close(g_wake);
    g_wake = -1;
    return 0;
}

}  // namespace bananads
