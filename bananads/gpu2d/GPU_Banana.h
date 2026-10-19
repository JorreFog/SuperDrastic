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
    void Start3DRendering() override;
    // render.2d_lazy: the 2D thread finishes a frame's lines while the emulator runs the next frame. At a frame's end
    // the emulator's thread waits only for the frame before's jobs (the buffers the next frame writes); whoever reads
    // a frame's pictures waits for its fence first (FrameFence after run_frame; GetFramebuffers waits by itself).
    void SetLazyFrames(bool on) { Drain2D(); LazyFrames = on; }
    // render.2d_kick: wake the 2D thread every this many jobs (1-NumJobs; it is also woken at VBlank, at the swap
    // and before any wait for it, so the pictures don't depend on it)
    void SetNice(int n) { Nice = n; }  // sched.2d_nice: the 2D thread's nice value (0: the default)
    void SetKickBatch(u32 jobs) { KickBatch = jobs < 1 ? 1 : jobs > NumJobs ? NumJobs : jobs; }
    bool FrameFence(u32& fence) const { fence = FenceCur; return LazyFrames && Thread2D; }
    void WaitFence(u32 fence) const;  // any thread: until the 2D thread has drawn the jobs before `fence`

    // render.2d_ring=split: the counters the two threads write each job on cache lines of their own (Head on one,
    // Tail and LogRead on another: shared, they sit on one line with Kicked and the mutex, and each job moves that
    // line between the cores twice), and the emulator's thread reads Tail and LogRead again only when the ring or
    // the log looks full by the last values it read (TailSeen, LogReadSeen: the 2D thread only moves them on).
    // The same jobs in the same order, so the same pictures. Takes effect when the 2D thread starts.
    void SetSplitRing(bool on) { SplitRing = on; }
    // render.2d_test_delay (tests only): the 2D thread sleeps this many microseconds after each job, so that it falls
    // behind and the ring and the log fill (their full paths are rare otherwise). The same pictures, slower.
    void SetTestDelay(u32 us) { TestDelayUs = us; }
    void SetThreaded2D(bool on);
    bool IsThreaded2D() const { return Thread2D; }
    // render.scale=2: the 3D renderer's four quarter lines of a DS line (quarter q: 2x row 2 * line + (q >> 1), its
    // even pixels or, q & 1, its odd ones). Unit A composites each line that shows the 3D layer four times, and the
    // framebuffers become 512x384 (GetFramebuffers); everything else is drawn at 1x and doubled.
    typedef const u32* (*QuarterFn)(void* ctx, u32 line, int q);
    typedef bool (*QuarterRowsFn)(void* ctx);  // true: the quarters are the 3D frame's own rows (render.3d_share)
    void SetHires(QuarterFn fn, void* ctx, QuarterRowsFn rows = nullptr);
    // render.3d_wait2d: a quarter's row without waiting, the render drawing it, and a wait for it (the 2D thread's)
    typedef unsigned (*RowsGenFn)(void* ctx);
    typedef void (*WaitRowsFn)(void* ctx, u32 line, unsigned gen);
    void SetHiresRows(QuarterFn row, RowsGenFn gen, WaitRowsFn wait) { Drain2D(); QuarterRow = row; RowsGen = gen; WaitRows = wait; }
    int Scale() const { return Quarter ? 2 : 1; }
    bool GetFramebuffers(void** top, void** bottom) override;
    // The frontend's own buffers for the next frame (frontend=kms: the scanout buffers themselves, so nothing is
    // copied): a screen at Scale() each, rows packed (256 or 512 pixels), only ever written, line by line. Set between
    // frames; that frame's lines all go there, and GetFramebuffers returns them once it is finished. Null, null: the
    // renderer's own framebuffers again (the last frame drawn elsewhere is forgotten).
    void SetOutput(u32* top, u32* bottom);

    bool Drain2D();   // wait until the 2D thread has drawn every line handed to it (false: it already had)
    // render.2d_fence_wait: the waits for the 2D thread (a frame's fence, a drain, a full ring or log) block on a
    // condition variable the 2D thread signals after a job while someone waits (block), or spin on
    // sched_yield as before (spin, the default until the device has A/B'd it). A spin from the KMS presenter, at
    // SCHED_FIFO, keeps its core from every other thread until the 2D thread catches up; blocking gives the core to
    // the thread being waited for
    void SetFenceWaitBlocking(bool on) { FenceBlock = on; }
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
    QuarterRowsFn QuarterRows = nullptr;
    QuarterFn QuarterRow = nullptr;
    RowsGenFn RowsGen = nullptr;
    WaitRowsFn WaitRows = nullptr;
    // render.3d_share: jobs point at the 3D frame's rows instead of copying them (4 KB a line at 2x): the rows stay
    // through the next frame's rendering (QuarterRowsFn), and the 2D thread is drained at every frame's end
    void* QuarterCtx = nullptr;
    std::unique_ptr<u32[]> HiFB[2][2];
    u32* Out[2] = {};    // SetOutput's: the frame being drawn goes here
    u32* Front[2] = {};  // the last finished frame's, when it went to SetOutput's

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
        const u32* Q3D[4];               // where the drawing reads them: Line3D's, or the 3D frame's rows (render.3d_share)
        bool WaitRows;                   // render.3d_wait2d: the 2D thread waits for the rows of render RowsGen first
        unsigned RowsGen;
        alignas(16) u16 Display[256];
        u64 LogEnd;                 // the copy log up to here is applied before the job is drawn
    };
    // a whole frame's jobs (192 lines and 191 sprite lines): the emulator's thread doesn't wait for a 2D thread that
    // lags within the frame, which catches up in VBlank (drained at the frame's end, SwapBuffers)
    static constexpr u32 NumJobs = 512;
    // jobs handed over before the thread is woken (render.2d_kick): each wake of a sleeping 2D thread is a futex
    // call on the emulator's thread (pthread_cond_signal, 0.9% of it in Mario Kart DS's race start at 16)
    u32 KickBatch = 16;
    int Nice = 0;

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
    // render.2d_ring (SetSplitRing): what the code uses, set as the 2D thread starts: Head, Tail and LogRead above
    // (shared), or the two lines below (split)
    bool SplitRing = false, RingSplit = false;
    std::atomic<u32>* HeadP = &Head;
    std::atomic<u32>* TailP = &Tail;
    std::atomic<u64>* LogReadP = &LogRead;
    u32 TailSeen = 0;           // (split) Tail as the emulator's thread last read it
    u32 TestDelayUs = 0;        // render.2d_test_delay
    u64 LogReadSeen = 0;        // (split) LogRead likewise
    struct alignas(64) ProducerLine { std::atomic<u32> Head{0}; };
    struct alignas(64) ConsumerLine { std::atomic<u32> Tail{0}; std::atomic<u64> LogRead{0}; };
    ProducerLine PLine;
    ConsumerLine CLine;
    bool LazyFrames = false;
    u32 FenceCur = 0, FencePrev = 0;  // Head at the last two frames' ends (render.2d_lazy)
    u32 Kicked = 0;             // Head at the last wakeup
    std::mutex M;
    std::condition_variable Wake;
    bool KickPending = false, Quit = false;
    std::thread Worker;
    u64 StatJobs = 0, StatInPlace = 0, StatLogBytes = 0, StatWaits = 0;
    // the blocking waits (render.2d_fence_wait): a waiter sets FenceWaiting, then re-tests under FenceM and waits
    // (timed, as a safety net); the 2D thread, after it publishes a job, notifies under FenceM when the flag is set
    template<class Pred> void WaitForWorker(Pred pred) const;
    bool FenceBlock = false;  // (render.2d_fence_wait=spin, the default)
    mutable std::mutex FenceM;
    mutable std::condition_variable FenceCv;
    mutable std::atomic<bool> FenceWaiting{false};
};

}

#endif // GPU_BANANA_H
