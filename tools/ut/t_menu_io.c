/* The in-game menu's two ways of losing the player's place:
 * - a slot picture read with fopen, while a resume load redirects every slot file onto the resume state
 * - SDL_QUIT (SIGTERM) taken by the menu's own poll and never given back to DraStic
 *
 * gcc -O2 -Wall -Wextra -o /tmp/t_menu_io tools/ut/t_menu_io.c src/resume.c -ldl -lpthread && /tmp/t_menu_io
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void dsflip_log(const char *fmt, ...) { (void)fmt; }
void cpugov_boost(int ms) { (void)ms; }
void dsflip_toast(const char *a, const char *b, unsigned c, int d) { (void)a; (void)b; (void)c; (void)d; }
void audio_mute(int on) { (void)on; }
volatile int dsflip_hold;

void resume_start(void);
void resume_frame(void);
FILE *resume_fopen_direct(const char *p, const char *m);
int resume_copy_sdl_quit(const void *ev, void *dst, int n);

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    assert(fclose(f) == 0);
}

static void read_all(FILE *f, char *out, size_t n) {
    assert(f);
    size_t k = fread(out, 1, n - 1, f);
    out[k] = 0;
    assert(fclose(f) == 0);
}

static int contains(const char *path, const char *needle) {
    FILE *f = resume_fopen_direct(path, "rb");
    if (!f) return 0;
    char buf[256 * 1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != 0;
}

static void find_menu(char *out, size_t n) {
    char cwd[480];
    assert(getcwd(cwd, sizeof cwd));
    for (int up = 0; up < 5; up++) {
        size_t L = strlen(cwd);
        assert(L + 12 < n);
        memcpy(out, cwd, L);
        memcpy(out + L, "/src/menu.c", 12);
        if (access(out, R_OK) == 0) return;
        char *s = strrchr(cwd, '/');
        if (!s || s == cwd) break;
        *s = 0;
    }
    assert(!"src/menu.c not found from the working directory");
}

int main(void) {
    char menu[512];
    find_menu(menu, sizeof menu);
    assert(contains(menu, "resume_fopen_direct(path, \"rb\")"));
    assert(contains(menu, "resume_copy_sdl_quit"));
    assert(contains(menu, "SDL_PushEvent"));

    unsigned char ev[64];
    memset(ev, 0, sizeof ev);
    uint32_t joy = 0x603;
    memcpy(ev, &joy, 4);
    ev[12] = 4;
    unsigned char dst[64];
    assert(resume_copy_sdl_quit(ev, dst, (int)sizeof dst) == 0);
    uint32_t quit = 0x100;
    memcpy(ev, &quit, 4);
    ev[12] = 9;
    memset(dst, 0xab, sizeof dst);
    assert(resume_copy_sdl_quit(ev, dst, (int)sizeof dst) == 1);
    uint32_t got; memcpy(&got, dst, 4);
    assert(got == 0x100 && dst[12] == 9);
    assert(resume_copy_sdl_quit(ev, dst, 55) == 0);
    assert(resume_copy_sdl_quit(0, dst, 64) == 0);

    char tmpl[] = "/tmp/dsflip-slot-XXXXXX";
    char *d = mkdtemp(tmpl);
    assert(d);
    char kept[64];
    snprintf(kept, sizeof kept, "%s", d);
    assert(chdir(d) == 0);
    assert(mkdir("config", 0755) == 0);
    assert(mkdir("savestates", 0755) == 0);
    write_file("config/drastic.cfg",
               "controls_a[CONTROL_INDEX_LOAD_STATE] = 1024\n"
               "controls_a[CONTROL_INDEX_SAVE_STATE] = 1025\n");
    write_file("savestates/Game_1.dss", "SLOT-ONE");
    write_file("resume.dss", "RESUME-STATE");

    char resume[512], slot[512];
    snprintf(resume, sizeof resume, "%s/resume.dss", kept);
    snprintf(slot, sizeof slot, "%s/savestates/Game_1.dss", kept);
    setenv("DSFLIP_RESUME_FILE", resume, 1);
    setenv("DSFLIP_RESUME_LOAD", "1", 1);
    setenv("DSFLIP_EXIT_COMBO", "0", 1);
    resume_start();
    for (int i = 0; i < 120; i++) resume_frame();

    char text[64];
    read_all(fopen(slot, "rb"), text, sizeof text);
    assert(strcmp(text, "RESUME-STATE") == 0);          /* the window where every slot read is the resume state */
    read_all(resume_fopen_direct(slot, "rb"), text, sizeof text);
    assert(strcmp(text, "SLOT-ONE") == 0);              /* the picture read has to see the slot itself */

    for (int i = 0; i < 160; i++) resume_frame();       /* the load window ends and the resume file is removed */
    read_all(fopen(slot, "rb"), text, sizeof text);
    assert(strcmp(text, "SLOT-ONE") == 0);
    return 0;
}
