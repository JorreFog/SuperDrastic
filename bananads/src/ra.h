// ra.h: RetroAchievements in BananaDS (ra.c: SuperDrastic's src/ra.c on BananaDS's own memory, rcheevos rc_client).
// The frontend's emulator thread calls ra_frame() after every DS frame; the in-game menu reads the progress and the
// latest unlocks; the pop-ups (a game loaded, an achievement unlocked, a login problem) are queued for the frontend
// to draw over the top screen (menu.c's bmenu_overlay).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* ctx;
    const uint8_t* (*main_ram)(void* ctx);  // DS main RAM (RA 0x000000-0x3FFFFF), 4 MB
    const uint8_t* (*dtcm)(void* ctx);      // the ARM9's 16 KB data TCM (RA 0x1000000-0x1003FFF), or 0
    void (*log)(void* ctx, const char* line);
} ra_host;

// once, before the first ra_frame: the game's file (an .nds) and the account from the environment
// (BANANADS_RA_USER and BANANADS_RA_PASSWORD; the login token is kept in $BANANADS_DATA/ra.token)
void ra_init(const ra_host* host, const char* rom_path);
void ra_frame(void);  // the emulator's thread, after each frame
void ra_reset(void);  // the game restarted from its start (the menu's Restart game), the emulator paused

// the in-game menu: 1 with the counts once a game with achievements is loaded
int ra_progress(unsigned* unlocked, unsigned* total);
typedef struct { char title[96], desc[200], badge[600]; unsigned points; long long when; } ra_recent_t;
int ra_recent(ra_recent_t* out, int max);  // the max most recently unlocked, newest first, with their cached badges

// the next pop-up to show: 1 and its lines, its badge's file ("" for none), its accent colour and how long, else 0
int ra_popup_take(char* l1, int n1, char* l2, int n2, char* badge, int nb, uint32_t* accent, int* ms);

#ifdef __cplusplus
}
#endif
