/* Host test: a quit-save must not destroy an existing resume file before the new state replaces it.
 * Links src/resume.c (its libc hooks and all) and drives the public entry points.
 * A save thread SIGKILLs this process after 5 s, so the checks finish with _exit. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void resume_start(void);
void resume_frame(void);
void dsflip_log(const char *fmt, ...) { (void)fmt; }
void dsflip_toast(const char *l1, const char *l2, unsigned accent, int ms) {
    (void)l1; (void)l2; (void)accent; (void)ms;
}

static void die(const char *m) { perror(m); _exit(2); }
static int read_file(const char *p, char *buf, int n) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    int r = (int)read(fd, buf, n);
    close(fd);
    return r;
}

int main(void) {
    char dir[] = "/tmp/resume-keep-XXXXXX";
    if (!mkdtemp(dir)) die("mkdtemp");
    if (chdir(dir)) die("chdir");
    if (mkdir("config", 0755)) die("mkdir");
    FILE *cfg = fopen("config/drastic.cfg", "w");
    if (!cfg) die("cfg");
    if (fputs("controls_a[CONTROL_INDEX_SAVE_STATE] = 1030\n"
              "controls_a[CONTROL_INDEX_LOAD_STATE] = 1031\n", cfg) < 0) die("cfg write");
    if (fclose(cfg)) die("cfg close");

    char path[512];
    snprintf(path, sizeof path, "%s/resume.dss", dir);
    FILE *old = fopen(path, "w");
    if (!old) die("resume");
    if (fputs("KEEP", old) < 0) die("resume write");
    if (fclose(old)) die("resume close");

    setenv("DSFLIP_RESUME_FILE", path, 1);
    unsetenv("DSFLIP_RESUME_LOAD");
    resume_start();
    raise(SIGUSR1);
    resume_frame();   /* starts the save; must not unlink the previous state */

    char got[8] = { 0 };
    int n = read_file(path, got, 4);
    if (n != 4 || memcmp(got, "KEEP", 4)) {
        fprintf(stderr, "previous resume state was destroyed at the start of a save (read %d)\n", n);
        _exit(1);
    }

    /* DraStic finishes: rename of the temp file onto a slot, which the hook sends to the resume file. */
    FILE *neu = fopen("new.dss", "w");
    if (!neu) die("new");
    if (fputs("NEWSTATE", neu) < 0) die("new write");
    if (fclose(neu)) die("new close");
    if (rename("new.dss", "game_0.dss")) die("rename");
    n = read_file(path, got, 7);
    if (n != 7 || memcmp(got, "NEWSTATE", 7)) {
        fprintf(stderr, "a finished save did not replace the resume file (read %d)\n", n);
        _exit(1);
    }
    if (access("game_0.dss", F_OK) == 0) {
        fprintf(stderr, "the save landed in the player's slot\n");
        _exit(1);
    }
    _exit(0);
}
