// menu.c: BananaDS's in-game menu: SuperDrastic's (its src/menu.c, the same pages, layout and sounds), in BananaOS's
// colours, driven by BananaDS's KMS frontend through menu.h's bmenu_host instead of DraStic's SDL calls. L3 (or F on
// its own) opens it; the game, its sound and its clock stand still while it is up.
//
// Bottom panel: six tiles (Resume, Save, Load, Quick settings, Restart game, Quit game), then a page for each.
// Top panel: the game's last frame, dimmed, under a card with the game, this session's play time and the last save;
// while a save slot is picked, that slot's picture instead. D-pad and touch work; A picks, B goes back, L3 or START
// closes it.
//
// Saves and loads are BananaDS's savestates (slots 1-8), their pictures the .pic BananaDS writes beside each one. A
// load first keeps the current game in slot 9, so a wrong slot never costs progress ("Undo last load", Y on the Load
// page).
#define _GNU_SOURCE
#include <dirent.h>
#include <glob.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WRITE
#include "third_party/stb_image.h"
#include "lang.h"
#include "menu.h"
#include "ra.h"

static bmenu_host H;
static void mlog(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (H.log) H.log(H.ctx, b);
}
static void boost(int ms) { if (H.boost) H.boost(H.ctx, ms); }

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---------- colours: ROCKNIXDS Pixel's dark theme through BananaOS's palette (BananaLinux tools/banana-palette.sh:
 * blue-greys turned warm charcoal, the blue accents banana yellow); the teal card stays, as banana-ui's ---------- */
#define C_BG      0x191820
#define C_DOT     0x25232b
#define C_PANEL   0x211f26
#define C_BORDER  0x4a443f
#define C_LIGHT   0xf1ede2
#define C_GREY    0xa39d8e
#define C_BLUE    0xf0b429      /* the selected tile's plate: banana yellow (dark text on it, C_ONSEL) */
#define C_CYAN    0xffe68a      /* the accent text: light banana */
#define C_DARK    0x17161c
#define C_ONSEL   0x17161c      /* text and icons on the yellow plate */
#define C_ONSELSUB 0x3a2e14
#define C_RED     0xa3283a
#define C_REDL    0xff8a96
#define C_REDDIM  0x6b2a33
#define C_TEAL    0x1f4645
#define C_TEALDOT 0x27524f
#define C_TEALTXT 0xa9c3c2
#define C_TEALLN  0x4c6a69
#define C_LINE    0x3a3540
#define C_PILLBG  0x2e2b33      /* a secondary pill's inside */
#define C_BODY    0xd5cfc2      /* body text in dialogs */
#define C_DIM     0x6b6559      /* an empty slot's number and date */
#define C_EMPTY0  0x1a191f      /* an empty slot's hatching */
#define C_EMPTY1  0x24222a
#define C_NOPIC   0x38333b

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
            mlog("[menu] font %s\n", paths[i]);
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
typedef struct { int used; time_t mt; uint16_t *pic; int pic_tried; } slot;
static slot SL[NSLOT];
static char rom[512], gbase[256], gtitle[200], ggenre[100];
static int undo_ok;                 /* the undo file holds the game from before this session's last menu load */
static long long session_t0;
static int last_slot = -1;          /* the slot this session saved to or loaded last */

static void game_info(void) {
    FILE *f;
    snprintf(rom, sizeof rom, "%s", H.rom ? H.rom : "");
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
    mlog("[menu] game '%s' (%s)\n", gtitle, gbase);
}
static void slot_path(int n, char *out, size_t sz) { H.state_file(H.ctx, n + 1, out, (int)sz); }

#define PIC_BYTES (256 * 192 * 2)
/* a savestate's picture: BananaDS's <state>.pic beside it, both screens 256x384 XRGB8888; the top one as 256x192
 * RGB565 (what draw_pic takes), or 0 */
static uint16_t *read_pic(const char *path) {
    char pp[760]; snprintf(pp, sizeof pp, "%s.pic", path);
    FILE *f = fopen(pp, "rb"); if (!f) return 0;
    uint32_t *top = malloc(256 * 192 * 4); uint16_t *out = 0;
    if (top && fread(top, 4, 256 * 192, f) == 256 * 192 && (out = malloc(PIC_BYTES)))
        for (int i = 0; i < 256 * 192; i++) {
            uint32_t p = top[i];
            out[i] = (uint16_t)(((p >> 19) & 31) << 11 | ((p >> 10) & 63) << 5 | ((p >> 3) & 31));
        }
    free(top); fclose(f);
    return out;
}
static void scan_slots(int pics) {
    for (int n = 0; n < NSLOT; n++) {
        char p[700]; slot_path(n, p, sizeof p); struct stat st;
        int used = !stat(p, &st) && st.st_size > 0;
        if (!used || st.st_mtime != SL[n].mt) { free(SL[n].pic); SL[n].pic = 0; SL[n].pic_tried = 0; }
        SL[n].used = used; SL[n].mt = used ? st.st_mtime : 0;
        if (pics && used && !SL[n].pic_tried) { SL[n].pic_tried = 1; SL[n].pic = read_pic(p); }
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

/* ---------- settings: volume and brightness, as BananaLinux keeps them ----------
 * The volume through BananaLinux's own script (/usr/libexec/banana/volume get|set: the card and control bananad names,
 * the RG DS Plus's amplifiers' table, the ear's curve, playback only), as the volume keys and Settings do; bananad is
 * poked to store it. Before, amixer on the named control: the Plus's "PCM" doesn't exist (its amplifiers are "Left
 * PCM"/"Right PCM", so nothing changed), and on the RG DS it set the microphone's level too and read another percent
 * than the keys'. While the menu's settings are open the level is read again twice a second, so the keys show.
 * The backlights: each panel its own (backlight1 the top, backlight0 the bottom); the row moves both by the same step,
 * shows their middle, and "<top> <bottom>" in thousandths goes to /data/banana/backlight for bananad to restore at
 * boot (before, the first backlight's raw value went there, which bananad read as thousandths). */
static const char *BLN[2] = { "backlight1", "backlight0" };   /* top, bottom */
static int bl_read(const char *path) { FILE *f = fopen(path, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
static int bl_pm(int i) {            /* a panel's level now, thousandths of its maximum (-1: none) */
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
static int volume_get(void) {         /* -1: the script gave no level (the refresh then keeps what is shown) */
    FILE *p = popen("/usr/libexec/banana/volume get 2>/dev/null", "r"); if (!p) return -1;
    int v = -1; if (fscanf(p, "%d", &v) != 1) v = -1;
    pclose(p);
    return v < 0 ? -1 : v > 100 ? 100 : v;
}
static int bl_base[2] = { -1, -1 }, bl_base_avg = 80;   /* the levels when the settings opened, and their middle in % */
static int brightness_get(void) {
    int n = 0, sum = 0;
    for (int i = 0; i < 2; i++) { bl_base[i] = bl_pm(i); if (bl_base[i] > 0) { sum += bl_base[i]; n++; } }
    bl_base_avg = n ? (sum / n + 5) / 10 : 80;
    return bl_base_avg;
}
static int vol, bri, bri_changed;
static long long vol_set_at;                               /* the menu's own change: no reading back for a moment */
static pthread_mutex_t wm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t wc = PTHREAD_COND_INITIALIZER;
static int w_vol = -1, w_bri = -1, w_read = 0;
static volatile int vol_read = -1;                         /* the level read by the worker, for the menu to show */
static void *worker(void *a) {       /* the volume script and the saved brightness: off the menu's thread */
    (void)a;
    for (;;) {
        pthread_mutex_lock(&wm);
        while (w_vol < 0 && w_bri < 0 && !w_read) pthread_cond_wait(&wc, &wm);
        int v = w_vol, b = w_bri, r = w_read; w_vol = w_bri = -1; w_read = 0;
        pthread_mutex_unlock(&wm);
        char cmd[96];
        if (v >= 0) {
            snprintf(cmd, sizeof cmd, "/usr/libexec/banana/volume set %d >/dev/null 2>&1", v); if (system(cmd)) {}
            if (system("killall -HUP bananad >/dev/null 2>&1")) {}   /* it stores the level across reboots */
        }
        if (b >= 0) {
            int t = bl_pm(0), u = bl_pm(1);
            FILE *f = fopen("/data/banana/backlight", "w"); if (f) { fprintf(f, "%d %d\n", t > 0 ? t : u, u > 0 ? u : t); fclose(f); }
        }
        if (r && v < 0) vol_read = volume_get();
    }
    return 0;
}
static void want_ex(int v, int b, int r) {
    static int started;
    pthread_mutex_lock(&wm);
    if (!started) { pthread_t t; if (!pthread_create(&t, 0, worker, 0)) { pthread_detach(t); pthread_setname_np(t, "bds-menu"); started = 1; } }
    if (v >= 0) w_vol = v; if (b >= 0) w_bri = b; if (r) w_read = 1;
    pthread_cond_signal(&wc); pthread_mutex_unlock(&wm);
}
static void want(int v, int b) { if (v >= 0) vol_set_at = now_ms(); want_ex(v, b, 0); }
static void backlight(int pct) {     /* at once, both panels moved by the row's step from where they were; saved at close */
    int d = (pct - bl_base_avg) * 10;
    for (int i = 0; i < 2; i++) {
        if (bl_base[i] <= 0) continue;
        int v = bl_base[i] + d; v = v < 50 ? 50 : v > 1000 ? 1000 : v;
        bl_set_pm(i, v);
    }
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
    if (n && H.sound_load) { H.sound_load(H.ctx, id, pcm, (int)n, rate, ch); snd_ok[id] = 1; }
    free(pcm);
}
static void play(int id) { if (snd_ok[id] && H.sound_play) H.sound_play(H.ctx, id); }

/* ---------- state ---------- */
enum { SC_MAIN, SC_SAVE, SC_LOAD, SC_SET, SC_QUIT, SC_OVERWRITE, SC_BUSY, SC_MIC, SC_RESTART, SC_CHEAT };
static int enabled = -1, inited, open_, scr, sel_main, sel_slot, sel_set, dirty_top, dirty_bot;
static char busy_msg[96];

static uint32_t *bgtop;                         /* the game's top frame, dimmed (panel size) */
static canvas CV[2];
static int pw[2], ph[2];

/* rows of Quick settings that exist on this setup */
enum { ROW_VOL, ROW_BRI, ROW_MIC, ROW_CHEAT, ROW_OPT = 100 };   /* ROW_OPT + i: the emulator's option i (bmenu_host's opt_*) */
static int rows[8], nrows;
#define CHEAT_ROWS 7                            /* the Cheats page's rows on screen at once */
static int sel_cheat, top_cheat;
static void make_rows(void) {
    nrows = 0; rows[nrows++] = ROW_VOL; rows[nrows++] = ROW_BRI; rows[nrows++] = ROW_MIC;
    if (H.cheat_count) rows[nrows++] = ROW_CHEAT;
    int n = H.opt_count ? H.opt_count(H.ctx) : 0;
    for (int i = 0; i < n && nrows < 8; i++) rows[nrows++] = ROW_OPT + i;
}

static int fonts_ready(void) {   /* the menu's and the pop-ups' fonts, loaded once */
    const char *const med[] = { getenv("BANANADS_MENU_FONT"), "/usr/share/banana/rnds/fonts/PixelifySans-Medium.ttf",
        "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Medium.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
    const char *const reg[] = { "/usr/share/banana/rnds/fonts/PixelifySans-Regular.ttf",
        "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/fonts/PixelifySans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
    static int lang_done;
    if (!lang_done) {   /* the language (BananaLinux's settings.conf), and the pixel font for its characters */
        lang_done = 1;
        if (lang_init("bananads")) {
            const char *const cjk[] = { getenv("BANANADS_MENU_FONT_CJK"),
                "/usr/share/banana/rnds/fonts/fusion-pixel-12px-proportional-zh_hans.ttf" };
            font_load(&F_CJK, cjk, 2);
            mlog("[menu] language %s, translated by %s\n", lang_name(), lang_by());
        }
    }
    return font_load(&F_MED, med, 5) && font_load(&F_REG, reg, 4);
}
static int setup(void) {
    if (inited) return inited > 0;
    inited = -1;
    for (int i = 0; i < 2; i++) {
        uint32_t *px; int pitch;
        if (H.canvas(H.ctx, i, &px, &pitch, &pw[i], &ph[i]) < 0) { mlog("[menu] no panel buffers\n"); return 0; }
        CV[i].w = pw[i]; CV[i].h = ph[i]; CV[i].s = pw[i] / 640.0f;
        if (!(CV[i].px = malloc((size_t)pw[i] * ph[i] * 4))) return 0;
    }
    if (!(bgtop = malloc((size_t)pw[0] * ph[0] * 4))) return 0;
    if (!fonts_ready()) { mlog("[menu] no font\n"); return 0; }
    const char *snd[SND_N] = { "scroll.wav", "select.wav", "back.wav" };
    for (int k = 0; k < SND_N; k++) {
        char p[300];
        snprintf(p, sizeof p, "/usr/share/banana/rnds/sounds/%s", snd[k]);
        if (access(p, R_OK)) snprintf(p, sizeof p, "/storage/.config/emulationstation/themes/rocknixds-pixel-dark/rnds/sounds/%s", snd[k]);
        load_wav(k, p);
    }
    inited = 1;
    return 1;
}

/* ---------- top panel ---------- */
static void grab_background(void) {
    canvas *c = &CV[0];
    if (H.grab_top(H.ctx, bgtop, pw[0] * 4, pw[0], ph[0])) {
        for (size_t i = 0, n = (size_t)pw[0] * ph[0]; i < n; i++) {   /* the game, at a third of its brightness */
            uint32_t p = bgtop[i];
            bgtop[i] = ((p >> 16 & 255) * 85 / 256) << 16 | ((p >> 8 & 255) * 85 / 256) << 8 | (p & 255) * 85 / 256;
        }
    } else {
        canvas t = { bgtop, pw[0], ph[0], c->s }; dots(&t, 0, 0, pw[0], ph[0], C_BG, C_DOT);
    }
}
static void status_bar(canvas *c) {
    int chg = 0, pct = H.battery ? H.battery(H.ctx, &chg) : -1;
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
/* an RGB565 256x192 picture into a rectangle (nearest) */
static void draw_pic(canvas *c, const uint16_t *pic, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; y++) {
        if (y0 + y < 0 || y0 + y >= c->h) continue;
        const uint16_t *row = pic + (size_t)(y * 192 / h) * 256; uint32_t *d = c->px + (size_t)(y0 + y) * c->w;
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
/* RetroAchievements (ra.c): the progress and the latest unlocks with their badges; without a game that has
 * achievements the card is laid out without them */
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
        slot *s = &SL[sel_slot];
        int pw_ = S(384), ph_ = S(288), px = (c->w - pw_) / 2, py = S(68);
        box(c, px - S(3), py - S(3), px + pw_ + S(3), py + ph_ + S(3), S(6), S(3), C_LIGHT, C_DARK);
        if (s->used && s->pic) draw_pic(c, s->pic, px, py, pw_, ph_);
        else if (s->used) { fill(c, px, py, px + pw_, py + ph_, C_PANEL, 1.0f); text(c, &F_REG, "No picture in this save", c->w / 2.0f, py + ph_ / 2 - S(10), S(18), C_GREY, 0, 1); }
        else { hatch(c, px, py, px + pw_, py + ph_, C_EMPTY0, C_EMPTY1); text(c, &F_MED, "Empty slot", c->w / 2.0f, py + ph_ / 2 - S(12), S(22), C_GREY, 0, 1); }
        char t[48]; snprintf(t, sizeof t, "Slot %d", sel_slot + 1);
        text(c, &F_MED, t, px, py + ph_ + S(14), S(24), C_LIGHT, 0, 0);
        if (s->used) { char w[48]; when_text(s->mt, w, sizeof w); text(c, &F_REG, w, px + pw_, py + ph_ + S(18), S(18), C_TEALTXT, 0, 2); }
        const char *hint = scr == SC_LOAD ? (s->used ? "A: load this save. You can undo it after." : "Nothing saved here yet")
                                          : (s->used ? "A: save here (replaces this save)" : "A: save here");
        text(c, &F_REG, hint, px, py + ph_ + S(50), S(16), C_TEALTXT, pw_, 0);
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
        /* without achievements: BananaDS's buttons, so they are found */
        if (!ra_has) {
            fill(c, L, y, x1 - S(28), y + S(2), C_TEALLN, 1.0f);
            y += S(16);
            text(c, &F_MED, "BUTTONS", L, y, S(14), C_TEALTXT, 0, 0);
            y += S(28);
            static const char *const keys[][2] = { { "F / L3", "this menu" }, { "F + R", "save to slot 1" },
                                                   { "F + L", "load slot 1" }, { "F + START", "save your spot and quit" } };
            for (int i = 0; i < 4 && y + S(24) <= y1 - S(12); i++, y += S(30)) {
                float kw = text_w(&F_MED, keys[i][0], S(15), 99) + S(14);
                rrect(c, L, y - S(3), L + (int)kw, y + S(21), S(4), C_LIGHT, 1.0f);
                text(c, &F_MED, keys[i][0], L + S(7), y, S(15), C_DARK, 0, 0);
                text(c, &F_REG, keys[i][1], L + S(132), y + S(1), S(16), C_LIGHT, cw - S(132), 0);
            }
        }
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
static rect R_TILE[6], R_SLOT[NSLOT], R_ROW[8], R_BAR[8], R_FOOT_L, R_FOOT_R, R_HEAD_B, R_DLG_B, R_DLG_A, R_UNDO;
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
static const char *const IC_RESTART[12] = { "............", "....####.#..", "...#....##..", "..#....###..", ".#..........", ".#..........",
    ".#..........", ".#........#.", "..#......#..", "...#....#...", "....####....", "............" };
static const char *tile_label[6] = { "Resume", "Save", "Load", "Quick settings", "Restart game", "Quit game" };
static const char *const *tile_icon[6] = { IC_PLAY, IC_SAVE, IC_LOAD, IC_SLIDERS, IC_RESTART, IC_EXIT };

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
    for (int k = 0; k < 10; k++) fill(c, x + k * S(15), y, x + k * S(15) + S(11), y + S(20), k < (v + 5) / 10 ? (sel ? C_ONSEL : C_CYAN) : (sel ? 0xc9921f : C_LINE), 1.0f);
}
static void dialog(canvas *c, const char *title, const char *body, const char *no, const char *yes, int danger) {
    fill(c, 0, 0, c->w, c->h, C_DARK, 0.65f);
    int w = S(460), h = S(210), x0 = (c->w - w) / 2, y0 = S(120);
    box(c, x0, y0, x0 + w, y0 + h, S(14), S(3), C_LIGHT, C_DARK);
    text(c, &F_MED, title, x0 + S(26), y0 + S(22), S(24), C_LIGHT, w - S(52), 0);
    char l1[200], l2[200]; int n = wrap2(&F_REG, body, S(17), w - S(52), l1, l2, sizeof l1, 1);
    text(c, &F_REG, l1, x0 + S(26), y0 + S(66), S(17), C_BODY, w - S(52), 0);
    if (n > 1) text(c, &F_REG, l2, x0 + S(26), y0 + S(90), S(17), C_BODY, w - S(52), 0);
    int by = y0 + h - S(60);
    R_DLG_A = pill(c, x0 + w - S(24), by, 1, "A", yes, danger ? C_REDL : C_LIGHT, C_LIGHT, danger ? C_RED : C_BLUE);
    R_DLG_B = pill(c, R_DLG_A.x0 - S(12), by, 1, "B", no, C_GREY, C_LIGHT, C_PILLBG);
}
static void draw_bottom(void) {
    canvas *c = &CV[1];
    dots(c, 0, 0, c->w, c->h, C_BG, C_DOT);
    int gx0 = S(20), gx1 = c->w - S(20), gy0 = S(58), gy1 = S(404), bw = S(3) < 2 ? 2 : S(3);
    int s = scr == SC_OVERWRITE ? SC_SAVE : (scr == SC_QUIT || scr == SC_RESTART) ? SC_MAIN : scr;
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
        int gap = S(12), tw = (gx1 - gx0 - 2 * gap) / 3, th = (gy1 - gy0 - gap) / 2;
        int nb = newest_slot();
        for (int i = 0; i < 6; i++) {
            int x = gx0 + (i % 3) * (tw + gap), y = gy0 + (i / 3) * (th + gap), sel = i == sel_main, red = i == 5;
            R_TILE[i] = (rect){ x, y, x + tw, y + th };
            uint32_t bd = sel ? (red ? C_REDL : C_LIGHT) : (red ? C_REDDIM : C_BORDER), bg = sel ? (red ? C_RED : C_BLUE) : C_PANEL;
            uint32_t fg = sel ? (red ? 0xffffff : C_ONSEL) : (red ? C_REDL : C_LIGHT), sub2 = sel ? (red ? 0xffd6dc : C_ONSELSUB) : (red ? 0xc98a92 : C_GREY);
            box(c, x, y, x + tw, y + th, S(14), bw, bd, bg);
            int k = S(4) < 2 ? 2 : S(4);
            bitmap(c, tile_icon[i], 12, x + (tw - 12 * k) / 2, y + S(24), k, fg);
            text(c, &F_MED, tile_label[i], x + tw / 2.0f, y + S(86), S(20), fg, tw - S(16), 1);
            char h[64] = "";
            switch (i) {
            case 0: snprintf(h, sizeof h, "or press F / L3"); break;
            case 1: snprintf(h, sizeof h, "%d slots", NSLOT); break;
            case 2: if (nb >= 0) snprintf(h, sizeof h, "last: slot %d", nb + 1); else snprintf(h, sizeof h, "no saves yet"); break;
            case 3: snprintf(h, sizeof h, H.opt_count && H.opt_count(H.ctx) ? "volume, shader, effects" : "volume, brightness"); break;
            case 4: snprintf(h, sizeof h, "from the title screen"); break;
            case 5: snprintf(h, sizeof h, H.resume_on ? "keeps your spot" : "back to the menu"); break;
            }
            text(c, &F_REG, h, x + tw / 2.0f, y + S(118), S(14), sub2, tw - S(16), 1);
        }
    } else if (s == SC_SAVE || s == SC_LOAD) {
        char sub[48]; snprintf(sub, sizeof sub, "Slot %d of %d", sel_slot + 1, NSLOT);
        draw_header(c, s == SC_SAVE ? "Save" : "Load", sub, 1);
        int gap = S(10), cw = (gx1 - gx0 - 3 * gap) / 4, ch = (gy1 - gy0 - gap) / 2, nb = newest_slot();
        for (int n = 0; n < NSLOT; n++) {
            slot *sl = &SL[n];
            int x = gx0 + (n % 4) * (cw + gap), y = gy0 + (n / 4) * (ch + gap), sel = n == sel_slot, dim = s == SC_LOAD && !sl->used;
            R_SLOT[n] = (rect){ x, y, x + cw, y + ch };
            box(c, x, y, x + cw, y + ch, S(10), bw, sel ? C_LIGHT : dim ? C_LINE : C_BORDER, sel ? C_BLUE : dim ? C_EMPTY0 : C_PANEL);
            int k = S(3) < 2 ? 2 : S(3);
            bitmap(c, DIG[(n + 1) % 10], 7, x + S(10), y + S(10), k, dim ? C_DIM : sel ? C_ONSEL : C_LIGHT);
            const char *tag = n == nb ? "LAST" : "";
            if (*tag) text(c, &F_MED, tag, x + cw - S(10), y + S(10), S(13), sel ? C_ONSEL : C_CYAN, 0, 2);
            int tx0 = x + S(8), tx1 = x + cw - S(8), tth = (tx1 - tx0) * 3 / 4, ty0 = y + S(36);
            if (sl->used && sl->pic) draw_pic(c, sl->pic, tx0, ty0, tx1 - tx0, tth);
            else if (sl->used) fill(c, tx0, ty0, tx1, ty0 + tth, C_NOPIC, 1.0f);
            else hatch(c, tx0, ty0, tx1, ty0 + tth, dim ? C_EMPTY0 : C_EMPTY0, dim ? C_EMPTY0 : C_EMPTY1);
            char w[48]; if (sl->used) when_text(sl->mt, w, sizeof w); else snprintf(w, sizeof w, "Empty");
            text(c, &F_REG, w, x + cw / 2.0f, ty0 + tth + S(8), S(13), dim ? C_DIM : sel ? C_ONSEL : C_BODY, cw - S(12), 1);
        }
    } else if (s == SC_SET) {
        draw_header(c, "Quick settings", "changes apply at once", 1);
        int y = gy0, rh = nrows > 6 ? S(38) : nrows > 3 ? S(48) : S(54), gap = nrows > 6 ? S(5) : nrows > 3 ? S(6) : S(10);
        for (int r = 0; r < nrows; r++) {
            int sel = r == sel_set, id = rows[r];
            R_ROW[r] = (rect){ gx0, y, gx1, y + rh }; R_BAR[r] = (rect){ 0, 0, 0, 0 };
            box(c, gx0, y, gx1, y + rh, S(10), bw, sel ? C_LIGHT : C_LINE, sel ? C_BLUE : C_PANEL);
            const char *const *ic = id == ROW_VOL ? IC_VOL : id == ROW_BRI ? IC_SUN : id == ROW_MIC ? IC_MIC : IC_SLIDERS;
            int k = S(2) < 2 ? 2 : S(2);
            bitmap(c, ic, 12, gx0 + S(16), y + (rh - 12 * k) / 2, k, sel ? C_ONSEL : C_LIGHT);
            const char *lab = id == ROW_VOL ? "Volume" : id == ROW_BRI ? "Brightness" : id == ROW_MIC ? "Microphone" : id == ROW_CHEAT ? "Cheats" : H.opt_label(H.ctx, id - ROW_OPT);
            text(c, &F_MED, lab, gx0 + S(56), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_LIGHT, 0, 0);
            char v[32] = "";
            if (id == ROW_VOL || id == ROW_BRI) {
                int val = id == ROW_VOL ? vol : bri, bx = gx1 - S(110) - S(146);
                bar(c, bx, y + (rh - S(20)) / 2, val, sel);
                R_BAR[r] = (rect){ bx - S(8), y, bx + S(150), y + rh };
                snprintf(v, sizeof v, "%d%%", val);
            } else if (id == ROW_MIC) snprintf(v, sizeof v, "Test / blow");
            else if (id == ROW_CHEAT) {
                int n = H.cheat_count(H.ctx), on = 0;
                for (int i = 0; i < n; i++) on += H.cheat_get(H.ctx, i) != 0;
                if (n) snprintf(v, sizeof v, "%d of %d on", on, n); else snprintf(v, sizeof v, "None found");
            }
            else { snprintf(v, sizeof v, "< %s >", lang_tr(H.opt_value_name(H.ctx, id - ROW_OPT, H.opt_get(H.ctx, id - ROW_OPT)))); lang_keep(v); }
            text(c, &F_MED, v, gx1 - S(18), y + (rh - S(20)) / 2.0f - S(1), S(20), sel ? C_ONSEL : C_CYAN, 0, 2);
            y += rh + gap;
        }
        const char *help = "";
        switch (rows[sel_set]) {
        case ROW_VOL: help = "Left / right to change, or tap the bar"; break;
        case ROW_BRI: help = "Left / right to change, both screens"; break;
        case ROW_MIC: help = "A live meter of the mic, and a blow for games that ask for one"; break;
        case ROW_CHEAT: help = "Action Replay codes for this game: A to see them"; break;
        default: help = "Left / right to change: it applies at once, and stays for the next game"; break;
        }
        text(c, &F_REG, help, gx0 + S(4), y + S(6), S(16), C_GREY, gx1 - gx0, 0);
    }
    else if (s == SC_CHEAT) {
        int n = H.cheat_count(H.ctx), on = 0;
        for (int i = 0; i < n; i++) on += H.cheat_get(H.ctx, i) != 0;
        char sub[48]; if (n) snprintf(sub, sizeof sub, "%d of %d on", on, n); else sub[0] = 0;
        draw_header(c, "Cheats", sub, 1);
        if (!n) {
            const char *l[] = { "No cheats for this game yet.", "",
                "In RetroArch: Online Updater > Update Cheats", "(BananaDS finds the game's file there),",
                "or put a melonDS .mch file in", "/data/bananads/cheats/ named after the game." };
            for (int i = 0; i < 6; i++)
                text(c, i ? &F_REG : &F_MED, l[i], gx0 + S(8), gy0 + S(16) + i * S(30), i ? S(17) : S(21), i ? C_BODY : C_LIGHT, gx1 - gx0 - S(16), 0);
        } else {
            int rh = S(38), gap = S(5), vis = CHEAT_ROWS;
            if (sel_cheat >= n) sel_cheat = n - 1;
            if (sel_cheat < top_cheat) top_cheat = sel_cheat;
            if (sel_cheat >= top_cheat + vis) top_cheat = sel_cheat - vis + 1;
            int y = gy0;
            for (int r = 0; r < vis; r++) {
                int i = top_cheat + r;
                R_ROW[r] = (rect){ 0, 0, 0, 0 };
                if (i >= n) break;
                int sel = i == sel_cheat, o = H.cheat_get(H.ctx, i) != 0;
                R_ROW[r] = (rect){ gx0, y, gx1, y + rh };
                box(c, gx0, y, gx1, y + rh, S(10), bw, sel ? C_LIGHT : C_LINE, sel ? C_BLUE : C_PANEL);
                int pw = S(58), px1 = gx1 - S(12), py0 = y + S(8), py1 = y + rh - S(8);
                rrect(c, px1 - pw, py0, px1, py1, (float)S(6), o ? (sel ? C_ONSEL : C_CYAN) : C_LINE, 1.0f);
                text(c, &F_MED, o ? "On" : "Off", px1 - pw / 2.0f, py0 + S(2), S(17), o ? (sel ? C_CYAN : C_DARK) : C_LIGHT, 0, 1);
                text_raw(c, &F_REG, H.cheat_name(H.ctx, i), gx0 + S(16), y + (rh - S(18)) / 2.0f - S(1), S(18),
                     sel ? C_ONSEL : C_LIGHT, gx1 - gx0 - pw - S(44), 0);
                y += rh + gap;
            }
            if (n > vis) {   /* where in the list: a thin bar at the right edge */
                int ty0 = gy0, ty1 = gy0 + vis * (rh + gap) - gap, th = (ty1 - ty0) * vis / n; if (th < S(12)) th = S(12);
                int t0 = ty0 + (int)((long long)(ty1 - ty0 - th) * top_cheat / (n - vis));
                fill(c, gx1 + S(6), t0, gx1 + S(10), t0 + th, C_GREY, 1.0f);
            }
            text(c, &F_REG, "Up / down to pick, L / R a page; some games need a restart for a code to work", gx0 + S(4), y + S(4), S(15), C_GREY, gx1 - gx0, 0);
        }
    }
    else if (s == SC_MIC) {
        draw_header(c, "Microphone", "", 1);
        float lv = 0, fl = 0, th = 0; int st = H.mic_meter ? H.mic_meter(H.ctx, &lv, &fl, &th) : -1;
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
        if (th > 0) snprintf(msg, sizeof msg, "The game hears a word or a blow past the red line (sensitivity: %s).",
                             th < 0.035f ? "high" : th < 0.1f ? "medium" : "low");
        else snprintf(msg, sizeof msg, "The microphone's gate is off, so the game hears everything it picks up.");
        int n = wrap2(&F_REG, msg, S(16), x1 - x0 - S(44), l1, l2, sizeof l1, 1);
        text(c, &F_REG, l1, x0 + S(22), my1 + S(26), S(16), C_BODY, x1 - x0 - S(44), 0);
        if (n > 1) text(c, &F_REG, l2, x0 + S(22), my1 + S(50), S(16), C_BODY, x1 - x0 - S(44), 0);
        text(c, &F_REG, "A: blow for 3 s, for when the real mic isn't picked up", gx0 + S(4), y1 + S(16), S(16), C_GREY, gx1 - gx0, 0);
    }
    /* footer */
    int fy = c->h - S(56);
    const char *bl = s == SC_MAIN ? "Resume" : "Back", *al = "Select", *mid = "";
    if (s == SC_SAVE) al = "Save"; else if (s == SC_LOAD) al = "Load";
    else if (s == SC_SET) al = rows[sel_set] == ROW_MIC || rows[sel_set] == ROW_CHEAT ? "Open" : "OK";
    else if (s == SC_CHEAT) al = H.cheat_count(H.ctx) ? (H.cheat_get(H.ctx, sel_cheat) ? "Turn off" : "Turn on") : "Back";
    else if (s == SC_MIC) al = "Blow for 3 s";
    if (s == SC_MAIN) mid = "D-pad or tap";
    /* the translator, where the menu's language shows (the file's translated-by: "简体中文 · Majirefy") */
    char credit[160];
    if (s == SC_SET && lang_active() && *lang_by()) {
        snprintf(credit, sizeof credit, "%s · %s", lang_name(), lang_by()); lang_keep(credit); mid = credit;
    }
    R_FOOT_L = pill(c, S(20), fy, 0, "B", bl, C_LIGHT, C_LIGHT, C_DARK);
    R_FOOT_R = pill(c, c->w - S(20), fy, 1, "A", al, C_CYAN, C_CYAN, C_DARK);
    R_UNDO = (rect){ 0, 0, 0, 0 };
    if (s == SC_LOAD && undo_ok) R_UNDO = pill(c, R_FOOT_R.x0 - S(12), fy, 1, "Y", "Undo last load", C_GREY, C_LIGHT, C_PILLBG);
    else if (*mid) text(c, &F_REG, mid, c->w / 2.0f, fy + S(11), S(15), C_GREY, 0, 1);
    if (scr == SC_OVERWRITE) {
        char t[64]; snprintf(t, sizeof t, "Replace slot %d?", sel_slot + 1);
        char w[48]; when_text(SL[sel_slot].mt, w, sizeof w);
        char b[160]; snprintf(b, sizeof b, "The save from %s will be replaced by this one.", w);
        dialog(c, t, b, "Cancel", "Replace", 0);
    } else if (scr == SC_QUIT)
        dialog(c, "Quit to the menu?", H.resume_on ? "Your spot is saved. Next time the game picks up right here."
                                                   : "Anything since your last save will be lost.", "Keep playing", "Quit", 1);
    else if (scr == SC_RESTART)
        dialog(c, "Restart the game?", "Back to the title screen. Anything since your last save will be lost.",
               "Keep playing", "Restart", 1);
}

static void show(int i) {
    uint32_t *px; int pitch, w, h;
    int k = H.canvas(H.ctx, i, &px, &pitch, &w, &h);
    if (k < 0) return;
    for (int y = 0; y < h; y++) memcpy((char *)px + (size_t)y * pitch, CV[i].px + (size_t)y * w, (size_t)w * 4);
    H.show(H.ctx, i, k);
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
    char m[64]; snprintf(m, sizeof m, "Saving to slot %d", n + 1);
    busy(m);
    if (H.save(H.ctx, n + 1) == 0) last_slot = n;
    else mlog("[menu] slot %d couldn't be saved\n", n + 1);
    play(SND_SELECT); close_menu();
}
static void do_load(int n) {
    busy(n < 0 ? "Undoing the load" : "Loading");
    int ok;
    if (n < 0) ok = H.load(H.ctx, BM_UNDO_SLOT) == 0;
    else {
        /* the game as it is now goes to the undo slot first: a wrong slot never costs progress */
        int kept = H.save(H.ctx, BM_UNDO_SLOT) == 0;
        ok = H.load(H.ctx, n + 1) == 0;
        if (ok) last_slot = n;
        undo_ok = ok && kept;
    }
    if (n < 0 && ok) undo_ok = 0;
    if (!ok) mlog("[menu] %s couldn't be loaded\n", n < 0 ? "the undo slot" : "the slot");
    play(SND_SELECT); close_menu();
}
static void back(void);
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
        case 3: scr = SC_SET; sel_set = 0; vol = volume_get(); if (vol < 0) vol = 50; bri = brightness_get();
                play(SND_SELECT); dirty_top = dirty_bot = 1; break;
        case 4: scr = SC_RESTART; play(SND_SELECT); dirty_bot = 1; break;
        case 5: scr = SC_QUIT; play(SND_SELECT); dirty_bot = 1; break;
        }
        break;
    case SC_SAVE: if (SL[sel_slot].used) { scr = SC_OVERWRITE; play(SND_SELECT); dirty_bot = 1; } else do_save(sel_slot); break;
    case SC_OVERWRITE: do_save(sel_slot); break;
    case SC_LOAD: if (SL[sel_slot].used) do_load(sel_slot); else play(SND_BACK); break;
    case SC_SET:
        if (rows[sel_set] == ROW_MIC) { scr = SC_MIC; play(SND_SELECT); dirty_bot = 1; }
        else if (rows[sel_set] == ROW_CHEAT) { scr = SC_CHEAT; sel_cheat = top_cheat = 0; play(SND_SELECT); dirty_bot = 1; }
        else play(SND_SELECT);
        break;
    case SC_CHEAT:
        if (!H.cheat_count(H.ctx)) { back(); break; }
        H.cheat_set(H.ctx, sel_cheat, !H.cheat_get(H.ctx, sel_cheat)); play(SND_SELECT); dirty_bot = 1;
        break;
    case SC_MIC:
        /* the game has to run to hear it: back to the game, with a pop-up for as long as it blows */
        play(SND_SELECT); if (H.blow) H.blow(H.ctx, 3000); close_menu();
        break;
    case SC_QUIT:
        play(SND_SELECT);
        if (H.resume_on) busy("Saving your spot");
        close_menu(); H.quit(H.ctx);
        break;
    case SC_RESTART:
        play(SND_SELECT); busy("Restarting"); H.restart(H.ctx); undo_ok = 0; close_menu();
        break;
    }
}
static void back(void) {
    play(SND_BACK);
    switch (scr) {
    case SC_MAIN: close_menu(); break;
    case SC_OVERWRITE: scr = SC_SAVE; dirty_bot = 1; break;
    case SC_QUIT: case SC_RESTART: scr = SC_MAIN; dirty_bot = 1; break;
    case SC_MIC: case SC_CHEAT: scr = SC_SET; dirty_bot = 1; break;
    default: scr = SC_MAIN; dirty_top = dirty_bot = 1; break;
    }
}
static void adjust(int d) {                     /* left/right on a Quick settings row */
    int id = rows[sel_set];
    if (id == ROW_VOL) { int v = vol + d * 5; v = v < 0 ? 0 : v > 100 ? 100 : v; if (v != vol) { vol = v; want(vol, -1); play(SND_MOVE); dirty_bot = 1; } }
    else if (id == ROW_BRI) { int v = bri + d * 5; v = v < 5 ? 5 : v > 100 ? 100 : v; if (v != bri) { bri = v; backlight(bri); bri_changed = 1; play(SND_MOVE); dirty_bot = 1; } }
    else if (id >= ROW_OPT) {                   /* an emulator option: the next value, round the list */
        int i = id - ROW_OPT, n = H.opt_values(H.ctx, i);
        if (n > 1) { int v = (H.opt_get(H.ctx, i) + d + n) % n; H.opt_set(H.ctx, i, v); play(SND_MOVE); dirty_bot = 1; }
    }
}
static void nav(int dx, int dy) {
    int moved = 0;
    if (scr == SC_MAIN) {
        int c = sel_main % 3, r = sel_main / 3;
        if (dx) { int n = c + dx; if (n >= 0 && n < 3) { c = n; moved = 1; } }
        if (dy) { int n = r + dy; if (n >= 0 && n < 2) { r = n; moved = 1; } }
        sel_main = r * 3 + c;
    } else if (scr == SC_SAVE || scr == SC_LOAD) {
        int c = sel_slot % 4, r = sel_slot / 4;
        if (dx) { int n = c + dx; if (n >= 0 && n < 4) { c = n; moved = 1; } else if (n == 4 && r == 0) { c = 0; r = 1; moved = 1; } else if (n < 0 && r == 1) { c = 3; r = 0; moved = 1; } }
        if (dy) { int n = r + dy; if (n >= 0 && n < 2) { r = n; moved = 1; } }
        sel_slot = r * 4 + c;
        if (moved) dirty_top = 1;
    } else if (scr == SC_SET) {
        if (dy) { int n = sel_set + dy; if (n >= 0 && n < nrows) { sel_set = n; moved = 1; } }
        if (dx) { adjust(dx); return; }
    } else if (scr == SC_CHEAT) {
        int n = H.cheat_count(H.ctx), m = sel_cheat + dy + dx * CHEAT_ROWS;
        if (m < 0) m = 0; if (m > n - 1) m = n - 1;
        if (n && m != sel_cheat) { sel_cheat = m; moved = 1; }
    }
    if (moved) { play(SND_MOVE); dirty_bot = 1; }
}
static void tap(int px, int py) {               /* a touch-down on the bottom panel, in panel pixels */
    if (scr == SC_OVERWRITE || scr == SC_QUIT || scr == SC_RESTART) {
        if (in_rect(R_DLG_A, px, py)) activate(); else if (in_rect(R_DLG_B, px, py)) back();
        return;
    }
    if (scr == SC_BUSY) return;
    if (in_rect(R_HEAD_B, px, py) || in_rect(R_FOOT_L, px, py)) { back(); return; }
    if (in_rect(R_FOOT_R, px, py)) { activate(); return; }
    if (scr == SC_LOAD && undo_ok && in_rect(R_UNDO, px, py)) { do_load(-1); return; }
    if (scr == SC_MAIN) { for (int i = 0; i < 6; i++) if (in_rect(R_TILE[i], px, py)) { sel_main = i; dirty_bot = 1; activate(); return; } }
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
                canvas *c = &CV[1];
                int v = (px - R_BAR[r].x0 - S(8)) * 100 / S(146); v = (v + 2) / 5 * 5; v = v < 0 ? 0 : v > 100 ? 100 : v;
                if (id == ROW_VOL) { vol = v; want(vol, -1); } else { bri = v < 5 ? 5 : v; backlight(bri); bri_changed = 1; }
                play(SND_MOVE); dirty_bot = 1;
            } else if (id == ROW_MIC) activate();
            else if (id == ROW_CHEAT) activate();
            else if (id >= ROW_OPT) { sel_set = r; adjust(1); }
            else play(SND_MOVE);
            return;
        }
    } else if (scr == SC_CHEAT) {
        for (int r = 0; r < CHEAT_ROWS; r++) if (in_rect(R_ROW[r], px, py)) { sel_cheat = top_cheat + r; activate(); return; }
    }
}

/* ---------- input: the host's events (the pad, the bottom touchscreen) ---------- */
static int held_dir, rep_dx, rep_dy; static long long rep_next;
static void dir_press(int dx, int dy) { nav(dx, dy); held_dir = 1; rep_dx = dx; rep_dy = dy; rep_next = now_ms() + 380; }
static void handle(const bmenu_ev *e) {
    if (e->kind == BM_EV_TOUCH) { if (scr != SC_BUSY) { boost(400); tap(e->x, e->y); } return; }
    int b = e->key;
    if (!e->down) { if (b == BM_UP || b == BM_DOWN || b == BM_LEFT || b == BM_RIGHT) held_dir = 0; return; }
    boost(400);
    if (scr == SC_BUSY) return;
    if (b == BM_MENU || b == BM_START) { play(SND_BACK); close_menu(); }
    else if (b == BM_A) activate();
    else if (b == BM_B) back();
    else if (b == BM_Y && scr == SC_LOAD && undo_ok) do_load(-1);
    else if (b == BM_UP) dir_press(0, -1); else if (b == BM_DOWN) dir_press(0, 1);
    else if (b == BM_LEFT) dir_press(-1, 0); else if (b == BM_RIGHT) dir_press(1, 0);
    else if (b == BM_L && scr == SC_SET) adjust(-1); else if (b == BM_R && scr == SC_SET) adjust(1);
    else if (b == BM_L && scr == SC_CHEAT) nav(-1, 0); else if (b == BM_R && scr == SC_CHEAT) nav(1, 0);
}

void bmenu_init(const bmenu_host *h) { H = *h; }
void bmenu_session_start(void) { session_t0 = now_ms(); }

int bmenu_run(int start) {
    if (!setup()) return 0;
    if (!gbase[0]) game_info();
    mlog("[menu] open\n");
    boost(800);
    open_ = 1; scr = SC_MAIN; sel_main = 0; held_dir = 0;
    bri_changed = 0; make_rows();
    scan_slots(0);
    load_recent();
    grab_background();
    dirty_top = dirty_bot = 1;
    play(SND_SELECT);
    if (start) { sel_main = start == 1 ? 1 : start == 2 ? 2 : start == 3 ? 3 : 5; activate(); }
    long long clock_t0 = now_ms();
    { bmenu_ev e; while (H.poll(H.ctx, &e)) {} }   /* the press that opened it */
    while (open_) {
        bmenu_ev e;
        while (open_ && H.poll(H.ctx, &e)) handle(&e);
        if (open_ && held_dir && now_ms() >= rep_next) { nav(rep_dx, rep_dy); rep_next = now_ms() + 110; }
        { static long long mt; if (scr == SC_MIC && now_ms() - mt >= 33) { mt = now_ms(); dirty_bot = 1; } }   /* the meter moves */
        { static long long vt; if (scr == SC_SET && now_ms() - vt >= 500) { vt = now_ms(); want_ex(-1, -1, 1); }   /* the volume keys' */
          int r = vol_read; if (r >= 0) { vol_read = -1; if (scr == SC_SET && r != vol && now_ms() - vol_set_at > 1500) { vol = r; dirty_bot = 1; } } }
        if (now_ms() - clock_t0 >= 15000) { clock_t0 = now_ms(); if (scr == SC_MAIN || scr == SC_SET || scr == SC_QUIT) dirty_top = 1; }   /* the clock */
        /* RetroAchievements finishes its login and the game's lookup 10-15 s after a start: a menu opened before that
         * had no achievements on its top screen until it was opened again. Looked at twice a second while open. */
        { static long long rt; if (now_ms() - rt >= 500) { rt = now_ms(); unsigned u = 0, t = 0; int has = ra_progress(&u, &t) && t;
            if (has != ra_has || (has && u != ra_un)) { load_recent(); dirty_top = 1; } } }
        if (open_) redraw();
        usleep(8000);
    }
    if (bri_changed) want(-1, 1);
    mlog("[menu] closed\n");
    return 1;
}

/* ---------- pop-ups over the game: ROCKNIXDS's (SuperDrastic's ui.c), in BananaOS's colours ----------
 * The frontend calls bmenu_overlay with the top panel's finished picture (over the shader's output when there is one), on
 * the presenter's thread. As in ROCKNIXDS a
 * strip 72 rows tall at 640 wide runs along the top of the top screen and slides in and out: an achievement's card
 * across it (its badge in a light frame, or a tile in the accent colour with a star or an "i"; the first line in the
 * accent colour, the second in light text), or the volume / brightness pill in its middle (a speaker or a sun, the
 * menu's 10-segment bar, the percent) for 1.5 s after a key, over a pop-up. bananad writes the keys' level to
 * /run/banana/osd. A picture is drawn once, over black and over white (its colour and coverage), then only blended
 * each frame. */
#define OV_SLIDE_MS 160
static const char *const PX_STAR[9] = { "....#....", "...###...", "#########", ".#######.", "..#####..", "..##.##..", ".##...##.", "##.....##", "........." };
static const char *const PX_INFO[9] = { "...###...", "..#####..", "..##.##..", "....##...", "...##....", "...##....", ".........", "...##....", "...##...." };
static const char *const PX_MUTE[12] = { "............", ".....#......", "....##......", "...###.#...#", "####.#..#.#.", "#..#.#...#..",
    "#..#.#...#..", "####.#..#.#.", "...###.#...#", "....##......", ".....#......", "............" };
typedef struct { uint32_t *rgb; uint8_t *a; int w, h, x, y; } ovpic;   /* a picture with coverage, where it goes */
static ovpic OP, OV;                                   /* the pop-up's card, the volume / brightness pill */
static int op_on, ov_on, ov_kind, ov_pct = -1, ov_built_pct = -2, ov_built_kind = -1, ov_for_w;
static long long op_from, op_until, ov_from, ov_until, osd_mt, osd_check;
static char op_l1[128], op_l2[160], op_badge[600]; static uint32_t op_accent;
static ovpic OF;                                       /* the FPS counter */
static int of_val = -1, of_ff, of_built = -2, of_built_ff = -1, of_for_w, of_game = -1, of_built_game = -2;

/* draw f twice (over black and over white) into a w x h picture with coverage */
static int ov_make(ovpic *o, int w, int h, int x, int y, float sc, void (*f)(canvas *)) {
    uint32_t *black = malloc((size_t)w * h * 4), *white = malloc((size_t)w * h * 4);
    free(o->rgb); free(o->a);
    o->rgb = malloc((size_t)w * h * 4); o->a = malloc((size_t)w * h);
    if (!black || !white || !o->rgb || !o->a) { free(black); free(white); o->w = 0; return 0; }
    for (int i = 0; i < w * h; i++) { black[i] = 0; white[i] = 0xffffff; }
    canvas cb = { black, w, h, sc }, cw = { white, w, h, sc };
    f(&cb); f(&cw);
    for (int i = 0; i < w * h; i++) {
        uint32_t b = black[i], k = white[i];
        int a = 255 - (int)(((k >> 8) & 255) - ((b >> 8) & 255));
        if (a < 0) a = 0; if (a > 255) a = 255;
        o->a[i] = (uint8_t)a;
        uint32_t r = 0, g = 0, bl = 0;
        if (a) { r = ((b >> 16) & 255) * 255 / a; g = ((b >> 8) & 255) * 255 / a; bl = (b & 255) * 255 / a; }
        o->rgb[i] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
    }
    free(black); free(white);
    o->w = w; o->h = h; o->x = x; o->y = y;
    return 1;
}
/* ROCKNIXDS's card: a light border, the panel inside with the theme's dots */
static void ov_card(canvas *c, int x0, int y0, int x1, int y1) {
    int bw = S(2) < 2 ? 2 : S(2);
    dotbox(c, x0, y0, x1, y1, (float)S(10), bw, C_LIGHT, C_PANEL, C_DOT);
}
/* text whose baseline is at y (ui.c's draw_text) */
static void ov_text(canvas *c, font *F, const char *t, float x, float base, float px, uint32_t rgb, float maxw) {
    text(c, F, t, x, base - F->asc * px, px, rgb, maxw, 0);
}
static void ov_text_raw(canvas *c, font *F, const char *t, float x, float base, float px, uint32_t rgb, float maxw) {
    text_raw(c, F, t, x, base - F->asc * px, px, rgb, maxw, 0);
}
static void ov_draw_popup(canvas *c) {
    int x0 = S(8), y0 = S(4), x1 = c->w - S(8), y1 = c->h - S(4), h = y1 - y0, bs = h - S(16), tx;
    ov_card(c, x0, y0, x1, y1);
    int iw = 0, ih = 0, ch;
    uint8_t *img = *op_badge ? stbi_load(op_badge, &iw, &ih, &ch, 4) : 0;
    if (img) {   /* the badge in a light frame */
        int bw = S(2) < 2 ? 2 : S(2);
        rrect(c, x0 + S(8), y0 + S(8), x0 + S(8) + bs, y0 + S(8) + bs, (float)S(6), C_LIGHT, 1.0f);
        draw_img(c, img, iw, ih, x0 + S(8) + bw, y0 + S(8) + bw, bs - 2 * bw, (float)S(4));
        stbi_image_free(img);
    } else {     /* no badge: a tile in the accent colour, a star for an unlock, else an "i" */
        rrect(c, x0 + S(8), y0 + S(8), x0 + S(8) + bs, y0 + S(8) + bs, (float)S(6), op_accent, 1.0f);
        int k = bs / 14 > 0 ? bs / 14 : 1;
        bitmap(c, op_accent == 0xffd84a ? PX_STAR : PX_INFO, 9, x0 + S(8) + (bs - 9 * k) / 2, y0 + S(8) + (bs - 9 * k) / 2, k, 0x12181f);
    }
    tx = x0 + S(8) + bs + S(12);
    float maxw = (float)(x1 - S(14) - tx);
    ov_text(c, &F_MED, op_l1, (float)tx, (float)(y0 + S(26)), (float)S(17), op_accent, maxw);
    ov_text(c, &F_REG, op_l2, (float)tx, (float)(y0 + S(52)), (float)S(20), C_LIGHT, maxw);
}
static void ov_draw_level(canvas *c) {   /* the pill's own picture: c is exactly the pill */
    int x0 = 0, y0 = 0, x1 = c->w, y1 = c->h, cy = (y0 + y1) / 2;
    ov_card(c, x0, y0, x1, y1);
    int k = S(2) < 2 ? 2 : S(2);
    const char *const *ic = ov_kind ? IC_SUN : ov_pct <= 0 ? PX_MUTE : IC_VOL;
    bitmap(c, ic, 12, x0 + S(14), cy - 6 * k, k, !ov_kind && ov_pct <= 0 ? C_REDL : C_LIGHT);
    char t[8]; snprintf(t, sizeof t, "%d%%", ov_pct);
    float tw = text_w_raw(&F_MED, t, (float)S(19), 8);
    ov_text_raw(c, &F_MED, t, (float)(x1 - S(14)) - tw, (float)(y0 + S(28)), (float)S(19), C_CYAN, tw + 2);
    int bx0 = x0 + S(14) + 12 * k + S(12), bx1 = x1 - S(14) - (int)ceilf(text_w_raw(&F_MED, "100%", (float)S(19), 8)) - S(10);
    float seg = (bx1 - bx0) / 10.0f; int gap = S(3) < 2 ? 2 : S(3);
    for (int i = 0; i < 10; i++) {
        int sx = bx0 + (int)(i * seg), ex = bx0 + (int)((i + 1) * seg) - gap;
        fill(c, sx, cy - S(8), ex, cy + S(8), i < (ov_pct + 5) / 10 ? C_CYAN : C_LINE, 1.0f);
    }
}
static void ov_draw_fps(canvas *c) {   /* the counter's own picture: c is exactly its card */
    ov_card(c, 0, 0, c->w, c->h);
    /* the game's own rate when it draws 3D and runs slower than the emulator (a heavy scene: the DS's frames come
     * every other one); the emulator's, which is also the 2D games' */
    char t[40];
    if (of_game >= 0 && of_val - of_game >= 15)
        snprintf(t, sizeof t, "%d.%d FPS  emu %d", of_game / 10, of_game % 10, (of_val + 5) / 10);
    else snprintf(t, sizeof t, "%d.%d FPS", of_val / 10, of_val % 10);
    ov_text_raw(c, &F_MED, t, (float)S(12), (float)S(23), (float)S(17), C_CYAN, (float)(c->w - S(20)));
    if (of_ff) {
        char f[16]; if (of_ff == 99) snprintf(f, sizeof f, ">> Max"); else snprintf(f, sizeof f, ">> %dx", of_ff);
        float fw = text_w_raw(&F_MED, f, (float)S(17), 16);
        ov_text_raw(c, &F_MED, f, (float)(c->w - S(12)) - fw, (float)S(23), (float)S(17), C_LIGHT, fw + 2);
    }
}
void bmenu_fps(int fps_x10, int ff) { of_val = fps_x10; of_ff = ff; }
void bmenu_fps_game(int fps_x10) { of_game = fps_x10; }
static pthread_mutex_t own_mu = PTHREAD_MUTEX_INITIALIZER;   /* the frontend's pop-up, waiting for its turn */
static char own_l1[128], own_l2[160]; static int own_ms;
void bmenu_popup(const char *line1, const char *line2, int ms) {
    pthread_mutex_lock(&own_mu);
    snprintf(own_l1, sizeof own_l1, "%s", line1); snprintf(own_l2, sizeof own_l2, "%s", line2); own_ms = ms;
    pthread_mutex_unlock(&own_mu);
}
static int own_take(void) {   /* the frontend's pop-up into the pop-up card's lines, if one is waiting */
    int ms = 0;
    pthread_mutex_lock(&own_mu);
    if (own_ms > 0) {
        snprintf(op_l1, sizeof op_l1, "%s", own_l1); snprintf(op_l2, sizeof op_l2, "%s", own_l2);
        op_badge[0] = 0; op_accent = C_CYAN; ms = own_ms; own_ms = 0;
    }
    pthread_mutex_unlock(&own_mu);
    return ms;
}
/* the keys' level, from bananad (a file written in the last 2 s, newer than the last one seen) */
static void ov_osd(long long t) {
    if (t < osd_check) return;
    osd_check = t + 100;
    struct stat st;
    if (stat("/run/banana/osd", &st)) return;
    long long mt = (long long)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
    if (mt == osd_mt) return;
    osd_mt = mt;
    if (time(0) - st.st_mtim.tv_sec > 2) return;
    FILE *f = fopen("/run/banana/osd", "r");
    if (!f) return;
    char what[32]; int pct;
    if (fscanf(f, "%31s %d", what, &pct) == 2 && pct >= 0) {
        ov_kind = !strcmp(what, "brightness");
        ov_pct = pct > 100 ? 100 : pct;
        if (!ov_on) ov_from = t;
        ov_on = 1; ov_until = t + 1500;
    }
    fclose(f);
}
/* blended over the frame, slid up by off rows */
static void ov_blit(const ovpic *o, uint32_t *px, int pitch, int fh, int off) {
    for (int y = 0; y < o->h; y++) {
        int dy = o->y + y - off;
        if (dy < 0 || dy >= fh) continue;
        uint32_t *d = px + (size_t)dy * pitch + o->x;
        const uint32_t *s = o->rgb + (size_t)y * o->w;
        const uint8_t *a = o->a + (size_t)y * o->w;
        for (int x = 0; x < o->w; x++) {
            int k = a[x];
            if (!k) continue;
            if (k == 255) { d[x] = s[x]; continue; }
            uint32_t q = d[x], n = s[x];
            uint32_t r = (((n >> 16) & 255) * k + ((q >> 16) & 255) * (255 - k)) / 255;
            uint32_t g = (((n >> 8) & 255) * k + ((q >> 8) & 255) * (255 - k)) / 255;
            uint32_t b = ((n & 255) * k + (q & 255) * (255 - k)) / 255;
            d[x] = r << 16 | g << 8 | b;
        }
    }
}
/* how far a strip is slid up (rows): in over OV_SLIDE_MS from `from`, out over the last OV_SLIDE_MS before `until` */
static int ov_slide(long long t, long long from, long long until, int rows) {
    long long in = t - from, out = until - t, m = in < out ? in : out;
    if (m >= OV_SLIDE_MS) return 0;
    if (m < 0) m = 0;
    float e = (float)m / OV_SLIDE_MS; e = 1 - (1 - e) * (1 - e);
    return (int)((1 - e) * rows + 0.5f);
}
void bmenu_overlay(uint32_t *px, int pitch, int w, int h) {
    long long t = now_ms();
    float sc = w / 640.0f;
    int strip = (int)lroundf(72 * sc);
    if (strip > h) strip = h;
    if (!fonts_ready()) return;
    if (of_val >= 0) {   /* the FPS counter, under the pop-ups and the pill */
        if (of_val != of_built || of_ff != of_built_ff || w != of_for_w || of_game != of_built_game) {
            canvas probe = { 0, w, strip, sc }, *c = &probe;
            int wide = of_game >= 0 && of_val - of_game >= 15;   /* "30.0 FPS  emu 60" */
            if (!ov_make(&OF, of_ff ? S(210) + (wide ? S(70) : 0) : wide ? S(188) : S(118), S(34), S(6), S(6), sc, ov_draw_fps)) of_val = -1;
            of_built = of_val; of_built_ff = of_ff; of_for_w = w; of_built_game = of_game;
        }
        if (of_val >= 0) ov_blit(&OF, px, pitch, h, 0);
    }
    if (w != ov_for_w) { ov_for_w = w; ov_built_pct = -2; if (op_on && !ov_make(&OP, w, strip, 0, 0, sc, ov_draw_popup)) op_on = 0; }
    ov_osd(t);
    if (ov_on && t >= ov_until) ov_on = 0;
    if (op_on && t >= op_until) op_on = 0;
    if (!op_on) {
        int ms = own_take();
        if (ms > 0 && ov_make(&OP, w, strip, 0, 0, sc, ov_draw_popup)) { op_on = 1; op_from = t; op_until = t + ms; }
    }
    if (!op_on) {
        int ms;
        if (ra_popup_take(op_l1, sizeof op_l1, op_l2, sizeof op_l2, op_badge, sizeof op_badge, &op_accent, &ms) &&
            ov_make(&OP, w, strip, 0, 0, sc, ov_draw_popup)) { op_on = 1; op_from = t; op_until = t + ms; }
    }
    if (ov_on) {   /* the pill over everything, as in ROCKNIXDS */
        if (ov_pct != ov_built_pct || ov_kind != ov_built_kind) {
            canvas probe = { 0, w, strip, sc }, *c = &probe;
            int pw = S(310), ph = S(42);
            if (!ov_make(&OV, pw, ph, (w - pw) / 2, S(4), sc, ov_draw_level)) { ov_on = 0; return; }
            ov_built_pct = ov_pct; ov_built_kind = ov_kind;
        }
        ov_blit(&OV, px, pitch, h, ov_slide(t, ov_from, ov_until, OV.y + OV.h));
        return;
    }
    if (op_on) ov_blit(&OP, px, pitch, h, ov_slide(t, op_from, op_until, strip));
}
