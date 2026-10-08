// libdsflip.so: DraStic's two DS screens scanned out directly on the RG DS's two DSI panels.
//
// DraStic renders each DS screen into an SDL streaming texture (ARGB8888, 256x192 or 512x384)
// via SDL_LockTexture/UnlockTexture, draws both with SDL_RenderCopy, then SDL_RenderPresent.
// This hook hands DraStic *DRM dumb buffers* on Lock (XRGB8888 has the same memory layout as
// SDL's ARGB8888), so DraStic writes straight into scanout memory: no upload, no GL, no copy.
// The VOP2 display controller scales each buffer to 640x480 in hardware.
//
// DraStic's menu is one 800x480 RGB565 streaming texture filled with SDL_UpdateTexture; it is
// copied into an RGB565 dumb buffer and shown on the bottom panel (the top keeps the game frame).
//
// A presenter thread (SCHED_FIFO) owns the atomic commits: both panels' new framebuffers go in ONE
// commit (same emulated frame on both screens). RenderPresent never blocks. Frames wait in a
// one-frame queue and are committed at an adaptive "latch" point in the refresh cycle, opposite
// where DraStic's presents arrive (see arm_latch). With a shader, a worker thread draws each frame
// on the GPU first (shader.c). Audio goes through a pump that paces DraStic exactly (audio.c).
//
// Experiments that measured worse were removed in 1.3 (clock lock + PLL on DraStic's gettimeofday,
// CPU pinning, SCHED_FIFO for DraStic's threads, A/B legacy pacing, fixed latch, audio chunk size);
// the last commit that has them is 5d67d79.
//
// Touch: with SDL's dummy video driver nothing delivers the touchscreen, so a thread reads the
// bottom panel's evdev node and injects mouse events (in DraStic's logical coordinates, inside
// the rectangle the bottom screen was last drawn to) through SDL_PollEvent.
//
// Requirements: DRM master (sway stopped), SDL_VIDEODRIVER=dummy. Env:
//   DSFLIP_TOP=DSI-2               connector that shows the DS top screen
//   DSFLIP_CARD=/dev/dri/cardN     display device (default: the first card with two connected DSI panels)
//   DSFLIP_TOUCH=fe5e0000.i2c      bottom touchscreen (i2c-5; calibrated: raw == panel pixels)
//   DSFLIP_TOUCH_INVERT=x/y/xy     axes to invert (default none)
//   DSFLIP_LOG=path                stats log (default /storage/dsflip/logs/dsflip.log)
//   DSHOOK_SHADER=name             ES's DraStic "shader" setting; anything but bilinear/none runs that shader on the
//                                  GPU (shader.c). DSFLIP_SHADER overrides it
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <glob.h>
#include <signal.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <math.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef DSFLIP_VERSION
#define DSFLIP_VERSION "dev"            /* build.sh passes the repo's VERSION file */
#endif

typedef struct { int x, y, w, h; } SDL_Rect;
#define QMAX 3                          /* deepest frame queue (DSFLIP_QUEUE): frames waiting behind `ready` */
#define NBUF (5 + QMAX)                 /* writing, written, ready + QMAX queued (see enqueue), committed, scanout */
#define NOUT (4 + QMAX)                 /* shader mode's panel buffers: shading, ready + QMAX queued, committed, scanout */
#define FMT_ARGB8888 0x16362004u         /* SDL_PIXELFORMAT_* */
#define FMT_RGB565   0x15151002u
enum { FREE, WRITING, WRITTEN, READY, QUEUED, SCANOUT };
enum { K_SCREEN, K_MENU, K_BLACK };

typedef struct { uint32_t fb, handle, pitch, w, h; uint64_t size; void *map; int state; uint64_t gen;
                 int fence; } dbuf;         /* fence: GPU "done" sync_file for a shaded frame, -1 if none */
typedef struct {                        /* one DraStic texture we scan out */
    void *tex; int kind, w, h;
    dbuf b[NBUF]; int nb;
    int writing, written;               /* index being written / last completed write, -1 if none */
} stex;
typedef struct {                        /* one panel */
    uint32_t conn, crtc, crtc_idx, plane, mode_blob;
    drmModeModeInfo mode;
    uint32_t p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch, p_fence;
    dbuf *ready, *queued, *scan;
    long long last_flip;
    dbuf *src;                          /* shader mode: DraStic's newest finished buffer, not yet shaded */
    long long src_t;                    /* ...and when DraStic presented it */
    int src_held;                       /* ...after a hold on a full queue (DSFLIP_QUEUE_WAIT): not a phase sample */
    dbuf out[NOUT];                     /* shader mode: the buffers the shader draws into (the panel's size, or less:
                                           shader_output_size; the display controller scales them to the panel) */
    dbuf *q[QMAX]; int nq;              /* frame queue: the frames after `ready`, oldest first (see enqueue) */
} panel;

static int fd = -1, efd = -1, ok;
static FILE *lg;
static panel P[2];                      /* [0] = top, [1] = bottom */
static stex T[6]; static int nt;
static stex blk;                        /* black 640x480 buffer for an unused panel */
static int menu_hw;                     /* VOP2 scales the 800x480 menu itself */
/* mu (buffers, panels, pacing) and tmu (the touch/event queue) are priority-inheriting mutexes: the presenter and
 * the audio pump run SCHED_FIFO while DraStic's threads and the RetroAchievements HTTP threads don't, and the old
 * spin lock (sched_yield) could spin above a preempted normal-priority holder until the kernel's RT throttle let
 * it run (up to ~1 s). The int tags only name the lock; the mutexes live in mtx[]. Critical sections are still a
 * few loads/stores plus one non-blocking commit ioctl. */
static int mu, tmu;
static pthread_mutex_t mtx[2];          /* [0] = mu, [1] = tmu */
static pthread_cond_t qcond;            /* with mtx[0]: a commit made room in the frame queue (DSFLIP_QUEUE_WAIT) */
__attribute__((constructor(101))) static void init_locks(void) {   /* before init() and any hook */
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    for (int i = 0; i < 2; i++) pthread_mutex_init(&mtx[i], &a);
    pthread_mutexattr_destroy(&a);
    pthread_condattr_t c; pthread_condattr_init(&c); pthread_condattr_setclock(&c, CLOCK_MONOTONIC);
    pthread_cond_init(&qcond, &c); pthread_condattr_destroy(&c);
}
static long long now_us(void);
static volatile long long lk_wait_us, lk_wait_max, lk_hold_max; static volatile int lk_waits;  /* diagnostics */
static long long lk_t;                  /* when mu was taken (holder only) */
static void lock(int *l) {
    pthread_mutex_t *m = &mtx[l == &tmu];
    if (pthread_mutex_trylock(m) == 0) { if (l == &mu) lk_t = now_us(); return; }
    long long t0 = now_us();
    pthread_mutex_lock(m);
    if (l == &mu) { long long w = now_us() - t0; lk_wait_us += w; lk_waits++; if (w > lk_wait_max) lk_wait_max = w; lk_t = now_us(); }
}
static void unlock_mu_stat(int *l) {
    if (l == &mu && lk_t) { long long h = now_us() - lk_t; if (h > lk_hold_max) lk_hold_max = h; }
}
static void unlock(int *l) { unlock_mu_stat(l); pthread_mutex_unlock(&mtx[l == &tmu]); }
static int logical_w = 512, logical_h = 192;
static stex *pending_route[2];          /* routes recorded by RenderCopy during this frame */
static SDL_Rect route_dst[2]; static int touch_rect_ok;   /* touch only while a DS screen is on the bottom panel */
static int menu_touch;                                     /* DraStic's menu is on the bottom panel (no touch: see touch_emit) */
static int vp_x, vp_y, vp_w, vp_h;                         /* where a shader draws the DS screen on the panel (0x0: all of it) */
static int touch_outside;                                  /* the current touch began outside that rectangle: ignore it */
int shader_viewport(int *v, int pw, int ph);
int shader_output_size(int *w, int *h, int pw, int ph);
static void *window;
static int cursor_log;                  /* DSFLIP_CURSOR_LOG=1: log where DraStic draws its 32x32 stylus cursor */
/* stats */
static int st_present, st_commit, st_drop, st_busy, st_flips[2], st_touch;
volatile int dsflip_presents;           /* SDL_RenderPresent calls since start (cpugov.c) */
volatile long long dsflip_frame_work_max; /* the heaviest frame's CPU time on DraStic's main thread (ns) since
                                           cpugov.c last took it: a frame over 16.7 ms is a late frame */
volatile int dsflip_queue_drops;        /* frames dropped from the queue since start (cpugov.c) */
volatile int dsflip_screen_w;           /* DraStic's screen texture width, 256 or 512 (cpugov.c: 1x or 2x) */
void cpugov_start(void);
void resume_start(void); void resume_frame(void); int resume_poll(void *e); void resume_saw_event(const void *e);   /* resume.c */
int menu_event(void *e); void menu_frame(void); int menu_touch_event(int down_change, int down, int x, int y, int xmax, int ymax);   /* menu.c */
volatile int dsflip_hold;               /* menu.c: DraStic's frames are dropped, not shown (a load runs behind the menu's screen) */
static int st_drop_src, st_drop_q, st_drop_buf;
static long long st_evt_max, st_c2f_max; static int st_c2f_long; /* vblank->event delivery; commit->flip (>1 refresh) */   /* drops by cause: replaced before shading, queue overflow, no buffer */
/* shader pass (shader.c) */
const char *shader_name(void);
int shader_init(int fd, const char *name);
int shader_draw(uint32_t sh, int sw, int shh, int sp, uint64_t sgen, uint32_t dh, int dw, int dhh, int dp, uint64_t dgen, int finish);
int shader_fence(void);
int shader_imports(void);
void audio_pump_log(void);
void audio_mic_start(void);
extern int shader_copy_mode;
int shader_draw_mem(int panel, const void *px, int sw, int shh, int sp, uint32_t dh, int dw, int dhh, int dp, uint64_t dgen, int finish);
static const char *shader_nm;
static volatile int shader_on, shader_done;
static void set_fifo(int prio) {
    struct sched_param sp = { .sched_priority = prio };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) && lg) fprintf(lg, "[dsflip] SCHED_FIFO %d failed: %s\n", prio, strerror(errno));
}
static long long st_shade_sum, st_shade_max; static int st_shade_n;
static long long st_t0, st_iv_max[2];

#define LOG(...) do { if (lg) fprintf(lg, __VA_ARGS__); } while (0)
#define REAL(ret, fn, ...) static ret (*real)(__VA_ARGS__); if (!real) real = dlsym(RTLD_NEXT, #fn)

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }

static uint32_t prop(uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; pr && i < pr->count_props && !id; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[i]);
        if (p && !strcmp(p->name, name)) id = p->prop_id;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return id;
}
static uint64_t propval(uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *pr = drmModeObjectGetProperties(fd, obj, type);
    uint64_t v = 0;
    for (uint32_t i = 0; pr && i < pr->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, pr->props[i]);
        if (p && !strcmp(p->name, name)) v = pr->prop_values[i];
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(pr);
    return v;
}
static uint64_t gens;                   /* buffer generations (the shader's image cache key) */
/* shader copy mode: DraStic's screen buffers are plain cached memory (like SDL's), uploaded by the presenter */
static int mkmem(dbuf *b, uint32_t w, uint32_t h) {
    memset(b, 0, sizeof *b);
    b->pitch = w * 4; b->w = w; b->h = h; b->size = (uint64_t)b->pitch * h;
    if (!(b->map = aligned_alloc(64, b->size))) return -1;
    memset(b->map, 0, b->size);
    b->state = FREE; b->gen = ++gens; b->fence = -1;
    return 0;
}
static int mkbuf(dbuf *b, uint32_t w, uint32_t h, uint32_t fourcc, uint32_t bpp) {
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = bpp };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return -1;
    b->handle = c.handle; b->pitch = c.pitch; b->size = c.size; b->w = w; b->h = h;
    uint32_t hs[4] = { c.handle }, ps[4] = { c.pitch }, os[4] = { 0 };
    if (drmModeAddFB2(fd, w, h, fourcc, hs, ps, os, &b->fb, 0)) return -1;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
    b->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    if (b->map == MAP_FAILED) return -1;
    memset(b->map, 0, c.size);
    b->state = FREE;
    b->gen = ++gens; b->fence = -1;
    return 0;
}
static void freebuf(dbuf *b) {
    if (!b->map) return;
    if (!b->handle) { free(b->map); memset(b, 0, sizeof *b); b->fence = -1; return; }   /* mkmem buffer */
    munmap(b->map, b->size); drmModeRmFB(fd, b->fb);
    struct drm_mode_destroy_dumb d = { .handle = b->handle }; drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    memset(b, 0, sizeof *b); b->fence = -1;
}

static void plane_props(panel *p) {
    uint32_t o = DRM_MODE_OBJECT_PLANE, id = p->plane;
    p->p_fb = prop(id, o, "FB_ID"); p->p_crtc = prop(id, o, "CRTC_ID"); p->p_fence = prop(id, o, "IN_FENCE_FD");
    p->p_sx = prop(id, o, "SRC_X"); p->p_sy = prop(id, o, "SRC_Y"); p->p_sw = prop(id, o, "SRC_W"); p->p_sh = prop(id, o, "SRC_H");
    p->p_cx = prop(id, o, "CRTC_X"); p->p_cy = prop(id, o, "CRTC_Y"); p->p_cw = prop(id, o, "CRTC_W"); p->p_ch = prop(id, o, "CRTC_H");
}
static void add_fb(drmModeAtomicReq *r, panel *p, dbuf *b) {
    drmModeAtomicAddProperty(r, p->plane, p->p_fb, b->fb);
    drmModeAtomicAddProperty(r, p->plane, p->p_crtc, p->crtc);
    drmModeAtomicAddProperty(r, p->plane, p->p_sx, 0);
    drmModeAtomicAddProperty(r, p->plane, p->p_sy, 0);
    drmModeAtomicAddProperty(r, p->plane, p->p_sw, (uint64_t)b->w << 16);
    drmModeAtomicAddProperty(r, p->plane, p->p_sh, (uint64_t)b->h << 16);
    drmModeAtomicAddProperty(r, p->plane, p->p_cx, 0);
    drmModeAtomicAddProperty(r, p->plane, p->p_cy, 0);
    drmModeAtomicAddProperty(r, p->plane, p->p_cw, p->mode.hdisplay);
    drmModeAtomicAddProperty(r, p->plane, p->p_ch, p->mode.vdisplay);
    if (p->p_fence && b->fence >= 0) drmModeAtomicAddProperty(r, p->plane, p->p_fence, b->fence);  /* flip once the GPU is done */
}

void ra_frame(void);

void dsflip_log(const char *fmt, ...) {
    if (!lg) return;
    va_list ap; va_start(ap, fmt); vfprintf(lg, fmt, ap); va_end(ap);
}

/* ---------- presenter ---------- */
static int pending_mask;                /* bit i: a flip event is still due for panel i's CRTC */

/* Pacing. DraStic finishes frames on its own 60.000 Hz clock, not in step with the panels' vblank. Showing each
 * frame at the very next vblank ("immediate") makes the vblank itself the cut-off: when DraStic's presents drift
 * into that phase, per-frame timing jitter puts two frames into one refresh (one dropped) and none into the next
 * (one repeated). "latch" mode instead measures where in the refresh cycle presents arrive (circular average,
 * tracking the slow drift between the two clocks) and commits the newest frame at the opposite phase, so the
 * cut-off sits as far from the presents as possible. DSFLIP_PACING=immediate restores the old behaviour. */
static int pacing_latch = 1, tfd = -1;
static long long vbl_ref;               /* a recent top-panel vblank (flip timestamp, CLOCK_MONOTONIC us) */
static double period = 16666.7;         /* measured refresh period, us */
static double ph_c, ph_s;               /* EMA of cos/sin of the present phase */
static int ph_n, ph_hist[16], st_late;  /* samples; histogram of present phases (logged); commits that missed */
static double latch_off = 14000;        /* current latch point, us after vblank */
static long long ready_since;           /* when the oldest uncommitted frame arrived */
static long long commit_t, commit_vref; /* last commit time and the vblank reference at that moment */
static int st_miss;                     /* commits that reached the screen a vblank later than intended */
#define LATCH_MARGIN 1300               /* starting commit margin before vblank; measured on the top panel: 600 us always
                                           makes it, 300 us doesn't. Grows on late commits (latch_margin) */
static double margin_base = LATCH_MARGIN;   /* DSFLIP_LATCH_MARGIN=<us>: the floor the adaptive margin returns to (tests) */
static int st_unfenced, commit_unfenced;    /* commits whose frame the GPU hadn't finished yet (shader mode); the last was */
static double latch_margin = LATCH_MARGIN;  /* adaptive: +400 us per latch that found the previous flip still pending,
                                               -400 us per clean 10 s */
static double bot_off;                  /* bottom panel's vblank phase relative to the top's, us in (-P/2, P/2] */
static int bot_n, latch_skipped;        /* bottom phase samples; a latch was skipped because a flip was pending */
static int in_latch, commit_latch;      /* a commit is being made at the latch point; the last commit was */
/* late-latch diagnostics: which kind of commit the latch found still pending, on which panel(s), and when in the
 * cycle it was made. Kinds: L latch, C catch-up after a flip, S safety net, T overlay only, I immediate/warm-up */
static char commit_src = 'I', last_src = 'I';
static int st_late_src[5], st_late_mask[4], st_safety, st_late_unf; static double st_late_ph[8]; static int st_late_n;
static int src_idx(char c) { return c == 'L' ? 0 : c == 'C' ? 1 : c == 'S' ? 2 : c == 'T' ? 3 : 4; }


static void drop_fence(dbuf *b) { if (b && b->fence >= 0) { close(b->fence); b->fence = -1; } }
static void freebuf(dbuf *b);
/* graveyard: buffers whose texture slot was recycled while a panel still held them (ready/queued/on screen).
 * They move here, the panel pointers follow, and the flip that retires them frees them. Without this the slot's
 * struct was zeroed and re-allocated under the panel's pointer: a commit with fb 0, or a buffer marked FREE while
 * DraStic was already writing into its replacement. */
#define NGRAVE 24
static dbuf grave[NGRAVE];
static int is_grave(dbuf *b) { return b >= grave && b < grave + NGRAVE; }
static void release(dbuf *b) {
    if (!b || b == &blk.b[0]) return;
    b->state = FREE; drop_fence(b);
    if (is_grave(b)) freebuf(b);        /* retired: nothing points at it any more */
}

/* toast: a small overlay plane on the top panel (RetroAchievements pop-ups), committed together with the
 * game frames. Rendered into one of two buffers, never the one being scanned out. */
/* 640x72 on a 640x480 panel; on a wider panel (RG DS Plus, 1024x768) the same card scaled up: 1024x115. ui.c scales
 * its layout by the height. Set from the top panel's mode before the buffers are made. */
static int TOAST_W = 640, TOAST_H = 72;
static uint32_t tp_plane, tp_fb, tp_crtc, tp_sx, tp_sy, tp_sw, tp_sh, tp_cx, tp_cy, tp_cw, tp_ch;
static dbuf toast[2];
static int toast_cur, toast_shown, toast_want, toast_dirty;
/* the overlay drops in from the top edge and slides back up: each commit shows the card's bottom `rows` rows at the
 * top of the panel (SRC_Y/SRC_H and CRTC_H change, no scaling), eased over TOAST_IN_US / TOAST_OUT_US */
#define TOAST_IN_US 280000
#define TOAST_OUT_US 200000
static int anim_dir;                    /* +1 dropping in, -1 sliding out, 0 still */
static int anim_log;                    /* DSFLIP_UI_DEMO: log the rows of every animated commit */
static long long anim_t0;
static long long now_us(void);
static int toast_rows(long long t) {    /* with mu held; ends the animation when it's done */
    if (!anim_dir) return TOAST_H;
    double e = (double)(t - anim_t0) / (anim_dir > 0 ? TOAST_IN_US : TOAST_OUT_US);
    if (e >= 1.0) { int out = anim_dir < 0; if (out) toast_want = 0; anim_dir = 0; return out ? 0 : TOAST_H; }
    if (e < 0) e = 0;
    double v = anim_dir > 0 ? 1.0 - (1.0 - e) * (1.0 - e) * (1.0 - e) : 1.0 - e * e;   /* ease out / ease in */
    return (int)(v * TOAST_H + 0.5);
}

static void add_toast(drmModeAtomicReq *r) {
    int rows = toast_want ? toast_rows(now_us()) : 0;
    if (rows < 4 && anim_dir < 0) { toast_want = 0; anim_dir = 0; }         /* too thin to show: slid out */
    if (rows < 4) rows = 4;
    if (anim_log && (anim_dir || rows != TOAST_H)) LOG("[ui] overlay rows %d%s\n", rows, toast_want ? "" : " (off)");
    if (toast_want) {
        drmModeAtomicAddProperty(r, tp_plane, tp_fb, toast[toast_cur].fb);
        drmModeAtomicAddProperty(r, tp_plane, tp_crtc, P[0].crtc);
        drmModeAtomicAddProperty(r, tp_plane, tp_sx, 0);
        drmModeAtomicAddProperty(r, tp_plane, tp_sy, (uint64_t)(TOAST_H - rows) << 16);
        drmModeAtomicAddProperty(r, tp_plane, tp_sw, (uint64_t)TOAST_W << 16);
        drmModeAtomicAddProperty(r, tp_plane, tp_sh, (uint64_t)rows << 16);
        drmModeAtomicAddProperty(r, tp_plane, tp_cx, 0);
        drmModeAtomicAddProperty(r, tp_plane, tp_cy, 0);
        drmModeAtomicAddProperty(r, tp_plane, tp_cw, TOAST_W);
        drmModeAtomicAddProperty(r, tp_plane, tp_ch, rows);
    } else {
        drmModeAtomicAddProperty(r, tp_plane, tp_fb, 0);
        drmModeAtomicAddProperty(r, tp_plane, tp_crtc, 0);
    }
}

/* frame queue (with mu held): the oldest frame waiting behind panel i's `ready`, or 0 */
static int queue_depth = 1;             /* DSFLIP_QUEUE: 0 = mailbox, 1..QMAX frames */
static int queue_wait_us;               /* DSFLIP_QUEUE_WAIT=<ms>: hold DraStic's present this long at most while the
                                           queue is full, instead of dropping its oldest frame (0: off) */
static int st_qwait_n; static long long st_qwait_us, st_qwait_max;   /* presents held, total and longest hold */
/* a held present is let go by a commit, so it arrives at the latch point: counting it in the phase average made the
 * latch move away from its own releases, the held frames follow, and so on (arrival spread R -> 0, latch pinned at
 * an edge, holds running into the limit and dropping after all: measured in 2 of 3 launches). Held frames don't
 * count (panel.src_held carries it to shade_pending in shader mode). */
static int st_wait[QMAX + 2], st_fcommit; /* frame commits by frames left waiting behind them; frame commits */
static dbuf *dequeue(int i) {
    if (!P[i].nq) return 0;
    dbuf *b = P[i].q[0];
    memmove(P[i].q, P[i].q + 1, (size_t)--P[i].nq * sizeof P[i].q[0]);
    return b;
}

static void try_commit(void) {         /* called with mu held */
    char src = commit_src; commit_src = 'I';   /* the caller's kind, for this attempt only */
    if (pending_mask) return;
    int mask = 0;
    for (int i = 0; i < 2; i++) if (P[i].ready) mask |= 1 << i;
    int with_toast = tp_plane && toast_dirty;
    if (with_toast) mask |= 1;          /* the toast plane lives on the top panel's CRTC */
    if (!mask) return;
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    int unf = 0;                        /* diagnostics: is the GPU still drawing a frame we're about to show? */
    for (int i = 0; i < 2; i++) if (P[i].ready && P[i].ready->fence >= 0) {
        struct pollfd pf = { .fd = P[i].ready->fence, .events = POLLIN };
        if (poll(&pf, 1, 0) == 0) unf = 1;
    }
    for (int i = 0; i < 2; i++) if (P[i].ready) add_fb(r, &P[i], P[i].ready);
    if (with_toast) add_toast(r);
    int ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, P);
    drmModeAtomicFree(r);
    if (ret) {
        st_busy++;
        if (ret != -EBUSY) {           /* rejected config: drop the frame rather than retry forever */
            LOG("[dsflip] commit rejected: %s%s\n", strerror(-ret), with_toast ? " (toasts disabled)" : "");
            if (with_toast) { tp_plane = 0; toast_dirty = 0; }
            for (int i = 0; i < 2; i++) { release(P[i].ready); P[i].ready = 0; while (P[i].nq) release(P[i].q[--P[i].nq]); }
        }
        return;
    }
    int waiting = 0, frames = 0;
    for (int i = 0; i < 2; i++) if (P[i].ready) {
        drop_fence(P[i].ready); P[i].queued = P[i].ready; P[i].queued->state = QUEUED; frames = 1;
        P[i].ready = dequeue(i);        /* the oldest queued frame is next */
        if (P[i].ready && 1 + P[i].nq > waiting) waiting = 1 + P[i].nq;
    }
    if (frames) { st_wait[waiting]++; st_fcommit++; }   /* frames left waiting behind this one = refreshes of latency */
    if (frames && queue_wait_us) pthread_cond_broadcast(&qcond);   /* a present held on a full queue may go on */
    if (P[0].ready || P[1].ready) ready_since = now_us();
    if (with_toast) { toast_dirty = 0; toast_shown = toast_want; }
    commit_t = now_us(); commit_vref = vbl_ref; commit_latch = in_latch; last_src = src; commit_unfenced = unf; st_unfenced += unf;
    pending_mask = mask;                /* one flip event per CRTC in the commit */
    st_commit++;
}

/* where in the refresh cycle (after the top panel's vblank) `t` is, and the last point of the cycle at which a commit
 * still makes both panels' next vblanks (arm_latch's upper bound) */
static double phase_of(long long t) { double ph = fmod((double)(t - vbl_ref), period); return ph < 0 ? ph + period : ph; }
static double latch_hi(void) { return period + fmin(0, bot_off) - latch_margin; }

static void on_flip(int f, unsigned seq, unsigned sec, unsigned usec, unsigned crtc, void *u) {
    (void)f; (void)seq; (void)u;
    lock(&mu);
    for (int i = 0; i < 2; i++) if (P[i].crtc == crtc) {
        pending_mask &= ~(1 << i);
        long long t = sec * 1000000LL + usec;
        { long long e = now_us() - t; if (e > st_evt_max) st_evt_max = e; }
        if (i == 0 && commit_t) { long long c = t - commit_t; if (c > st_c2f_max) st_c2f_max = c; if (c > (long long)period) st_c2f_long++; }
        if (i == 0) {
            if (commit_t && commit_vref && P[0].queued) {  /* did the commit make the first vblank after it? */
                double c = fmod((double)(commit_t - commit_vref), period); if (c < 0) c += period;
                if ((double)(t - commit_t) > period - c + period / 2) st_miss++;
            }
            if (vbl_ref) {
                double iv = (double)(t - vbl_ref), k = floor(iv / period + 0.5);
                if (k >= 1 && k <= 4 && fabs(iv / k - period) < 300) period += 0.02 * (iv / k - period);
            }
            vbl_ref = t;
        } else if (vbl_ref) {           /* bottom panel: learn its vblank phase relative to the top's */
            double d = remainder((double)(t - vbl_ref), period);
            bot_off = bot_n++ < 10 ? d : bot_off + 0.05 * (d - bot_off);
        }
        if (!P[i].queued) continue;     /* toast-only flip on this CRTC */
        if (P[i].last_flip && t - P[i].last_flip > st_iv_max[i]) st_iv_max[i] = t - P[i].last_flip;
        P[i].last_flip = t; st_flips[i]++;
        if (P[i].scan != P[i].queued) release(P[i].scan);
        P[i].scan = P[i].queued; P[i].scan->state = SCANOUT; P[i].queued = 0;
    }
    if (!pending_mask && (!pacing_latch || ph_n < 120 || latch_skipped)) {
        /* a skipped latch catches up now, but only while a commit still makes both panels' next vblanks: later in
         * the cycle (after the bottom panel's flip, when its vblank comes first) it misses one, the next latch finds
         * that flip pending, catches up after it again, and so on: every latch "late" from then on, and the margin
         * grew each time until the latch zone was squeezed into the frames' arrivals (drop storms, measured) */
        if (latch_skipped && pacing_latch && ph_n >= 120 && vbl_ref && phase_of(now_us()) > latch_hi()) latch_skipped = 0;
        else { commit_src = latch_skipped ? 'C' : 'I'; latch_skipped = 0; try_commit(); }
    }
    unlock(&mu);
}

/* the overlay plane is drawn by ui.c (RetroAchievements pop-ups and progress, in the DSi font) on its own thread:
 * begin hands it the buffer that isn't on screen (TOAST_W x TOAST_H ARGB8888, premultiplied), end shows it or hides
 * the plane; the presenter commits it with the next frame (or at once if no frame is coming) */
static int ov_next;
uint32_t *dsflip_overlay_begin(int *pitch, int *w, int *h) {
    if (!ok || !tp_plane) return 0;
    lock(&mu); ov_next = toast_shown ? !toast_cur : toast_cur; unlock(&mu);
    *pitch = (int)toast[ov_next].pitch; *w = TOAST_W; *h = TOAST_H;
    return toast[ov_next].map;
}
void dsflip_overlay_end(int show) {
    lock(&mu);
    if (show) {
        toast_cur = ov_next;
        if (!toast_want || anim_dir < 0) {  /* not on screen (or leaving): drop in, from where it is if it was leaving */
            long long t = now_us(), from = 0;
            if (anim_dir < 0 && toast_want) { double v = (double)toast_rows(t) / TOAST_H; from = (long long)((1.0 - cbrt(1.0 - v)) * TOAST_IN_US); }
            anim_dir = 1; anim_t0 = t - from;
        }
        toast_want = 1;
    } else if (toast_want && anim_dir >= 0) { anim_dir = -1; anim_t0 = now_us(); }   /* slide out; off when done */
    toast_dirty = 1;
    unlock(&mu);
    uint64_t one = 1; if (write(efd, &one, 8) < 0) {}
}
void ui_popup(const char *l1, const char *l2, const char *badge_png, uint32_t accent, int ms);
void volume_start(void);
/* public: a two-line pop-up without a badge (thread-safe) */
void dsflip_toast(const char *l1, const char *l2, uint32_t accent, int ms) {
    if (!ok || !tp_plane) return;
    ui_popup(l1, l2, 0, accent, ms);
}

/* DraStic's menu opened: a card on the top panel with the time and the battery, like ROCKNIX's status bar in its
 * menus (the game is paused, so this is when the player looks). DSFLIP_STATUS_CARD=0: off. */
static int battery_pct(int *charging) {
    glob_t g; int pct = -1; *charging = 0;
    if (glob("/sys/class/power_supply/*/type", 0, 0, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && pct < 0; i++) {
            char t[32] = "", p[300]; FILE *f = fopen(g.gl_pathv[i], "r");
            if (f) { if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f); }
            if (strncmp(t, "Battery", 7)) continue;
            size_t n = strlen(g.gl_pathv[i]) - 4;                 /* ".../type" -> ".../" */
            snprintf(p, sizeof p, "%.*scapacity", (int)n, g.gl_pathv[i]);
            if ((f = fopen(p, "r"))) { if (fscanf(f, "%d", &pct) != 1) pct = -1; fclose(f); }
            snprintf(p, sizeof p, "%.*sstatus", (int)n, g.gl_pathv[i]);
            if ((f = fopen(p, "r"))) { char st[32] = ""; if (fgets(st, sizeof st, f)) *charging = !strncmp(st, "Charging", 8) || !strncmp(st, "Full", 4); fclose(f); }
        }
        globfree(&g);
    }
    return pct;
}
int menu_opened_drastic(void);
static void status_card(void) {
    const char *e = getenv("DSFLIP_STATUS_CARD"); if (e && *e == '0') return;
    if (menu_opened_drastic()) return;             /* from the in-game menu (menu.c), which showed them already */
    char l1[64], l2[96]; time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
    strftime(l1, sizeof l1, "%H:%M", &lt);
    int chg, pct = battery_pct(&chg);
    if (pct >= 0) snprintf(l2, sizeof l2, "Battery %d%%%s", pct, chg ? ", charging" : "");
    else snprintf(l2, sizeof l2, "Paused");
    dsflip_toast(l1, l2, pct >= 0 && pct <= 15 && !chg ? 0xe04040 : 0x3aa0ff, 3000);
}

/* SIGUSR2: dump what each panel scans out to <logdir>/scan<i>.raw (header "w h pitch bpp\n" + pixels) */
static volatile int want_dump, touch_dumps;   /* DSFLIP_TOUCH_DUMP=N: dump the bottom screen at the first N touch-downs */
static int tdump_x, tdump_y, tdump_n;
static void on_usr2(int sig) { (void)sig; want_dump = 1; }
static void dump_scan(void) {
    for (int i = 0; i < 2; i++) {
        dbuf *b = P[i].scan; if (!b || !b->map) continue;
        char fn[96]; snprintf(fn, sizeof fn, "/storage/dsflip/logs/scan%d.raw", i);
        FILE *f = fopen(fn, "wb"); if (!f) continue;
        fprintf(f, "%u %u %u %u\n", b->w, b->h, b->pitch, (uint32_t)(b->pitch / b->w * 8));
        fwrite(b->map, 1, (size_t)b->pitch * b->h, f); fclose(f);
    }
    if (tp_plane && toast_shown) {
        dbuf *b = &toast[toast_cur];
        FILE *f = fopen("/storage/dsflip/logs/toast.raw", "wb");
        if (f) { fprintf(f, "%u %u %u 32\n", b->w, b->h, b->pitch); fwrite(b->map, 1, (size_t)b->pitch * b->h, f); fclose(f); }
    }
    LOG("[dsflip] dumped scanout buffers\n");
}

static void dump_touch(void) {         /* bottom panel's scanout + the DS coordinate we sent */
    dbuf *b = P[1].scan; if (!b || !b->map) return;
    char fn[96]; snprintf(fn, sizeof fn, "/storage/dsflip/logs/touch%d.raw", tdump_n);
    FILE *f = fopen(fn, "wb"); if (!f) return;
    fprintf(f, "%u %u %u %u %d %d\n", b->w, b->h, b->pitch, (uint32_t)(b->pitch / b->w * 8), tdump_x, tdump_y);
    fwrite(b->map, 1, (size_t)b->pitch * b->h, f); fclose(f);
}


static void arm_latch(void) {         /* with mu held: next latch point after now */
    if (tfd < 0 || !vbl_ref) return;
    if (ph_n >= 120) {
        double mean = atan2(ph_s, ph_c) / (2 * M_PI) * period;          /* where presents cluster */
        double l = fmod(mean + period / 2 + 2 * period, period);        /* opposite phase */
        /* forbidden: latch_margin before the earlier panel's vblank (the commit wouldn't make it on both CRTCs)
         * to a sliver after the later one's. The bottom panel's vblank is ~0.8 ms off the top's; ignoring that made
         * one panel miss every commit near the edge, which kept a flip pending at every latch: 30 fps bursts. */
        /* lo: the previous flip's event must have arrived before the latch, or the latch counts as late */
        double lo = fmax(0, bot_off) + 1000, hi = latch_hi();
        if (l < lo || l > hi) {         /* pick the allowed edge that is farther (circularly) from the presents... */
            double m = fmod(mean + 2 * period, period);
            double dlo = fabs(remainder(lo - m, period)), dhi = fabs(remainder(hi - m, period));
            int at_lo = fabs(latch_off - lo) < 1, at_hi = fabs(latch_off - hi) < 1;
            if (at_lo && dlo > dhi - 1500) l = lo;          /* ...with hysteresis: switching edges moves the cut-off */
            else if (at_hi && dhi > dlo - 1500) l = hi;     /* across the vblank, so only do it when clearly better */
            else l = dlo > dhi ? lo : hi;
        }
        latch_off = l;
    }
    long long now = now_us();
    double k = ceil(((double)(now - vbl_ref) - latch_off + 300) / period);
    long long at = vbl_ref + (long long)(k * period + latch_off);
    struct itimerspec its = { { 0, 0 }, { at / 1000000, (at % 1000000) * 1000 } };
    timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, 0);
}

/* shader mode: draw each panel's newest DraStic buffer into a free output buffer, then publish both together
 * (one commit shows the same emulated frame on both screens). GL work happens with mu released. */
/* diagnostics: DraStic's raw present timing (before any shading), and present -> ready delay in shader mode */
static double raw_c, raw_s; static int raw_hist[16], iv_hist[7]; static long long last_present;
static long long lat_sum, lat_max; static int lat_n;
static void note_raw(long long tp) {    /* with mu held */
    if (last_present) { long long iv = tp - last_present; int b = (int)(iv / 4000); iv_hist[b > 6 ? 6 : b]++; }
    last_present = tp;
    if (!vbl_ref) return;
    double ph = fmod((double)(tp - vbl_ref), period); if (ph < 0) ph += period;
    double ang = ph / period * 2 * M_PI;
    raw_c += 0.01 * (cos(ang) - raw_c); raw_s += 0.01 * (sin(ang) - raw_s);
    raw_hist[(int)(ph / period * 16) & 15]++;
}

static void note_phase(long long tp) {  /* with mu held: where in the refresh cycle a frame became ready */
    if (!vbl_ref) return;
    double ph = fmod((double)(tp - vbl_ref), period); if (ph < 0) ph += period;
    double ang = ph / period * 2 * M_PI, a = ph_n < 120 ? 1.0 / (ph_n + 1) : 0.01;
    ph_c += a * (cos(ang) - ph_c); ph_s += a * (sin(ang) - ph_s); ph_n++;
    ph_hist[(int)(ph / period * 16) & 15]++;
}

/* frame queue (DSFLIP_QUEUE=N frames, 0: mailbox). DraStic's frame times are uneven at 2x (measured: intervals
 * alternate ~13 / ~21 ms), so two frames can arrive between two commits. A mailbox shows only the newest and drops
 * the other (a visible hitch); the queue keeps up to N more frames and shows them in order, one per refresh, which
 * turns uneven arrival into even display at the cost of a refresh of latency per waiting frame. A frame that finds
 * the queue full drops the oldest, so latency never grows beyond N refreshes. A deeper queue also rides out a
 * frame that takes longer than a refresh (the waiting frames cover the gap) instead of a higher CPU clock doing it.
 * With mu held. */
static void enqueue(int i, dbuf *b, long long t) {
    if (!P[i].ready) { if (!P[0].ready && !P[1].ready) ready_since = t; P[i].ready = b; return; }
    if (P[i].nq < queue_depth) { P[i].q[P[i].nq++] = b; return; }
    release(P[i].ready); st_drop++; st_drop_q++; dsflip_queue_drops++;
    if (queue_depth) { P[i].ready = dequeue(i); P[i].q[P[i].nq++] = b; } else P[i].ready = b;
}

static void shade_pending(void) {
    dbuf *src[2] = { 0, 0 }, *dst[2] = { 0, 0 }; long long st[2] = { 0, 0 }; int held_any = 0;
    lock(&mu);
    for (int i = 0; i < 2; i++) {
        if (!(src[i] = P[i].src)) continue;
        st[i] = P[i].src_t; held_any |= P[i].src_held;
        P[i].src = 0;
        for (int k = 0; k < NOUT && !dst[i]; k++) if (P[i].out[k].state == FREE) dst[i] = &P[i].out[k];
        if (!dst[i] || !src[i]->map) { release(src[i]); src[i] = 0; st_drop++; st_drop_buf++; continue; }
        dst[i]->state = WRITING;
    }
    unlock(&mu);
    if (!src[0] && !src[1]) return;
    /* both draws are queued, then one fence covers them: the display controller waits for the GPU (IN_FENCE_FD)
     * instead of this thread, so the GPU time stays out of the pacing path. No fence support: wait here. */
    int bad[2] = { 0, 0 }, last = src[1] ? 1 : 0, fence = -1;
    long long t0 = now_us();
    for (int i = 0; i < 2; i++) if (src[i]) {
        dbuf *a = src[i], *b = dst[i];
        bad[i] = (a->handle ? shader_draw(a->handle, a->w, a->h, a->pitch, a->gen, b->handle, b->w, b->h, b->pitch, b->gen,
                                          i == last && !P[0].p_fence)
                            : shader_draw_mem(i, a->map, a->w, a->h, a->pitch, b->handle, b->w, b->h, b->pitch, b->gen,
                                              i == last && !P[0].p_fence)) != 0;
    }
    if (P[0].p_fence) fence = shader_fence();
    for (int i = 0; i < 2; i++) if (src[i] && !bad[i] && fence >= 0) dst[i]->fence = dup(fence);
    if (fence >= 0) close(fence);
    long long dt = now_us() - t0;
    st_shade_sum += dt; st_shade_n++; if (dt > st_shade_max) st_shade_max = dt;
    lock(&mu);
    long long tr = now_us();
    if (!held_any) note_phase(tr);
    for (int i = 0; i < 2; i++) if (src[i] && st[i]) {
        long long l = tr - st[i]; lat_sum += l; lat_n++; if (l > lat_max) lat_max = l;
    }
    for (int i = 0; i < 2; i++) if (src[i]) {
        release(src[i]);
        if (bad[i]) { dst[i]->state = FREE; st_drop++; continue; }
        dst[i]->state = READY;
        enqueue(i, dst[i], tr);
    }
    unlock(&mu);
}

/* shader mode: the GL work runs on its own thread. Measured: with it on the presenter, flip events waited
 * behind uploads and draws (vblank -> event handled up to 47 ms late), latches then found the previous flip
 * "still pending" and the display fell into 30 fps bursts. The presenter now only handles DRM events, the latch
 * timer and commits; this worker shades each frame DraStic presents (woken via wfd) and then wakes the presenter. */
static int wfd = -1;
static void *shader_worker(void *a) {
    (void)a;
    int on = shader_init(fd, shader_nm);           /* GL lives on this thread; init() waits for the verdict */
    if (on) {
        /* the buffers it draws into, at the size the shader asked for (shader_output_size: the panel's unless its
         * source says "dsflip-output: Nx"; a smaller buffer is scaled to the panel by the display controller, as
         * DraStic's own buffers are without a shader). Under mu: mkbuf's generation counter is shared with
         * SDL_CreateTexture on DraStic's thread. */
        lock(&mu);
        for (int i = 0; i < 2 && on; i++) {
            int ow, oh, pw = P[i].mode.hdisplay, ph = P[i].mode.vdisplay;
            int scaled = shader_output_size(&ow, &oh, pw, ph);
            for (int k = 0; k < NOUT && on; k++)
                if (mkbuf(&P[i].out[k], ow, oh, DRM_FORMAT_XRGB8888, 32)) { LOG("[dsflip] shader output buffers %dx%d: alloc failed\n", ow, oh); on = 0; }
            if (on && (i == 0 || pw != P[0].mode.hdisplay || ph != P[0].mode.vdisplay)) {
                if (scaled) LOG("[dsflip] shader output: %dx%d per panel, scaled to %dx%d by the display controller\n", ow, oh, pw, ph);
                else LOG("[dsflip] shader output: %dx%d per panel\n", ow, oh);
            }
        }
        unlock(&mu);
    }
    shader_on = on;
    if (!shader_on) LOG("[dsflip] shader \"%s\" unavailable: zero-copy\n", shader_nm);
    __sync_synchronize();                          /* the output sizes and shader_on, before shader_done is seen */
    shader_done = 1;
    if (!shader_on) return 0;
    struct pollfd pw = { .fd = wfd, .events = POLLIN };
    for (;;) {
        if (poll(&pw, 1, 100) <= 0) continue;
        uint64_t v; if (read(wfd, &v, 8) < 0) {}
        shade_pending();
        uint64_t one = 1; if (write(efd, &one, 8) < 0) {}
    }
    return 0;
}

static void stall_watch(long long t);
static void *presenter(void *a) {
    (void)a;
    /* the presenter reacts to vblank events and the latch timer; its work per wake-up is tiny, but a late wake-up
     * makes the next latch find the previous flip "pending" (measured: events handled up to 7-9 ms late in shader
     * mode at normal priority). So it always runs SCHED_FIFO, below the audio pump (20). DSFLIP_PRESENTER_RT=0: off */
    { const char *pr = getenv("DSFLIP_PRESENTER_RT"); if (!(pr && *pr == '0')) set_fifo(10); }
    if (shader_nm) { pthread_t th; if (!pthread_create(&th, 0, shader_worker, 0)) pthread_setname_np(th, "dsf-shader"); }   /* it sets shader_done */
    else shader_done = 1;
    drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
    struct pollfd pf[3] = { { .fd = fd, .events = POLLIN }, { .fd = efd, .events = POLLIN }, { .fd = tfd, .events = POLLIN } };
    long long st10 = now_us();
    for (;;) {
        if (poll(pf, tfd >= 0 ? 3 : 2, 50) < 0) continue;
        if (pf[0].revents & POLLIN) drmHandleEvent(fd, &ev);
        if (pf[1].revents & POLLIN) {
            uint64_t v; if (read(efd, &v, 8) < 0) {}
            lock(&mu);
            if (!pacing_latch || ph_n < 120) try_commit();              /* immediate (and while learning) */
            else arm_latch();
            unlock(&mu);
        }
        if (tfd >= 0 && (pf[2].revents & POLLIN)) {                        /* latch point reached */
            uint64_t v; if (read(tfd, &v, 8) < 0) {}
            lock(&mu);
            long long now = now_us();
            if (pending_mask && vbl_ref && commit_t >= now - (long long)phase_of(now)) {
                /* this cycle's frames went out already (a catch-up commit after a late flip): nothing to do */
            } else if (pending_mask) {          /* the previous commit missed a vblank: commit as soon as its */
                st_late_src[src_idx(last_src)]++; st_late_mask[pending_mask & 3]++; st_late_unf += commit_unfenced;
                if (st_late_n < 8) st_late_ph[st_late_n++] = phase_of(commit_t) / 1000.0;
                st_late++;                      /* flip lands (one repeat, not a 30 fps lock), and if the latch */
                latch_skipped = 1;              /* was near the vblank edge, widen the margin */
                /* only when a latch commit missed (a catch-up or safety-net commit made elsewhere in the cycle says
                 * nothing about the margin), and not during warm-up: a shader's first frames are slow and would pin
                 * the margin at max */
                /* "near the edge" = the latch sits at its upper bound (latch_hi), wherever that is in the cycle: on the
                 * RG DS Plus the bottom panel's vblank comes 6.8 ms before the top's, which puts that bound at 8.2 ms,
                 * before mid-period, and the old test (latch_off > period / 2) never let the margin grow there:
                 * 2-4 late latches/s for a whole session at 1700 us (measured 2026-10-01) */
                if (commit_latch && latch_off >= latch_hi() - 1 && latch_margin < 5000 && ph_n >= 600) latch_margin += 400;
            } else { in_latch = 1; commit_src = 'L'; try_commit(); in_latch = 0; }
            if (P[0].ready || P[1].ready) arm_latch();
            unlock(&mu);
        }
        if (pacing_latch && ph_n >= 120) {                                /* safety net: never sit on a frame */
            lock(&mu);
            if ((P[0].ready || P[1].ready) && !pending_mask && now_us() - ready_since > 2 * (long long)period) { st_late++; st_safety++; commit_src = 'S'; try_commit(); }
            unlock(&mu);
        }
        if (anim_dir) { lock(&mu); toast_dirty = 1; unlock(&mu); }       /* the overlay is moving: every commit carries it */
        if (toast_dirty && !pending_mask && !P[0].ready && !P[1].ready) {   /* an overlay change with no frame coming */
            lock(&mu); commit_src = 'T'; try_commit(); unlock(&mu);
        }
        stall_watch(now_us());                                            /* DraStic still presenting? */
        if (want_dump) { want_dump = 0; lock(&mu); dump_scan(); unlock(&mu); }
        if (tdump_n < touch_dumps && tdump_x >= 0) { lock(&mu); dump_touch(); unlock(&mu); tdump_n++; tdump_x = -1; }
        long long t = now_us();
        if (t - st10 >= 10000000) {                                        /* pacing summary every 10 s */
            lock(&mu);
            char h[64]; for (int i = 0; i < 16; i++) h[i] = ph_hist[i] > 99 ? '#' : ph_hist[i] > 30 ? '+' : ph_hist[i] > 5 ? '.' : ' ';
            h[16] = 0; memset(ph_hist, 0, sizeof ph_hist);
            double mean = fmod(atan2(ph_s, ph_c) / (2 * M_PI) * period + period, period);
            LOG("[pace] mode=%s period=%.1f us, presents at %.0f us after vblank (spread R=%.2f) [%s], latch at %.0f us, margin %.0f us, bottom %+.0f us, late=%d missed=%d\n",
                pacing_latch && ph_n >= 120 ? "latch" : "immediate", period, mean, sqrt(ph_c * ph_c + ph_s * ph_s), h, latch_off, latch_margin, bot_off, st_late, st_miss);
            if (st_late) {
                char ph[96]; size_t m = 0; ph[0] = 0;
                for (int k = 0; k < st_late_n; k++) m += snprintf(ph + m, sizeof ph - m, "%s%.1f", k ? " " : "", st_late_ph[k]);
                LOG("[late] %d: pending commit was latch %d, catch-up %d, safety %d, overlay %d, other %d; still pending top %d "
                    "bottom %d both %d; GPU unfinished at commit %d; safety-net commits %d; pending commits made at (ms after top vblank) %s\n",
                    st_late, st_late_src[0], st_late_src[1], st_late_src[2], st_late_src[3], st_late_src[4],
                    st_late_mask[1], st_late_mask[2], st_late_mask[3], st_late_unf, st_safety, ph);
            }
            if (st_unfenced) LOG("[fence] %d commits showed a frame the GPU hadn't finished\n", st_unfenced);
            memset(st_late_src, 0, sizeof st_late_src); memset(st_late_mask, 0, sizeof st_late_mask);
            st_safety = 0; st_late_n = 0; st_late_unf = 0; st_unfenced = 0;
            audio_pump_log();
            LOG("[flip] event delivery max %.2f ms, commit->flip max %.2f ms, %d flips >1 refresh after their commit\n",
                st_evt_max / 1000.0, st_c2f_max / 1000.0, st_c2f_long);
            st_evt_max = st_c2f_max = 0; st_c2f_long = 0;
            LOG("[lock] mu contended %d times, %.1f ms waiting in total, max wait %.2f ms, max hold %.2f ms\n",
                lk_waits, lk_wait_us / 1000.0, lk_wait_max / 1000.0, lk_hold_max / 1000.0);
            lk_waits = 0; lk_wait_us = lk_wait_max = lk_hold_max = 0;
            if (st_shade_n) LOG("[shader] submit %.2f ms avg, %.2f ms max per frame (both screens), %d buffer imports\n",
                                st_shade_sum / 1000.0 / st_shade_n, st_shade_max / 1000.0, shader_imports());
            st_shade_sum = st_shade_max = 0; st_shade_n = 0;
            { char rh[17]; for (int i = 0; i < 16; i++) rh[i] = raw_hist[i] > 99 ? '#' : raw_hist[i] > 30 ? '+' : raw_hist[i] > 5 ? '.' : ' ';
              rh[16] = 0; memset(raw_hist, 0, sizeof raw_hist);
              LOG("[raw] DraStic presents R=%.2f [%s] intervals(ms) 0-4:%d 4-8:%d 8-12:%d 12-16:%d 16-20:%d 20-24:%d 24+:%d",
                  sqrt(raw_c * raw_c + raw_s * raw_s), rh, iv_hist[0], iv_hist[1], iv_hist[2], iv_hist[3], iv_hist[4], iv_hist[5], iv_hist[6]);
              if (lat_n) LOG(" | present->ready %.2f ms avg, %.2f max", lat_sum / 1000.0 / lat_n, lat_max / 1000.0);
              LOG("\n"); memset(iv_hist, 0, sizeof iv_hist); lat_sum = lat_max = 0; lat_n = 0; }
            /* the margin shrinks back after a clean 10 s, so one bad moment doesn't narrow the latch zone forever */
            if (!st_late && latch_margin > margin_base) latch_margin = fmax(margin_base, latch_margin - 400);
            {   /* how full the queue runs: each waiting frame is a refresh of latency (and cover for a late frame) */
                int n = 0; double sum = 0; char w[96]; size_t m = 0;
                for (int k = 0; k <= QMAX + 1; k++) { n += st_wait[k]; sum += (double)k * st_wait[k]; }
                for (int k = 0; k <= queue_depth + 1 && k <= QMAX + 1; k++) m += snprintf(w + m, sizeof w - m, " %d:%d", k, st_wait[k]);
                if (n) LOG("[queue] depth %d, frames waiting after a commit%s, avg %.2f (%.1f ms added)",
                           queue_depth, w, sum / n, sum / n * period / 1000.0);
                if (n && queue_wait_us) LOG(" | DraStic held %d times, %.1f ms in total, longest %.1f ms",
                                            st_qwait_n, st_qwait_us / 1000.0, st_qwait_max / 1000.0);
                if (n) LOG("\n");
                memset(st_wait, 0, sizeof st_wait); st_qwait_n = 0; st_qwait_us = st_qwait_max = 0;
            }
            st_late = 0; st_miss = 0; st10 = t;
            unlock(&mu);
        }
        if (t - st_t0 >= 1000000) {
            lock(&mu);
            /* repeat: refreshes that showed no new frame (a late frame the queue couldn't cover; also loading pauses) */
            long rep = st_present ? lround((t - st_t0) / period) - st_fcommit : 0;
            LOG("[dsflip] present/s=%.1f commits=%d dropped=%d busy=%d flips top=%d bot=%d max-iv top=%lld bot=%lld us touch=%d drop-src=%d drop-q=%d drop-buf=%d repeat=%ld\n",
                st_present * 1e6 / (t - st_t0), st_commit, st_drop, st_busy, st_flips[0], st_flips[1], st_iv_max[0], st_iv_max[1], st_touch,
                st_drop_src, st_drop_q, st_drop_buf, rep > 0 ? rep : 0);
            st_drop_src = st_drop_q = st_drop_buf = 0; st_fcommit = 0;
            st_present = st_commit = st_drop = st_busy = st_flips[0] = st_flips[1] = st_touch = 0; st_iv_max[0] = st_iv_max[1] = 0; st_t0 = t;
            unlock(&mu);
        }
    }
    return 0;
}

/* ---------- stall watch ----------
 * DraStic's main thread presents the frames, and it is also what saves the resume state and quits on the exit hotkey
 * (resume.c). When it stops, the panels keep the last frame, the hotkey does nothing and nothing else ends the game:
 * on 2026-10-05 an RG DS Plus needed a hard reset (Black 2, just after its resume load). The presenter runs on
 * regardless, so it watches. No frame for STALL_WARN_S while the game should be running (DraStic's own menu presents
 * only when it changes, and the in-game menu holds DraStic on purpose: neither counts; nor does the quit's save)
 * logs what every thread is doing, shows a card, and makes the exit hotkey quit at once (resume.c). After
 * DSFLIP_STALL_QUIT seconds (20; 0: never) the game is ended here, with a notice for the menu (session.sh shows
 * DSFLIP_NOTICE, default /tmp/dsflip-notice). */
#define STALL_WARN_S 5
volatile int dsflip_stalled;            /* no frame for STALL_WARN_S: resume.c's SIGUSR1 then quits at once */
static int stall_quit_s = 20;
static long long stall_t0;              /* since when no frame came (only while one is expected) */
static char log_path[512];
int menu_is_open(void);                 /* menu.c */
int resume_saving(void);                /* resume.c: the quit's save is being written (DraStic's thread is busy) */
static void stall_dump(FILE *f) {       /* every thread: name, state, CPU time, allowed CPUs, wait channel, kernel stack */
    glob_t g;
    if (glob("/proc/self/task/[0-9]*", 0, 0, &g)) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char p[300], comm[32] = "?", st[1024] = "", wchan[64] = "", cpus[64] = "?", sc[64] = "", line[256];
        const char *tid = strrchr(g.gl_pathv[i], '/') + 1;
        FILE *x;
        snprintf(p, sizeof p, "%s/comm", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) { if (fgets(comm, sizeof comm, x)) comm[strcspn(comm, "\n")] = 0; fclose(x); }
        snprintf(p, sizeof p, "%s/stat", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) { if (!fgets(st, sizeof st, x)) st[0] = 0; fclose(x); }
        char state = '?'; unsigned long ut = 0, stt = 0; int cpu = -1;
        { char *r = strrchr(st, ')');           /* after "(comm)": state, then field 14/15 utime/stime, 39 processor */
          if (r && sscanf(r + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*u %*d %d",
                             &state, &ut, &stt, &cpu) < 3) state = '?'; }
        snprintf(p, sizeof p, "%s/wchan", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) { if (!fgets(wchan, sizeof wchan, x)) wchan[0] = 0; fclose(x); }
        snprintf(p, sizeof p, "%s/syscall", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) { if (fgets(sc, sizeof sc, x)) sc[strcspn(sc, " \n")] = 0; fclose(x); }
        snprintf(p, sizeof p, "%s/status", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) {
            while (fgets(line, sizeof line, x)) if (!strncmp(line, "Cpus_allowed_list:", 18)) { snprintf(cpus, sizeof cpus, "%s", line + 18 + strspn(line + 18, " \t")); cpus[strcspn(cpus, "\n")] = 0; }
            fclose(x);
        }
        fprintf(f, "[stall] thread %s %-15s %c cpu-time %lu ms, on CPU %d, allowed %s, syscall %s, waiting in %s\n", tid, comm,
                state, (ut + stt) * 10, cpu, cpus, sc[0] ? sc : "-", wchan[0] && strcmp(wchan, "0") ? wchan : "-");
        snprintf(p, sizeof p, "%s/stack", g.gl_pathv[i]);
        if ((x = fopen(p, "r"))) {                 /* the kernel side (root only): the first frames say enough */
            for (int k = 0; k < 4 && fgets(line, sizeof line, x); k++) fprintf(f, "[stall]     %s", line);
            fclose(x);
        }
    }
    globfree(&g);
}
static void *stall_report(void *a) {    /* its own thread: the presenter never waits on files or the overlay */
    long long idle = (long long)(intptr_t)a;
    if (lg) { flockfile(lg); fprintf(lg, "[stall] no frame from DraStic for %.1f s (not in a menu): its threads\n", idle / 1e6); stall_dump(lg); funlockfile(lg); fflush(lg); fsync(fileno(lg)); }
    return 0;
}
/* the card: 15 s at a time (up to the default ending), again while the stall lasts; gone soon if frames come back */
static void stall_card(void) {
    dsflip_toast("The game stopped responding", stall_quit_s > 0 ? "Exit hotkey: back to the menu now (or wait)" : "Exit hotkey: back to the menu", 0xe04040, 15000);
}
static void *stall_card_thread(void *a) { (void)a; stall_card(); return 0; }
static void stall_watch(long long t) {  /* the presenter, every wake-up (at least every 50 ms), mu not held */
    static int seen; static long long card_at;
    int p = dsflip_presents;
    if (p != seen || !p || menu_touch || menu_is_open() || resume_saving()) {
        if (dsflip_stalled) LOG("[stall] DraStic shows frames again after %.1f s\n", (t - stall_t0) / 1e6);
        dsflip_stalled = 0; seen = p; stall_t0 = t;
        return;
    }
    long long idle = t - stall_t0;
    if (!dsflip_stalled && idle >= STALL_WARN_S * 1000000LL) {
        dsflip_stalled = 1;
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&th, &at, stall_report, (void *)(intptr_t)idle)) pthread_setname_np(th, "dsf-stall");
        pthread_attr_destroy(&at);
        card_at = 0;
    }
    if (dsflip_stalled && t - card_at >= 15000000LL) {   /* the card (ui.c may take a moment: its own thread) */
        card_at = t;
        pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (!pthread_create(&th, &at, stall_card_thread, 0)) pthread_setname_np(th, "dsf-stallcard");
        pthread_attr_destroy(&at);
    }
    if (dsflip_stalled && stall_quit_s > 0 && idle >= stall_quit_s * 1000000LL) {
        const char *np = getenv("DSFLIP_NOTICE"); if (!np || !*np) np = "/tmp/dsflip-notice";
        FILE *f = fopen(np, "w");
        if (f) { fprintf(f, "The game stopped responding (no picture for %d seconds) and was closed. Log: %s\n", stall_quit_s, log_path); fclose(f); }
        LOG("[stall] still no frame after %d s: ending the game (DSFLIP_STALL_QUIT=0 keeps it)\n", stall_quit_s);
        if (lg) { fflush(lg); fsync(fileno(lg)); }
        kill(getpid(), SIGKILL);
    }
}

/* Threads start on every CPU, not just their creator's. session.sh's CPU placement (the RG DS Plus) confines DraStic's
 * main thread to CPU 3, and a thread created from a confined thread inherits that one CPU: DraStic's 3D helpers made
 * that way never ran, missed their first hand-off and DraStic's main thread and the helpers waited on each other for
 * good (68dfccd in ROCKNIXDS: 8 of 9 starts froze when the placement came too early). The placement only waits for the
 * helpers that exist when it starts; whatever DraStic (or SDL, PipeWire, Mali, libcurl, libdsflip) creates later --
 * after a state load, say -- started on CPU 3 alone until the next placement pass. glibc applies an attribute's CPU
 * set before the new thread first runs. DSFLIP_SPREAD_THREADS=0: inherit as before. */
static cpu_set_t all_cpus; static int spread_threads = -1;
static int create_spread(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real) real = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))dlsym(RTLD_NEXT, "pthread_create");
    if (spread_threads <= 0) return real(th, attr, fn, arg);
    cpu_set_t cur;
    if (sched_getaffinity(0, sizeof cur, &cur) || CPU_EQUAL(&cur, &all_cpus)) return real(th, attr, fn, arg);   /* creator not confined */
    if (attr) {
        cpu_set_t want;                 /* an attribute that names CPUs itself is left alone (unset reads as all bits) */
        if (pthread_attr_getaffinity_np(attr, sizeof want, &want) == 0 && CPU_COUNT(&want) != CPU_SETSIZE) return real(th, attr, fn, arg);
        pthread_attr_setaffinity_np((pthread_attr_t *)attr, sizeof all_cpus, &all_cpus);
        return real(th, attr, fn, arg);
    }
    pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setaffinity_np(&a, sizeof all_cpus, &all_cpus);
    int r = real(th, &a, fn, arg);
    pthread_attr_destroy(&a);
    return r;
}

/* DraStic's helper threads wait at a door until the locks they wait on exist.
 *
 * DraStic r2.5.2.2 (aarch64, build id 7a5e0e5f...0748) starts four of its helper threads before it initialises what
 * each of them waits on: pthread_create, then pthread_mutex_init twice and pthread_cond_init twice. That is at
 * +0x312ac for the thread that starts at +0x2fa50 (a screen's lines), and at +0x597b4, three times over, for the
 * three that start at +0x58e50 (the 3D bins; the thread at +0x59430 that hands them their work is created after its
 * locks, as it should be). A helper that is quick is already inside pthread_cond_wait when its creator wipes that
 * condition variable. glibc (2.25 on) keeps a condition variable's waiters in the variable itself, so the
 * pthread_cond_signal of the first hand-off finds no waiter and wakes nobody, and whoever handed the work over waits
 * for the answer for good: the game shows no frame at all, or none from the first frame that needs that helper (the
 * first 3D frame after a state load: the freeze of 2026-10-05 on an RG DS Plus).
 * Measured on an RG DS Plus on 2026-10-06, 1.6's library: 4 of 25 starts of Pokemon HeartGold showed no frame. gdb:
 * three with a 3D helper still in the pthread_cond_wait at +0x58eb8 after its hand-off, the 3D thread waiting for it
 * in the one at +0x592f8 and the main thread in the one at +0x596a8; one with the lines helper in the one at +0x2faf0
 * and the main thread waiting for it in the one at +0x30d30. Every thread was allowed on all four CPUs: session.sh's
 * CPU placement was not the cause; placed too early it made the creator late more often (68dfccd in ROCKNIXDS: 8 of
 * 9 starts).
 *
 * So the pthread_create below starts these four through helper_start, where a helper's first act is to wait until
 * its creator has made its second pthread_cond_init call since the pthread_create: the last of the four
 * initialisations, counted in the pthread_cond_init below. A door opens by itself after 2 s (with a line in the log),
 * so nothing waits here for good, and a DraStic whose code is elsewhere is started as before.
 * DSFLIP_HELPER_DOOR=0: start them as DraStic does. DSFLIP_HELPER_RACE=<ms>: the creator sleeps that long right after
 * each of the four pthread_create calls, which is the freeze at every start without the door and changes nothing with
 * it: the test on a handheld. */
#define DS_HELPER_LINES 0x2fa50
#define DS_HELPER_BINS  0x58e50
typedef struct { void *(*fn)(void *); void *arg; unsigned off; int open; long long t0; } door_t;
#define DOORS 64
static door_t doors[DOORS]; static unsigned door_n;
static __thread door_t *door_mine; static __thread int door_inits;     /* the creator's: the helper it started last */
static int helper_door = -1, helper_race_ms; static uintptr_t exe_base;
static int exe_base_cb(struct dl_phdr_info *i, size_t n, void *u) { (void)n; (void)u; exe_base = i->dlpi_addr; return 1; }   /* the first is the program */

static void *helper_start(void *p) {
    door_t *d = p; void *(*fn)(void *) = d->fn; void *arg = d->arg; unsigned off = d->off; long long t0 = d->t0;
    int i = 0;
    for (; i < 20000 && !__atomic_load_n(&d->open, __ATOMIC_ACQUIRE); i++) { struct timespec t = { 0, 100000 }; nanosleep(&t, 0); }
    if (i == 20000) LOG("[dsflip] DraStic's helper +0x%x waited 2 s for its locks and starts all the same\n", off);
    else LOG("[dsflip] DraStic's helper +0x%x waited %lld us for its locks\n", off, now_us() - t0);
    return fn(arg);
}

int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    if (helper_door < 0) {
        const char *e = getenv("DSFLIP_HELPER_DOOR"); int on = !(e && *e == '0');
        e = getenv("DSFLIP_HELPER_RACE"); helper_race_ms = e ? atoi(e) : 0;
        dl_iterate_phdr(exe_base_cb, 0);
        helper_door = on;
    }
    uintptr_t off = (uintptr_t)fn - exe_base;
    int helper = exe_base && (off == DS_HELPER_LINES || off == DS_HELPER_BINS);
    door_t *d = 0;
    if (helper && helper_door) {
        if (door_mine) __atomic_store_n(&door_mine->open, 1, __ATOMIC_RELEASE);     /* the one before: never left waiting */
        d = &doors[__atomic_fetch_add(&door_n, 1, __ATOMIC_RELAXED) % DOORS];
        d->fn = fn; d->arg = arg; d->off = (unsigned)off; d->t0 = now_us(); __atomic_store_n(&d->open, 0, __ATOMIC_RELEASE);
        door_mine = d; door_inits = 0;
    }
    int r = d ? create_spread(th, attr, helper_start, d) : create_spread(th, attr, fn, arg);
    if (r && d) door_mine = 0;                                          /* no thread: nobody at this door */
    if (helper && helper_race_ms > 0) { struct timespec t = { helper_race_ms / 1000, (helper_race_ms % 1000) * 1000000L }; nanosleep(&t, 0); }
    return r;
}

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) {
    static int (*real)(pthread_cond_t *, const pthread_condattr_t *);
    if (!real) real = (int (*)(pthread_cond_t *, const pthread_condattr_t *))dlsym(RTLD_NEXT, "pthread_cond_init");
    int r = real(c, a);
    if (door_mine && ++door_inits == 2) { __atomic_store_n(&door_mine->open, 1, __ATOMIC_RELEASE); door_mine = 0; }
    return r;
}

/* ---------- the in-game menu's screens (menu.c) ---------- */
/* two panel-sized XRGB8888 buffers per panel; the menu draws into one that isn't on screen or on its way there and
 * puts it up in place of whatever frames were waiting. DraStic's next frame after the menu replaces them. */
static dbuf mscr[2][2]; static int mscr_ok = -1;
int dsflip_drastic_menu(void) { return menu_touch; }
int dsflip_battery(int *charging) { return battery_pct(charging); }
int dsflip_menu_canvas(int i, uint32_t **px, int *pitch, int *w, int *h) {
    if (!ok || i < 0 || i > 1) return -1;
    if (mscr_ok < 0) {
        mscr_ok = 1;
        for (int p = 0; p < 2 && mscr_ok; p++) for (int k = 0; k < 2; k++)
            if (mkbuf(&mscr[p][k], P[p].mode.hdisplay, P[p].mode.vdisplay, DRM_FORMAT_XRGB8888, 32)) { mscr_ok = 0; break; }
        LOG("[menu] screen buffers: %s\n", mscr_ok ? "ok" : "alloc failed");
    }
    if (!mscr_ok) return -1;
    for (int tries = 0; tries < 100; tries++) {   /* both busy: one is on screen, the other's flip is pending */
        lock(&mu);
        for (int k = 0; k < 2; k++) {
            dbuf *b = &mscr[i][k];
            int busy = P[i].scan == b || P[i].queued == b || P[i].ready == b;
            for (int j = 0; j < P[i].nq; j++) busy |= P[i].q[j] == b;
            if (!busy) { unlock(&mu); *px = b->map; *pitch = (int)b->pitch; *w = (int)b->w; *h = (int)b->h; return k; }
        }
        unlock(&mu);
        usleep(2000);
    }
    return -1;
}
void dsflip_menu_show(int i, int k) {
    lock(&mu);
    dbuf *b = &mscr[i][k];
    while (P[i].nq) release(P[i].q[--P[i].nq]);
    if (P[i].ready) release(P[i].ready);
    if (P[i].src) { release(P[i].src); P[i].src = 0; }   /* shader mode: a frame not shaded yet */
    b->state = READY; P[i].ready = b; ready_since = now_us();
    unlock(&mu);
    uint64_t one = 1; if (write(efd, &one, 8) < 0) {}
}
/* the top panel's current picture (the game), scaled to w x h; 0 if there is none to take */
int dsflip_menu_grab_top(uint32_t *dst, int dpitch, int w, int h) {
    lock(&mu);
    dbuf *b = P[0].scan;
    int okb = b && b->map && b != &blk.b[0] && b->pitch >= b->w * 4 && b != &mscr[0][0] && b != &mscr[0][1];
    if (okb) {
        static uint32_t row[4096]; int last = -1;
        for (int y = 0; y < h; y++) {
            int sy = (int)((long long)y * b->h / h);
            if (sy != last) { memcpy(row, (char *)b->map + (size_t)sy * b->pitch, (size_t)(b->w < 4096 ? b->w : 4096) * 4); last = sy; }
            uint32_t *d = (uint32_t *)((char *)dst + (size_t)y * dpitch);
            for (int x = 0; x < w; x++) d[x] = row[(long long)x * b->w / w];
        }
    }
    unlock(&mu);
    return okb;
}

/* ---------- touch ---------- */
/* SDL2 event layouts (SDL_events.h): motion {type,timestamp,windowID,which,state,x,y,xrel,yrel},
 * button {type,timestamp,windowID,which,u8 button,state,clicks,pad,x,y}. SDL_Event is 56 bytes. */
typedef struct { uint32_t type, ts, win, which; uint8_t button, state, clicks, pad; int32_t x, y; } btn_ev;
typedef struct { uint32_t type, ts, win, which, state; int32_t x, y, xrel, yrel; } mot_ev;
typedef struct { uint32_t type, ts, win; uint8_t state, repeat, p2, p3; uint32_t scancode; int32_t sym; uint16_t mod; uint32_t unused; } key_ev;
typedef union { uint32_t type; btn_ev b; mot_ev m; key_ev k; uint8_t pad[56]; } sdl_ev;
#define QN 128
#define EV_BARRIER 0xFFFFu                 /* queue marker: SDL_PollEvent reports "no event" once (ends DraStic's poll loop) */
static sdl_ev tq[QN]; static int tq_head, tq_tail;
static int touch_inv_x = 0, touch_inv_y = 0;

extern uint32_t SDL_GetTicks(void);
extern uint32_t SDL_GetWindowID(void *);

static void push_ev(sdl_ev *e) {
    lock(&tmu);
    int n = (tq_head + 1) % QN;
    if (n != tq_tail) { tq[tq_head] = *e; tq_head = n; }
    unlock(&tmu);
}

/* microphone (audio.c): hold/release DraStic's fake-mic key, Scroll Lock (control 327 = 256 + scancode 71) */
/* a key DraStic has a control on (control = 256 + scancode), pressed or released */
static void dsflip_key(int scancode, int down) {
    sdl_ev e; memset(&e, 0, sizeof e);
    e.k.type = down ? 0x300 : 0x301; e.k.ts = SDL_GetTicks(); e.k.win = window ? SDL_GetWindowID(window) : 1;
    e.k.state = down ? 1 : 0; e.k.scancode = (uint32_t)scancode; e.k.sym = 0x40000000 | scancode;
    push_ev(&e);
}
void dsflip_mic_key(int down) { dsflip_key(71, down); }
/* the same through a joystick button (SDL_JOYBUTTONDOWN/UP, as resume.c presses DraStic's save/load controls): for a
 * drastic.cfg whose keyboard set has no fake microphone but whose joystick set does (DSFLIP_MIC_KEY, audio.c) */
int resume_joy_id(void);
void dsflip_mic_button(int button, int down) {
    sdl_ev e; memset(&e, 0, sizeof e);
    uint32_t type = down ? 0x603 : 0x604, ts = SDL_GetTicks(); int32_t which = resume_joy_id();
    memcpy(e.pad, &type, 4); memcpy(e.pad + 4, &ts, 4); memcpy(e.pad + 8, &which, 4);
    e.pad[12] = (uint8_t)button; e.pad[13] = down ? 1 : 0;
    push_ev(&e);
}
/* DraStic ignores the absolute x/y of mouse events: it moves its stylus by the RELATIVE deltas (xrel/yrel),
 * 1:1 in DS pixels, clamped to the bottom screen, starting from the centre (measured by logging where it
 * draws its 32x32 cursor). So we track its stylus position and send exact deltas. On every touch-down we
 * first pin it to (0,0) with a large negative move, then move to the target: no drift can accumulate. */
static int sty_x = -1, sty_y = -1;
static void stylus_move(int tx, int ty, int down, int resync) {
    sdl_ev e; uint32_t wid = window ? SDL_GetWindowID(window) : 1, ts = SDL_GetTicks();
    if (resync || sty_x < 0) {
        memset(&e, 0, sizeof e);
        e.m.type = 0x400; e.m.ts = ts; e.m.win = wid; e.m.state = 0; e.m.x = 0; e.m.y = 0; e.m.xrel = -4096; e.m.yrel = -4096;
        push_ev(&e); sty_x = 0; sty_y = 0;
        /* DraStic sums a frame's deltas and clamps once, so the move away from (0,0) must land in the next frame */
        memset(&e, 0, sizeof e); e.type = EV_BARRIER; push_ev(&e);
    }
    if (tx == sty_x && ty == sty_y) return;
    memset(&e, 0, sizeof e);
    e.m.type = 0x400; e.m.ts = ts; e.m.win = wid; e.m.state = down ? 1 : 0;
    e.m.x = tx; e.m.y = ty; e.m.xrel = tx - sty_x; e.m.yrel = ty - sty_y;
    push_ev(&e); sty_x = tx; sty_y = ty;
}

static void touch_emit(int down_change, int down, int x, int y, int xmax, int ymax) {
    int rx = x, ry = y;
    if (touch_inv_x) x = xmax - x;      /* raw touch is already aligned with the panel on the RG DS */
    if (touch_inv_y) y = ymax - y;
    if (menu_touch_event(down_change, down, x, y, xmax, ymax)) return;   /* the in-game menu is up (menu.c) */
    /* the bottom panel shows exactly the bottom DS screen: panel pixels -> DS pixels is a straight rescale.
     * DraStic's menu (also on the bottom panel) takes no touch: its input loop (r2.5.2.2, aarch64) handles only
     * SDL key, joystick axis/hat/button events, never mouse or finger events, so taps in it go nowhere. */
    if (!touch_rect_ok) return;
    /* a shader may draw the DS screen into a rectangle of the panel (ds-integer: 512x384, centred): map into it.
     * A touch that starts outside it (on the bezel) is ignored until it lifts; one that slides out is clamped. */
    int x0 = 0, y0 = 0, w = xmax + 1, h = ymax + 1;
    if (vp_w > 0 && vp_h > 0) {
        int pw = P[1].mode.hdisplay ? P[1].mode.hdisplay : 640, ph = P[1].mode.vdisplay ? P[1].mode.vdisplay : 480;
        x0 = vp_x * (xmax + 1) / pw; y0 = vp_y * (ymax + 1) / ph; w = vp_w * (xmax + 1) / pw; h = vp_h * (ymax + 1) / ph;
    }
    if (down_change && down) touch_outside = x < x0 || x >= x0 + w || y < y0 || y >= y0 + h;
    if (touch_outside) { if (down_change && !down) touch_outside = 0; return; }
    int lx = (int)((long long)(x - x0) * 256 / w);
    int ly = (int)((long long)(y - y0) * 192 / h);
    sdl_ev e;
    uint32_t wid = window ? SDL_GetWindowID(window) : 1, ts = SDL_GetTicks();
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx > 255) lx = 255;
    if (ly > 191) ly = 191;
    stylus_move(lx, ly, down, down_change && down);
    if (down_change && down) {
        LOG("[touch] down raw %d,%d -> ds %d,%d\n", rx, ry, lx, ly);
        if (tdump_n < touch_dumps) { tdump_y = ly; tdump_x = lx; }
    }
    if (down_change) {
        memset(&e, 0, sizeof e);
        e.b.type = down ? 0x401 : 0x402; e.b.ts = ts; e.b.win = wid; e.b.button = 1; e.b.state = down ? 1 : 0; e.b.clicks = 1;
        e.b.x = lx; e.b.y = ly;
        push_ev(&e);
    }
    st_touch++;
}

/* test hook: lines "x y" written to /tmp/dsflip-tap inject a tap at those bottom-PANEL pixels, through the same
 * path as a real touch */
static void *tap_fifo_thread(void *a) {
    (void)a;
    unlink("/tmp/dsflip-tap"); mkfifo("/tmp/dsflip-tap", 0600);
    for (;;) {
        FILE *f = fopen("/tmp/dsflip-tap", "r"); if (!f) return 0;
        int x, y;
        while (fscanf(f, "%d %d", &x, &y) == 2) {
            touch_emit(1, 1, x, y, 639, 479);
            usleep(150000);
            touch_emit(1, 0, x, y, 639, 479);
            LOG("[tap] injected panel %d,%d%s\n", x, y, menu_touch ? " (menu: ignored)" : "");
        }
        fclose(f);
    }
    return 0;
}

static int touch_find(const char *want, char *path, size_t n) {   /* the evdev node whose device path contains want */
    glob_t g; *path = 0;
    if (glob("/sys/class/input/event*", 0, 0, &g)) return -1;
    for (size_t i = 0; i < g.gl_pathc && !*path; i++) {
        char dev[300], real_[512]; snprintf(dev, sizeof dev, "%s/device", g.gl_pathv[i]);
        if (realpath(dev, real_) && strstr(real_, want)) snprintf(path, n, "/dev/input/%s", strrchr(g.gl_pathv[i], '/') + 1);
    }
    globfree(&g);
    return *path ? 0 : -1;
}

/* Reads the touchscreen for the whole session. If the device goes away (a suspend/resume or a driver rebind removes
 * and recreates it), a held stylus is released and the device is found and opened again. */
static void *touch_thread(void *a) {
    const char *want = a;
    int missing_logged = 0;
    for (;;) {
        char path[64];
        int tfd = touch_find(want, path, sizeof path) ? -1 : open(path, O_RDONLY | O_CLOEXEC);
        if (tfd < 0) {
            if (!missing_logged++) LOG("[dsflip] no touchscreen matching %s (still looking)\n", want);
            usleep(500000); continue;
        }
        missing_logged = 0;
        int ax[6] = { 0 }, ay[6] = { 0 };
        ioctl(tfd, 0x80184540 + 0x35, ax); ioctl(tfd, 0x80184540 + 0x36, ay);   /* EVIOCGABS(ABS_MT_POSITION_X/Y) */
        int xmax = ax[2] > 0 ? ax[2] : 639, ymax = ay[2] > 0 ? ay[2] : 479;
        LOG("[dsflip] touch %s range %dx%d\n", path, xmax + 1, ymax + 1);
        struct { long s, us; uint16_t type, code; int32_t value; } ev[32];
        int x = 0, y = 0, down = 0, was_down = 0;
        ssize_t n;
        for (;;) {
            n = read(tfd, ev, sizeof ev);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
                if (ev[i].type == 3 && (ev[i].code == 0x35 || ev[i].code == 0)) x = ev[i].value;
                else if (ev[i].type == 3 && (ev[i].code == 0x36 || ev[i].code == 1)) y = ev[i].value;
                else if (ev[i].type == 1 && ev[i].code == 0x14a) down = ev[i].value != 0;   /* BTN_TOUCH */
                else if (ev[i].type == 0 && ev[i].code == 0) {                                /* SYN_REPORT */
                    if (down || was_down) touch_emit(down != was_down, down, x, y, xmax, ymax);
                    was_down = down;
                }
            }
        }
        LOG("[dsflip] touch read ended (%s): reopening\n", n < 0 ? strerror(errno) : "end of file");
        close(tfd);
        if (was_down) touch_emit(1, 0, x, y, xmax, ymax);      /* don't leave DraStic's stylus pressed */
        usleep(200000);
    }
    return 0;
}

int SDL_PollEvent(void *e) {
    REAL(int, SDL_PollEvent, void *);
    if (ok && e && resume_poll(e)) return 1;         /* a save/load state control press (resume.c) */
    if (tq_head != tq_tail) {
        lock(&tmu);
        if (tq_head != tq_tail) {
            if (tq[tq_tail].type == EV_BARRIER) { tq_tail = (tq_tail + 1) % QN; unlock(&tmu); return 0; }
            if (e) { memcpy(e, &tq[tq_tail], sizeof tq[0]); tq_tail = (tq_tail + 1) % QN; }
            unlock(&tmu);
            return 1;
        }
        unlock(&tmu);
    }
    int r = real(e);
    if (r && e && ok) {
        resume_saw_event(e);                         /* a save/load state press: CPU boost (resume.c, cpugov.c) */
        if (menu_event(e)) return 0;                 /* the menu button: the in-game menu (menu.c), not DraStic's */
    }
    return r;
}

/* ---------- init ---------- */
/* The verdict for session.sh: DSFLIP_STATE (default /tmp/dsflip-state) gets "ready" once libdsflip has both panels,
 * or "passthrough: <reason>" when it gives up. session.sh deletes the file before starting DraStic and waits for
 * one of the two, so it never reads the previous session's log (which libdsflip rotates only once it runs). */
static void verdict(const char *v) {
    const char *sp = getenv("DSFLIP_STATE"); if (!sp) sp = "/tmp/dsflip-state";
    FILE *f = fopen(sp, "w"); if (f) { fprintf(f, "%s\n", v); fclose(f); }
}
static void give_up(const char *why) {
    char v[160]; snprintf(v, sizeof v, "passthrough: %s", why);
    LOG("[dsflip] %s -> passthrough\n", why);
    verdict(v);
}

/* the display controller's interrupt count (/proc/interrupts, all CPUs), -1 if not found */
static long long vop_irqs(void) {
    FILE *f = fopen("/proc/interrupts", "r"); if (!f) return -1;
    char line[512]; long long n = -1;
    while (fgets(line, sizeof line, f)) {
        if (!strstr(line, "fe040000.vop")) continue;
        char *p = strchr(line, ':'); if (!p) break;
        n = 0; p++;
        for (;;) { char *e; long long v = strtoll(p, &e, 10); if (e == p) break; n += v; p = e; }
        break;
    }
    fclose(f);
    return n;
}

/* The display device: DSFLIP_CARD, else the first /dev/dri/card* with two connected DSI panels (card0 on ROCKNIX;
 * other firmwares can number them differently, e.g. with a second DRM driver loaded). */
static int open_card(void) {
    const char *c = getenv("DSFLIP_CARD");
    if (c && *c) return open(c, O_RDWR | O_CLOEXEC);
    glob_t g; int found = -1;
    if (glob("/dev/dri/card*", 0, 0, &g)) return open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    for (size_t i = 0; i < g.gl_pathc && found < 0; i++) {
        int f = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC); if (f < 0) continue;
        drmModeRes *res = drmModeGetResources(f); int dsi = 0;
        for (int k = 0; res && k < res->count_connectors; k++) {
            drmModeConnector *cn = drmModeGetConnector(f, res->connectors[k]);
            if (cn && cn->connector_type == DRM_MODE_CONNECTOR_DSI && cn->connection == DRM_MODE_CONNECTED) dsi++;
            drmModeFreeConnector(cn);
        }
        drmModeFreeResources(res);
        if (dsi >= 2) { found = f; LOG("[dsflip] display: %s\n", g.gl_pathv[i]); } else close(f);
    }
    globfree(&g);
    return found >= 0 ? found : open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
}

__attribute__((constructor)) static void init(void) {
    /* Only the process that was actually exec'd with LD_PRELOAD may take the display.
       Its children inherit the preload (DraStic starts `sh -c pactl subscribe` for the volume
       keys). Those shells were rotating this log out from under the game and, after failing
       to become DRM master for 3 s, writing "passthrough" over the session's verdict. */
    if (getenv("DSFLIP_IN_GAME")) return;
    setenv("DSFLIP_IN_GAME", "1", 1);
    {   /* the CPUs new threads start on (the pthread_create above): the whole set, before anything is confined */
        const char *sp = getenv("DSFLIP_SPREAD_THREADS");
        spread_threads = !(sp && *sp == '0') && sched_getaffinity(0, sizeof all_cpus, &all_cpus) == 0 && CPU_COUNT(&all_cpus) > 1;
    }
    const char *lp = getenv("DSFLIP_LOG"); if (!lp) lp = "/storage/dsflip/logs/dsflip.log";
    {   /* keep the previous three sessions' logs (.1 = the last one): testers lost evidence to the overwrite */
        char a[512], b[512];
        for (int k = 3; k >= 1; k--) {
            if (k == 1) snprintf(a, sizeof a, "%s", lp); else snprintf(a, sizeof a, "%s.%d", lp, k - 1);
            snprintf(b, sizeof b, "%s.%d", lp, k);
            rename(a, b);
        }
    }
    snprintf(log_path, sizeof log_path, "%s", lp);
    lg = fopen(lp, "w");
    if (lg) setvbuf(lg, 0, _IOLBF, 0);
    LOG("[dsflip] libdsflip %s\n", DSFLIP_VERSION);
    const char *top = getenv("DSFLIP_TOP"); if (!top) top = "DSI-2";

    fd = open_card();
    long long t_init = now_us();
    int master = -1;                    /* sway/seatd may still be letting go of the device: retry for up to 3 s */
    for (int k = 0; fd >= 0 && k < 300 && (master = drmSetMaster(fd)) != 0; k++) usleep(10000);
    if (fd < 0 || master) { give_up(fd < 0 ? "can't open the display device" : "the display stayed busy for 3 s"); return; }
    if (now_us() - t_init > 1000) LOG("[dsflip] DRM master after %lld ms\n", (now_us() - t_init) / 1000);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) { give_up("no atomic modesetting"); return; }
    drmModeRes *res = drmModeGetResources(fd);
    drmModePlaneRes *pres = drmModeGetPlaneResources(fd);
    if (!res || !pres) { give_up("no DRM resources"); return; }
    int np = 0; uint32_t used_crtc = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c->connection != DRM_MODE_CONNECTED || !c->count_modes || np == 2) { drmModeFreeConnector(c); continue; }
        char name[32]; snprintf(name, sizeof name, "DSI-%u", c->connector_type_id);
        panel *p = &P[strcmp(name, top) ? 1 : 0];
        if (p->conn) p = &P[p == &P[0] ? 1 : 0];
        p->conn = c->connector_id; p->mode = c->modes[0];
        drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id ? c->encoder_id : c->encoders[0]);
        for (int k = 0; k < res->count_crtcs; k++)
            if ((e->possible_crtcs & (1u << k)) && !(used_crtc & (1u << k))) { p->crtc = res->crtcs[k]; p->crtc_idx = k; used_crtc |= 1u << k; break; }
        drmModeFreeEncoder(e);
        LOG("[dsflip] %s conn %u crtc %u -> %s\n", name, c->connector_id, p->crtc, p == &P[0] ? "top" : "bottom");
        drmModeFreeConnector(c);
        drmModeCreatePropertyBlob(fd, &p->mode, sizeof p->mode, &p->mode_blob);
        np++;
    }
    for (int i = 0; i < 2; i++)
        for (uint32_t k = 0; k < pres->count_planes && !P[i].plane; k++) {
            drmModePlane *pl = drmModeGetPlane(fd, pres->planes[k]);
            if ((pl->possible_crtcs & (1u << P[i].crtc_idx)) && propval(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type") == 1)
                P[i].plane = pl->plane_id;
            drmModeFreePlane(pl);
        }
    if (np < 2 || !P[0].plane || !P[1].plane) { give_up("need 2 panels with primary planes"); return; }
    for (int i = 0; i < 2; i++) plane_props(&P[i]);

    /* modeset both with the black buffer; other planes off */
    blk.kind = K_BLACK; blk.nb = 1; blk.w = P[1].mode.hdisplay; blk.h = P[1].mode.vdisplay;
    if (mkbuf(&blk.b[0], blk.w, blk.h, DRM_FORMAT_XRGB8888, 32)) { give_up("dumb buffer alloc failed"); return; }
    drmModeAtomicReq *r;
    int ret = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        r = drmModeAtomicAlloc();
        for (uint32_t k = 0; k < pres->count_planes; k++) {
            uint32_t id = pres->planes[k];
            if (id == P[0].plane || id == P[1].plane) continue;
            drmModeAtomicAddProperty(r, id, prop(id, DRM_MODE_OBJECT_PLANE, "FB_ID"), 0);
            drmModeAtomicAddProperty(r, id, prop(id, DRM_MODE_OBJECT_PLANE, "CRTC_ID"), 0);
        }
        for (int i = 0; i < 2; i++) {
            panel *p = &P[i];
            drmModeAtomicAddProperty(r, p->conn, prop(p->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"), p->crtc);
            drmModeAtomicAddProperty(r, p->crtc, prop(p->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"), p->mode_blob);
            drmModeAtomicAddProperty(r, p->crtc, prop(p->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 1);
            add_fb(r, p, &blk.b[0]);
            p->scan = &blk.b[0];
        }
        ret = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0);
        drmModeAtomicFree(r);
        if (ret) { char w[96]; snprintf(w, sizeof w, "modeset failed: %s", strerror(-ret)); give_up(w); return; }
        /* The display controller can be stuck in an underrun loop when we get it: sway stopped while its buffers
         * were still being scanned out, the controller read freed memory, and one video port then raised
         * POST_BUF_EMPTY ~84,000 times a second (~60% of a core) until a full modeset. The modeset above keeps the
         * mode, so it doesn't clear that. A normal controller raises at most a few interrupts in 30 ms; if it's
         * storming, switch both panels off and on again (a slow panel power cycle, so only then).
         * DSFLIP_TEST_REMODESET=1: take that path anyway (testing). */
        if (attempt) break;
        long long a = vop_irqs(); usleep(30000); long long b = vop_irqs();
        if (!(a >= 0 && b - a > 100) && !getenv("DSFLIP_TEST_REMODESET")) break;
        LOG("[dsflip] display controller interrupt storm (%lld in 30 ms): switching the panels off and on\n", b - a);
        r = drmModeAtomicAlloc();
        for (int i = 0; i < 2; i++) drmModeAtomicAddProperty(r, P[i].crtc, prop(P[i].crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"), 0);
        if (drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, 0)) LOG("[dsflip] switching the panels off failed\n");
        drmModeAtomicFree(r);
    }

    /* can the top plane scale the 800x480 menu down itself? */
    dbuf t; memset(&t, 0, sizeof t);
    if (!mkbuf(&t, 800, 480, DRM_FORMAT_RGB565, 16)) {
        r = drmModeAtomicAlloc(); add_fb(r, &P[1], &t);
        menu_hw = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY, 0) == 0;
        drmModeAtomicFree(r); freebuf(&t);
    }
    LOG("[dsflip] menu scaling: %s\n", menu_hw ? "hardware" : "CPU nearest");

    /* toast plane: a free overlay plane that can go on the top panel's CRTC with ARGB8888 */
    if (P[0].mode.hdisplay > 640) { TOAST_W = P[0].mode.hdisplay; TOAST_H = (72 * TOAST_W + 320) / 640; }
    for (int k = 0; k < 2; k++) if (mkbuf(&toast[k], TOAST_W, TOAST_H, DRM_FORMAT_ARGB8888, 32)) { toast[0].map = 0; break; }
    for (uint32_t k = 0; k < pres->count_planes && !tp_plane && toast[0].map; k++) {
        drmModePlane *pl = drmModeGetPlane(fd, pres->planes[k]);
        uint32_t id = pl->plane_id;
        int fmt = 0; for (uint32_t f = 0; f < pl->count_formats; f++) if (pl->formats[f] == DRM_FORMAT_ARGB8888) fmt = 1;
        if (id != P[0].plane && id != P[1].plane && fmt && (pl->possible_crtcs & (1u << P[0].crtc_idx)) &&
            propval(id, DRM_MODE_OBJECT_PLANE, "type") == 0) {
            uint32_t o = DRM_MODE_OBJECT_PLANE;
            tp_plane = id; tp_fb = prop(id, o, "FB_ID"); tp_crtc = prop(id, o, "CRTC_ID");
            tp_sx = prop(id, o, "SRC_X"); tp_sy = prop(id, o, "SRC_Y"); tp_sw = prop(id, o, "SRC_W"); tp_sh = prop(id, o, "SRC_H");
            tp_cx = prop(id, o, "CRTC_X"); tp_cy = prop(id, o, "CRTC_Y"); tp_cw = prop(id, o, "CRTC_W"); tp_ch = prop(id, o, "CRTC_H");
            toast_want = 1; toast_cur = 0;
            drmModeAtomicReq *t = drmModeAtomicAlloc(); add_toast(t);
            if (drmModeAtomicCommit(fd, t, DRM_MODE_ATOMIC_TEST_ONLY, 0)) tp_plane = 0;
            drmModeAtomicFree(t); toast_want = 0;
        }
        drmModeFreePlane(pl);
    }
    LOG("[dsflip] toast plane: %u (%dx%d)\n", tp_plane, TOAST_W, TOAST_H);
    if (tp_plane) volume_start();           /* the volume keys' indicator (volume.c): mako is down with sway */
    signal(SIGUSR2, on_usr2);
    { const char *sq = getenv("DSFLIP_STALL_QUIT"); if (sq && *sq) stall_quit_s = atoi(sq) > 0 ? atoi(sq) : 0; }
    { LOG("[dsflip] stall watch: a card after %d s without a frame, the game ended after %d s%s; new threads start on %d CPUs%s\n",
          STALL_WARN_S, stall_quit_s, stall_quit_s ? "" : " (never: DSFLIP_STALL_QUIT=0)", spread_threads > 0 ? CPU_COUNT(&all_cpus) : 0,
          spread_threads > 0 ? "" : " (inherited: DSFLIP_SPREAD_THREADS=0)"); }
    { const char *q = getenv("DSFLIP_QUEUE"); if (q && *q) { queue_depth = atoi(q); if (queue_depth < 0) queue_depth = 0; if (queue_depth > QMAX) queue_depth = QMAX; } }
    const char *pm = getenv("DSFLIP_PACING");
    if (pm && !strcmp(pm, "immediate")) pacing_latch = 0;
    if (pacing_latch) tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (tfd < 0) pacing_latch = 0;                                   /* no timer: never wait for a latch */
    { const char *lm = getenv("DSFLIP_LATCH_MARGIN"); if (lm && atoi(lm) > 0) latch_margin = margin_base = atoi(lm); }
    { const char *qw = getenv("DSFLIP_QUEUE_WAIT"); if (qw && *qw) queue_wait_us = atoi(qw) * 1000; if (queue_wait_us < 0) queue_wait_us = 0; }
    if (queue_depth && queue_wait_us) LOG("[dsflip] pacing: %s, %d-frame queue, a full queue holds DraStic up to %d ms\n",
                                          pacing_latch ? "latch (adaptive)" : "immediate", queue_depth, queue_wait_us / 1000);
    else if (queue_depth) LOG("[dsflip] pacing: %s, %d-frame queue\n", pacing_latch ? "latch (adaptive)" : "immediate", queue_depth);
    else LOG("[dsflip] pacing: %s, mailbox\n", pacing_latch ? "latch (adaptive)" : "immediate");
    tdump_x = -1; if (getenv("DSFLIP_TOUCH_DUMP")) touch_dumps = atoi(getenv("DSFLIP_TOUCH_DUMP"));
    efd = eventfd(0, EFD_CLOEXEC); wfd = eventfd(0, EFD_CLOEXEC);
    st_t0 = now_us();
    shader_nm = shader_name();          /* its output buffers are made by the shader worker, once it knows their size */
    /* shader input: DraStic's buffers imported as dma-bufs (default), or DSFLIP_SHADER_COPY=1: uploaded from memory.
     * Measured 2026-09-28: the upload was most of every shader's cost (ds-crisp 1.96 -> 0.75 ms per panel at 2x) and
     * 17% of a core on the shader thread (-> 5.5%); HeartGold 2x drops the same (0.04/s) either way. */
    { const char *sc = getenv("DSFLIP_SHADER_COPY"); shader_copy_mode = sc && *sc == '1'; }
    pthread_t th; if (!pthread_create(&th, 0, presenter, 0)) pthread_setname_np(th, "dsf-present");
    for (int k = 0; k < 300 && !shader_done; k++) usleep(10000);
    if (shader_nm) LOG("[dsflip] shader input: %s\n", shader_copy_mode ? "upload from memory" : "dma-buf import");
    anim_log = getenv("DSFLIP_UI_DEMO") != 0;
    { int v[4], pw = P[1].mode.hdisplay ? P[1].mode.hdisplay : 640, ph = P[1].mode.vdisplay ? P[1].mode.vdisplay : 480;
      if (shader_on && shader_viewport(v, pw, ph)) { vp_x = v[0]; vp_y = v[1]; vp_w = v[2]; vp_h = v[3]; } }
    const char *inv = getenv("DSFLIP_TOUCH_INVERT");        /* "x", "y", "xy" or unset/"none" */
    if (inv) { touch_inv_x = strchr(inv, 'x') != 0; touch_inv_y = strchr(inv, 'y') != 0; }
    const char *tp = getenv("DSFLIP_TOUCH");
    if (!pthread_create(&th, 0, touch_thread, (void *)(tp ? tp : "fe5e0000.i2c"))) pthread_setname_np(th, "dsf-touch");
    cursor_log = getenv("DSFLIP_CURSOR_LOG") != 0;
    if (getenv("DSFLIP_TAP_FIFO")) pthread_create(&th, 0, tap_fifo_thread, 0);
    ok = 1;
    LOG("[dsflip] ready: top plane %u, bottom plane %u (init %lld ms)\n", P[0].plane, P[1].plane, (now_us() - t_init) / 1000);
    verdict("ready");
    cpugov_start();
    resume_start();
}

/* ---------- audio: the pump (audio.c); DSFLIP_AUDIO_PUMP=0 leaves DraStic's audio to SDL ---------- */
struct SDL_AudioSpec_;
int audio_pump_enabled(void);
int audio_pump_open(struct SDL_AudioSpec_ *want, struct SDL_AudioSpec_ *have, int (*real)(struct SDL_AudioSpec_ *, struct SDL_AudioSpec_ *));
void audio_pump_pause(int on);
static int pump_on;
void SDL_PauseAudio(int on) {
    REAL(void, SDL_PauseAudio, int);
    if (pump_on) audio_pump_pause(on);
    real(on);
}
struct SDL_AudioSpec_ { int freq; unsigned short format; unsigned char channels, silence;
    unsigned short samples, padding; uint32_t size; void (*callback)(void *, unsigned char *, int); void *userdata; };
int SDL_OpenAudio(struct SDL_AudioSpec_ *want, struct SDL_AudioSpec_ *have) {
    REAL(int, SDL_OpenAudio, struct SDL_AudioSpec_ *, struct SDL_AudioSpec_ *);
    if (!ok || !want) return real(want, have);
    int r;
    if (audio_pump_enabled()) { pump_on = 1; r = audio_pump_open(want, have, real); }   /* audio.c */
    else r = real(want, have);
    /* the mic after the output: the pump's output feeds its echo gate, and the two must not be opened at once */
    static int mic_started;
    if (!mic_started) { mic_started = 1; audio_mic_start(); }
    return r;
}

/* ---------- SDL interception ---------- */
static struct { void *t; int w, h; } other[16]; static int nother;

static stex *find(void *t) { for (int i = 0; i < nt; i++) if (T[i].tex == t) return &T[i]; return 0; }

void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) {
    REAL(void *, SDL_CreateWindow, const char *, int, int, int, int, uint32_t);
    void *win = real(title, x, y, w, h, flags);
    if (!window) window = win;
    return win;
}

void *SDL_CreateTexture(void *rn, uint32_t fmt, int access, int w, int h) {
    REAL(void *, SDL_CreateTexture, void *, uint32_t, int, int, int);
    void *t = real(rn, fmt, access, w, h);
    int screen = fmt == FMT_ARGB8888 && ((w == 256 && h == 192) || (w == 512 && h == 384));
    int menu = fmt == FMT_RGB565 && w >= 256 && h >= 192;
    if (!ok || !t || access != 1 || !(screen || menu)) {
        if (t && nother < 16) { other[nother].t = t; other[nother].w = w; other[nother].h = h; nother++; }
        return t;
    }
    lock(&mu);
    stex *s = find(t);
    if (!s) for (int i = 0; i < nt && !s; i++) if (!T[i].tex) s = &T[i];
    if (!s && nt < 6) s = &T[nt++];
    if (s) {
        for (int k = 0; k < s->nb; k++) {  /* recycled slot: free buffers no panel holds, park the others */
            dbuf *b = &s->b[k];
            if (b->state == FREE || b->state == WRITTEN || b->state == WRITING) { freebuf(b); continue; }
            dbuf *g = 0;
            for (int j = 0; j < NGRAVE && !g; j++) if (!grave[j].map) g = &grave[j];
            if (g) {
                *g = *b;                /* the panel pointers follow the buffer to its new home */
                for (int i = 0; i < 2; i++) {
                    if (P[i].ready == b) P[i].ready = g;
                    for (int j = 0; j < P[i].nq; j++) if (P[i].q[j] == b) P[i].q[j] = g;
                    if (P[i].queued == b) P[i].queued = g;
                    if (P[i].scan == b) P[i].scan = g;
                    if (P[i].src == b) P[i].src = g;
                }
            } else LOG("[dsflip] graveyard full: leaking a buffer\n");
            memset(b, 0, sizeof *b); b->fence = -1;
        }
        s->tex = t; s->kind = screen ? K_SCREEN : K_MENU; s->w = w; s->h = h; s->writing = s->written = -1;
        if (screen) dsflip_screen_w = w;
        s->nb = screen ? NBUF : 3;
        int bw = w, bh = h;
        if (!screen && !menu_hw) { bw = P[1].mode.hdisplay; bh = P[1].mode.vdisplay; }
        int mem = screen && shader_on && shader_copy_mode;
        for (int k = 0; k < s->nb; k++)
            if (mem ? mkmem(&s->b[k], bw, bh) : mkbuf(&s->b[k], bw, bh, screen ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_RGB565, screen ? 32 : 16)) {
                LOG("[dsflip] alloc %dx%d failed\n", bw, bh); s->tex = 0; break;
            }
        LOG("[dsflip] %s texture %p %dx%d -> %d %s buffers %dx%d\n", screen ? "screen" : "menu", t, w, h, s->nb, mem ? "memory" : "dumb", bw, bh);
    }
    unlock(&mu);
    return t;
}

void SDL_DestroyTexture(void *t) {
    REAL(void, SDL_DestroyTexture, void *);
    lock(&mu);
    stex *s = find(t);
    if (s) s->tex = 0;
    unlock(&mu);
    real(t);
}

static int take_free(stex *s) {        /* with mu held */
    for (int k = 0; k < s->nb; k++) if (s->b[k].state == FREE) return k;
    return s->written >= 0 ? s->written : 0;   /* can't happen with enough buffers */
}

int SDL_LockTexture(void *t, const SDL_Rect *rc, void **px, int *pitch) {
    REAL(int, SDL_LockTexture, void *, const SDL_Rect *, void **, int *);
    stex *s = ok ? find(t) : 0;
    if (!s || s->kind != K_SCREEN || rc) return real(t, rc, px, pitch);
    lock(&mu);
    int k = take_free(s);
    s->b[k].state = WRITING; s->writing = k;
    unlock(&mu);
    *px = s->b[k].map; *pitch = (int)s->b[k].pitch;
    return 0;
}

static void finish_write(stex *s) {    /* with mu held */
    if (s->written >= 0 && s->b[s->written].state == WRITTEN) s->b[s->written].state = FREE;  /* never presented */
    s->b[s->writing].state = WRITTEN; s->written = s->writing; s->writing = -1;
}

void SDL_UnlockTexture(void *t) {
    REAL(void, SDL_UnlockTexture, void *);
    stex *s = ok ? find(t) : 0;
    if (!s || s->writing < 0) { real(t); return; }
    lock(&mu); finish_write(s); unlock(&mu);
}

int SDL_UpdateTexture(void *t, const SDL_Rect *rc, const void *pixels, int pitch) {
    REAL(int, SDL_UpdateTexture, void *, const SDL_Rect *, const void *, int);
    stex *s = ok ? find(t) : 0;
    if (!s || s->kind != K_MENU || rc) return real(t, rc, pixels, pitch);
    lock(&mu);
    int k = take_free(s);
    s->b[k].state = WRITING; s->writing = k;
    unlock(&mu);
    dbuf *b = &s->b[k];
    const uint8_t *src = pixels;
    if (b->w == (uint32_t)s->w && b->h == (uint32_t)s->h) {
        for (int y = 0; y < s->h; y++) memcpy((uint8_t *)b->map + y * b->pitch, src + y * pitch, s->w * 2);
    } else {                            /* nearest-neighbour downscale into the panel-sized buffer */
        static int xmap[2048];
        for (uint32_t x = 0; x < b->w; x++) xmap[x] = (int)(x * s->w / b->w);
        uint16_t row[2048];
        for (uint32_t y = 0; y < b->h; y++) {
            const uint16_t *sr = (const uint16_t *)(src + (y * s->h / b->h) * pitch);
            for (uint32_t x = 0; x < b->w; x++) row[x] = sr[xmap[x]];
            memcpy((uint8_t *)b->map + y * b->pitch, row, b->w * 2);
        }
    }
    lock(&mu); finish_write(s); unlock(&mu);
    return 0;
}

int SDL_RenderSetLogicalSize(void *rn, int w, int h) {
    REAL(int, SDL_RenderSetLogicalSize, void *, int, int);
    if (w > 0 && h > 0) { logical_w = w; logical_h = h; }
    return real(rn, w, h);
}

int SDL_RenderCopy(void *rn, void *t, const SDL_Rect *src, const SDL_Rect *dst) {
    REAL(int, SDL_RenderCopy, void *, void *, const SDL_Rect *, const SDL_Rect *);
    stex *s = ok ? find(t) : 0;
    if (!s) {                          /* other draws (overlays, stylus cursor) are not shown */
        if (cursor_log && dst)
            for (int i = 0; i < nother; i++) if (other[i].t == t && other[i].w == 32) {
                static int n; if (n++ % 10 == 0) LOG("[cursor] dst %d,%d %dx%d logical %dx%d\n", dst->x, dst->y, dst->w, dst->h, logical_w, logical_h);
            }
        return ok ? 0 : real(rn, t, src, dst);
    }
    if (s->kind == K_MENU) { pending_route[1] = s; pending_route[0] = 0; return 0; }   /* bottom = touch screen; top keeps the game */
    SDL_Rect d = dst ? *dst : (SDL_Rect){ 0, 0, logical_w, logical_h };
    int bottom = logical_w > logical_h ? d.x >= logical_w / 2 : d.y >= logical_h / 2;
    pending_route[bottom] = s; route_dst[bottom] = d;
    return 0;
}

int SDL_RenderClear(void *rn) {
    REAL(int, SDL_RenderClear, void *);
    return ok ? 0 : real(rn);
}

void SDL_RenderPresent(void *rn) {
    REAL(void, SDL_RenderPresent, void *);
    if (!ok) { real(rn); return; }
    {   /* DSFLIP_STALL_TEST=<s>: that long after the first frame, DraStic's main thread stops here for good, as it did
         * on 2026-10-05: tests the stall watch, the exit hotkey on a stuck game and the launcher's notice */
        static long long stall_test_at = -1;
        if (stall_test_at < 0) { const char *e = getenv("DSFLIP_STALL_TEST"); stall_test_at = e && atoi(e) > 0 ? now_us() + atoi(e) * 1000000LL : 0; }
        if (stall_test_at && now_us() >= stall_test_at) { LOG("[stall] DSFLIP_STALL_TEST: DraStic's main thread stops here\n"); for (;;) pause(); }
    }
    int held = 0;                       /* this present waited on a full queue */
    if (queue_wait_us && queue_depth && pacing_latch) {
        /* a full queue would drop its oldest frame for this one: hold DraStic until a commit makes room (it then
         * runs a few ms later, and its audio ring covers that), at most queue_wait_us. The queue sits full once it
         * has filled (the panels are only ~50 ppm faster than DraStic), so without this every early frame was a
         * drop, and at lower clocks frames come early more often. Game screens only, never DraStic's menu. */
        lock(&mu);
        long long t0 = 0;
        while (ph_n >= 120) {   /* (held stays 0 unless this loop waits) */
            int full = 0;
            for (int i = 0; i < 2; i++) {
                stex *s = pending_route[i]; if (!s || s->kind != K_SCREEN) continue;
                if ((P[i].ready != 0) + P[i].nq + (P[i].src != 0) > queue_depth) full = 1;
            }
            if (!full) break;
            long long now = now_us(); if (!t0) t0 = now;
            if (now - t0 >= queue_wait_us) break;
            long long dl = t0 + queue_wait_us;
            struct timespec ts = { dl / 1000000, (dl % 1000000) * 1000 };
            pthread_cond_timedwait(&qcond, &mtx[0], &ts);
        }
        if (t0) { long long w = now_us() - t0; st_qwait_n++; st_qwait_us += w; if (w > st_qwait_max) st_qwait_max = w; lk_t = now_us(); held = 1; }
        unlock(&mu);
    }
    long long tp = now_us();
    {   /* this frame's CPU time on DraStic's main thread (the one presenting) */
        static long long last; struct timespec ct; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ct);
        long long c = ct.tv_sec * 1000000000LL + ct.tv_nsec, w = last ? c - last : 0; last = c;
        if (w > dsflip_frame_work_max) dsflip_frame_work_max = w;
    }
    lock(&mu);
    st_present++; dsflip_presents++;
    if (vbl_ref) {                       /* where in the refresh cycle did this frame arrive? */
        note_raw(tp);
        if (!shader_on && !held) note_phase(tp);  /* shader mode: when the shaded frame is ready (shade_pending) */
    }
    touch_rect_ok = pending_route[1] && pending_route[1]->kind == K_SCREEN;
    int menu_was = menu_touch;
    if (pending_route[1]) menu_touch = pending_route[1]->kind == K_MENU;
    int menu_opened = menu_touch && !menu_was;

    for (int i = 0; i < 2; i++) {
        stex *s = pending_route[i]; pending_route[i] = 0;
        if (!s) continue;
        dbuf *b;
        if (dsflip_hold && s->kind == K_SCREEN) {    /* the menu's screen stays up: this frame is dropped */
            if (s->written >= 0 && s->b[s->written].state == WRITTEN) s->b[s->written].state = FREE;
            s->written = -1;
            continue;
        }
        if (s->kind == K_BLACK) {
            b = &blk.b[0];
            int held = P[i].scan == b || P[i].queued == b || P[i].ready == b;
            for (int j = 0; j < P[i].nq; j++) held |= P[i].q[j] == b;
            if (held) continue;
        } else {
            if (s->written < 0 || s->b[s->written].state != WRITTEN) continue;
            b = &s->b[s->written]; s->written = -1; b->state = READY;
            if (shader_on && s->kind == K_SCREEN) {   /* the presenter shades it into a panel buffer */
                if (P[i].src) { release(P[i].src); st_drop++; st_drop_src++; }
                P[i].src = b; P[i].src_t = tp; P[i].src_held = held;
                continue;
            }
        }
        enqueue(i, b, tp);
    }
    unlock(&mu);
    uint64_t one = 1; if (write(shader_on ? wfd : efd, &one, 8) < 0) {}   /* shader mode: the worker shades first */
    if (menu_opened) status_card();
    ra_frame();
    resume_frame();
    menu_frame();
}
