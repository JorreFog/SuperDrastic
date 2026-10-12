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
// each scanline and sprite line only snapshots what it needs into a ring of jobs: the registers
// (BananaRenderer2D::Prepare*), the 3D layer's line and the VRAM or FIFO display line when shown, and, through a
// copy log, every VRAM block, palette half and OAM that changed since the last line. The 2D thread applies a job's
// log to the engines' own copies of that memory, then draws the job into the back framebuffer, in order. So every
// line is drawn from memory as it was at its own time, and the emulator never waits for the drawing, except
// before SwapBuffers (the frontend reads the framebuffer after the frame), Reset, Stop and savestates, and when the
// job ring or the log is full. While display capture is on, unit A is drawn on the emulator's thread (capture reads
// its lines back into VRAM at their time) and only unit B goes to the 2D thread.
class BananaRenderer : public SoftRenderer
{
public:
    explicit BananaRenderer(melonDS::NDS& nds);
    ~BananaRenderer() override;

    void Reset() override;
    void Stop() override;
    void PreSavestate() override;
    void PostSavestate() override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;
    void VBlank() override;
    void SwapBuffers() override;

    void SetThreaded2D(bool on);
    bool IsThreaded2D() const { return Thread2D; }
    // render.scale=2: the 3D renderer's four quarter lines of a DS line (quarter q: 2x row 2 * line + (q >> 1), its
    // even pixels or, q & 1, its odd ones). Unit A composites each line that shows the 3D layer four times, and the
    // framebuffers become 512x384 (GetFramebuffers); everything else is drawn at 1x and doubled.
    typedef const u32* (*QuarterFn)(void* ctx, u32 line, int q);
    void SetHires(QuarterFn fn, void* ctx);
    int Scale() const { return Quarter ? 2 : 1; }
    bool GetFramebuffers(void** top, void** bottom) override;

    bool Drain2D();   // wait until the 2D thread has drawn every line handed to it (false: it already had)
    void Resync2D();  // after GPU::ResetVRAMCache (a savestate): the copies are made again from GPU's memory

private:
    void OutputLineA(u32* dst, u32 dispcnt, u16 brightness, const u16* display);
    void OutputLineB(u32* dst, u32 dispcnt, u16 brightness);
    static void WriteLine(u32* dst, const u32* src, u16 brightness);
    static void FillLine(u32* dst, u32 val);
    // a DS line into the framebuffer at either scale (at 2x dst is two rows of 512)
    u32* LineDst(int screen, u32 line);
    void EmitA(u32* dst, u32 dispcnt, u16 brightness, const u16* display);
    void EmitB(u32* dst, u32 dispcnt, u16 brightness);
    void Fill(u32* dst, u32 val);
    QuarterFn Quarter = nullptr;
    void* QuarterCtx = nullptr;
    std::unique_ptr<u32[]> HiFB[2][2];

    // ---- the 2D thread (render.2d_thread) ----
    struct Job
    {
        enum : u8 { Scanline, Sprites } Kind;
        bool ScreensEnabled;
        bool HasDisplay;            // Display holds unit A's VRAM or FIFO display line
        bool OnlyB;                 // unit A is drawn on the emulator's thread (display capture)
        u32 Line;                   // VCOUNT for a scanline, the sprite line for sprites
        u32* DstA;
        u32* DstB;
        u16 BrightnessA, BrightnessB;
        Banana2DRegs Regs[2];
        alignas(16) u32 Line3D[4][256];  // the 3D layer's line, when unit A's DISPCNT enables 3D (2x: its quarters)
        alignas(16) u16 Display[256];
        u64 LogEnd;                 // the copy log up to here is applied before the job is drawn
    };
    static constexpr u32 NumJobs = 128;
    static constexpr u32 KickBatch = 16;  // jobs handed over before the thread is woken (fewer wakeups)

    Job& NewJob();
    void Submit();
    void Kick();
    void Execute(Job& job);
    void WorkerLoop();
    void StopWorker();
    // the copy log: a ring of {destination, length, data} records, 16-byte aligned. LogWrite (the emulator's
    // thread) and LogRead (the 2D thread, as it applies records) count bytes since the start.
    static constexpr u32 LogSize = 4 << 20;
    static constexpr u32 LogChunk = 64 << 10;
    struct LogRecord { u8* Dst; u64 Len; };   // Dst null: skip to the ring's end
    std::unique_ptr<u8[]> Log;
    u64 LogWrite = 0;
    std::atomic<u64> LogRead{0};
    static void CopyHook(void* self, void* dst, const void* src, u32 len) { ((BananaRenderer*)self)->LogCopy(dst, src, len); }
    void LogCopy(void* dst, const void* src, u32 len);
    void LogReserve(u64 bytes);
    void ApplyLog(u64 end);
    void DrawScanlineInPlace(u32 line);
    void SetAInPlace(bool on);
    static void DirectCopy(void*, void* dst, const void* src, u32 len) { memcpy(dst, src, len); }
    bool AInPlace = false;      // unit A drawn on the emulator's thread, its copies written directly
    void PrepareScanlineJob(Job& job, u32 line, u32*& line3d);
    void PrepareSpritesJob(Job& job, u32 line);

    bool Thread2D = false;
    std::unique_ptr<Job[]> Jobs;
    std::atomic<u32> Head{0};   // jobs handed over (written by the emulator's thread)
    std::atomic<u32> Tail{0};   // jobs drawn (written by the 2D thread)
    u32 Kicked = 0;             // Head at the last wakeup
    std::mutex M;
    std::condition_variable Wake;
    bool KickPending = false, Quit = false;
    std::thread Worker;
    u64 StatJobs = 0, StatInPlace = 0, StatLogBytes = 0, StatWaits = 0;
};

}

#endif // GPU_BANANA_H
