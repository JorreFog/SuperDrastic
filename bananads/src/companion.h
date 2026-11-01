// the universal companion's producer (companion.cpp)
#pragma once
#include <cstdint>
namespace bananads { class Emu; }
void companion_start(const bananads::Emu& emu, const char* saves, const char* states, const char* path);
void companion_frame(uint64_t frame);   // the emulator's thread, after each frame
void companion_stop();
