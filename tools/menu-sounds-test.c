/* Host test (issue #229): the in-game menu's clicks follow BananaOS's "Menu sounds" setting (settings.conf
 * "sounds"). Links src/menu.c and src/lang.c with the rest of libsuperdrastic as stubs, opens the menu as HOME does,
 * presses DOWN then START (closes it) and counts the sounds the menu played: some with "sounds = on" (or no line),
 * none with "sounds = off". The setting is read again at every opening, as it would be after a change in Settings.
 *   cc -O1 -Isrc -Isrc/third_party/stb -o /tmp/menu-sounds-test tools/menu-sounds-test.c src/menu.c src/lang.c \
 *      -ldl -lpthread -lm && /tmp/menu-sounds-test
 * Needs a TTF font: $DSFLIP_MENU_FONT, else DejaVuSans in one of the usual places. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void menu_home(void);

static int plays, loads;

/* dsflip.c, lid.c, cpugov.c, audio.c, ra.c, resume.c: what menu.c calls, as stubs */
static uint32_t panel[2][640 * 480];
int dsflip_menu_canvas(int i, uint32_t **px, int *pitch, int *w, int *h) { *px = panel[i]; *pitch = 640 * 4; *w = 640; *h = 480; return 0; }
void dsflip_menu_show(int i, int k) { (void)i; (void)k; }
int dsflip_menu_grab_top(uint32_t *dst, int pitch, int w, int h) { (void)dst; (void)pitch; (void)w; (void)h; return 0; }
int dsflip_battery(int *charging) { *charging = 0; return 80; }
int dsflip_drastic_menu(void) { return 0; }
volatile int dsflip_hold, dsflip_swap;
void dsflip_mic_key(int down) { (void)down; }
int lid_home_pressed(void) { return 0; }
int lid_home_on(void) { return 1; }
long long lid_wall_ms(void) { return 0; }
int lid_vol_since(long long since) { (void)since; return 0; }
void dsflip_toast(const char *l1, const char *l2, uint32_t accent, int ms) { (void)l1; (void)l2; (void)accent; (void)ms; }
void dsflip_log(const char *fmt, ...) { if (getenv("TEST_VERBOSE")) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); } }
void cpugov_boost(int ms) { (void)ms; }
void cpugov_menu(int open) { (void)open; }
void SDL_PauseAudio(int on) { (void)on; }
int SDL_PushEvent(void *event) { (void)event; return 1; }
void audio_mute(int on) { (void)on; }
int audio_ui_load(int id, const int16_t *pcm, int frames, int rate, int ch) { (void)id; (void)pcm; (void)frames; (void)rate; (void)ch; loads++; return 0; }
void audio_ui_play(int id) { (void)id; plays++; }
int audio_mic_meter(float *level, float *floor_, float *thresh) { (void)level; (void)floor_; (void)thresh; return -1; }
void audio_mic_quiet(int on) { (void)on; }
int ra_progress(unsigned *unlocked, unsigned *total) { *unlocked = *total = 0; return 0; }
typedef struct { char title[96], desc[200], badge[600]; unsigned points; long long when; } ra_recent_t;
int ra_recent(ra_recent_t *out, int max) { (void)out; (void)max; return 0; }
int resume_control_button(const char *name) { (void)name; return -1; }   /* -1: the RG DS pad's own numbers */
int resume_menu_save(const char *path, const char *msg, const char *msg2) { (void)path; (void)msg; (void)msg2; return 1; }
int resume_menu_load(const char *path, const char *backup, const char *msg, const char *msg2) { (void)path; (void)backup; (void)msg; (void)msg2; return 1; }
int resume_menu_busy(void) { return 0; }
void resume_press(int b) { (void)b; }
int resume_quit(void) { return 0; }
void resume_quit_nosave(void) {}
int resume_on(void) { return 0; }
int resume_quit_pending(void) { return 0; }
int resume_exit_combo(int *start, int *select, int *menu, int *menu2) { *start = *select = *menu = *menu2 = -1; return 0; }

/* the menu's own SDL_PollEvent (it finds DraStic's with dlsym): DOWN pressed and let go, then START (closes it) */
static int ev_i;
static int poll_events(void *ev) {
    static const struct { uint32_t type; uint8_t button; } seq[] = { { 0x603, 14 }, { 0x604, 14 }, { 0x603, 9 }, { 0x604, 9 } };
    if (ev_i >= (int)(sizeof seq / sizeof seq[0])) return 0;
    uint8_t *e = ev; memset(e, 0, 56); memcpy(e, &seq[ev_i].type, 4); e[12] = seq[ev_i].button; ev_i++;
    return 1;
}
void *dlsym(void *handle, const char *name) {   /* (everything else, e.g. a sanitizer's lookups: the real dlsym) */
    if (!strcmp(name, "SDL_PollEvent")) return (void *)poll_events;
    static void *(*real)(void *, const char *);
    const char *const vers[] = { "GLIBC_2.34", "GLIBC_2.17", "GLIBC_2.2.5" };
    for (int i = 0; i < 3 && !real; i++) real = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", vers[i]);
    return real ? real(handle, name) : 0;
}

static void die(const char *m) { perror(m); exit(2); }
static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w"); if (!f) die(path);
    fputs(text, f); fclose(f);
}
/* a short 16-bit mono WAV the menu's loader takes */
static void wav(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) die(path);
    static const uint8_t h[44] = { 'R','I','F','F', 36 + 8,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x80,0xbb,0,0, 0,0x77,1,0, 2,0, 16,0, 'd','a','t','a', 8,0,0,0 };
    static const int16_t s[4] = { 0, 1000, -1000, 0 };
    fwrite(h, 1, sizeof h, f); fwrite(s, 2, 4, f); fclose(f);
}

static int fails;
static void open_menu(const char *case_, int want_sounds) {
    plays = 0; ev_i = 0;
    menu_home();
    int ok = want_sounds ? plays > 0 : plays == 0;
    printf("%s %s: %d menu sounds played (want %s)\n", ok ? "PASS" : "FAIL", case_, plays, want_sounds ? "some" : "none");
    if (!ok) fails++;
}

int main(void) {
    char dir[] = "/tmp/menu-sounds-XXXXXX";
    if (!mkdtemp(dir)) die("mkdtemp");
    if (chdir(dir)) die("chdir");
    if (mkdir("sounds", 0755) || mkdir("config", 0755)) die("mkdir");
    wav("sounds/scroll.wav"); wav("sounds/select.wav"); wav("sounds/back.wav");
    char p[300];
    snprintf(p, sizeof p, "%s/sounds", dir); setenv("DSFLIP_MENU_SOUNDS", p, 1);
    snprintf(p, sizeof p, "%s/settings.conf", dir); setenv("BANANA_SETTINGS", p, 1);
    if (!getenv("DSFLIP_MENU_FONT")) {
        const char *const fonts[] = { "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf" };
        for (int i = 0; i < 3; i++) if (!access(fonts[i], R_OK)) { setenv("DSFLIP_MENU_FONT", fonts[i], 1); break; }
    }
    setenv("BANANA_LANGUAGE", "en", 1);

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
