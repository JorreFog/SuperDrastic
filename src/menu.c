// menu.c: the in-game menu. DraStic's menu button (L3 on the RG DS) opens this instead of DraStic's own menu: it is
// drawn in ROCKNIXDS Pixel's look (Pixelify Sans, the theme's colours, button pills and sounds) on both panels while
// DraStic waits inside SDL_PollEvent, so the game, its sound and its clock all stand still.
//
// Bottom panel: six tiles (Resume, Save, Load, Quick settings, DraStic menu, Quit game) over a Brightness row (both
// panels' backlights, as Quick settings has it: down from the tiles, left / right or L / R to change), then a page for
// each; X (or its pill in the footer) lists the hotkeys, as drastic.cfg and SuperDrastic have them.
// Top panel: the game's last frame, dimmed, under a card with the game, this session's play time, the last save and
// RetroAchievements progress; while a save slot is picked, that slot's picture instead. D-pad, left stick and touch
// all work; A picks, B goes back, the menu button (or START) closes it.
//
// Saving and loading press DraStic's own save/load state controls once the menu has closed (resume.c), with the
// slot file renamed or looked up for the slot picked here. A load first saves the current game to an undo file, so a
// wrong slot never costs progress ("Undo last load", Y on the Load page). While a load runs DraStic's frames and
// sound are held back (dsflip_hold, audio_mute) and the menu's "Loading" screen stays up. Slot pictures come from the
// savestates themselves: a DraStic .dss starts (after a 0x44-byte header) with a zlib stream whose first 196,608
// bytes are both DS screens, 256x384 RGB565; both are shown, top over bottom (a state with only the top one is
// shown the way it was before, one screen wide).
//
// DSFLIP_MENU=0: DraStic's own menu, as before. DSFLIP_MENU_TEST=<n> opens the menu 3 s in, on the main page (1),
// Save (2), Load (3), Quick settings (4) or the quit question (5), for tests.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <glob.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"
#include "lang.h"

/* dsflip.c */
int dsflip_menu_canvas(int i, uint32_t **px, int *pitch, int *w, int *h);
void dsflip_menu_show(int i, int k);
int dsflip_menu_grab_top(uint32_t *dst, int pitch, int w, int h);
int dsflip_battery(int *charging);
int dsflip_drastic_menu(void);
extern volatile int dsflip_hold;
void dsflip_mic_key(int down);
int lid_home_pressed(void);   /* lid.c: HOME let go while the menu is open */
int lid_home_on(void);        /* lid.c: HOME opens the menu */
void dsflip_toast(const char *l1, const char *l2, uint32_t accent, int ms);
void dsflip_log(const char *fmt, ...);
void cpugov_boost(int ms);
void cpugov_menu(int open);
void SDL_PauseAudio(int on);
/* audio.c */
void audio_mute(int on);
int audio_ui_load(int id, const int16_t *pcm, int frames, int rate, int ch);
void audio_ui_play(int id);
int audio_mic_meter(float *level, float *floor_, float *thresh);
void audio_mic_quiet(int on);
/* ra.c */
int ra_progress(unsigned *unlocked, unsigned *total);
typedef struct { char title[96], desc[200], badge[600]; unsigned points; long long when; } ra_recent_t;
int ra_recent(ra_recent_t *out, int max);
/* resume.c */
int resume_control_button(const char *name);
int resume_menu_save(const char *path, const char *msg, const char *msg2);
int resume_menu_load(const char *path, const char *backup, const char *msg, const char *msg2);
int resume_menu_busy(void);
void resume_press(int b);
int resume_quit(void);
void resume_quit_nosave(void);
int resume_on(void);
int resume_quit_pending(void);
int resume_exit_combo(int *start, int *select, int *menu, int *menu2);

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---------- colours: ROCKNIXDS Pixel, dark; or BananaOS's (DSFLIP_MENU_THEME=banana: BananaLinux tools/banana-palette.sh's
 * map over the same theme, the blue accents banana yellow with dark text on them) ---------- */
static uint32_t C_BG = 0x141b23;
static uint32_t C_DOT = 0x1f2833;
static uint32_t C_PANEL = 0x1a222c;
static uint32_t C_BORDER = 0x3d4a5a;
static uint32_t C_LIGHT = 0xe7eef6;
static uint32_t C_GREY = 0x93a0b0;
static uint32_t C_BLUE = 0x1a6ea3;
static uint32_t C_CYAN = 0x7ec8ee;
static uint32_t C_DARK = 0x12181f;
static uint32_t C_RED = 0xa3283a;
static uint32_t C_REDL = 0xff8a96;
static uint32_t C_REDDIM = 0x6b2a33;
static uint32_t C_TEAL = 0x1f4645;
static uint32_t C_TEALDOT = 0x27524f;
static uint32_t C_TEALTXT = 0xa9c3c2;
static uint32_t C_TEALLN = 0x4c6a69;
static uint32_t C_LINE = 0x2e3a49;
static uint32_t C_ONSEL = 0xffffff, C_ONSELSUB = 0xd6e6f5;      /* text and icons on the selected (blue) plate */
static uint32_t C_PILLBG = 0x243040, C_BODY = 0xc2ccd8, C_DIM = 0x5a6676, C_DIMBG = 0x161d26, C_DIMBORDER = 0x2a3442;
static uint32_t C_EMPTY0 = 0x1a222c, C_EMPTY1 = 0x202a36, C_NOPIC = 0x2c3a4c, C_BARSEL = 0xe7eef6, C_BAROFFSEL = 0x2e3a49;
static void theme_banana(void) {
    C_BG = 0x191820; C_DOT = 0x25232b; C_PANEL = 0x211f26; C_BORDER = 0x4a443f; C_LIGHT = 0xf1ede2; C_GREY = 0xa39d8e;
    C_BLUE = 0xf0b429; C_CYAN = 0xffe68a; C_DARK = 0x17161c; C_LINE = 0x3a3540;
    C_ONSEL = 0x17161c; C_ONSELSUB = 0x3a2e14; C_PILLBG = 0x2e2b33; C_BODY = 0xd5cfc2; C_DIM = 0x6b6559;
    C_DIMBG = 0x1a191f; C_DIMBORDER = 0x3a3540; C_EMPTY0 = 0x1a191f; C_EMPTY1 = 0x24222a; C_NOPIC = 0x38333b;
    C_BARSEL = 0x17161c; C_BAROFFSEL = 0xc9921f;
}

/* ---------- drawing: XRGB8888 canvases in ordinary memory, copied to the panel's buffer when done ---------- */
typedef struct { uint32_t *px; int w, h; float s; } canvas;   /* s: panel width / 640 (layouts are made at 640x480) */
#define S(v) ((int)lroundf((v) * c->s))

static inline void blendpx(canvas *c, int x, int y, uint32_t rgb, float a) {
    if ((unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h || a <= 0.0f) return;
    uint32_t *d = c->px + (size_t)y * c->w + x;
    if (a >= 1.0f) { *d = rgb; return; }
    uint32_t o = *d; float ia = 1.0f - a;
    uint32_t r = (uint32_t)(((rgb >> 16) & 255) * a + ((o >> 16) & 255) * ia + 0.5f);
    uint32_t g = (uint32_t)(((rgb >> 8) & 255) * a + ((o >> 8) & 255) * ia + 0.5f);
    uint32_t b = (uint32_t)((rgb & 255) * a + (o & 255) * ia + 0.5f);
    *d = r << 16 | g << 8 | b;
}
static void fill(canvas *c, int x0, int y0, int x1, int y1, uint32_t rgb, float a) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > c->w) x1 = c->w; if (y1 > c->h) y1 = c->h;
    for (int y = y0; y < y1; y++) {
        if (a >= 1.0f) { uint32_t *d = c->px + (size_t)y * c->w; for (int x = x0; x < x1; x++) d[x] = rgb; }
        else for (int x = x0; x < x1; x++) blendpx(c, x, y, rgb, a);
    }
}
/* the theme's dotted background: a dot every `step` px (6 at 640 wide) */
static void dots(canvas *c, int x0, int y0, int x1, int y1, uint32_t bg, uint32_t dot) {
    fill(c, x0, y0, x1, y1, bg, 1.0f);
    int step = S(6), d = S(1) < 1 ? 1 : S(1);
    for (int y = y0 + step / 2; y + d <= y1; y += step) for (int x = x0 + step / 2; x + d <= x1; x += step) fill(c, x, y, x + d, y + d, dot, 1.0f);
}
/* coverage of a rounded rectangle at a pixel centre: signed distance, antialiased over one pixel */
static float rr_cov(float px, float py, float x0, float y0, float x1, float y1, float rad) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hx = (x1 - x0) * 0.5f - rad, hy = (y1 - y0) * 0.5f - rad;
    float dx = fabsf(px - cx) - hx, dy = fabsf(py - cy) - hy;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    float dd = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.0f) - rad;
    return fminf(fmaxf(0.5f - dd, 0.0f), 1.0f);
}
static void rrect(canvas *c, int x0, int y0, int x1, int y1, float rad, uint32_t rgb, float a) {
    int r = (int)ceilf(rad) + 1;
    for (int y = y0; y < y1; y++) {
        int edge_y = y < y0 + r || y >= y1 - r;
        if (!edge_y) { fill(c, x0, y, x1, y + 1, rgb, a); continue; }
        for (int x = x0; x < x1; x++) {
            if (x >= x0 + r && x < x1 - r) { blendpx(c, x, y, rgb, a); continue; }
            float cov = rr_cov(x + 0.5f, y + 0.5f, (float)x0, (float)y0, (float)x1, (float)y1, rad);
            if (cov > 0) blendpx(c, x, y, rgb, cov * a);
        }
    }
}
/* a filled, bordered rounded box (the theme's cards and buttons) */
static void box(canvas *c, int x0, int y0, int x1, int y1, float rad, int bw, uint32_t border, uint32_t inside) {
    rrect(c, x0, y0, x1, y1, rad, border, 1.0f);
    rrect(c, x0 + bw, y0 + bw, x1 - bw, y1 - bw, fmaxf(rad - bw, 1.0f), inside, 1.0f);
}
static void dotbox(canvas *c, int x0, int y0, int x1, int y1, float rad, int bw, uint32_t border, uint32_t bg, uint32_t dot) {
    box(c, x0, y0, x1, y1, rad, bw, border, bg);
    int step = S(6), d = S(1) < 1 ? 1 : S(1), in = bw + (int)(rad * 0.3f);
    for (int y = y0 + in + step / 2; y + d <= y1 - in; y += step)
        for (int x = x0 + in + step / 2; x + d <= x1 - in; x += step) fill(c, x, y, x + d, y + d, dot, 1.0f);
}

/* pixel art: rows of '#' scaled by an integer factor */
static void bitmap(canvas *c, const char *const *rows, int n, int x, int y, int k, uint32_t rgb) {
    for (int r = 0; r < n; r++) for (int col = 0; rows[r][col]; col++)
        if (rows[r][col] == '#') fill(c, x + col * k, y + r * k, x + (col + 1) * k, y + (r + 1) * k, rgb, 1.0f);
}
static const char *const IC_PLAY[12] = { "............", "...#........", "...##.......", "...###......", "...####.....", "...#####....",
    "...######...", "...#####....", "...####.....", "...###......", "...##.......", "...#........" };
static const char *const IC_SAVE[12] = { "##########..", "#..#####.##.", "#..#####..##", "#..#####...#", "#..........#", "#..........#",
    "#.########.#", "#.#......#.#", "#.#......#.#", "#.#......#.#", "#.########.#", "############" };
static const char *const IC_LOAD[12] = { "............", ".####.......", "#....#......", "#.....######", "#..........#", "#....##....#",
    "#....##....#", "#..######..#", "#...####...#", "#....##....#", "#..........#", "############" };
static const char *const IC_SLIDERS[12] = { "............", "..##........", "############", "..##........", "............", "........##..",
    "############", "........##..", "............", ".....##.....", "############", ".....##....." };
static const char *const IC_LIST[12] = { "............", "##.#########", "##.#########", "............", "............", "##.#########",
    "##.#########", "............", "............", "##.#########", "##.#########", "............" };
static const char *const IC_EXIT[12] = { "######......", "#....#......", "#....#...#..", "#....#...##.", "#....#######", "#....#######",
    "#....#...##.", "#....#...#..", "#....#......", "#....#......", "######......", "............" };
static const char *const IC_VOL[12] = { "............", ".....#......", "....##...#..", "...###....#.", "####.#..#..#", "#..#.#...#.#",
    "#..#.#...#.#", "####.#..#..#", "...###....#.", "....##...#..", ".....#......", "............" };
static const char *const IC_SUN[12] = { ".....##.....", ".#...##...#.", "..#......#..", "....####....", "...######...", "##.######.##",
    "##.######.##", "...######...", "....####....", "..#......#..", ".#...##...#.", ".....##....." };
static const char *const IC_MIC[12] = { "....####....", "...######...", "...######...", "...######...", "...######...", ".#.######.#.",
    ".#..####..#.", "..#......#..", "...######...", ".....##.....", ".....##.....", "...######..." };
/* the slot numbers' 5x7 face (the same shapes as ROCKNIXDS's Pixelify Sans after its issue #34 fix) */
static const char *const DIG[10][7] = {
    { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." }, { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." },
    { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" }, { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." },
    { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." }, { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." },
    { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." }, { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." },
    { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." }, { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } };

/* ---------- text (stb_truetype) ---------- */
typedef struct { stbtt_fontinfo f; unsigned char *data; int ok; float asc; } font;   /* asc: per pixel of size */
static font F_MED, F_REG;
/* A translation's characters that Pixelify lacks (Chinese) come from Fusion Pixel 12px, a pixel font: drawn at a whole
 * multiple of its 12 px in the panel's pixels, at whole-pixel positions, so it stays sharp (lang.h) */
static font F_CJK;
static int font_load(font *F, const char *const *paths, int n) {
    for (int i = 0; i < n && !F->ok; i++) {
        if (!paths[i]) continue;
        FILE *f = fopen(paths[i], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *d = sz > 0 ? malloc((size_t)sz) : 0;
        if (d && fread(d, 1, (size_t)sz, f) == (size_t)sz && stbtt_InitFont(&F->f, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            int a, de, lg; stbtt_GetFontVMetrics(&F->f, &a, &de, &lg);
            F->data = d; F->ok = 1; F->asc = (float)a / (a - de);
            dsflip_log("[menu] font %s\n", paths[i]);
        } else free(d);
        fclose(f);
    }
    return F->ok;
}
static int utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++, extra = 0;
    if (cp >= 0xF0) { cp &= 7; extra = 3; } else if (cp >= 0xE0) { cp &= 15; extra = 2; } else if (cp >= 0xC0) { cp &= 31; extra = 1; }
    while (extra-- && (*p & 0xC0) == 0x80) cp = cp << 6 | (*p++ & 63);
    *s = (const char *)p;
    return cp;
}
/* the font a character is drawn from, and its scale: F's, or the pixel font's at k x 12 px for one F lacks */
static font *glyph_font(font *F, int cp, float px, float *sc) {
    if (F_CJK.ok && cp >= 0x80 && !stbtt_FindGlyphIndex(&F->f, cp) && stbtt_FindGlyphIndex(&F_CJK.f, cp)) {
        int k = (int)lroundf(px / 12.0f); if (k < 1) k = 1;
        *sc = stbtt_ScaleForMappingEmToPixels(&F_CJK.f, 12.0f * k);
        return &F_CJK;
    }
    *sc = stbtt_ScaleForPixelHeight(&F->f, px);
    return F;
}
static float text_w_raw(font *F, const char *t, float px, int max_cp) {
    if (!F->ok) return 0;
    float w = 0; int prev = 0, n = 0; font *pf = 0;
    for (const char *s = t; *s && n < max_cp; n++) {
        int cp = utf8_next(&s), adv, lsb; float sc;
        font *g = glyph_font(F, cp, px, &sc);
        if (g == &F_CJK) w = ceilf(w);   /* (whole pixels) */
        stbtt_GetCodepointHMetrics(&g->f, cp, &adv, &lsb);
        if (prev && g == pf) w += stbtt_GetCodepointKernAdvance(&g->f, prev, cp) * sc;
        w += adv * sc; prev = cp; pf = g;
    }
    return w;
}
/* text whose box starts at y (top), aligned left (0), centred (1) or right (2) on x; cut with "..." past maxw. As
 * given: text() and text_w() are the ones that translate */
static void text_raw(canvas *c, font *F, const char *t, float x, float y, float px, uint32_t rgb, float maxw, int align) {
    if (!F->ok || !*t) return;
    int n = 0; for (const char *s = t; *s; n++) utf8_next(&s);
    int keep = n, dots3 = 0;
    float w = text_w_raw(F, t, px, n);
    if (maxw > 0 && w > maxw) {
        float dw = text_w_raw(F, "...", px, 3);
        while (keep > 0 && text_w_raw(F, t, px, keep) + dw > maxw) keep--;
        dots3 = 1; w = text_w_raw(F, t, px, keep) + dw;
    }
    if (align == 1) x -= w / 2; else if (align == 2) x -= w;
    float pen = x;
    int base = (int)lroundf(y + F->asc * px);
    static unsigned char gb[256 * 256];
    int prev = 0; const char *s = t; font *pf = 0;
    for (int i = 0; i < keep + (dots3 ? 3 : 0); i++) {
        int cp = i < keep ? utf8_next(&s) : '.';
        float sc; font *g = glyph_font(F, cp, px, &sc);
        int cjk = g == &F_CJK;
        if (cjk) pen = ceilf(pen);
        if (prev && g == pf) pen += stbtt_GetCodepointKernAdvance(&g->f, prev, cp) * sc;
        int adv, lsb; stbtt_GetCodepointHMetrics(&g->f, cp, &adv, &lsb);
        int x0, y0, x1, y1; float fx = cjk ? 0 : pen - floorf(pen);
        stbtt_GetCodepointBitmapBoxSubpixel(&g->f, cp, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
            stbtt_MakeCodepointBitmapSubpixel(&g->f, gb, gw, gh, gw, sc, sc, fx, 0, cp);
            int ox = (int)floorf(pen) + x0, oy = base + y0;
            for (int yy = 0; yy < gh; yy++) for (int xx = 0; xx < gw; xx++) {
                int v = gb[yy * gw + xx];
                if (cjk) v = v >= 128 ? 255 : 0;   /* a pixel font: on or off */
                if (v) blendpx(c, ox + xx, oy + yy, rgb, v / 255.0f);
            }
        }
        pen += adv * sc; prev = cp; pf = g;
    }
}
/* the menu's own words, in the chosen language (lang.h); a game's name, an achievement's, a cheat's: text_raw */
static float text_w(font *F, const char *t, float px, int max_cp) { return text_w_raw(F, lang_tr(t), px, max_cp); }
static void text(canvas *c, font *F, const char *t, float x, float y, float px, uint32_t rgb, float maxw, int align) {
    text_raw(c, F, lang_tr(t), x, y, px, rgb, maxw, align);
}
/* split t into at most two lines that fit maxw (the second cut with "..." if needed); returns the line count. tr: t is
 * the menu's own text (translated first: whole, never in pieces). A line breaks at a space, or between two Chinese
 * characters (not before a closing mark nor after an opening one) */
static int wrap2(font *F, const char *t, float px, float maxw, char *l1, char *l2, size_t n, int tr) {
    if (tr) t = lang_tr(t);
    l1[0] = l2[0] = 0;
    if (text_w_raw(F, t, px, 1 << 20) <= maxw) { snprintf(l1, n, "%s", t); lang_keep(l1); return 1; }
    const char *best = 0; int skip = 0, prev = 0;
    for (const char *p = t; *p;) {
        const char *at = p; int cp = utf8_next(&p);
        int brk = cp == ' ' ? 1 : lang_break_between(prev, cp) ? 2 : 0;
        prev = cp;
        if (!brk) continue;
        char tmp[256]; size_t k = (size_t)(at - t); if (k >= sizeof tmp) break;
        memcpy(tmp, t, k); tmp[k] = 0;
        if (text_w_raw(F, tmp, px, 1 << 20) <= maxw) { best = at; skip = brk == 1; } else break;
    }
    if (!best) { snprintf(l1, n, "%s", t); lang_keep(l1); return 1; }
    snprintf(l1, n, "%.*s", (int)(best - t), t); snprintf(l2, n, "%s", best + skip);
    lang_keep(l1); lang_keep(l2);   /* (pieces of a line looked up already: drawn as they are) */
    return 2;
}

/* ---------- the game, its saves ---------- */
#define NSLOT 8
typedef struct { int used; time_t mt; uint16_t *pic; int pic_rows, pic_tried; } slot;   /* pic_rows: 384 both screens, 192 the top */
static slot SL[NSLOT];
static char rom[512], gbase[256], gtitle[200], ggenre[100], sdir[512], undo_path[600];
static int undo_ok;                 /* the undo file holds the game from before this session's last menu load */
static long long session_t0;
static int last_slot = -1;          /* the slot this session saved to or loaded last */

static void game_info(void) {
    FILE *f = fopen("/proc/self/cmdline", "rb");
    if (f) {
        char buf[4096]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
        for (size_t i = 0; i < n; i += strlen(buf + i) + 1) if (i && buf[i] && buf[i] != '-') snprintf(rom, sizeof rom, "%s", buf + i);
    }
    const char *fn = strrchr(rom, '/'); fn = fn ? fn + 1 : rom;
    snprintf(gbase, sizeof gbase, "%s", fn);
    char *dot = strrchr(gbase, '.'); if (dot) *dot = 0;
    /* the name ES shows: <name> of the gamelist entry whose <path> is this file */
    char gl[600]; snprintf(gl, sizeof gl, "%.*s/gamelist.xml", (int)(fn - rom > 0 ? fn - rom - 1 : 0), rom);
    if ((f = fopen(gl, "rb"))) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *x = sz > 0 && sz < (64 << 20) ? malloc((size_t)sz + 1) : 0;
        if (x && fread(x, 1, (size_t)sz, f) == (size_t)sz) {
            x[sz] = 0;
            char want[600]; snprintf(want, sizeof want, "<path>./%s</path>", fn);
            char *p = strstr(x, want);
            if (p) {
                char *end = strstr(p, "</game>"); if (end) *end = 0;
                char *a = strstr(p, "<name>"), *b = a ? strstr(a, "</name>") : 0;
                if (a && b) snprintf(gtitle, sizeof gtitle, "%.*s", (int)(b - a - 6), a + 6);
                a = strstr(p, "<genre>"); b = a ? strstr(a, "</genre>") : 0;
                if (a && b) snprintf(ggenre, sizeof ggenre, "%.*s", (int)(b - a - 7), a + 7);
            }
        }
        free(x); fclose(f);
    }
    if (!gtitle[0]) {               /* the file name without its "(USA)"-style tags */
        snprintf(gtitle, sizeof gtitle, "%s", gbase);
        char *p = strstr(gtitle, " ("); if (p && p != gtitle) *p = 0;
    }
    /* &amp; and friends, as ES writes them */
    const char *ent[][2] = { { "&amp;", "&" }, { "&apos;", "'" }, { "&quot;", "\"" }, { "&lt;", "<" }, { "&gt;", ">" } };
    for (char *t = gtitle; *t; t++) for (int k = 0; k < 5; k++) if (!strncmp(t, ent[k][0], strlen(ent[k][0]))) {
        size_t el = strlen(ent[k][0]); *t = ent[k][1][0]; memmove(t + 1, t + el, strlen(t + el) + 1);
    }
    char cwd[400]; struct stat st;
    if (getcwd(cwd, sizeof cwd) && snprintf(sdir, sizeof sdir, "%s/savestates", cwd) > 0 && !stat(sdir, &st) && S_ISDIR(st.st_mode)) {}
    else snprintf(sdir, sizeof sdir, "/storage/.config/drastic/savestates");
    snprintf(undo_path, sizeof undo_path, "/tmp/dsflip-undo.dss");
    dsflip_log("[menu] game '%s' (%s), savestates in %s\n", gtitle, gbase, sdir);
}
static void slot_path(int n, char *out, size_t sz) { snprintf(out, sz, "%s/%s_%d.dss", sdir, gbase, n); }
/* the game's name, for resume.c's pop-ups (DraStic's save / load state buttons) */
const char *menu_game_title(void) { if (!gbase[0]) game_info(); return gtitle; }

/* zlib, as ra.c loads it */
typedef struct {
    const uint8_t *next_in; unsigned avail_in; unsigned long total_in;
    uint8_t *next_out; unsigned avail_out; unsigned long total_out;
    const char *msg; void *state, *zalloc, *zfree, *opaque;
    int data_type; unsigned long adler, reserved;
} zs_t;
static int (*z_init)(zs_t *, const char *, int), (*z_inflate)(zs_t *, int), (*z_end)(zs_t *);
#define PIC_BYTES (256 * 192 * 2)   /* one screen */
/* the DS screens saved in a .dss, RGB565: both (*rows 384, top over bottom), or the top one alone (192); or 0 */
static uint16_t *read_pic(const char *path, int *rows) {
    if (!z_init) {
        void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL); if (!h) return 0;
        *(void **)&z_init = dlsym(h, "inflateInit_"); *(void **)&z_inflate = dlsym(h, "inflate"); *(void **)&z_end = dlsym(h, "inflateEnd");
        if (!z_init || !z_inflate || !z_end) { z_init = 0; return 0; }
    }
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint8_t hdr[0x44], in[32768]; uint16_t *out = 0;
    if (fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "DraStic-SaveState", 17) && (out = malloc(2 * PIC_BYTES))) {
        zs_t z; memset(&z, 0, sizeof z);
        if (z_init(&z, "1.2.11", (int)sizeof z) == 0) {
            z.next_out = (uint8_t *)out; z.avail_out = 2 * PIC_BYTES;
            int r = 0;
            while (z.avail_out && r == 0) {
                if (!z.avail_in) { size_t n = fread(in, 1, sizeof in, f); if (!n) break; z.next_in = in; z.avail_in = (unsigned)n; }
                r = z_inflate(&z, 0);
            }
            unsigned got = 2 * PIC_BYTES - z.avail_out; z_end(&z);
            *rows = got == 2 * PIC_BYTES ? 384 : 192;
            if (got < PIC_BYTES) { free(out); out = 0; }
        } else { free(out); out = 0; }
    }
    fclose(f);
    return out;
}
static void scan_slots(int pics) {
    for (int n = 0; n < NSLOT; n++) {
        char p[700]; slot_path(n, p, sizeof p); struct stat st;
        int used = !stat(p, &st) && st.st_size > 0x44;
        if (!used || st.st_mtime != SL[n].mt) { free(SL[n].pic); SL[n].pic = 0; SL[n].pic_tried = 0; }
        SL[n].used = used; SL[n].mt = used ? st.st_mtime : 0;
        if (pics && used && !SL[n].pic_tried) { SL[n].pic_tried = 1; SL[n].pic = read_pic(p, &SL[n].pic_rows); }
    }
}
static int newest_slot(void) {
    int b = -1; for (int n = 0; n < NSLOT; n++) if (SL[n].used && (b < 0 || SL[n].mt > SL[b].mt)) b = n;
    return b;
}
static void when_text(time_t t, char *out, size_t n) {
    time_t now = time(0); struct tm a, b; localtime_r(&t, &a); localtime_r(&now, &b);
    char hm[16]; strftime(hm, sizeof hm, "%H:%M", &a);
    long days = (long)(mktime(&(struct tm){ .tm_year = b.tm_year, .tm_mon = b.tm_mon, .tm_mday = b.tm_mday, .tm_isdst = -1 }) -
                       mktime(&(struct tm){ .tm_year = a.tm_year, .tm_mon = a.tm_mon, .tm_mday = a.tm_mday, .tm_isdst = -1 })) / 86400;
    if (days == 0) snprintf(out, n, "Today %s", hm);
    else if (days == 1) snprintf(out, n, "Yesterday %s", hm);
    else { char d[16]; strftime(d, sizeof d, lang_active() ? "%m/%d" : "%b %e", &a); snprintf(out, n, "%s %s", d, hm); }   /* (no month name in another language) */
}

/* ---------- settings: volume and brightness, as ROCKNIX keeps them ---------- */
#define SYSCFG "/storage/.config/system/configs/system.cfg"
static int cfg_int(const char *key, int def) {
    FILE *f = fopen(SYSCFG, "r"); if (!f) return def;
    char line[256]; size_t kl = strlen(key); int v = def;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, kl) && line[kl] == '=') v = atoi(line + kl + 1);
    fclose(f);
    return v;
}
static int vol, bri, bri_changed;
/* BananaLinux (/run/banana/mixer, "<card> <control>"): amixer for the volume and the backlights themselves, the value
 * kept in /data/banana/backlight for bananad to restore at boot; ROCKNIX's volume and brightness scripts otherwise */
static int banana_os = -1;
static char mix_card[16] = "0", mix_ctl[64] = "Master";
static int is_banana(void) {
    if (banana_os < 0) {
        FILE *f = fopen("/run/banana/mixer", "r"); banana_os = f != 0;
        if (f) { char a[16], b[64]; if (fscanf(f, "%15s %63[^\n]", a, b) == 2) { snprintf(mix_card, sizeof mix_card, "%s", a); snprintf(mix_ctl, sizeof mix_ctl, "%s", b); } fclose(f); }
    }
    return banana_os;
}
/* (0.4.2: the volume through BananaLinux's own script, as its keys and Settings set it: amixer on the named control
 * found no "PCM" on the RG DS Plus, whose amplifiers are "Left PCM"/"Right PCM", and on the RG DS read another percent
 * than the keys' and set the microphone's level too. The backlights each their own, backlight1 the top panel and
 * backlight0 the bottom: the row moves both by its step and shows their middle, and "<top> <bottom>" in thousandths
 * goes to /data/banana/backlight; before, the first panel's raw value went there, which bananad reads as thousandths) */
static int banana_volume(void) {
    /* (without this library: DraStic's children inherit LD_PRELOAD, and in them it printed its Gengis Engine line
     * into the output and amixer crashed: the menu read 50% and set nothing) */
    FILE *p = popen("env -u LD_PRELOAD /usr/libexec/banana/volume get 2>/dev/null", "r"); if (!p) return -1;
    int v = -1; if (fscanf(p, "%d", &v) != 1) v = -1;
    pclose(p);
    return v < 0 ? -1 : v > 100 ? 100 : v;   /* -1: no level (the refresh keeps what is shown) */
}
static const char *BLN[2] = { "backlight1", "backlight0" };   /* top, bottom */
static int bl_read(const char *path) { FILE *f = fopen(path, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
static int bl_pm(int i) {
    char p[96]; snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", BLN[i]); int mx = bl_read(p);
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", BLN[i]); int v = bl_read(p);
    return mx > 0 && v >= 0 ? v * 1000 / mx : -1;
}
static void bl_set_pm(int i, int pm) {
    char p[96]; snprintf(p, sizeof p, "/sys/class/backlight/%s/max_brightness", BLN[i]); int mx = bl_read(p);
    if (mx <= 0) return;
    int v = mx * pm / 1000; if (v < 1) v = 1;
    snprintf(p, sizeof p, "/sys/class/backlight/%s/brightness", BLN[i]);
    FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); }
}
static int bl_base[2] = { -1, -1 }, bl_base_avg = 80;
static int banana_brightness(void) {
    int n = 0, sum = 0;
    for (int i = 0; i < 2; i++) { bl_base[i] = bl_pm(i); if (bl_base[i] > 0) { sum += bl_base[i]; n++; } }
    bl_base_avg = n ? (sum / n + 5) / 10 : 80;
    return bl_base_avg;
}
static long long vol_set_at;
static volatile int vol_read = -1;
static pthread_mutex_t wm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
static int w_vol = -1, w_bri = -1, w_read;
static void *worker(void *a) {       /* ROCKNIX's own scripts (pactl, the settings file): off DraStic's thread */
    (void)a;
    for (;;) {
        pthread_mutex_lock(&wm);
        while (w_vol < 0 && w_bri < 0 && !w_read) pthread_cond_wait(&wc, &wm);
        int v = w_vol, b = w_bri, r = w_read; w_vol = w_bri = -1; w_read = 0;
        pthread_mutex_unlock(&wm);
        char cmd[200];
        if (is_banana()) {
            if (v >= 0) {
                snprintf(cmd, sizeof cmd, "env -u LD_PRELOAD /usr/libexec/banana/volume set %d >/dev/null 2>&1", v); if (system(cmd)) {}
                if (system("env -u LD_PRELOAD killall -HUP bananad >/dev/null 2>&1")) {}   /* it stores the level across reboots */
            }
            if (b >= 0) {
                int t = bl_pm(0), u = bl_pm(1);
                FILE *f = fopen("/data/banana/backlight", "w"); if (f) { fprintf(f, "%d %d\n", t > 0 ? t : u, u > 0 ? u : t); fclose(f); }
            }
            if (r && v < 0) vol_read = banana_volume();
            continue;
        }
        if (r) continue;
        if (v >= 0) { snprintf(cmd, sizeof cmd, "volume %d >/dev/null 2>&1", v); if (system(cmd)) {} }
        if (b >= 0) { snprintf(cmd, sizeof cmd, "brightness set all %d >/dev/null 2>&1", b); if (system(cmd)) {} }
    }
    return 0;
}
static void want_ex(int v, int b, int r) {
    static int started;
    pthread_mutex_lock(&wm);
    if (!started) { pthread_t t; if (!pthread_create(&t, 0, worker, 0)) { pthread_detach(t); pthread_setname_np(t, "dsf-menu"); started = 1; } }
    if (v >= 0) w_vol = v; if (b >= 0) w_bri = b; if (r) w_read = 1;
    pthread_cond_signal(&wc); pthread_mutex_unlock(&wm);
}
static void want(int v, int b) { if (v >= 0) vol_set_at = now_ms(); want_ex(v, b, 0); }
/* the brightness changed in this menu, saved now on BananaLinux (a small file, written on this thread: a Quit game
 * killed DraStic before the worker got to it, and the next boot came back at the old level); ROCKNIX's script through
 * the worker as before */
static void bri_save_now(void) {
    if (!bri_changed) return;
    bri_changed = 0;
    if (!is_banana()) { want(-1, bri); return; }
    int t = bl_pm(0), u = bl_pm(1);
    FILE *f = fopen("/data/banana/backlight", "w");
    if (f) { fprintf(f, "%d %d\n", t > 0 ? t : u, u > 0 ? u : t); fflush(f); fsync(fileno(f)); fclose(f); }
}
static void backlight(int pct) {     /* at once, straight to the panels; the setting is saved when the menu closes */
    if (is_banana()) {               /* both moved by the row's step from where they were */
        int d = (pct - bl_base_avg) * 10;
        for (int i = 0; i < 2; i++) { if (bl_base[i] <= 0) continue; int v = bl_base[i] + d; bl_set_pm(i, v < 50 ? 50 : v > 1000 ? 1000 : v); }
        return;
    }
    glob_t g;
    if (glob("/sys/class/backlight/*/brightness", 0, 0, &g)) return;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        char mp[300]; snprintf(mp, sizeof mp, "%.*smax_brightness", (int)(strlen(g.gl_pathv[i]) - 10), g.gl_pathv[i]);
        FILE *f = fopen(mp, "r"); int mx = 0; if (f) { if (fscanf(f, "%d", &mx) != 1) mx = 0; fclose(f); }
        if (mx <= 0) continue;
        if ((f = fopen(g.gl_pathv[i], "w"))) { fprintf(f, "%d\n", pct * mx / 100); fclose(f); }
    }
    globfree(&g);
}
/* the brightness as it is now (both panels' middle on BananaLinux, as bananad's F + volume left it; ROCKNIX's setting) */
static int bri_now(void) { return is_banana() ? banana_brightness() : cfg_int("display.brightness", 80); }
static long long bri_set_at;
static int bri_set(int v) {          /* 1 if it changed */
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    if (v == bri) return 0;
    bri = v; backlight(bri); bri_changed = 1; bri_set_at = now_ms();
    return 1;
}

/* ---------- sounds: the theme's WAVs ---------- */
enum { SND_MOVE, SND_SELECT, SND_BACK, SND_N };
static int snd_ok[SND_N];
static void load_wav(int id, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    uint8_t h[12]; int16_t *pcm = 0; int ch = 0, rate = 0, bits = 0; uint32_t n = 0;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) {
        uint8_t ck[8];
        while (fread(ck, 1, 8, f) == 8) {
            uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
            if (!memcmp(ck, "fmt ", 4)) {
                uint8_t fm[40]; size_t k = len < sizeof fm ? len : sizeof fm;
                if (fread(fm, 1, k, f) != k) break;
                if (len > k) fseek(f, (long)(len - k), SEEK_CUR);
                ch = fm[2] | fm[3] << 8; rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24; bits = fm[14] | fm[15] << 8;
            } else if (!memcmp(ck, "data", 4) && bits == 16 && ch > 0 && len < (8u << 20)) {
                pcm = malloc(len); if (pcm && fread(pcm, 1, len, f) == len) n = len / 2 / (uint32_t)ch;
                break;
            } else fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fclose(f);
    if (n && audio_ui_load(id, pcm, (int)n, rate, ch) == 0) snd_ok[id] = 1;
    free(pcm);
}
static void play(int id) { if (snd_ok[id]) audio_ui_play(id); }

/* ---------- state ---------- */
enum { SC_MAIN, SC_SAVE, SC_LOAD, SC_SET, SC_QUIT, SC_OVERWRITE, SC_BUSY, SC_MIC, SC_QUIT_NOSAVE, SC_KEYS };
static int enabled = -1, inited, open_, scr, sel_main, sel_slot, sel_set, dirty_top, dirty_bot;
#define MAIN_BRI 6                                /* sel_main: the main page's Brightness row, under the tiles */
static int main_col;                              /* the tiles' column the d-pad came down from (back up to it) */
static char busy_msg[96];
static int btn_menu[2] = { -1, -1 }, b_a = -1, b_b = -1, b_x = -1, b_y = -1, b_l = -1, b_r = -1, b_start = -1;
static int b_up = -1, b_down = -1, b_left = -1, b_right = -1;
static long long mic_until;
static uint32_t *bgtop;                         /* the game's top frame, dimmed (panel size) */
static canvas CV[2];
static int pw[2], ph[2];

/* rows of Quick settings that exist on this setup */
enum { ROW_VOL, ROW_BRI, ROW_MIC };
static int rows[4], nrows;
static void make_rows(void) { nrows = 0; rows[nrows++] = ROW_VOL; rows[nrows++] = ROW_BRI; rows[nrows++] = ROW_MIC; }

static int setup(void) {
    if (inited) return inited > 0;
    inited = -1;
    for (int i = 0; i < 2; i++) {
        uint32_t *px; int pitch;
        if (dsflip_menu_canvas(i, &px, &pitch, &pw[i], &ph[i]) < 0) { dsflip_log("[menu] no panel buffers: DraStic's menu instead\n"); return 0; }
        CV[i].w = pw[i]; CV[i].h = ph[i]; CV[i].s = pw[i] / 640.0f;
        if (!(CV[i].px = malloc((size_t)pw[i] * ph[i] * 4))) return 0;
    }
    if (!(bgtop = malloc((size_t)pw[0] * ph[0] * 4))) return 0;
    { const char *t = getenv("DSFLIP_MENU_THEME"); if (t && !strcmp(t, "banana")) theme_banana(); }
    const char *const med[] = { getenv("DSFLIP_MENU_FONT"), "/usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf",
        "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Medium.ttf",
        "/storage/.config/emulationstation/themes/rocknixds-pixel-light/rnds/fonts/PixelifySans-Medium.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
    const char *const reg[] = { "/usr/share/banana/rnds/fonts/PixelifySans-Regular.ttf", getenv("DSFLIP_MENU_FONT"),
        "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Regular.ttf",
        "/storage/.config/emulationstation/themes/rocknixds-pixel-light/rnds/fonts/PixelifySans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
    if (!font_load(&F_MED, med, 8) || !font_load(&F_REG, reg, 7)) { dsflip_log("[menu] no font: DraStic's menu instead\n"); return 0; }
    if (lang_init("superdrastic") && !F_CJK.ok) {   /* the language, and the pixel font for its characters */
        const char *const cjk[] = { getenv("DSFLIP_FONT_CJK"), "/usr/share/banana/rnds/fonts/fusion-pixel-12px-proportional-zh_hans.ttf" };
        font_load(&F_CJK, cjk, 2);
    }
    const char *snd[SND_N] = { "scroll.wav", "select.wav", "back.wav" };
    for (int k = 0; k < SND_N; k++) {
        char p[300];
        snprintf(p, sizeof p, "/usr/share/banana/rnds/sounds/%s", snd[k]);
        if (access(p, R_OK)) snprintf(p, sizeof p, "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/sounds/%s", snd[k]);
        if (access(p, R_OK)) snprintf(p, sizeof p, "/storage/.config/emulationstation/themes/rocknixds-pixel-light/rnds/sounds/%s", snd[k]);
        load_wav(k, p);
    }
    inited = 1;
    return 1;
}

/* ---------- top panel ---------- */
static void grab_background(void) {
    canvas *c = &CV[0];
    if (dsflip_menu_grab_top(bgtop, pw[0] * 4, pw[0], ph[0])) {
        for (size_t i = 0, n = (size_t)pw[0] * ph[0]; i < n; i++) {   /* the game, at a third of its brightness */
            uint32_t p = bgtop[i];
            bgtop[i] = ((p >> 16 & 255) * 85 / 256) << 16 | ((p >> 8 & 255) * 85 / 256) << 8 | (p & 255) * 85 / 256;
        }
    } else {
        canvas t = { bgtop, pw[0], ph[0], c->s }; dots(&t, 0, 0, pw[0], ph[0], C_BG, C_DOT);
    }
}
static void status_bar(canvas *c) {
    int chg, pct = dsflip_battery(&chg);
    float x = S(22);
    if (pct >= 0) {
        char b[16]; snprintf(b, sizeof b, "%d%%", pct);
        text_raw(c, &F_MED, b, x, S(12), S(18), C_LIGHT, 0, 0);
        x += text_w(&F_MED, b, S(18), 99) + S(10);
        int bx = (int)x, by = S(15), bw = S(26), bh = S(14);           /* the battery, outlined, filled to its level */
        fill(c, bx, by, bx + bw, by + bh, pct <= 15 && !chg ? C_REDL : C_LIGHT, 1.0f);
        fill(c, bx + S(2), by + S(2), bx + bw - S(2), by + bh - S(2), C_DARK, 1.0f);
        fill(c, bx + bw, by + S(4), bx + bw + S(3), by + bh - S(4), C_LIGHT, 1.0f);
        int lv = (bw - S(6)) * (pct > 100 ? 100 : pct) / 100;
        fill(c, bx + S(3), by + S(3), bx + S(3) + lv, by + bh - S(3), pct <= 15 && !chg ? C_REDL : C_LIGHT, 1.0f);
        if (chg) text(c, &F_MED, "+", bx + bw + S(8), S(12), S(18), C_LIGHT, 0, 0);
    }
    char tm[32]; time_t t = time(0); struct tm lt; localtime_r(&t, &lt); strftime(tm, sizeof tm, "%m/%d %H:%M", &lt);
    text_raw(c, &F_MED, tm, c->w - S(22), S(12), S(18), C_LIGHT, 0, 2);
}
/* an RGB565 picture 256 wide and `rows` tall into a rectangle (nearest) */
static void draw_pic(canvas *c, const uint16_t *pic, int rows, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; y++) {
        if (y0 + y < 0 || y0 + y >= c->h) continue;
        const uint16_t *row = pic + (size_t)(y * rows / h) * 256; uint32_t *d = c->px + (size_t)(y0 + y) * c->w;
        for (int x = 0; x < w; x++) {
            if (x0 + x < 0 || x0 + x >= c->w) continue;
            uint16_t p = row[x * 256 / w];
            uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            d[x0 + x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        }
    }
}
static void hatch(canvas *c, int x0, int y0, int x1, int y1, uint32_t a, uint32_t b) {   /* an empty slot */
    int k = S(6) > 1 ? S(6) : 2;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) blendpx(c, x, y, ((x + y) / k) & 1 ? a : b, 1.0f);
}
/* the latest achievements, read when the menu opens; badges decoded from ra.c's cache (RGBA) */
#define NREC 3
static ra_recent_t REC[NREC]; static int nrec, ra_has; static unsigned ra_un, ra_tot;
static uint8_t *rec_img[NREC]; static int rec_w[NREC], rec_h[NREC];
static void load_recent(void) {
    for (int i = 0; i < NREC; i++) { if (rec_img[i]) stbi_image_free(rec_img[i]); rec_img[i] = 0; }
    ra_has = ra_progress(&ra_un, &ra_tot) && ra_tot;
    nrec = ra_has ? ra_recent(REC, NREC) : 0;
    for (int i = 0; i < nrec; i++) { int ch; rec_img[i] = stbi_load(REC[i].badge, &rec_w[i], &rec_h[i], &ch, 4); }
}
/* an RGBA image (straight alpha) scaled bilinearly into a size x size square with rounded corners */
static void draw_img(canvas *c, const uint8_t *img, int iw, int ih, int x0, int y0, int size, float rad) {
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float cov = rr_cov(x + 0.5f, y + 0.5f, 0, 0, (float)size, (float)size, rad);
            if (cov <= 0) continue;
            float sx = (x + 0.5f) * iw / size - 0.5f, sy = (y + 0.5f) * ih / size - 0.5f;
            int ix = (int)floorf(sx), iy = (int)floorf(sy); float fx = sx - ix, fy = sy - iy;
            float acc[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < 4; k++) {
                int xx = ix + (k & 1), yy = iy + (k >> 1);
                if (xx < 0) xx = 0; if (yy < 0) yy = 0; if (xx >= iw) xx = iw - 1; if (yy >= ih) yy = ih - 1;
                float w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
                const uint8_t *p = img + (yy * iw + xx) * 4; float a = p[3] / 255.0f;
                acc[0] += p[0] * a * w; acc[1] += p[1] * a * w; acc[2] += p[2] * a * w; acc[3] += a * w;
            }
            if (acc[3] <= 0) continue;
            uint32_t rgb = (uint32_t)(acc[0] / acc[3]) << 16 | (uint32_t)(acc[1] / acc[3]) << 8 | (uint32_t)(acc[2] / acc[3]);
            blendpx(c, x0 + x, y0 + y, rgb, acc[3] * cov);
        }
}
static void fmt_dur(long long ms, char *o, size_t n) {
    long m = (long)(ms / 60000);
    snprintf(o, n, "%ldh %02ldm", m / 60, m % 60);
}
static void draw_top(void) {
    canvas *c = &CV[0];
    memcpy(c->px, bgtop, (size_t)pw[0] * ph[0] * 4);
    status_bar(c);
    int x0 = S(24), y0 = S(48), x1 = c->w - S(24), y1 = c->h - S(22);
    dotbox(c, x0, y0, x1, y1, S(16), S(3) < 2 ? 2 : S(3), C_LIGHT, C_TEAL, C_TEALDOT);
    int L = S(52);
    if (scr == SC_SAVE || scr == SC_LOAD || (scr == SC_OVERWRITE)) {
        /* the slot's two screens, top over bottom, on the left; its number, date and what A does beside them */
        slot *s = &SL[sel_slot];
        int ph_ = S(348), pw_ = ph_ * 2 / 3, px = x0 + S(30), py = (y0 + y1 - ph_) / 2;
        box(c, px - S(3), py - S(3), px + pw_ + S(3), py + ph_ + S(3), S(6), S(3), C_LIGHT, C_DARK);
        if (s->used && s->pic && s->pic_rows == 384) draw_pic(c, s->pic, 384, px, py, pw_, ph_);
        else if (s->used && s->pic) {               /* a state with the top screen alone: that one, the bottom left blank */
            draw_pic(c, s->pic, 192, px, py, pw_, ph_ / 2);
            fill(c, px, py + ph_ / 2, px + pw_, py + ph_, C_PANEL, 1.0f);
        } else if (s->used) {
            fill(c, px, py, px + pw_, py + ph_, C_PANEL, 1.0f);
            char l1[200], l2[200]; int n = wrap2(&F_REG, "No picture in this save", S(18), pw_ - S(20), l1, l2, sizeof l1, 1);
            text(c, &F_REG, l1, px + pw_ / 2.0f, py + ph_ / 2 - S(n > 1 ? 22 : 10), S(18), C_GREY, 0, 1);
            if (n > 1) text(c, &F_REG, l2, px + pw_ / 2.0f, py + ph_ / 2 + S(2), S(18), C_GREY, 0, 1);
        } else { hatch(c, px, py, px + pw_, py + ph_, C_EMPTY0, C_EMPTY1); text(c, &F_MED, "Empty slot", px + pw_ / 2.0f, py + ph_ / 2 - S(12), S(22), C_GREY, pw_ - S(12), 1); }
        int ix = px + pw_ + S(30), iw = x1 - S(28) - ix, iy = py + S(6);
        char t[48]; snprintf(t, sizeof t, "Slot %d", sel_slot + 1);
        text(c, &F_MED, t, ix, iy, S(28), C_LIGHT, iw, 0); iy += S(40);
        if (s->used) { char w[48]; when_text(s->mt, w, sizeof w); text(c, &F_REG, w, ix, iy, S(18), C_TEALTXT, iw, 0); iy += S(30); }
        fill(c, ix, iy, x1 - S(28), iy + S(2), C_TEALLN, 1.0f); iy += S(16);
        const char *hint = scr == SC_LOAD ? (s->used ? "A: load this save. You can undo it after." : "Nothing saved here yet")
                                          : (s->used ? "A: save here (replaces this save)" : "A: save here");
        char l1[200], l2[200]; int n = wrap2(&F_REG, hint, S(17), iw, l1, l2, sizeof l1, 1);
        text(c, &F_REG, l1, ix, iy, S(17), C_TEALTXT, iw, 0);
        if (n > 1) text(c, &F_REG, l2, ix, iy + S(24), S(17), C_TEALTXT, iw, 0);
    } else {
        /* PAUSED pill, the game, its genre */
        int cw = x1 - L - S(28);
        const char *paused = lang_active() ? "Paused" : "PAUSED";   /* (the translator's line is "Paused") */
        float pwid = text_w(&F_MED, paused, S(14), 99) + S(18);
        rrect(c, L, S(68), L + (int)pwid, S(68) + S(24), S(4), C_LIGHT, 1.0f);
        text(c, &F_MED, paused, L + S(9), S(71), S(14), C_DARK, 0, 0);
        int y = ra_has ? S(102) : S(126);           /* without achievements the rest sits lower, centred */
        char l1[200], l2[200]; int n = wrap2(&F_MED, gtitle, S(28), cw, l1, l2, sizeof l1, 0);
        text(c, &F_MED, l1, L, y, S(28), C_LIGHT, cw, 0); y += S(34);
        if (n > 1) { text(c, &F_MED, l2, L, y, S(28), C_LIGHT, cw, 0); y += S(34); }
        if (ggenre[0]) { text_raw(c, &F_REG, ggenre, L, y, S(16), C_TEALTXT, cw, 0); y += S(26); }
        y += S(6);
        fill(c, L, y, x1 - S(28), y + S(2), C_TEALLN, 1.0f);
        y += S(14);
        /* this session, the last save */
        char v[48];
        fmt_dur(now_ms() - session_t0, v, sizeof v);
        text_raw(c, &F_MED, v, L, y, S(22), C_LIGHT, 0, 0);
        text(c, &F_REG, "THIS SESSION", L, y + S(28), S(13), C_TEALTXT, 0, 0);
        int nb = newest_slot(), cx = L + S(200);
        if (nb >= 0) {
            snprintf(v, sizeof v, "Slot %d", nb + 1); text(c, &F_MED, v, cx, y, S(22), C_LIGHT, 0, 0);
            char w[48]; when_text(SL[nb].mt, w, sizeof w);
            char lab[80]; snprintf(lab, sizeof lab, "LAST SAVE  %s", w);
            text(c, &F_REG, lab, cx, y + S(28), S(13), C_TEALTXT, x1 - cx - S(28), 0);
        } else {
            text(c, &F_MED, "None yet", cx, y, S(22), C_LIGHT, 0, 0);
            text(c, &F_REG, "LAST SAVE", cx, y + S(28), S(13), C_TEALTXT, 0, 0);
        }
        y += S(56);
        /* RetroAchievements: progress, then the latest unlocks with their badges */
        if (ra_has) {
            text(c, &F_MED, "LATEST ACHIEVEMENTS", L, y, S(14), C_TEALTXT, 0, 0);
            snprintf(v, sizeof v, "%u / %u", ra_un, ra_tot);
            text_raw(c, &F_MED, v, x1 - S(28), y, S(14), C_TEALTXT, 0, 2);
            int bx0 = L, bx1 = x1 - S(28), bb = y + S(22);
            fill(c, bx0, bb, bx1, bb + S(10), C_LIGHT, 1.0f);
            fill(c, bx0 + S(2), bb + S(2), bx1 - S(2), bb + S(8), C_DARK, 1.0f);
            int fw = (int)((long long)(bx1 - bx0 - S(4)) * ra_un / ra_tot);
            if (ra_un && fw < S(2)) fw = S(2);
            fill(c, bx0 + S(2), bb + S(2), bx0 + S(2) + fw, bb + S(8), 0x4fb3b0, 1.0f);
            y = bb + S(20);
            int rh = S(52), bs = S(42);
            if (!nrec) text(c, &F_REG, "None unlocked yet. Good luck!", L, y + S(8), S(16), C_TEALTXT, cw, 0);
            for (int i = 0; i < nrec && y + rh <= y1 - S(10); i++, y += rh) {
                ra_recent_t *r = &REC[i];
                box(c, L, y, L + bs, y + bs, S(6), S(2) < 1 ? 1 : S(2), C_LIGHT, C_DARK);
                if (rec_img[i]) draw_img(c, rec_img[i], rec_w[i], rec_h[i], L + S(2), y + S(2), bs - 2 * S(2), S(4));
                int tx = L + bs + S(14), tw = x1 - S(28) - tx;
                char w[48]; when_text((time_t)r->when, w, sizeof w);
                float ww = text_w(&F_REG, w, S(13), 99);
                text_raw(c, &F_MED, r->title, tx, y + S(2), S(17), C_LIGHT, tw - ww - S(12), 0);
                text(c, &F_REG, w, x1 - S(28), y + S(5), S(13), C_TEALTXT, 0, 2);
                text_raw(c, &F_REG, r->desc, tx, y + S(24), S(14), C_TEALTXT, tw, 0);
            }
        }
    }
}

/* ---------- bottom panel ---------- */
typedef struct { int x0, y0, x1, y1; } rect;
static rect R_TILE[6], R_SLOT[NSLOT], R_ROW[4], R_BAR[4], R_FOOT_L, R_FOOT_R, R_HEAD_B, R_DLG_B, R_DLG_A, R_DLG_X, R_UNDO, R_MBRI, R_MBRI_BAR, R_KEYS;
static int in_rect(rect r, int x, int y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

static void keycap(canvas *c, int x, int y, int sz, const char *k) {   /* the theme's light square with a button letter */
    rrect(c, x, y, x + sz, y + sz, S(5), C_LIGHT, 1.0f);
    text_raw(c, &F_MED, k, x + sz / 2.0f, y + (sz - sz * 0.62f) / 2 - S(1), sz * 0.62f, C_DARK, 0, 1);
}
/* a footer/dialog pill: [key] label; returns its rect */
static rect pill(canvas *c, int x, int y, int right, const char *k, const char *label, uint32_t border, uint32_t txt, uint32_t bg) {
    int h = S(40), ks = S(24), pad = S(10);
    float lw = text_w(&F_MED, label, S(18), 99);
    int w = pad + ks + S(10) + (int)lw + pad + S(4);
    if (right) x -= w;
    box(c, x, y, x + w, y + h, S(10), S(3) < 2 ? 2 : S(3), border, bg);
    keycap(c, x + pad, y + (h - ks) / 2, ks, k);
    text(c, &F_MED, label, x + pad + ks + S(10), y + (h - S(18)) / 2.0f - S(1), S(18), txt, 0, 0);
    return (rect){ x, y, x + w, y + h };
}
static const char *tile_label[6] = { "Resume", "Save", "Load", "Quick settings", "DraStic menu", "Quit game" };
static const char *const *tile_icon[6] = { IC_PLAY, IC_SAVE, IC_LOAD, IC_SLIDERS, IC_LIST, IC_EXIT };

/* ---------- the Hotkeys page: DraStic's own controls from config/drastic.cfg (both sets), SuperDrastic's exit combos and
 * HOME, BananaLinux's volume keys ---------- */
#define NKEYS 10
static struct { char keys[80]; const char *what; } KEYS[NKEYS];
static int nkeys;
/* a joystick button's name: the pad's order (retrogame_joypad's, as the fallbacks in menu_event) */
static const char *pad_name(int b) {
    static const char *const n[] = { "B", "A", "X", "Y", "L", "R", "L2", "R2", "SELECT", "START", "F", "L3", "R3", "UP", "DOWN", "LEFT", "RIGHT" };
    return b >= 0 && b < (int)(sizeof n / sizeof n[0]) ? n[b] : 0;
}
static int ctl_button(const char *set, const char *name) {   /* controls_<set>[CONTROL_INDEX_<name>]'s button, or -1 */
    FILE *f = fopen("config/drastic.cfg", "r"); if (!f) return -1;
    char key[96], line[256]; int b = -1;
    snprintf(key, sizeof key, "controls_%s[CONTROL_INDEX_%s] = ", set, name);
    while (fgets(line, sizeof line, f)) if (!strncmp(line, key, strlen(key))) { int v = atoi(line + strlen(key)); if (v >= 1024 && v < 1088) b = v - 1024; break; }
    fclose(f);
    return b;
}
/* a control's buttons ("L3 / F"); "" if neither set has one on the pad */
static void ctl_keys(const char *name, char *o, size_t n) {
    int a = ctl_button("a", name), b = ctl_button("b", name);
    const char *na = pad_name(a), *nb = b != a ? pad_name(b) : 0;
    snprintf(o, n, "%s%s%s", na ? na : "", na && nb ? " / " : "", nb ? nb : "");
}
static void key_add(const char *keys, const char *what) {
    if (nkeys >= NKEYS) return;
    snprintf(KEYS[nkeys].keys, sizeof KEYS[0].keys, "%s", keys); KEYS[nkeys].what = what; nkeys++;
}
static void make_keys(void) {
    char k[80];
    nkeys = 0;
    ctl_keys("MENU", k, sizeof k);
    if (lid_home_on()) { size_t l = strlen(k); snprintf(k + l, sizeof k - l, "%sHOME", l ? " / " : ""); }
    key_add(k, "This menu");
    ctl_keys("SAVE_STATE", k, sizeof k); key_add(k, "Save state");
    ctl_keys("LOAD_STATE", k, sizeof k); key_add(k, "Load state");
    ctl_keys("FAST_FORWARD", k, sizeof k); key_add(k, "Fast-forward");
    ctl_keys("SWAP_SCREENS", k, sizeof k); key_add(k, "Swap screens");
    ctl_keys("FAKE_MICROPHONE", k, sizeof k); if (*k) key_add(k, "Blow on the mic");
    int st, se, m1, m2;
    if (resume_exit_combo(&st, &se, &m1, &m2)) {   /* (held half a second) */
        const char *m = pad_name(m2 >= 0 ? m2 : m1);
        if (m) { snprintf(k, sizeof k, "%s + %s", m, pad_name(st)); key_add(k, resume_on() ? "Save your spot and quit (hold)" : "Quit (hold)"); }
        if (pad_name(se)) { snprintf(k, sizeof k, "%s + %s", pad_name(st), pad_name(se)); key_add(k, resume_on() ? "Save your spot and quit (hold)" : "Quit (hold)"); }
    }
    if (is_banana()) { key_add("VOL + / -", "Volume"); key_add("F + VOL + / -", "Brightness"); }   /* (bananad's) */
}

static void draw_header(canvas *c, const char *h, const char *sub, int sub_page) {
    int x = S(20);
    if (sub_page) { keycap(c, x, S(16), S(26), "B"); R_HEAD_B = (rect){ x - S(6), S(8), x + S(34), S(50) }; x += S(38); }
    else R_HEAD_B = (rect){ 0, 0, 0, 0 };
    text(c, &F_MED, h, x, S(16), S(22), C_LIGHT, 0, 0);
    float hw = text_w(&F_MED, h, S(22), 99), sw = sub && *sub ? text_w(&F_REG, sub, S(16), 99) : 0;
    if (sw > S(300)) sw = S(300);
    int lx0 = x + (int)hw + S(14), lx1 = c->w - S(20) - (sw > 0 ? (int)sw + S(14) : 0);
    if (lx1 > lx0) fill(c, lx0, S(27), lx1, S(30), C_LINE, 1.0f);
    if (sub && *sub) text(c, &F_REG, sub, c->w - S(20), S(19), S(16), C_GREY, S(300), 2);
}
static void bar(canvas *c, int x, int y, int v, int sel) {      /* 10 segments, the theme's progress look */
    for (int k = 0; k < 10; k++) fill(c, x + k * S(15), y, x + k * S(15) + S(11), y + S(20), k < (v + 5) / 10 ? (sel ? C_BARSEL : C_CYAN) : (sel ? C_BAROFFSEL : C_LINE), 1.0f);
}
/* a Volume / Brightness row (Quick settings, and Brightness under the main page's tiles): its icon, its name, the bar
 * and the percent; *bar_r: where a tap sets the level */
static void level_row(canvas *c, int x0, int y, int x1, int rh, int sel, const char *const *ic, const char *lab, int val, rect *bar_r) {
    int bw = S(3) < 2 ? 2 : S(3);
    box(c, x0, y, x1, y + rh, S(10), bw, sel ? C_LIGHT : C_LINE, sel ? C_BLUE : C_PANEL);
    int k = S(2) < 2 ? 2 : S(2);
    bitmap(c, ic, 12, x0 + S(16), y + (rh - 12 * k) / 2, k, sel ? C_ONSEL : C_LIGHT);
    text(c, &F_MED, lab, x0 + S(56), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_LIGHT, 0, 0);
    int bx = x1 - S(110) - S(146);
    bar(c, bx, y + (rh - S(20)) / 2, val, sel);
    *bar_r = (rect){ bx - S(8), y, bx + S(150), y + rh };
    char v[16]; snprintf(v, sizeof v, "%d%%", val);
    text_raw(c, &F_MED, v, x1 - S(18), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_CYAN, 0, 2);
}
static int bar_value(rect b, int px, canvas *c) {   /* a tap on a row's bar: its level, in steps of 5 */
    int v = (px - b.x0 - S(8)) * 100 / S(146); v = (v + 2) / 5 * 5;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
/* a question over the page: B no, A yes; with alt, a second row under them: X alt, in red (Quit without saving) */
static void dialog3(canvas *c, const char *title, const char *body, const char *no, const char *yes, int danger, const char *alt) {
    fill(c, 0, 0, c->w, c->h, C_DARK, 0.65f);
    int w = S(460), h = alt ? S(260) : S(210), x0 = (c->w - w) / 2, y0 = alt ? S(100) : S(120);
    box(c, x0, y0, x0 + w, y0 + h, S(14), S(3), C_LIGHT, C_DARK);
    text(c, &F_MED, title, x0 + S(26), y0 + S(22), S(24), C_LIGHT, w - S(52), 0);
    char l1[200], l2[200]; int n = wrap2(&F_REG, body, S(17), w - S(52), l1, l2, sizeof l1, 1);
    text(c, &F_REG, l1, x0 + S(26), y0 + S(66), S(17), C_BODY, w - S(52), 0);
    if (n > 1) text(c, &F_REG, l2, x0 + S(26), y0 + S(90), S(17), C_BODY, w - S(52), 0);
    int by = y0 + h - (alt ? S(110) : S(60));
    R_DLG_A = pill(c, x0 + w - S(24), by, 1, "A", yes, danger ? C_REDL : C_LIGHT, danger ? C_LIGHT : C_ONSEL, danger ? C_RED : C_BLUE);
    R_DLG_B = pill(c, R_DLG_A.x0 - S(12), by, 1, "B", no, C_GREY, C_LIGHT, C_PILLBG);
    R_DLG_X = alt ? pill(c, x0 + w - S(24), by + S(50), 1, "X", alt, C_REDDIM, C_REDL, C_DARK) : (rect){ 0, 0, 0, 0 };
}
static void dialog(canvas *c, const char *title, const char *body, const char *no, const char *yes, int danger) {
    dialog3(c, title, body, no, yes, danger, 0);
}
static void draw_bottom(void) {
    canvas *c = &CV[1];
    dots(c, 0, 0, c->w, c->h, C_BG, C_DOT);
    int gx0 = S(20), gx1 = c->w - S(20), gy0 = S(58), gy1 = S(404), bw = S(3) < 2 ? 2 : S(3);
    int s = scr == SC_OVERWRITE ? SC_SAVE : scr == SC_QUIT || scr == SC_QUIT_NOSAVE ? SC_MAIN : scr;
    if (scr == SC_BUSY) {
        draw_header(c, "Please wait", "", 0);
        int w = S(420), h = S(150), x0 = (c->w - w) / 2, y0 = S(150);
        box(c, x0, y0, x0 + w, y0 + h, S(14), bw, C_LIGHT, C_DARK);
        text(c, &F_MED, busy_msg, c->w / 2.0f, y0 + S(40), S(24), C_LIGHT, w - S(40), 1);
        text(c, &F_REG, "One moment...", c->w / 2.0f, y0 + S(86), S(17), C_GREY, 0, 1);
        return;
    }
    if (s == SC_MAIN) {
        char sub[120]; snprintf(sub, sizeof sub, "%s", gtitle);
        draw_header(c, "Paused", sub, 0);
        int rh = S(54), gap = S(12), ty1 = gy1 - rh - S(10), tw = (gx1 - gx0 - 2 * gap) / 3, th = (ty1 - gy0 - gap) / 2;
        int nb = newest_slot();
        for (int i = 0; i < 6; i++) {
            int x = gx0 + (i % 3) * (tw + gap), y = gy0 + (i / 3) * (th + gap), sel = i == sel_main, red = i == 5;
            R_TILE[i] = (rect){ x, y, x + tw, y + th };
            uint32_t bd = sel ? (red ? C_REDL : C_LIGHT) : (red ? C_REDDIM : C_BORDER), bg = sel ? (red ? C_RED : C_BLUE) : C_PANEL;
            uint32_t fg = sel ? (red ? 0xffffff : C_ONSEL) : (red ? C_REDL : C_LIGHT), sub2 = sel ? (red ? 0xffd6dc : C_ONSELSUB) : (red ? 0xc98a92 : C_GREY);
            box(c, x, y, x + tw, y + th, S(14), bw, bd, bg);
            int k = S(4) < 2 ? 2 : S(4), cy = y + (th - S(108)) / 2;   /* icon, name and hint, centred in the tile */
            bitmap(c, tile_icon[i], 12, x + (tw - 12 * k) / 2, cy, k, fg);
            text(c, &F_MED, tile_label[i], x + tw / 2.0f, cy + S(60), S(20), fg, tw - S(16), 1);
            char h[64] = "";
            switch (i) {
            case 0: snprintf(h, sizeof h, "or press L3"); break;
            case 1: snprintf(h, sizeof h, "%d slots", NSLOT); break;
            case 2: if (nb >= 0) snprintf(h, sizeof h, "last: slot %d", nb + 1); else snprintf(h, sizeof h, "no saves yet"); break;
            case 3: snprintf(h, sizeof h, "volume, brightness"); break;
            case 4: snprintf(h, sizeof h, "cheats, controls"); break;
            case 5: snprintf(h, sizeof h, resume_on() ? "keeps your spot" : "back to the menu"); break;
            }
            text(c, &F_REG, h, x + tw / 2.0f, cy + S(92), S(14), sub2, tw - S(16), 1);
        }
        R_MBRI = (rect){ gx0, gy1 - rh, gx1, gy1 };
        level_row(c, gx0, gy1 - rh, gx1, rh, sel_main == MAIN_BRI, IC_SUN, "Brightness", bri, &R_MBRI_BAR);
    } else if (s == SC_SAVE || s == SC_LOAD) {
        char sub[48]; snprintf(sub, sizeof sub, "Slot %d of %d", sel_slot + 1, NSLOT);
        draw_header(c, s == SC_SAVE ? "Save" : "Load", sub, 1);
        int gap = S(10), cw = (gx1 - gx0 - 3 * gap) / 4, ch = (gy1 - gy0 - gap) / 2, nb = newest_slot();
        for (int n = 0; n < NSLOT; n++) {
            slot *sl = &SL[n];
            int x = gx0 + (n % 4) * (cw + gap), y = gy0 + (n / 4) * (ch + gap), sel = n == sel_slot, dim = s == SC_LOAD && !sl->used;
            R_SLOT[n] = (rect){ x, y, x + cw, y + ch };
            box(c, x, y, x + cw, y + ch, S(10), bw, sel ? C_LIGHT : dim ? C_DIMBORDER : C_BORDER, sel ? C_BLUE : dim ? C_DIMBG : C_PANEL);
            int k = S(3) < 2 ? 2 : S(3);
            bitmap(c, DIG[(n + 1) % 10], 7, x + S(10), y + S(10), k, dim ? C_DIM : sel ? C_ONSEL : C_LIGHT);
            const char *tag = n == nb ? "LAST" : "";
            if (*tag) text(c, &F_MED, tag, x + cw - S(10), y + S(10), S(13), sel ? C_ONSEL : C_CYAN, 0, 2);
            /* both screens, top over bottom (2:3), centred; a state with the top screen alone: that one, card-wide (4:3) */
            int ty0 = y + S(36), tth = ch - S(36) - S(28), ttw = tth * 2 / 3, tx0 = x + (cw - ttw) / 2, tx1 = tx0 + ttw;
            if (sl->used && sl->pic && sl->pic_rows == 384) draw_pic(c, sl->pic, 384, tx0, ty0, ttw, tth);
            else if (sl->used && sl->pic) {
                int ow = cw - S(16), oh = ow * 3 / 4, oy = ty0 + (tth - oh) / 2;
                draw_pic(c, sl->pic, 192, x + S(8), oy, ow, oh);
            } else if (sl->used) fill(c, tx0, ty0, tx1, ty0 + tth, C_NOPIC, 1.0f);
            else hatch(c, tx0, ty0, tx1, ty0 + tth, dim ? C_DIMBG : C_EMPTY0, dim ? C_EMPTY0 : C_EMPTY1);
            char w[48]; if (sl->used) when_text(sl->mt, w, sizeof w); else snprintf(w, sizeof w, "Empty");
            text(c, &F_REG, w, x + cw / 2.0f, ty0 + tth + S(7), S(13), dim ? C_DIM : sel ? C_ONSEL : C_BODY, cw - S(12), 1);
        }
    } else if (s == SC_SET) {
        draw_header(c, "Quick settings", "changes apply at once", 1);
        int y = gy0, rh = S(54), gap = S(10);
        for (int r = 0; r < nrows; r++) {
            int sel = r == sel_set, id = rows[r];
            R_ROW[r] = (rect){ gx0, y, gx1, y + rh }; R_BAR[r] = (rect){ 0, 0, 0, 0 };
            if (id == ROW_VOL || id == ROW_BRI) {
                level_row(c, gx0, y, gx1, rh, sel, id == ROW_VOL ? IC_VOL : IC_SUN, id == ROW_VOL ? "Volume" : "Brightness",
                          id == ROW_VOL ? vol : bri, &R_BAR[r]);
                y += rh + gap; continue;
            }
            box(c, gx0, y, gx1, y + rh, S(10), bw, sel ? C_LIGHT : C_LINE, sel ? C_BLUE : C_PANEL);
            int k = S(2) < 2 ? 2 : S(2);
            bitmap(c, IC_MIC, 12, gx0 + S(16), y + (rh - 12 * k) / 2, k, sel ? C_ONSEL : C_LIGHT);
            text(c, &F_MED, "Microphone", gx0 + S(56), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_LIGHT, 0, 0);
            text(c, &F_MED, "Test / blow", gx1 - S(18), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_CYAN, 0, 2);
            y += rh + gap;
        }
        const char *help = "";
        switch (rows[sel_set]) {
        case ROW_VOL: help = "Left / right to change, or tap the bar"; break;
        case ROW_BRI: help = "Left / right to change, both screens"; break;
        case ROW_MIC: help = "A live meter of the mic, and a blow for games that ask for one"; break;
        }
        text(c, &F_REG, help, gx0 + S(4), y + S(6), S(16), C_GREY, gx1 - gx0, 0);
    }
    else if (s == SC_KEYS) {
        draw_header(c, "Hotkeys", "while you play", 1);
        int rh = nkeys > 8 ? S(34) : S(38), y = gy0 + S(4), kx = gx0 + S(8), dx = gx0 + S(196);
        for (int i = 0; i < nkeys; i++, y += rh) {
            if (i) fill(c, gx0, y - S(4), gx1, y - S(3), C_LINE, 1.0f);
            if (KEYS[i].keys[0]) {                  /* the theme's light key pill, as the top panel's */
                float kw = text_w_raw(&F_MED, KEYS[i].keys, S(16), 99) + S(16);
                rrect(c, kx, y + S(2), kx + (int)kw, y + rh - S(10), S(4), C_LIGHT, 1.0f);
                text_raw(c, &F_MED, KEYS[i].keys, kx + S(8), y + (rh - S(8) - S(16)) / 2.0f, S(16), C_DARK, 0, 0);
            } else text(c, &F_REG, "Not set", kx, y + S(5), S(16), C_DIM, 0, 0);
            text(c, &F_REG, KEYS[i].what, dx, y + S(4), S(17), C_LIGHT, gx1 - dx, 0);
        }
    }
    else if (s == SC_MIC) {
        draw_header(c, "Microphone", "", 1);
        float lv = 0, fl = 0, th = 0; int st = audio_mic_meter(&lv, &fl, &th);
        /* above the room's noise floor, like the mic's own test: a blow counts past floor + threshold (medium when
         * ES has the mic off) */
        float line = th > 0 ? th : 0.15f;
        lv = lv > fl ? lv - fl : 0;
        int heard = st == 1 && lv > line;
        /* the bar: the room's own flicker (a little above its average) counts as silence; up at once, down slowly */
        #define QUIET 0.015f
        static float shown;
        float v = lv > QUIET ? lv - QUIET : 0;
        shown = v > shown ? v : shown * 0.8f;
        int x0 = gx0, x1 = gx1, y0 = gy0, y1 = S(276);
        box(c, x0, y0, x1, y1, S(14), bw, heard ? C_CYAN : C_BORDER, C_PANEL);
        text(c, &F_MED, "Blow on the mic", x0 + S(22), y0 + S(18), S(22), C_LIGHT, 0, 0);
        const char *stt = st < 0 ? "No microphone found" : st == 0 ? "Starting..." : heard ? "Heard!" : "Listening";
        float sw = text_w(&F_MED, stt, S(17), 99) + S(24);
        rrect(c, x1 - S(20) - (int)sw, y0 + S(16), x1 - S(20), y0 + S(16) + S(30), S(6), heard ? C_CYAN : C_LINE, 1.0f);
        text(c, &F_MED, stt, x1 - S(20) - sw / 2, y0 + S(21), S(17), heard ? C_DARK : C_LIGHT, 0, 1);
        /* the meter: 24 segments on a square-root scale (a quiet room still moves it), the line, the recent peak */
        #define MAPV(v) (sqrtf(fminf(fmaxf((v) / 0.4f, 0.0f), 1.0f)))   /* square root: small sounds still show */
        int mx0 = x0 + S(22), mx1 = x1 - S(22), my0 = y0 + S(72), my1 = my0 + S(56), nseg = 24, gap = S(4);
        float segw = (float)(mx1 - mx0 - (nseg - 1) * gap) / nseg;
        int lit = (int)lroundf(MAPV(shown) * nseg);
        float lpos = MAPV(line - QUIET) * nseg;
        for (int k = 0; k < nseg; k++) {
            int sx = mx0 + (int)(k * (segw + gap)), ex = mx0 + (int)(k * (segw + gap) + segw);
            uint32_t col = k < lit ? (k + 0.5f >= lpos ? C_CYAN : C_LIGHT) : C_LINE;
            fill(c, sx, my0, ex, my1, col, 1.0f);
        }
        int lx = mx0 + (int)(lpos * (segw + gap)) - gap / 2;
        fill(c, lx - S(2), my0 - S(10), lx + S(2), my1 + S(10), C_REDL, 1.0f);
        #undef MAPV
        #undef QUIET
        char l1[200], l2[200], msg[240];
        if (th > 0) snprintf(msg, sizeof msg, "The game hears a blow past the red line (sensitivity: %s).",
                             th < 0.08f ? "high" : th < 0.22f ? "medium" : "low");
        else snprintf(msg, sizeof msg, "%s", lang_active() || !is_banana() ? "Microphone sensitivity is off in ES, so the game only gets the blow below."
                      : "Microphone sensitivity is off in Settings, so the game only gets the blow below.");   /* (the translator's line is the ES one: it says Settings) */
        int n = wrap2(&F_REG, msg, S(16), x1 - x0 - S(44), l1, l2, sizeof l1, 1);
        text(c, &F_REG, l1, x0 + S(22), my1 + S(26), S(16), C_BODY, x1 - x0 - S(44), 0);
        if (n > 1) text(c, &F_REG, l2, x0 + S(22), my1 + S(50), S(16), C_BODY, x1 - x0 - S(44), 0);
        text(c, &F_REG, "A: blow for 3 s, for when the real mic isn't picked up", gx0 + S(4), y1 + S(16), S(16), C_GREY, gx1 - gx0, 0);
    }
    /* footer */
    int fy = c->h - S(56);
    const char *bl = s == SC_MAIN ? "Resume" : "Back", *al = "Select", *mid = "";
    if (s == SC_SAVE) al = "Save"; else if (s == SC_LOAD) al = "Load";
    else if (s == SC_SET) al = rows[sel_set] == ROW_MIC ? "Open" : "OK";
    else if (s == SC_MIC) al = "Blow for 3 s";
    else if (s == SC_KEYS) al = "OK";
    if (s == SC_MAIN) mid = "D-pad or tap";
    /* the translator, where the menu's language shows (the file's translated-by: "简体中文 · Majirefy") */
    char credit[160];
    if (s == SC_SET && lang_active() && *lang_by()) {
        snprintf(credit, sizeof credit, "%s · %s", lang_name(), lang_by()); lang_keep(credit); mid = credit;
    }
    R_FOOT_L = pill(c, S(20), fy, 0, "B", bl, C_LIGHT, C_LIGHT, C_DARK);
    R_FOOT_R = pill(c, c->w - S(20), fy, 1, "A", al, C_CYAN, C_CYAN, C_DARK);
    R_UNDO = R_KEYS = (rect){ 0, 0, 0, 0 };
    if (s == SC_LOAD && undo_ok) R_UNDO = pill(c, R_FOOT_R.x0 - S(12), fy, 1, "Y", "Undo last load", C_GREY, C_LIGHT, C_PILLBG);
    else {
        if (s == SC_MAIN) R_KEYS = pill(c, R_FOOT_R.x0 - S(12), fy, 1, "X", "Hotkeys", C_GREY, C_LIGHT, C_PILLBG);
        if (*mid) text(c, &F_REG, mid, s == SC_MAIN ? (R_FOOT_L.x1 + R_KEYS.x0) / 2.0f : c->w / 2.0f, fy + S(11), S(15), C_GREY, 0, 1);
    }
    if (scr == SC_OVERWRITE) {
        char t[64]; snprintf(t, sizeof t, "Replace slot %d?", sel_slot + 1);
        char w[48]; when_text(SL[sel_slot].mt, w, sizeof w);
        char b[160]; snprintf(b, sizeof b, "The save from %s will be replaced by this one.", w);
        dialog(c, t, b, "Cancel", "Replace", 0);
    } else if (scr == SC_QUIT && resume_on())   /* saving the spot is the first choice; quitting without it, under it */
        dialog3(c, "Quit to the menu?", "Your spot is saved. Next time the game picks up right here.", "Keep playing",
                "Save and quit", 0, "Quit without saving");
    else if (scr == SC_QUIT)
        dialog(c, "Quit to the menu?", "Anything since your last save will be lost.", "Keep playing", "Quit", 1);
    else if (scr == SC_QUIT_NOSAVE)
        dialog(c, "Quit without saving?", "Anything since your last save will be lost.", "Cancel", "Quit", 1);
}

static void show(int i) {
    uint32_t *px; int pitch, w, h;
    int k = dsflip_menu_canvas(i, &px, &pitch, &w, &h);
    if (k < 0) return;
    for (int y = 0; y < h; y++) memcpy((char *)px + (size_t)y * pitch, CV[i].px + (size_t)y * w, (size_t)w * 4);
    dsflip_menu_show(i, k);
}
static void redraw(void) {
    if (dirty_top) { draw_top(); show(0); dirty_top = 0; }
    if (dirty_bot) { draw_bottom(); show(1); dirty_bot = 0; }
}

/* ---------- actions ---------- */
static void close_menu(void) { open_ = 0; }
static void busy(const char *msg) {
    snprintf(busy_msg, sizeof busy_msg, "%s", msg); scr = SC_BUSY; dirty_bot = 1; redraw();
}
static void do_save(int n) {
    char p[700], m[64]; slot_path(n, p, sizeof p); snprintf(m, sizeof m, "Saved to slot %d", n + 1);
    if (resume_menu_save(p, m, gtitle)) { dsflip_toast("Couldn't save", "DraStic's Save state control has no button", 0xff7a4a, 4000); }
    else last_slot = n;
    play(SND_SELECT); close_menu();
}
static void do_load(int n) {
    char p[700], m[64];
    if (n < 0) { snprintf(p, sizeof p, "%s", undo_path); snprintf(m, sizeof m, "Back to before the load"); }
    else { slot_path(n, p, sizeof p); snprintf(m, sizeof m, "Loaded slot %d", n + 1); }
    busy(n < 0 ? "Undoing the load" : "Loading");
    /* DraStic runs to take the presses; its frames and sound stay hidden until the state is in (resume.c clears it) */
    dsflip_hold = 1; audio_mute(1);
    if (resume_menu_load(p, n >= 0 ? undo_path : 0, m, n >= 0 ? "Wrong one? Load, then Y undoes it" : gtitle)) {
        dsflip_hold = 0; audio_mute(0);
        dsflip_toast("Couldn't load", "DraStic's Load state control has no button", 0xff7a4a, 4000);
    } else { if (n >= 0) { undo_ok = 1; last_slot = n; } else undo_ok = 0; }
    play(SND_SELECT); close_menu();
}
static void activate(void) {
    switch (scr) {
    case SC_MAIN:
        switch (sel_main) {
        case 0: play(SND_BACK); close_menu(); break;
        case 1: case 2: {
            scan_slots(1);
            int nb = newest_slot();
            scr = sel_main == 1 ? SC_SAVE : SC_LOAD;
            sel_slot = last_slot >= 0 ? last_slot : nb >= 0 ? nb : 0;
            play(SND_SELECT); dirty_top = dirty_bot = 1; break;
        }
        case 3: scr = SC_SET; sel_set = 0;
                if (is_banana()) { vol = banana_volume(); if (vol < 0) vol = 50; } else vol = cfg_int("audio.volume", 50);
                bri = bri_now();
                play(SND_SELECT); dirty_top = dirty_bot = 1; break;
        case 4: play(SND_SELECT); close_menu(); resume_press(btn_menu[0] >= 0 ? btn_menu[0] : btn_menu[1]); break;
        case 5: scr = SC_QUIT; play(SND_SELECT); dirty_bot = 1; break;
        case MAIN_BRI: play(SND_SELECT); break;   /* (left / right change it) */
        }
        break;
    case SC_SAVE: if (SL[sel_slot].used) { scr = SC_OVERWRITE; play(SND_SELECT); dirty_bot = 1; } else do_save(sel_slot); break;
    case SC_OVERWRITE: do_save(sel_slot); break;
    case SC_LOAD: if (SL[sel_slot].used) do_load(sel_slot); else play(SND_BACK); break;
    case SC_SET:
        if (rows[sel_set] == ROW_MIC) { scr = SC_MIC; play(SND_SELECT); dirty_bot = 1; }
        else play(SND_SELECT);
        break;
    case SC_MIC:
        /* the game has to run to hear it: back to the game, with a pop-up for as long as it blows */
        play(SND_SELECT); mic_until = now_ms() + 3000; close_menu();
        dsflip_toast("Blowing into the mic...", "The game hears it for 3 seconds", 0x7ec8ee, 3000);
        break;
    case SC_QUIT:
        play(SND_SELECT);
        if (resume_on()) { busy("Saving your spot"); dsflip_hold = 1; audio_mute(1); }
        close_menu(); bri_save_now(); resume_quit();   /* (the brightness written before DraStic goes: no worker after) */
        break;
    case SC_QUIT_NOSAVE:
        play(SND_SELECT); close_menu(); bri_save_now(); resume_quit_nosave();
        break;
    case SC_KEYS: play(SND_BACK); scr = SC_MAIN; dirty_bot = 1; break;
    }
}
static void keys_open(void) { make_keys(); scr = SC_KEYS; play(SND_SELECT); dirty_bot = 1; }
static void quit_nosave_ask(void) {             /* X on the quit question: Quit without saving, asked again */
    if (scr != SC_QUIT || !resume_on()) return;
    scr = SC_QUIT_NOSAVE; play(SND_SELECT); dirty_bot = 1;
}
static void back(void) {
    play(SND_BACK);
    switch (scr) {
    case SC_MAIN: close_menu(); break;
    case SC_OVERWRITE: scr = SC_SAVE; dirty_bot = 1; break;
    case SC_QUIT: scr = SC_MAIN; dirty_bot = 1; break;
    case SC_QUIT_NOSAVE: scr = SC_QUIT; dirty_bot = 1; break;
    case SC_MIC: scr = SC_SET; dirty_bot = 1; break;
    default: scr = SC_MAIN; dirty_top = dirty_bot = 1; break;
    }
}
static void adjust(int d) {                     /* left/right on a Quick settings row */
    int id = rows[sel_set];
    if (id == ROW_VOL) { int v = vol + d * 5; v = v < 0 ? 0 : v > 100 ? 100 : v; if (v != vol) { vol = v; want(vol, -1); play(SND_MOVE); dirty_bot = 1; } }
    else if (id == ROW_BRI) { if (bri_set(bri + d * 5)) { play(SND_MOVE); dirty_bot = 1; } }
}
static void adjust_main_bri(int d) { if (bri_set(bri + d * 5)) { play(SND_MOVE); dirty_bot = 1; } }
static void nav(int dx, int dy) {
    int moved = 0;
    if (scr == SC_MAIN) {
        if (sel_main == MAIN_BRI) {               /* the Brightness row: left / right change it, up is the tiles */
            if (dx) { adjust_main_bri(dx); return; }
            if (dy < 0) { sel_main = 3 + main_col; moved = 1; }
        } else {
            int c = sel_main % 3, r = sel_main / 3;
            if (dx) { int n = c + dx; if (n >= 0 && n < 3) { c = n; moved = 1; } }
            if (dy) { int n = r + dy; if (n >= 0 && n < 2) { r = n; moved = 1; } else if (n == 2) { main_col = c; sel_main = MAIN_BRI; moved = 1; } }
            if (sel_main != MAIN_BRI) sel_main = r * 3 + c;
        }
    } else if (scr == SC_SAVE || scr == SC_LOAD) {
        int c = sel_slot % 4, r = sel_slot / 4;
        if (dx) { int n = c + dx; if (n >= 0 && n < 4) { c = n; moved = 1; } else if (n == 4 && r == 0) { c = 0; r = 1; moved = 1; } else if (n < 0 && r == 1) { c = 3; r = 0; moved = 1; } }
        if (dy) { int n = r + dy; if (n >= 0 && n < 2) { r = n; moved = 1; } }
        sel_slot = r * 4 + c;
        if (moved) dirty_top = 1;
    } else if (scr == SC_SET) {
        if (dy) { int n = sel_set + dy; if (n >= 0 && n < nrows) { sel_set = n; moved = 1; } }
        if (dx) { adjust(dx); return; }
    }
    if (moved) { play(SND_MOVE); dirty_bot = 1; }
}
static void tap(int px, int py) {               /* a touch-down on the bottom panel, in panel pixels */
    if (scr == SC_OVERWRITE || scr == SC_QUIT || scr == SC_QUIT_NOSAVE) {
        if (in_rect(R_DLG_A, px, py)) activate(); else if (in_rect(R_DLG_B, px, py)) back();
        else if (scr == SC_QUIT && in_rect(R_DLG_X, px, py)) quit_nosave_ask();
        return;
    }
    if (scr == SC_BUSY) return;
    if (in_rect(R_HEAD_B, px, py) || in_rect(R_FOOT_L, px, py)) { back(); return; }
    if (in_rect(R_FOOT_R, px, py)) { activate(); return; }
    if (scr == SC_LOAD && undo_ok && in_rect(R_UNDO, px, py)) { do_load(-1); return; }
    if (scr == SC_MAIN) {
        for (int i = 0; i < 6; i++) if (in_rect(R_TILE[i], px, py)) { sel_main = i; dirty_bot = 1; activate(); return; }
        if (in_rect(R_KEYS, px, py)) { keys_open(); return; }
        if (in_rect(R_MBRI, px, py)) {
            if (sel_main != MAIN_BRI) { if (sel_main >= 3) main_col = sel_main - 3; sel_main = MAIN_BRI; }
            if (in_rect(R_MBRI_BAR, px, py)) bri_set(bar_value(R_MBRI_BAR, px, &CV[1]));
            play(SND_MOVE); dirty_bot = 1; return;
        }
    }
    else if (scr == SC_SAVE || scr == SC_LOAD) {
        for (int n = 0; n < NSLOT; n++) if (in_rect(R_SLOT[n], px, py)) {
            if (n == sel_slot) activate();       /* a second tap picks it: the first one shows it on the top screen */
            else { sel_slot = n; play(SND_MOVE); dirty_top = dirty_bot = 1; }
            return;
        }
    } else if (scr == SC_SET) {
        for (int r = 0; r < nrows; r++) if (in_rect(R_ROW[r], px, py)) {
            if (sel_set != r) { sel_set = r; dirty_bot = 1; }
            int id = rows[r];
            if ((id == ROW_VOL || id == ROW_BRI) && in_rect(R_BAR[r], px, py)) {
                int v = bar_value(R_BAR[r], px, &CV[1]);
                if (id == ROW_VOL) { vol = v; want(vol, -1); } else bri_set(v);
                play(SND_MOVE); dirty_bot = 1;
            } else if (id == ROW_MIC) activate();
            else play(SND_MOVE);
            return;
        }
    }
}

/* ---------- input ---------- */
/* touch, from dsflip.c's touch thread: queued for the menu loop */
static pthread_mutex_t tm_ = PTHREAD_MUTEX_INITIALIZER;
static int tq[16][2], tqn; static volatile int touch_swallow;
int menu_touch_event(int down_change, int down, int x, int y, int xmax, int ymax) {
    if (!open_) {
        if (touch_swallow) { if (down_change && !down) touch_swallow = 0; return 1; }   /* a touch from the menu, still down */
        return 0;
    }
    if (down_change && down) {
        pthread_mutex_lock(&tm_);
        if (tqn < 16) { tq[tqn][0] = x * pw[1] / (xmax + 1); tq[tqn][1] = y * ph[1] / (ymax + 1); tqn++; }
        pthread_mutex_unlock(&tm_);
    }
    touch_swallow = down;
    return 1;
}

static int is_menu_btn(int b) { return b >= 0 && (b == btn_menu[0] || b == btn_menu[1]); }
static int held_dir, rep_dx, rep_dy; static long long rep_next;
static int axis_dir[2];
static void dir_press(int dx, int dy) { nav(dx, dy); held_dir = 1; rep_dx = dx; rep_dy = dy; rep_next = now_ms() + 380; }
static void dir_release(void) { held_dir = 0; }
static void handle_sdl(const uint8_t *e) {
    uint32_t type; memcpy(&type, e, 4);
    if (type == 0x603 || type == 0x604) {           /* SDL_JOYBUTTONDOWN / UP */
        int b = e[12], down = type == 0x603;
        if (!down) { if (b == b_up || b == b_down || b == b_left || b == b_right) dir_release(); return; }
        cpugov_boost(400);
        if (scr == SC_BUSY) return;
        if (is_menu_btn(b) || b == b_start) { play(SND_BACK); close_menu(); }
        else if (b == b_a) activate();
        else if (b == b_b) back();
        else if (b == b_y && scr == SC_LOAD && undo_ok) do_load(-1);
        else if (b == b_x && scr == SC_QUIT) quit_nosave_ask();
        else if (b == b_x && scr == SC_MAIN) keys_open();
        else if (b == b_up) dir_press(0, -1); else if (b == b_down) dir_press(0, 1);
        else if (b == b_left) dir_press(-1, 0); else if (b == b_right) dir_press(1, 0);
        else if (b == b_l && scr == SC_SET) adjust(-1); else if (b == b_r && scr == SC_SET) adjust(1);
        else if (b == b_l && scr == SC_MAIN) adjust_main_bri(-1); else if (b == b_r && scr == SC_MAIN) adjust_main_bri(1);
    } else if (type == 0x602) {                     /* SDL_JOYHATMOTION */
        int v = e[13];
        if (!v) { dir_release(); return; }
        if (scr == SC_BUSY) return;
        cpugov_boost(400);
        dir_press(v & 8 ? -1 : v & 2 ? 1 : 0, v & 1 ? -1 : v & 4 ? 1 : 0);
    } else if (type == 0x600) {                     /* SDL_JOYAXISMOTION: the left stick, as a d-pad */
        int ax = e[12]; int16_t v; memcpy(&v, e + 16, 2);
        if (ax > 1) return;
        int d = v > 16000 ? 1 : v < -16000 ? -1 : 0;
        if (d == axis_dir[ax]) return;
        axis_dir[ax] = d;
        if (!d) { dir_release(); return; }
        if (scr == SC_BUSY) return;
        cpugov_boost(400);
        if (ax == 0) dir_press(d, 0); else dir_press(0, d);
    }
}

static void menu_run(int start) {
    if (!setup()) { enabled = 0; return; }
    static int (*poll_real)(void *);
    if (!poll_real) poll_real = (int (*)(void *))dlsym(RTLD_NEXT, "SDL_PollEvent");
    if (!gbase[0]) game_info();
    dsflip_log("[menu] open\n");
    SDL_PauseAudio(1);
    audio_mic_quiet(1);             /* the real mic presses nothing while the game is paused */
    cpugov_boost(800);
    cpugov_menu(1);
    open_ = 1; scr = SC_MAIN; sel_main = 0; held_dir = 0; axis_dir[0] = axis_dir[1] = 0; tqn = 0;
    bri_changed = 0; make_rows(); main_col = 0;
    bri = bri_now();                /* the main page's Brightness row */
    scan_slots(0);
    load_recent();
    grab_background();
    dirty_top = dirty_bot = 1;
    play(SND_SELECT);
    if (start != SC_MAIN) { sel_main = start == SC_SAVE ? 1 : start == SC_LOAD ? 2 : start == SC_SET ? 3 : 5; activate(); }
    redraw();
    usleep(50000);                  /* a frame the shader thread was still finishing may have gone up after ours */
    dirty_top = dirty_bot = 1;
    long long clock_t0 = now_ms();
    while (open_) {
        uint8_t ev[64];
        while (open_ && poll_real(ev)) handle_sdl(ev);
        pthread_mutex_lock(&tm_);
        int n = tqn, q[16][2]; memcpy(q, tq, sizeof q); tqn = 0;
        pthread_mutex_unlock(&tm_);
        for (int i = 0; i < n && open_; i++) { cpugov_boost(400); tap(q[i][0], q[i][1]); }
        if (resume_quit_pending()) {   /* the exit hotkey: its save runs on the game's next frame, so let it run */
            dsflip_log("[menu] exit hotkey: closing\n"); busy("Saving your spot"); dsflip_hold = 1; audio_mute(1); close_menu();
        }
        if (open_ && held_dir && now_ms() >= rep_next) { nav(rep_dx, rep_dy); rep_next = now_ms() + 110; }
        if (lid_home_pressed() && scr != SC_BUSY) { play(SND_BACK); close_menu(); }   /* HOME again: closed (lid.c) */
        { static long long mt; if (scr == SC_MIC && now_ms() - mt >= 33) { mt = now_ms(); dirty_bot = 1; } }   /* the meter moves */
        if (is_banana()) {                                 /* the volume keys' level while the settings are open */
            static long long vt; if (scr == SC_SET && now_ms() - vt >= 500) { vt = now_ms(); want_ex(-1, -1, 1); }
            int r = vol_read; if (r >= 0) { vol_read = -1; if (scr == SC_SET && r != vol && now_ms() - vol_set_at > 1500) { vol = r; dirty_bot = 1; } }
            /* the brightness, as bananad's F + volume may have moved it (a step of its own: 3 or more off) */
            static long long bt; if ((scr == SC_MAIN || scr == SC_SET) && now_ms() - bt >= 500 && now_ms() - bri_set_at > 1500) {
                bt = now_ms(); int n = 0, sum = 0;
                for (int i = 0; i < 2; i++) { int v = bl_pm(i); if (v > 0) { sum += v; n++; } }
                int b = n ? (sum / n + 5) / 10 : bri;
                if (b - bri >= 3 || bri - b >= 3) { bri = banana_brightness(); dirty_bot = 1; } }
        }
        if (now_ms() - clock_t0 >= 15000) { clock_t0 = now_ms(); if (scr == SC_MAIN || scr == SC_SET || scr == SC_QUIT || scr == SC_QUIT_NOSAVE) dirty_top = 1; }   /* the clock */
        if (open_) redraw();
        usleep(8000);
    }
    bri_save_now();
    if (mic_until) dsflip_mic_key(1);
    SDL_PauseAudio(0);
    audio_mic_quiet(0);
    cpugov_menu(0);
    dsflip_log("[menu] closed\n");
}

/* SDL_PollEvent (dsflip.c) shows this each event DraStic would get: 1 = swallow it. The menu button opens the menu
 * when it is released with nothing else pressed meanwhile (so "menu + start" still quits, resume.c). */
static int pending, other;
int menu_event(void *ev) {
    if (enabled < 0) {
        const char *e = getenv("DSFLIP_MENU"); enabled = !(e && *e == '0');
        btn_menu[0] = resume_control_button("MENU");
        { /* both control sets: ROCKNIX maps the menu to a different button in each */
          int a = btn_menu[0]; btn_menu[1] = -1;
          FILE *f = fopen("config/drastic.cfg", "r"); char line[256];
          if (f) { while (fgets(line, sizeof line, f)) {
                      int v; if (sscanf(line, "controls_b[CONTROL_INDEX_MENU] = %d", &v) == 1 && v >= 1024 && v < 1088 && v - 1024 != a) btn_menu[1] = v - 1024;
                      if (sscanf(line, "controls_a[CONTROL_INDEX_MENU] = %d", &v) == 1 && v >= 1024 && v < 1088 && v - 1024 != a && btn_menu[1] < 0) btn_menu[1] = v - 1024; }
                   fclose(f); } }
        b_a = resume_control_button("A"); b_b = resume_control_button("B"); b_x = resume_control_button("X"); b_y = resume_control_button("Y");
        b_l = resume_control_button("L"); b_r = resume_control_button("R"); b_start = resume_control_button("START");
        b_up = resume_control_button("UP"); b_down = resume_control_button("DOWN"); b_left = resume_control_button("LEFT"); b_right = resume_control_button("RIGHT");
        /* a button DraStic has no control on any more (a setup gone wrong): the RG DS pad's own (retrogame_joypad's
         * order: B A X Y L R L2 R2 Select Start F L3 R3 up down left right), so the menu still works from HOME */
        if (b_a < 0) b_a = 1; if (b_b < 0) b_b = 0; if (b_x < 0) b_x = 2; if (b_y < 0) b_y = 3; if (b_l < 0) b_l = 4; if (b_r < 0) b_r = 5;
        if (b_start < 0) b_start = 9; if (b_up < 0) b_up = 13; if (b_down < 0) b_down = 14; if (b_left < 0) b_left = 15; if (b_right < 0) b_right = 16;
        /* (no menu button at all, a button setup gone wrong: HOME still opens it, lid.c) */
        dsflip_log("[menu] %s: menu buttons %d %d, A %d B %d Y %d, d-pad %d %d %d %d\n", enabled ? "on" : "off",
                   btn_menu[0], btn_menu[1], b_a, b_b, b_y, b_up, b_down, b_left, b_right);
    }
    if (!enabled || !ev) return 0;
    const uint8_t *e = ev; uint32_t type; memcpy(&type, e, 4);
    if (type != 0x603 && type != 0x604) return 0;
    int b = e[12], down = type == 0x603;
    if (is_menu_btn(b)) {
        if (dsflip_drastic_menu() || resume_menu_busy()) return 0;   /* DraStic's own menu is up: its button */
        if (down) { pending = 1; other = 0; return 1; }
        int go = pending && !other; pending = 0;
        if (go) menu_run(SC_MAIN);
        return 1;
    }
    if (pending && down) other = 1;
    return 0;
}

/* HOME (lid.c): the menu whatever DraStic's buttons are, unless DraStic's own menu or a save/load is up */
void menu_home(void) {
    if (enabled < 0) menu_event(0);
    if (!enabled || open_ || dsflip_drastic_menu() || resume_menu_busy()) return;
    menu_run(SC_MAIN);
}

/* every present (dsflip.c) */
void menu_frame(void) {
    static int test = -1; static int frames;
    if (test < 0) { const char *t = getenv("DSFLIP_MENU_TEST"); test = t ? atoi(t) + 1 : 0; }
    if (!frames++) session_t0 = now_ms();
    if (mic_until && now_ms() >= mic_until) { mic_until = 0; dsflip_mic_key(0); }

    if (test && frames == 180) {
        if (enabled < 0) menu_event(0);
        /* 1 main, 2 save, 3 load, 4 quick settings, 5 quit */
        static const int sc[] = { SC_MAIN, SC_MAIN, SC_SAVE, SC_LOAD, SC_SET, SC_QUIT };
        if (enabled) menu_run(test - 1 < 6 ? sc[test - 1] : SC_MAIN);
    }
}
