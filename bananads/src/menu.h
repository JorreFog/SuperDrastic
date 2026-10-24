// menu.h: the in-game menu (menu.c: SuperDrastic's, in BananaOS's colours) and what the frontend gives it. The
// frontend pauses the game and calls bmenu_run, which draws both panels and reads the input itself until the menu
// closes; everything it needs from the emulator and the handheld goes through bmenu_host.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BM_A, BM_B, BM_Y, BM_L, BM_R, BM_START, BM_UP, BM_DOWN, BM_LEFT, BM_RIGHT, BM_MENU, BM_X };
enum { BM_EV_KEY = 1, BM_EV_TOUCH = 2 };
typedef struct {
    int kind;      // BM_EV_KEY: key (BM_*) and down; BM_EV_TOUCH: a touch going down at (x, y), bottom panel pixels
    int key, down;
    int x, y;
} bmenu_ev;

// the menu's own slots 1-8 are the emulator's savestate slots 1-8; slot 9 keeps the game from before a load (undo)
enum { BM_UNDO_SLOT = 9 };

typedef struct {
    void* ctx;
    const char* rom;  // the game's file
    int resume_on;    // quitting writes the resume state ("keeps your spot")
    // a buffer to draw the panel's next picture in (panel 0 top, 1 bottom): its index, or -1
    int (*canvas)(void* ctx, int panel, uint32_t** px, int* pitch, int* w, int* h);
    void (*show)(void* ctx, int panel, int k);  // buffer k onto the panel
    // the game's top screen as it was when the menu opened, at the panel's size; 0: none
    int (*grab_top)(void* ctx, uint32_t* dst, int pitch, int w, int h);
    int (*poll)(void* ctx, bmenu_ev* ev);           // the next input event; 0: none waiting
    int (*battery)(void* ctx, int* charging);       // percent, or -1
    int (*save)(void* ctx, int slot);               // 0: done
    int (*load)(void* ctx, int slot);               // 0: done
    void (*state_file)(void* ctx, int slot, char* out, int n);  // the slot's file (+ ".pic": 256x384 XRGB8888)
    void (*quit)(void* ctx);                        // after the menu closes: the resume state, then the menu
    // Quit without saving: as quit, but no resume state is written (the one from before stays as it was); unset: the
    // quit question has no such choice
    void (*quit_nosave)(void* ctx);
    void (*restart)(void* ctx);                     // the game from its start
    void (*blow)(void* ctx, int ms);                // the game hears a blow on the mic
    int (*mic_meter)(void* ctx, float* level, float* floor_, float* bar);  // -1 no mic, 0 starting, 1 listening
    void (*sound_load)(void* ctx, int id, const int16_t* pcm, int frames, int rate, int channels);
    void (*sound_play)(void* ctx, int id);
    void (*boost)(void* ctx, int ms);               // the top CPU clock for a while (the governor)
    void (*log)(void* ctx, const char* line);
    // the emulator's own settings on the Quick settings page, after Volume, Brightness and Microphone (the shader,
    // edge marking, fog...): how many, each one's label, its values' names and the current one; opt_set applies a new
    // value at once (and keeps it for the next game). opt_count 0 or unset: none
    int (*opt_count)(void* ctx);
    const char* (*opt_label)(void* ctx, int i);
    int (*opt_values)(void* ctx, int i);
    const char* (*opt_value_name)(void* ctx, int i, int v);
    int (*opt_get)(void* ctx, int i);
    void (*opt_set)(void* ctx, int i, int v);
    // the game's cheats (Quick settings > Cheats): how many, each one's name, on or off; cheat_set turns one on or
    // off and keeps it for the next time. cheat_count unset: no Cheats row
    int (*cheat_count)(void* ctx);
    const char* (*cheat_name)(void* ctx, int i);
    int (*cheat_get)(void* ctx, int i);
    void (*cheat_set)(void* ctx, int i, int on);
} bmenu_host;

void bmenu_init(const bmenu_host* host);  // once, before the first bmenu_run
// open the menu and run it until it closes: start 0 the main page, 1 Save, 2 Load, 3 Quick settings, 4 the quit
// question; returns 0 if it couldn't open (no fonts, no panel buffers)
int bmenu_run(int start);
void bmenu_session_start(void);  // the game started: "this session" counts from here
// a pop-up over the game's top screen (an achievement unlocked...): the frontend calls this with each finished top
// frame (XRGB8888, pitch in pixels), on the emulator's thread; it draws the current pop-up, if any, into it
void bmenu_overlay(uint32_t* px, int pitch, int w, int h);
// the FPS counter in the top panel's corner (drawn by bmenu_overlay): the frames emulated a second x 10, -1 hidden; and
// the fast-forward speed beside it (0: off, 99: as fast as it goes)
void bmenu_fps(int fps_x10, int ff);
/* the game's own frame rate (its 3D frames a second, x10) beside the emulator's, -1 when it draws no 3D */
void bmenu_fps_game(int fps_x10);
// a pop-up of the frontend's own (a screenshot saved...): shown like an achievement's, for ms; any thread
void bmenu_popup(const char* line1, const char* line2, int ms);

#ifdef __cplusplus
}
#endif
