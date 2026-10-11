/*
    BananaDS 2D engine: a fast software 2D renderer for melonDS-based emulators.
    Copyright 2026 ROCKNIXDS contributors. Based on the melonDS software renderer,
    Copyright 2016-2026 melonDS team.

    This file is part of BananaDS.

    BananaDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    BananaDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with BananaDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef GPU_BANANA_H
#define GPU_BANANA_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "GPU_Soft.h"
#include "GPU2D_Banana.h"

namespace melonDS
{

// BananaRenderer: melonDS's software renderer with the Banana 2D engine for both 2D units, and the
// per-scanline output stage (display mode, master brightness, 6-to-8-bit expansion) fused into one
// pass per screen instead of three. The 3D rasterizer and display capture are melonDS's own.
//
// Use it exactly like SoftRenderer: nds->GPU.SetRenderer(std::make_unique<BananaRenderer>(*nds)).
//
// SetThreaded2D(true) (render.2d_thread) draws the 2D lines on a thread of their own. On the emulator's thread,
// each scanline and sprite line only snapshots what it needs (BananaRenderer2D::Prepare*: the registers, plus the
// 3D layer's line and the VRAM or FIFO display line when used) into a ring of jobs; the thread draws them in order
// into the back framebuffer. Memory the drawing reads (VRAM flats, converted palettes, OAM) changes only after the
// thread has caught up (Drain2D), so every line sees it as it was at its own time. The emulator's thread waits for
// the thread before SwapBuffers (the frontend reads the framebuffer after the frame), before Reset/Stop and
// savestates, and draws in place, as without the thread, while display capture is on (capture reads the lines
// back into VRAM at their time).
class BananaRenderer : public SoftRenderer
{
public:
    explicit BananaRenderer(melonDS::NDS& nds);
    ~BananaRenderer() override;

    void Reset() override;
    void Stop() override;
    void PreSavestate() override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;
    void VBlank() override;
    void SwapBuffers() override;

    void SetThreaded2D(bool on);
    bool IsThreaded2D() const { return Thread2D; }
    bool Drain2D();   // wait until the 2D thread has drawn every line handed to it (false: it already had)

private:
    void OutputLineA(u32* dst, u32 dispcnt, u16 brightness, const u16* display);
    void OutputLineB(u32* dst, u32 dispcnt, u16 brightness);
    static void WriteLine(u32* dst, const u32* src, u16 brightness);
    static void FillLine(u32* dst, u32 val);

    // ---- the 2D thread (render.2d_thread) ----
    struct Job
    {
        enum : u8 { Scanline, Sprites } Kind;
        bool ScreensEnabled;
        bool HasDisplay;            // Display holds unit A's VRAM or FIFO display line
        u32 Line;                   // VCOUNT for a scanline, the sprite line for sprites
        u32* DstA;
        u32* DstB;
        u16 BrightnessA, BrightnessB;
        Banana2DRegs Regs[2];
        alignas(16) u32 Line3D[256];  // the 3D layer's line, when unit A's DISPCNT enables 3D
        alignas(16) u16 Display[256];
    };
    static constexpr u32 NumJobs = 128;
    static constexpr u32 KickBatch = 16;  // jobs handed over before the thread is woken (fewer wakeups)

    Job& NewJob();
    void Submit();
    void Kick();
    void Execute(Job& job);
    void WorkerLoop();
    void StopWorker();
    static bool DrainHook(void* self) { return ((BananaRenderer*)self)->Drain2D(); }
    void DrawScanlineInPlace(u32 line);

    bool Thread2D = false;
    std::unique_ptr<Job[]> Jobs;
    std::atomic<u32> Head{0};   // jobs handed over (written by the emulator's thread)
    std::atomic<u32> Tail{0};   // jobs drawn (written by the 2D thread)
    u32 Kicked = 0;             // Head at the last wakeup
    std::mutex M;
    std::condition_variable Wake;
    bool KickPending = false, Quit = false;
    std::thread Worker;
    u64 StatJobs = 0, StatInPlace = 0;
};

}

#endif // GPU_BANANA_H
