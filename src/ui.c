// ui.c: the RetroAchievements overlay on the top panel -- pop-up cards (badge, title, text) and a progress pill,
// in the theme's DSi font, drawn into libdsflip's overlay plane on a thread of its own so no game frame waits.
//
// The plane is TOAST_W x TOAST_H ARGB8888 at the top of the top panel, scanned out over the game with per-pixel
// alpha. The layouts are designed at 72 rows (a 640 px wide panel) and scale with the plane's height (115 rows on the
// RG DS Plus's 1024x768 panels). The volume indicator (volume.c feeds it) shows over everything for 1.5 s. DRM's default blend mode is premultiplied, so everything here is drawn premultiplied. Pop-ups queue up
// (several achievements can unlock in one frame) and show one after another; the progress pill shows whenever no
// pop-up does. Look: ROCKNIXDS Pixel's, like the in-game menu (menu.c): Pixelify Sans, the theme's dark dotted card
// with a light border, its cyan / gold / red accents. Font: Pixelify Sans if that theme is installed, else the
// dii-ess-aye theme's DSi font, else Liberation Sans (stb_truetype reads them all);
// badges: PNG files (stb_image), drawn with rounded corners.
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"

uint32_t *dsflip_overlay_begin(int *pitch, int *w, int *h);
void dsflip_overlay_end(int show);
void dsflip_log(const char *fmt, ...);

static const char *font_paths[] = {
    0,                                                  /* DSFLIP_FONT, if set */
    "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Medium.ttf",
    "/storage/.config/emulationstation/themes/dii-ess-aye/assets/fonts/dsi_font.otf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   /* other firmwares' usual fonts */
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};
static stbtt_fontinfo font, font_reg;          /* the titles' font; the second lines' (Pixelify Sans Regular, or font) */
static stbtt_fontinfo *fnt = &font;             /* the one text_width and draw_text use */
static int font_ok;
#define REG_FONT "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Regular.ttf"

typedef struct { char l1[128], l2[192]; uint32_t accent; int ms; uint8_t *badge; int bw, bh; } popup;
#define QMAX 8
static popup q[QMAX]; static int qn;                  /* pending pop-ups, q[0] is the one showing */
static long long q_until;                              /* when q[0] goes away (0: not shown yet) */
static char prog_text[48]; static uint8_t *prog_badge; static int prog_bw, prog_bh, prog_on;
static int vol_pct, vol_on; static long long vol_until;    /* the volume indicator: percent, showing, until when */
static float sc_ = 1.0f;                                    /* layout scale: plane height / 72 */
#define S(v) ((int)((v) * sc_ + 0.5f))
#define SF(v) ((v) * sc_)
static int dirty;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int started;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---------- drawing (premultiplied ARGB) ---------- */
typedef struct { uint32_t *px; int pitch, w, h; } canvas;

static void blend(canvas *c, int x, int y, uint32_t rgb, float a) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint32_t *d = (uint32_t *)((char *)c->px + y * c->pitch) + x, o = *d;
    float ia = 1.0f - a;
    uint32_t r = (uint32_t)(((rgb >> 16) & 255) * a + ((o >> 16) & 255) * ia + 0.5f);
    uint32_t g = (uint32_t)(((rgb >> 8) & 255) * a + ((o >> 8) & 255) * ia + 0.5f);
    uint32_t b = (uint32_t)((rgb & 255) * a + (o & 255) * ia + 0.5f);
    uint32_t al = (uint32_t)(255.0f * a + (o >> 24) * ia + 0.5f);
    *d = al << 24 | r << 16 | g << 8 | b;
}

/* coverage of a rounded rectangle at pixel (x, y): signed distance, antialiased over one pixel */
static float rrect_cov(float px, float py, float x0, float y0, float x1, float y1, float rad) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f - rad, hy = (y1 - y0) * 0.5f - rad;
    float dx = fabsf(px - cx) - hx, dy = fabsf(py - cy) - hy;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.0f) - rad;
    return fminf(fmaxf(0.5f - d, 0.0f), 1.0f);
}

static void fill_rrect(canvas *c, int x0, int y0, int x1, int y1, float rad, uint32_t rgb, float alpha) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float cov = rrect_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, (float)x1, (float)y1, rad);
            if (cov > 0) blend(c, x, y, rgb, cov * alpha);
        }
}

static void stroke_rrect(canvas *c, int x0, int y0, int x1, int y1, float rad, uint32_t rgb, float alpha) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float outer = rrect_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, (float)x1, (float)y1, rad);
            float inner = rrect_cov(x + 0.5f, y + 0.5f, x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y1 - 1.0f, rad - 1.0f);
            if (outer - inner > 0) blend(c, x, y, rgb, (outer - inner) * alpha);
        }
}

/* an RGBA (straight alpha) image scaled bilinearly into a size x size square with rounded corners */
static void draw_image(canvas *c, const uint8_t *img, int iw, int ih, int x0, int y0, int size, float rad) {
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float cov = rrect_cov(x + 0.5f, y + 0.5f, 0, 0, (float)size, (float)size, rad);
            if (cov <= 0) continue;
            float sx = (x + 0.5f) * iw / size - 0.5f, sy = (y + 0.5f) * ih / size - 0.5f;
            int ix = (int)floorf(sx), iy = (int)floorf(sy); float fx = sx - ix, fy = sy - iy;
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int xx = ix + (k & 1), yy = iy + (k >> 1);
                if (xx < 0) xx = 0; if (yy < 0) yy = 0; if (xx >= iw) xx = iw - 1; if (yy >= ih) yy = ih - 1;
                float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const uint8_t *p = img + (yy * iw + xx) * 4;
                float a = p[3] / 255.0f;
                acc[0] += p[0] * a * w; acc[1] += p[1] * a * w; acc[2] += p[2] * a * w; acc[3] += a * w;
            }
            if (acc[3] <= 0) continue;
            uint32_t rgb = (uint32_t)(acc[0] / acc[3]) << 16 | (uint32_t)(acc[1] / acc[3]) << 8 | (uint32_t)(acc[2] / acc[3]);
            blend(c, x0 + x, y0 + y, rgb, acc[3] * cov);
        }
}

/* ---------- text ---------- */
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++, extra = 0;
    if (cp >= 0xF0) { cp &= 7; extra = 3; } else if (cp >= 0xE0) { cp &= 15; extra = 2; } else if (cp >= 0xC0) { cp &= 31; extra = 1; }
    while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 63);
    *s = (const char *)p;
    return cp;
}

static float text_width(const char *t, float px, int max_cp) {
    float sc = stbtt_ScaleForPixelHeight(fnt, px), w = 0; int prev = 0, n = 0;
    for (const char *s = t; *s && n < max_cp; n++) {
        int cp = utf8_next(&s), adv, lsb;
        stbtt_GetCodepointHMetrics(fnt, cp, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(fnt, prev, cp) * sc;
        w += adv * sc; prev = cp;
    }
    return w;
}

/* draw at baseline (x, y); if wider than maxw, cut with "..." */
static void draw_text(canvas *c, const char *t, float x, int y, float px, uint32_t rgb, float maxw) {
    int n = 0; for (const char *s = t; *s; n++) utf8_next(&s);
    int keep = n, dots = 0;
    if (text_width(t, px, n) > maxw) {
        float dw = text_width("...", px, 3);
        while (keep > 0 && text_width(t, px, keep) + dw > maxw) keep--;
        dots = 1;
    }
    float sc = stbtt_ScaleForPixelHeight(fnt, px);
    int prev = 0; const char *s = t;
    for (int i = 0; i < keep + (dots ? 3 : 0); i++) {
        int cp = i < keep ? utf8_next(&s) : '.';
        if (prev) x += stbtt_GetCodepointKernAdvance(fnt, prev, cp) * sc;
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(fnt, cp, &adv, &lsb);
        float sub = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(fnt, cp, sc, sc, sub, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw < 256 && gh < 256) {
            static unsigned char g[256 * 256];
            stbtt_MakeCodepointBitmapSubpixel(fnt, g, gw, gh, gw, sc, sc, sub, 0, cp);
            for (int yy = 0; yy < gh; yy++)
                for (int xx = 0; xx < gw; xx++)
                    if (g[yy * gw + xx]) blend(c, (int)floorf(x) + x0 + xx, y + y0 + yy, rgb, g[yy * gw + xx] / 255.0f);
        }
        x += adv * sc; prev = cp;
    }
}

/* ---------- the layouts (ROCKNIXDS Pixel) ---------- */
#define P_PANEL 0x1a222c
#define P_DOT   0x232d39
#define P_LIGHT 0xe7eef6
#define P_GREY  0xa9b4c2
#define P_LINE  0x2e3a49
/* the theme's card: a light border, the dark panel with its dot pattern */
static void card(canvas *c, int x0, int y0, int x1, int y1, float rad, uint32_t border) {
    fill_rrect(c, x0, y0, x1, y1, rad, border, 1.0f);
    int bw = S(2) < 2 ? 2 : S(2);
    fill_rrect(c, x0 + bw, y0 + bw, x1 - bw, y1 - bw, rad - bw, P_PANEL, 1.0f);
    int step = S(5) < 3 ? 3 : S(5), d = S(1) < 1 ? 1 : S(1), in = bw + (int)(rad * 0.4f);
    for (int y = y0 + in + step / 2; y + d <= y1 - in; y += step)
        for (int x = x0 + in + step / 2; x + d <= x1 - in; x += step)
            for (int yy = 0; yy < d; yy++) for (int xx = 0; xx < d; xx++) blend(c, x + xx, y + yy, P_DOT, 1.0f);
}
/* a badge in the menu's frame: light border, rounded */
static void framed(canvas *c, const uint8_t *img, int iw, int ih, int x, int y, int size) {
    int bw = S(2) < 2 ? 2 : S(2);
    fill_rrect(c, x, y, x + size, y + size, SF(6), P_LIGHT, 1.0f);
    draw_image(c, img, iw, ih, x + bw, y + bw, size - 2 * bw, SF(4));
}
/* pixel art rows ('#'), scaled by k */
static void pix(canvas *c, const char *const *rows, int n, int x, int y, int k, uint32_t rgb) {
    for (int r = 0; r < n; r++) for (int col = 0; rows[r][col]; col++)
        if (rows[r][col] == '#') for (int yy = 0; yy < k; yy++) for (int xx = 0; xx < k; xx++) blend(c, x + col * k + xx, y + r * k + yy, rgb, 1.0f);
}
static const char *const PX_STAR[9] = { "....#....", "...###...", "#########", ".#######.", "..#####..", "..##.##..", ".##...##.", "##.....##", "........." };
static const char *const PX_INFO[9] = { "...###...", "..#####..", "..##.##..", "....##...", "...##....", "...##....", ".........", "...##....", "...##...." };
static const char *const PX_VOL[12] = { "............", ".....#......", "....##...#..", "...###....#.", "####.#..#..#", "#..#.#...#.#",
    "#..#.#...#.#", "####.#..#..#", "...###....#.", "....##...#..", ".....#......", "............" };
static const char *const PX_MUTE[12] = { "............", ".....#......", "....##......", "...###.#...#", "####.#..#.#.", "#..#.#...#..",
    "#..#.#...#..", "####.#..#.#.", "...###.#...#", "....##......", ".....#......", "............" };

static void render_popup(canvas *c, const popup *p) {
    int x0 = S(8), y0 = S(4), x1 = c->w - S(8), y1 = c->h - S(4), h = y1 - y0;
    card(c, x0, y0, x1, y1, SF(10), P_LIGHT);
    int tx, bs = h - S(16);
    if (p->badge) { framed(c, p->badge, p->bw, p->bh, x0 + S(8), y0 + S(8), bs); tx = x0 + S(8) + bs + S(12); }
    else {                                              /* no badge: an icon tile in the accent colour */
        fill_rrect(c, x0 + S(8), y0 + S(8), x0 + S(8) + bs, y0 + S(8) + bs, SF(6), p->accent, 1.0f);
        int k = bs / 14 > 0 ? bs / 14 : 1;
        pix(c, p->accent == 0xffd84a ? PX_STAR : PX_INFO, 9, x0 + S(8) + (bs - 9 * k) / 2, y0 + S(8) + (bs - 9 * k) / 2, k, 0x12181f);
        tx = x0 + S(8) + bs + S(12);
    }
    float maxw = (float)(x1 - S(14) - tx);
    fnt = &font;     draw_text(c, p->l1, (float)tx, y0 + S(26), SF(17), p->accent, maxw);
    fnt = &font_reg; draw_text(c, p->l2, (float)tx, y0 + S(52), SF(20), P_LIGHT, maxw);
    fnt = &font;
}

static void render_pill(canvas *c) {
    float tw = text_width(prog_text, SF(19), 64);
    int h = S(40), bs = prog_badge ? S(30) : 0, w = S(14) + bs + (bs ? S(10) : 0) + (int)ceilf(tw) + S(16);
    int x1 = c->w - S(8), x0 = x1 - w, y0 = S(4), y1 = y0 + h;
    card(c, x0, y0, x1, y1, SF(10), P_LIGHT);
    if (prog_badge) framed(c, prog_badge, prog_bw, prog_bh, x0 + S(6), y0 + S(5), bs);
    draw_text(c, prog_text, (float)(x0 + S(12) + bs + (bs ? S(4) : 0)), y0 + S(27), SF(19), P_LIGHT, tw + 2);
}

/* the volume indicator: a speaker, the menu's 10-segment bar and the percentage */
static void render_volume(canvas *c, int pct) {
    int w = S(310), h = S(42), x0 = (c->w - w) / 2, y0 = S(4), x1 = x0 + w, y1 = y0 + h, cy = (y0 + y1) / 2;
    card(c, x0, y0, x1, y1, SF(10), P_LIGHT);
    int k = S(2) < 2 ? 2 : S(2);
    pix(c, pct <= 0 ? PX_MUTE : PX_VOL, 12, x0 + S(14), cy - 6 * k, k, pct <= 0 ? 0xff8a96 : P_LIGHT);
    char t[8]; snprintf(t, sizeof t, "%d%%", pct);
    float tw = text_width(t, SF(19), 8);
    draw_text(c, t, (float)(x1 - S(14)) - tw, y0 + S(28), SF(19), 0x7ec8ee, tw + 2);
    int bx0 = x0 + S(14) + 12 * k + S(12), bx1 = x1 - S(14) - (int)ceilf(text_width("100%", SF(19), 8)) - S(10);
    float seg = (bx1 - bx0) / 10.0f; int gap = S(3) < 2 ? 2 : S(3);
    for (int i = 0; i < 10; i++) {
        int sx = bx0 + (int)(i * seg), ex = bx0 + (int)((i + 1) * seg) - gap;
        uint32_t col = i < (pct + 5) / 10 ? 0x7ec8ee : P_LINE;
        for (int y = cy - S(8); y < cy + S(8); y++) for (int x = sx; x < ex; x++) blend(c, x, y, col, 1.0f);
    }
}

static void render(void) {                            /* with mx held */
    int pitch, w, h;
    uint32_t *px = dsflip_overlay_begin(&pitch, &w, &h);
    if (!px) return;
    int show = qn > 0 || prog_on || vol_on;
    if (show) {
        for (int y = 0; y < h; y++) memset((char *)px + y * pitch, 0, (size_t)w * 4);
        canvas c = { px, pitch, w, h };
        sc_ = h / 72.0f;
        if (vol_on) render_volume(&c, vol_pct); else if (qn) render_popup(&c, &q[0]); else render_pill(&c);
    }
    dsflip_overlay_end(show);
    if (vol_on) dsflip_log("[ui] overlay: volume %d%%\n", vol_pct);
    else dsflip_log("[ui] overlay: %s\n", qn ? q[0].l2 : prog_on ? prog_text : "hidden");
}

static void *ui_thread(void *a) {
    (void)a;
    pthread_mutex_lock(&mx);
    for (;;) {
        long long t = now_ms();
        if (qn && q_until && t >= q_until) {           /* the pop-up's time is up: next one, or the pill, or nothing */
            free(q[0].badge); memmove(q, q + 1, (size_t)(qn - 1) * sizeof *q); qn--; q_until = 0; dirty = 1;
        }
        if (qn && !q_until) { q_until = t + q[0].ms; dirty = 1; }
        if (vol_on && t >= vol_until) { vol_on = 0; dirty = 1; }
        if (dirty) { dirty = 0; render(); }
        if (qn || vol_on) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            long long until = qn ? q_until : vol_until;
            if (vol_on && vol_until < until) until = vol_until;
            long long wait = until - now_ms(); if (wait < 1) wait = 1;
            ts.tv_sec += wait / 1000; ts.tv_nsec += (wait % 1000) * 1000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&cv, &mx, &ts);
        } else pthread_cond_wait(&cv, &mx);
    }
    return 0;
}

static int ui_start(void) {                           /* with mx held */
    if (started) return font_ok;
    started = 1;
    font_paths[0] = getenv("DSFLIP_FONT");
    for (size_t i = 0; i < sizeof font_paths / sizeof *font_paths && !font_ok; i++) {
        FILE *f = font_paths[i] ? fopen(font_paths[i], "rb") : 0; if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *d = n > 0 ? malloc((size_t)n) : 0;
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            font_ok = 1; dsflip_log("[ui] font %s\n", font_paths[i]);
        } else free(d);
        fclose(f);
    }
    if (!font_ok) { dsflip_log("[ui] no font: pop-ups off\n"); return 0; }
    font_reg = font;                                 /* Pixelify Sans Regular for second lines, when the theme has it */
    {   FILE *f = fopen(REG_FONT, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            unsigned char *d = n > 0 ? malloc((size_t)n) : 0;
            if (!(d && fread(d, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&font_reg, d, stbtt_GetFontOffsetForIndex(d, 0)))) { free(d); font_reg = font; }
            fclose(f);
        } }
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &at, ui_thread, 0)) pthread_setname_np(t, "dsf-ui");
    pthread_attr_destroy(&at);
    return 1;
}

static uint8_t *load_png(const char *path, int *w, int *h) {
    if (!path || !*path) return 0;
    int n; uint8_t *d = stbi_load(path, w, h, &n, 4);
    if (d && (*w <= 0 || *h <= 0 || *w > 512 || *h > 512)) { stbi_image_free(d); return 0; }
    return d;
}

/* ---------- public ---------- */
/* queue a pop-up; badge: a PNG path or NULL. Thread-safe. */
void ui_popup(const char *l1, const char *l2, const char *badge_png, uint32_t accent, int ms) {
    pthread_mutex_lock(&mx);
    if (!ui_start()) { pthread_mutex_unlock(&mx); return; }
    if (qn == QMAX) { free(q[QMAX - 1].badge); qn--; }  /* keep the newest */
    popup *p = &q[qn++]; memset(p, 0, sizeof *p);
    snprintf(p->l1, sizeof p->l1, "%s", l1 ? l1 : ""); snprintf(p->l2, sizeof p->l2, "%s", l2 ? l2 : "");
    /* the theme's accents: gold for achievements, cyan for news, red for trouble */
    accent &= 0xffffff;
    if (accent != 0xffd84a) {
        int r = accent >> 16, b = accent & 255;
        accent = r > b ? 0xff8a96 : 0x7ec8ee;
    }
    p->accent = accent; p->ms = ms;
    p->badge = load_png(badge_png, &p->bw, &p->bh);
    dirty = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mx);
}

/* show (text != NULL) or hide the progress pill. Thread-safe. */
void ui_progress(const char *text, const char *badge_png) {
    pthread_mutex_lock(&mx);
    if (!ui_start()) { pthread_mutex_unlock(&mx); return; }
    if (prog_badge) { stbi_image_free(prog_badge); prog_badge = 0; }
    prog_on = text && *text;
    if (prog_on) { snprintf(prog_text, sizeof prog_text, "%s", text); prog_badge = load_png(badge_png, &prog_bw, &prog_bh); }
    dirty = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mx);
}

/* show the volume indicator at pct for 1.5 s (again: refreshed, the time restarts). Thread-safe. */
void ui_volume(int pct) {
    pthread_mutex_lock(&mx);
    if (!ui_start()) { pthread_mutex_unlock(&mx); return; }
    vol_pct = pct; vol_on = 1; vol_until = now_ms() + 1500;
    dirty = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mx);
}
