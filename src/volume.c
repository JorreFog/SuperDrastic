// volume.c: the volume keys' on-screen indicator while a game runs.
//
// ROCKNIX's input_sense service handles the volume keys itself (pactl, then audio.volume in system.cfg) and shows
// "Volume: N%" through mako, a desktop notification daemon that stops with sway for a libdsflip session. This
// watches the volume keys' evdev device and, after a press, follows audio.volume for a second and shows it in the
// overlay (ui.c) as the setting changes. The value shown is ROCKNIX's own, so it matches the menus' indicator.
//
// The same keys with ROCKNIX's function key held change the brightness instead (input_sense: "brightness up/down"):
// display.brightness is followed too, and the indicator shows the brightness when the gamepad's Menu (BTN_MODE, read
// with EVIOCGKEY at the press) is held or the brightness setting is what changed, else the volume (it showed the
// volume, unchanged, while the brightness went up: ROCKNIXDS issue 49). Nothing shows until a setting has changed,
// or for 400 ms (a press at the end of the scale changes nothing: the volume, as before). Reading system.cfg is all
// this does per press; nothing here touches the audio.
//
// While the keys are in use the CPU runs at its top clock (cpugov_boost): input_sense runs ROCKNIX's volume script
// (a shell, pactl, sed on system.cfg) for every press and every 100 ms of a held key, in processes the governor
// doesn't see. At the low clock it had settled on for the game they took CPU time from DraStic's threads and
// PipeWire's, which is the audio stutter and the late volume changes of quick presses in ROCKNIXDS issue 49 (the
// menu's slider runs the same script, but with the game and its sound paused).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

void ui_volume(int pct);
void ui_brightness(int pct);
void dsflip_log(const char *fmt, ...);
void cpugov_boost(int ms);

#define CFG "/storage/.config/system/configs/system.cfg"
#define FOLLOW_MS 1200      /* how long after the last key event the setting is watched */
#define POLL_MS 50
#define GUESS_MS 400        /* nothing changed this long after a press: show the volume anyway */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

static int has_bit(const unsigned long *bits, int b) { return (bits[b / (8 * sizeof(long))] >> (b % (8 * sizeof(long)))) & 1; }

/* the device with KEY_VOLUMEUP that isn't a gamepad (gpio-keys-volume on the RG DS) */
static int open_dev(char *name, size_t nlen) {
    for (int i = 0; i < 32; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))]; memset(bits, 0, sizeof bits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && has_bit(bits, KEY_VOLUMEUP) && !has_bit(bits, BTN_SOUTH)) {
            if (ioctl(fd, EVIOCGNAME(nlen), name) < 0) snprintf(name, nlen, "%s", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* ROCKNIX's function key (input_sense's key.function.a, the RG DS's Menu / mode button, BTN_MODE) held right now on
 * the gamepad: EVIOCGKEY reads the key state without taking events from anyone. -1: no gamepad found */
static int pad_fd = -2;
static int modifier_held(void) {
    if (pad_fd == -2) {
        pad_fd = -1;
        for (int i = 0; i < 32 && pad_fd < 0; i++) {
            char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
            unsigned long bits[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))]; memset(bits, 0, sizeof bits);
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && has_bit(bits, BTN_SOUTH) && has_bit(bits, BTN_MODE)) pad_fd = fd;
            else close(fd);
        }
    }
    if (pad_fd < 0) return -1;
    unsigned long st[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))]; memset(st, 0, sizeof st);
    if (ioctl(pad_fd, EVIOCGKEY(sizeof st), st) < 0) { close(pad_fd); pad_fd = -2; return -1; }
    return has_bit(st, BTN_MODE);
}

static void read_settings(int *vol, int *bri) {
    *vol = *bri = -1;
    FILE *f = fopen(CFG, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {                                 /* the last one wins, like get_setting */
        if (!strncmp(line, "audio.volume=", 13)) *vol = atoi(line + 13);
        else if (!strncmp(line, "display.brightness=", 19)) *bri = atoi(line + 19);
    }
    fclose(f);
}

static void *vol_thread(void *a) {
    (void)a;
    char name[64] = "";
    int fd = -1;
    long long until = 0, next = 0, pressed = 0; int shown = -1, kind = 0, base_v = -1, base_b = -1;   /* kind: 1 volume, 2 brightness */
    for (;;) {
        if (fd < 0) {
            fd = open_dev(name, sizeof name);
            if (fd < 0) { sleep(2); continue; }
            dsflip_log("[volume] keys: %s\n", name);
        }
        int to = -1;
        if (until) { to = (int)(next - now_ms()); if (to < 0) to = 0; }
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, to);
        if (r > 0) {
            struct input_event ev[16];
            ssize_t n = read(fd, ev, sizeof ev);
            if (n < 0 && errno != EAGAIN && errno != EINTR) { dsflip_log("[volume] keys gone: %s\n", strerror(errno)); close(fd); fd = -1; continue; }
            for (ssize_t k = 0; k < n / (ssize_t)sizeof *ev; k++)
                if (ev[k].type == EV_KEY && (ev[k].code == KEY_VOLUMEUP || ev[k].code == KEY_VOLUMEDOWN)) {
                    long long t = now_ms();
                    if (!until) { shown = -1; kind = 0; next = t; pressed = t; read_settings(&base_v, &base_b); }   /* a new press: follow */
                    if (ev[k].value == 1 && modifier_held() == 1) kind = 2;   /* Menu held: input_sense changes the brightness */
                    until = t + FOLLOW_MS;
                    cpugov_boost(FOLLOW_MS + 300);
                }
        }
        if (until && now_ms() >= next) {
            int v, b; read_settings(&v, &b);
            if (kind == 2 || (b >= 0 && b != base_b)) kind = 2;          /* the function key: brightness */
            else if (v >= 0 && v != base_v) kind = 1;
            else if (!kind && now_ms() - pressed >= GUESS_MS) kind = 1;
            int val = kind == 2 ? b : v;
            if (kind && val >= 0 && val != shown) { shown = val; if (kind == 2) ui_brightness(val); else ui_volume(val); }
            if (kind) { base_v = v; base_b = b; }                        /* what changes from here on */
            next += POLL_MS;
            if (now_ms() >= until) until = 0;
        }
    }
    return 0;
}

void volume_start(void) {
    pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (!pthread_create(&t, &at, vol_thread, 0)) pthread_setname_np(t, "dsf-vol");
    pthread_attr_destroy(&at);
}
