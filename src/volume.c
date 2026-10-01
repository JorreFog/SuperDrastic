// volume.c: the volume keys' on-screen indicator while a game runs.
//
// ROCKNIX's input_sense service handles the volume keys itself (pactl, then audio.volume in system.cfg) and shows
// "Volume: N%" through mako, a desktop notification daemon that stops with sway for a libdsflip session. This
// watches the volume keys' evdev device and, after a press, follows audio.volume for a second and shows it in the
// overlay (ui.c) as the setting changes. The value shown is ROCKNIX's own, so it matches the menus' indicator.
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
void dsflip_log(const char *fmt, ...);

#define CFG "/storage/.config/system/configs/system.cfg"
#define FOLLOW_MS 1200      /* how long after the last key event the setting is watched */
#define POLL_MS 100

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

static int read_volume(void) {
    FILE *f = fopen(CFG, "r"); if (!f) return -1;
    char line[256]; int v = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "audio.volume=", 13)) v = atoi(line + 13);     /* the last one wins, like get_setting */
    fclose(f);
    return v;
}

static void *vol_thread(void *a) {
    (void)a;
    char name[64] = "";
    int fd = -1;
    long long until = 0, next = 0; int shown = -1;
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
                    if (!until) { shown = -1; next = t; }          /* a new press: show at once, then follow */
                    until = t + FOLLOW_MS;
                }
        }
        if (until && now_ms() >= next) {
            int v = read_volume();
            if (v >= 0 && v != shown) { shown = v; ui_volume(v); }
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
