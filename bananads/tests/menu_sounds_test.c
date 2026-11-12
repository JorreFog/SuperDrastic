/* The in-game menu's clicks follow BananaOS's "Menu sounds" setting (issue #229: settings.conf "sounds"). Runs
 * src/menu.c with a made-up bmenu_host: opens the menu, presses DOWN then START (closes it) and counts the sounds it
 * played: some with "sounds = on" (or no line), none with "sounds = off". The setting is read again at every opening.
 * Needs a TTF font ($BANANADS_MENU_FONT, else DejaVuSans in one of the usual places); exits 77 (skipped) without one. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "menu.h"
#include "ra.h"

/* ra.c, as stubs */
int ra_progress(unsigned *unlocked, unsigned *total) { *unlocked = *total = 0; return 0; }
int ra_recent(ra_recent_t *out, int max) { (void)out; (void)max; return 0; }
int ra_popup_take(char *l1, int n1, char *l2, int n2, char *badge, int nb, uint32_t *accent, int *ms) {
    (void)l1; (void)n1; (void)l2; (void)n2; (void)badge; (void)nb; (void)accent; (void)ms; return 0;
}

static int plays, loads, ev_i;
static uint32_t panel[2][640 * 480];
static int h_canvas(void *c, int i, uint32_t **px, int *pitch, int *w, int *h) {
    (void)c; *px = panel[i]; *pitch = 640 * 4; *w = 640; *h = 480; return 0;
}
static void h_show(void *c, int i, int k) { (void)c; (void)i; (void)k; }
static int h_grab_top(void *c, uint32_t *dst, int pitch, int w, int h) { (void)c; (void)dst; (void)pitch; (void)w; (void)h; return 0; }
/* nothing at first (the menu drains the press that opened it), then DOWN pressed and let go, then START */
static int h_poll(void *c, bmenu_ev *e) {
    (void)c;
    static const int seq[][2] = { { BM_DOWN, 1 }, { BM_DOWN, 0 }, { BM_START, 1 }, { BM_START, 0 } };
    if (ev_i++ == 0) return 0;
    int k = ev_i - 2;
    if (k >= (int)(sizeof seq / sizeof seq[0])) return 0;
    memset(e, 0, sizeof *e); e->kind = BM_EV_KEY; e->key = seq[k][0]; e->down = seq[k][1];
    return 1;
}
static int h_save(void *c, int slot) { (void)c; (void)slot; return 1; }
static void h_state_file(void *c, int slot, char *out, int n) { (void)c; snprintf(out, (size_t)n, "slot%d.mst", slot); }
static void h_quit(void *c) { (void)c; }
static void h_sound_load(void *c, int id, const int16_t *pcm, int frames, int rate, int ch) {
    (void)c; (void)id; (void)pcm; (void)frames; (void)rate; (void)ch; loads++;
}
static void h_sound_play(void *c, int id) { (void)c; (void)id; plays++; }
static void h_log(void *c, const char *line) { (void)c; if (getenv("TEST_VERBOSE")) fputs(line, stderr); }

static void die(const char *m) { perror(m); exit(2); }
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) die(path);
    fputs(text, f); fclose(f);
}
static void wav(const char *path) {   /* a short 16-bit mono WAV the menu's loader takes */
    FILE *f = fopen(path, "wb"); if (!f) die(path);
    static const uint8_t h[44] = { 'R','I','F','F', 36 + 8,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x80,0xbb,0,0, 0,0x77,1,0, 2,0, 16,0, 'd','a','t','a', 8,0,0,0 };
    static const int16_t s[4] = { 0, 1000, -1000, 0 };
    fwrite(h, 1, sizeof h, f); fwrite(s, 2, 4, f); fclose(f);
}

static int fails;
static void open_menu(const char *case_, int want_sounds) {
    plays = 0; ev_i = 0;
    if (!bmenu_run(0)) { printf("FAIL %s: the menu didn't open\n", case_); fails++; return; }
    int ok = want_sounds ? plays > 0 : plays == 0;
    printf("%s %s: %d menu sounds played (want %s)\n", ok ? "PASS" : "FAIL", case_, plays, want_sounds ? "some" : "none");
    if (!ok) fails++;
}

int main(void) {
    if (!getenv("BANANADS_MENU_FONT")) {
        const char *const fonts[] = { "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
        for (int i = 0; i < 3; i++) if (!access(fonts[i], R_OK)) { setenv("BANANADS_MENU_FONT", fonts[i], 1); break; }
        if (!getenv("BANANADS_MENU_FONT")) { puts("SKIP: no TTF font (set BANANADS_MENU_FONT)"); return 77; }
    }
    char dir[] = "/tmp/bananads-menu-sounds-XXXXXX";
    if (!mkdtemp(dir)) die("mkdtemp");
    if (chdir(dir)) die("chdir");
    if (mkdir("sounds", 0755)) die("mkdir");
    wav("sounds/scroll.wav"); wav("sounds/select.wav"); wav("sounds/back.wav");
    char p[300];
    snprintf(p, sizeof p, "%s/sounds", dir); setenv("BANANADS_MENU_SOUNDS", p, 1);
    snprintf(p, sizeof p, "%s/settings.conf", dir); setenv("BANANA_SETTINGS", p, 1);
    setenv("BANANA_LANGUAGE", "en", 1);

    bmenu_host h; memset(&h, 0, sizeof h);
    h.rom = "test.nds"; h.canvas = h_canvas; h.show = h_show; h.grab_top = h_grab_top; h.poll = h_poll;
    h.save = h_save; h.load = h_save; h.state_file = h_state_file; h.quit = h_quit;
    h.sound_load = h_sound_load; h.sound_play = h_sound_play; h.log = h_log;
    bmenu_init(&h);

    put(p, "theme = dark\nsounds = on\n");
    open_menu("sounds = on", 1);
    if (!loads) { printf("FAIL the menu loaded no sounds (setup: %s/sounds)\n", dir); return 1; }
    put(p, "theme = dark\nsounds = off\n");
    open_menu("sounds = off", 0);
    put(p, "theme = dark\nsounds=off\n");
    open_menu("sounds=off (no spaces)", 0);
    put(p, "theme = dark\n");
    open_menu("no sounds line (the default: on)", 1);
    put(p, "sounds = off\nsoundsx = on\n");
    open_menu("sounds = off beside another key starting with sounds", 0);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
