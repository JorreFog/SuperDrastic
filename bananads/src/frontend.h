// The ways BananaDS can run a game. Each returns the process's exit code.
#pragma once
#include "config.h"
#include "emu.h"

namespace bananads {

// No display, no sound: frames as fast as the CPU can, for benchmarks and tests. headless.frames (default 600),
// headless.dump_dir and headless.dump_every write both screens as PPM pictures.
int run_headless(Emu& emu, const Config& cfg);

#ifdef BANANADS_SDL
// A desktop window with both screens, sound, the mouse as the stylus and the keyboard as the pad. For working on
// BananaDS without a handheld; the handheld path is KMS.
int run_sdl(Emu& emu, const Config& cfg);
#endif

#ifdef BANANADS_KMS
// The handheld: both screens straight onto the two panels through DRM/KMS, the pad and the touchscreen from evdev,
// sound through ALSA (frontend_kms.cpp; BananaLinux's docs/EMULATORS.md is the contract it keeps).
int run_kms(Emu& emu, const Config& cfg);
#endif

#ifdef BANANADS_ALSA
// Sound out and the microphone in through ALSA, on their own threads (audio.out_device, mic.device).
struct Alsa;
Alsa* alsa_start(Emu& emu, const Config& cfg);
// the in-game menu's sounds: a WAV's samples kept as sound id (0-3), played over the game's output
void alsa_ui_load(Alsa* a, int id, const int16_t* pcm, int frames, int rate, int channels);
void alsa_ui_play(Alsa* a, int id);
void alsa_stop(Alsa* a);
#endif

}  // namespace bananads
