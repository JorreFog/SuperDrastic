// lid.c: the handheld's own lid and its HOME key, for DraStic.
//
// - The lid (the hall sensor's SW_LID): DraStic's Hinge control is held down while the lid is shut, so the game sees
//   the DS's lid close as on the real thing (most go to their sleep mode, some play a sound or an easter egg, a
//   puzzle that wants the DS shut works) and opens again with it. Hinge in drastic.cfg (a key, 256 + scancode, or a
//   pad button, 1024 + n; the first set that has one): BananaLinux's run-drastic puts Pause on it when neither set
//   has one. DSFLIP_LID=0: off.
// - HOME (BTN_Z on a device of its own, adc-keys: the RG DS's Anbernic button, the RG DS Plus's back button) opens
//   the in-game menu whatever DraStic's buttons are set to, so a button setup gone wrong can't lock the menu away.
//   DSFLIP_HOME=0: off.
// - A modifier is not a press: bananad's F + volume (brightness) and HOME held with a volume key open nothing when
//   F / HOME is let go. The volume keys' device is read here for that (lid_vol_since); menu.c asks it at the
//   menu button's release, and HOME's release here checks it the same way.
//
// Both are read (never grabbed: bananad reads the same devices) from SDL_PollEvent (dsflip.c), DraStic's own
// thread, where the menu may run.
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void dsflip_log(const char *fmt, ...);
void dsflip_control(int control, int down);   /* dsflip.c: a DraStic control's key or button, pressed or released */
void menu_home(void);                         /* menu.c: the in-game menu, from HOME */

static int lid_fd = -1, home_fd = -1, vol_fd = -1, inited, hinge = -1, closed;
static long long vol_down_ms = -1, home_down_ms = -1;   /* wall ms: a volume key's last press, HOME's (-1: up) */
static long long next_check;

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int bit(const unsigned long *a, int n) { return a[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long))) & 1; }

/* the Hinge control in drastic.cfg (DraStic runs in its own folder): set a's, else set b's; -1 when neither has one */
static int hinge_control(void) {
    FILE *f = fopen("config/drastic.cfg", "r"); if (!f) return -1;
    char line[256]; int a = -1, b = -1, v;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "controls_a[CONTROL_INDEX_HINGE] = %d", &v) == 1 && v >= 256 && v < 65535) a = v;
        if (sscanf(line, "controls_b[CONTROL_INDEX_HINGE] = %d", &v) == 1 && v >= 256 && v < 65535) b = v;
    }
    fclose(f);
    return a >= 0 ? a : b;
}

static void init(void) {
    inited = 1;
    /* on by default on BananaLinux only (its bananad delays the lid's sleep so the game sees it, and HOME is free there);
     * elsewhere (ROCKNIX: its own HOME handling) only with DSFLIP_LID=1 / DSFLIP_HOME=1 */
    const char *l = getenv("DSFLIP_LID"), *h = getenv("DSFLIP_HOME");
    int banana = access("/run/banana/mixer", F_OK) == 0;
    int want_lid = l ? *l != '0' : banana, want_home = h ? *h != '0' : banana;
    if (want_lid) hinge = hinge_control();
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[300]; snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        unsigned long sw[SW_MAX / (8 * sizeof(long)) + 1] = {0}, keys[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_SW, sizeof sw), sw);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        if (want_lid && hinge >= 0 && lid_fd < 0 && bit(sw, SW_LID)) {
            lid_fd = fd;
            unsigned long st[SW_MAX / (8 * sizeof(long)) + 1] = {0};
            /* shut already when the game starts: nothing to close, it's held from the next close */
            if (ioctl(fd, EVIOCGSW(sizeof st), st) >= 0 && bit(st, SW_LID)) dsflip_log("[lid] shut at the start: ignored until it opens\n");
            dsflip_log("[lid] %s: holds DraStic's Hinge (control %d) while shut\n", path, hinge);
        } else if (want_home && home_fd < 0 && bit(keys, BTN_Z) && !bit(keys, BTN_SOUTH)) {
            home_fd = fd;
            dsflip_log("[lid] %s: HOME opens the menu\n", path);
        } else if (banana && vol_fd < 0 && bit(keys, KEY_VOLUMEUP) && !bit(keys, BTN_SOUTH)) {
            vol_fd = fd;     /* (bananad's volume keys: F + volume is brightness, so F's release must not open the menu) */
        } else close(fd);
    }
    closedir(d);
    if (want_lid && hinge < 0) dsflip_log("[lid] no Hinge control in drastic.cfg: the lid isn't passed on\n");
}

static long long wall_ms(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static long long ev_ms(const struct input_event *e) { return e->input_event_sec * 1000LL + e->input_event_usec / 1000; }

/* the volume keys' events so far: the last press's time (evdev's clock is CLOCK_REALTIME) */
static void vol_drain(void) {
    if (!inited) init();
    if (vol_fd < 0) return;
    struct input_event ev[16]; ssize_t n;
    while ((n = read(vol_fd, ev, sizeof ev)) > 0)
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++)
            if (ev[i].type == EV_KEY && (ev[i].code == KEY_VOLUMEUP || ev[i].code == KEY_VOLUMEDOWN) && ev[i].value)
                vol_down_ms = ev_ms(&ev[i]);
}
/* a wall-clock ms for "now", to give lid_vol_since later (menu.c: the menu button's press) */
long long lid_wall_ms(void) { return wall_ms(); }
/* 1 when a volume key was pressed at or after `since` (wall ms): the button held meanwhile was a modifier */
int lid_vol_since(long long since) { vol_drain(); return since >= 0 && vol_down_ms >= since; }

/* HOME's events: 1 when it was let go and wasn't used as a modifier (a volume key pressed while it was held) */
static int home_released(void) {
    struct input_event ev[16]; ssize_t n; int home = 0;
    while ((n = read(home_fd, ev, sizeof ev)) > 0)
        for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
            if (ev[i].type != EV_KEY || ev[i].code != BTN_Z || ev[i].value == 2) continue;
            if (ev[i].value) { home_down_ms = ev_ms(&ev[i]); continue; }
            int mod = lid_vol_since(home_down_ms);
            if (mod) dsflip_log("[lid] HOME let go after a volume key: a modifier, no menu\n");
            home = !mod; home_down_ms = -1;
        }
    return home;
}

/* SDL_PollEvent (dsflip.c), on DraStic's thread: at most every 15 ms */
void lid_poll(void) {
    long long t = now_ms();
    if (t < next_check) return;
    next_check = t + 15;
    if (!inited) init();
    struct input_event ev[16];
    ssize_t n;
    if (lid_fd >= 0)
        while ((n = read(lid_fd, ev, sizeof ev)) > 0)
            for (int i = 0; i < (int)(n / sizeof ev[0]); i++) {
                if (ev[i].type != EV_SW || ev[i].code != SW_LID || (ev[i].value != 0) == closed) continue;
                closed = ev[i].value != 0;
                dsflip_log("[lid] %s\n", closed ? "shut: the DS's lid closes" : "open: the DS's lid opens");
                dsflip_control(hinge, closed);
            }
    vol_drain();
    int home = home_fd >= 0 && home_released();       /* on release */
    if (home) { dsflip_log("[lid] HOME: the menu\n"); menu_home(); }
}

/* HOME opens the menu (for the menu's Hotkeys page) */
int lid_home_on(void) { return home_fd >= 0; }

/* the menu's loop (menu.c): HOME pressed while the menu is open closes it, and isn't kept for after (it reopened
 * the menu as soon as it closed); 1 when HOME was let go */
int lid_home_pressed(void) {
    if (home_fd < 0) return 0;
    return home_released();
}
