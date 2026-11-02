/* A direction or button held when the in-game menu opens has to be released to DraStic if the player let go
 * while the menu had the event loop. Presses that only drove the menu must not be handed to the game.
 * gcc -O2 -Wall -o /tmp/t_hold_diff tools/ut/t_hold_diff.c src/resume.c && /tmp/t_hold_diff */
#include <assert.h>
#include <stdint.h>
#include <string.h>

void dsflip_log(const char *fmt, ...) { (void)fmt; }
void cpugov_boost(int ms) { (void)ms; }
void dsflip_toast(const char *a, const char *b, unsigned c, int d) { (void)a; (void)b; (void)c; (void)d; }
void audio_mute(int on) { (void)on; }
volatile int dsflip_hold;

int resume_hold_edges(uint64_t was, uint64_t now, unsigned char *btn, unsigned char *down, int max);
void resume_start(void);
void resume_saw_event(const void *ev);
uint64_t resume_buttons_held(void);
int resume_sync_buttons(uint64_t now);
void resume_sync_axis(int axis, int value);
void resume_sync_hat(int hat, int value);
int resume_poll(void *ev);

static void joy(unsigned char *ev, uint32_t type, int button) {
    memset(ev, 0, 56);
    memcpy(ev, &type, 4);
    ev[12] = (unsigned char)button;
    ev[13] = type == 0x603;
}

int main(void) {
    resume_start();
    unsigned char b[64], d[64];
    assert(resume_hold_edges(0, 0, b, d, 64) == 0);
    assert(resume_hold_edges(1, 0, b, d, 64) == 1 && b[0] == 0 && d[0] == 0);
    assert(resume_hold_edges(0, 1ull << 63, b, d, 64) == 1 && b[0] == 63 && d[0] == 1);
    uint64_t was = (1ull << 3) | (1ull << 10), now = (1ull << 10) | (1ull << 11);
    int n = resume_hold_edges(was, was & now, b, d, 64);   /* the menu's mask: releases only */
    assert(n == 1 && b[0] == 3 && d[0] == 0);
    assert(resume_hold_edges(0, 0xff, b, d, 1) == 1);

    unsigned char ev[56];
    joy(ev, 0x603, 5);
    resume_saw_event(ev);
    joy(ev, 0x603, 9);
    resume_saw_event(ev);
    assert(resume_buttons_held() == ((1ull << 5) | (1ull << 9)));
    was = resume_buttons_held();
    now = (1ull << 9) | (1ull << 2);                        /* 5 released, 2 pressed only inside the menu */
    assert(resume_sync_buttons(was & now) == 1);
    assert(resume_buttons_held() == (1ull << 9));
    memset(ev, 0, 56);
    assert(resume_poll(ev) == 1);
    uint32_t type; memcpy(&type, ev, 4);
    assert(type == 0x604 && ev[12] == 5 && ev[13] == 0);
    assert(resume_poll(ev) == 0);                           /* the menu press of button 2 is not delivered */

    resume_sync_axis(1, -20000);
    resume_sync_hat(0, 0);
    assert(resume_poll(ev) == 1);
    memcpy(&type, ev, 4);
    int16_t v; memcpy(&v, ev + 16, 2);
    assert(type == 0x600 && ev[12] == 1 && v == -20000);
    assert(resume_poll(ev) == 1);
    memcpy(&type, ev, 4);
    assert(type == 0x602 && ev[12] == 0 && ev[13] == 0);
    assert(resume_poll(ev) == 0);
    return 0;
}
