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

#include <string.h>
#ifdef __linux__
#include <pthread.h>
#endif
#include "NDS.h"
#include "Platform.h"
#include "GPU_Banana.h"
#include "GPU_ColorOp.h"

namespace melonDS
{

BananaRenderer::BananaRenderer(melonDS::NDS& nds)
    : SoftRenderer(nds)
{
    Rend2D_A = std::make_unique<BananaRenderer2D>(GPU.GPU2D_A, *this);
    Rend2D_B = std::make_unique<BananaRenderer2D>(GPU.GPU2D_B, *this);
}

BananaRenderer::~BananaRenderer()
{
    StopWorker();
}

// 18-bit RGB to the 32-bit BGRA the frontend takes (melonDS ExpandColor, one pixel)
static inline u32 Expand(u32 c)
{
    u32 r = (c << 18) & 0xFC0000;
    u32 g = (c << 2) & 0xFC00;
    u32 b = (c >> 14) & 0xFC;
    c = r | g | b;
    return c | ((c & 0xC0C0C0) >> 6) | 0xFF000000;
}

void BananaRenderer::FillLine(u32* dst, u32 val)
{
    for (int i = 0; i < 256; i++)
        dst[i] = val;
}

// master brightness and colour expansion in one pass
void BananaRenderer::WriteLine(u32* dst, const u32* src, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;

    if (mode == 1 && factor)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = Expand(ColorBrightnessUp(src[i], factor, 0x0));
    }
    else if (mode == 2 && factor)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = Expand(ColorBrightnessDown(src[i], factor, 0xF));
    }
    else
    {
        for (int i = 0; i < 256; i++)
            dst[i] = Expand(src[i]);
    }
}

// display: unit A's VRAM display line (mode 2; null when the bank isn't mapped to LCDC) or display FIFO line (mode 3)
void BananaRenderer::OutputLineA(u32* dst, u32 dispcnt, u16 brightness, const u16* display)
{
    switch ((dispcnt >> 16) & 0x3)
    {
    case 0: // screen off: white, no master brightness
        FillLine(dst, 0xFFFFFFFF);
        return;

    case 1: // regular display
        WriteLine(dst, Output2D[0], brightness);
        return;

    case 2: // VRAM display
    case 3: // FIFO display
        {
            alignas(16) u32 tmp[256];
            if (display)
            {
                for (int i = 0; i < 256; i++)
                {
                    u16 color = display[i];
                    u32 r = (color & 0x001F) << 1;
                    u32 g = (color & 0x03E0) >> 4;
                    u32 b = (color & 0x7C00) >> 9;
                    tmp[i] = r | (g << 8) | (b << 16);
                }
            }
            else
                memset(tmp, 0, sizeof(tmp));
            WriteLine(dst, tmp, brightness);
        }
        return;
    }
}

void BananaRenderer::OutputLineB(u32* dst, u32 dispcnt, u16 brightness)
{
    if (!((dispcnt >> 16) & 0x1))
    {
        FillLine(dst, 0xFFFFFFFF);
        return;
    }
    WriteLine(dst, Output2D[1], brightness);
}

// the line unit A's display mode 2 or 3 shows, in GPU memory (null: none)
static const u16* DisplaySource(melonDS::GPU& gpu, u32 line)
{
    u32 dispcnt = gpu.GPU2D_A.DispCnt;
    switch ((dispcnt >> 16) & 0x3)
    {
    case 2:
        {
            u32 vrambank = (dispcnt >> 18) & 0x3;
            if (gpu.VRAMMap_LCDC & (1<<vrambank))
                return &((const u16*)gpu.VRAM[vrambank])[line * 256];
            return nullptr;
        }
    case 3:
        return gpu.DispFIFOBuffer;
    }
    return nullptr;
}

void BananaRenderer::DrawScanline(u32 line)
{
    if (!Thread2D)
    {
        DrawScanlineInPlace(line);
        return;
    }

    if (GPU.CaptureEnable)
    {
        // display capture writes the lines back into VRAM at their time: drawn here and now
        if (Drain2D()) StatWaits++;
        Job& job = NewJob();
        u32* line3d;
        PrepareScanlineJob(job, line, line3d);
        Execute(job);
        StatInPlace++;
        if (job.Line < 192)
        {
            Output3D = line3d;
            DoCapture(job.Line);
        }
        return;
    }

    Job& job = NewJob();
    u32* line3d;
    PrepareScanlineJob(job, line, line3d);
    Submit();
}

void BananaRenderer::PrepareScanlineJob(Job& job, u32 line, u32*& line3d)
{
    job.Kind = Job::Scanline;
    u32 dstoffset = 256 * line;
    job.DstA = &Framebuffer[BackBuffer][GPU.ScreenSwap ? 0 : 1][dstoffset];
    job.DstB = &Framebuffer[BackBuffer][GPU.ScreenSwap ? 1 : 0][dstoffset];
    job.ScreensEnabled = GPU.ScreensEnabled;

    // the position used for drawing operations is based on VCOUNT
    line = GPU.VCount;
    job.Line = line;
    line3d = nullptr;
    if (line < 192)
    {
        line3d = Rend3D->GetLine(line);
        if (GPU.GPU2D_A.DispCnt & (1<<3))
            memcpy(job.Line3D, line3d, sizeof(job.Line3D));

        static_cast<BananaRenderer2D&>(*Rend2D_A).PrepareScanline(job.Regs[0]);
        static_cast<BananaRenderer2D&>(*Rend2D_B).PrepareScanline(job.Regs[1]);

        job.BrightnessA = GPU.MasterBrightnessA;
        job.BrightnessB = GPU.MasterBrightnessB;
        const u16* display = DisplaySource(GPU, line);
        job.HasDisplay = display != nullptr;
        if (display)
            memcpy(job.Display, display, sizeof(job.Display));
    }
    job.LogEnd = LogWrite;
}

void BananaRenderer::PrepareSpritesJob(Job& job, u32 line)
{
    job.Kind = Job::Sprites;
    job.Line = line;
    static_cast<BananaRenderer2D&>(*Rend2D_A).PrepareSprites(job.Regs[0]);
    static_cast<BananaRenderer2D&>(*Rend2D_B).PrepareSprites(job.Regs[1]);
    job.LogEnd = LogWrite;
}

void BananaRenderer::DrawScanlineInPlace(u32 line)
{
    u32 *dstA, *dstB;
    u32 dstoffset = 256 * line;
    if (GPU.ScreenSwap)
    {
        dstA = &Framebuffer[BackBuffer][0][dstoffset];
        dstB = &Framebuffer[BackBuffer][1][dstoffset];
    }
    else
    {
        dstA = &Framebuffer[BackBuffer][1][dstoffset];
        dstB = &Framebuffer[BackBuffer][0][dstoffset];
    }

    // the position used for drawing operations is based on VCOUNT
    line = GPU.VCount;
    if (line < 192)
    {
        Output3D = Rend3D->GetLine(line);

        Rend2D_A->DrawScanline(line);
        Rend2D_B->DrawScanline(line);

        if (GPU.ScreensEnabled)
        {
            OutputLineA(dstA, GPU.GPU2D_A.DispCnt, GPU.MasterBrightnessA, DisplaySource(GPU, line));
            OutputLineB(dstB, GPU.GPU2D_B.DispCnt, GPU.MasterBrightnessB);
        }
        else
        {
            FillLine(dstA, 0xFF000000);
            FillLine(dstB, 0xFF000000);
        }

        if (GPU.CaptureEnable)
            DoCapture(line);
    }
    else
    {
        // scanlines outside VCOUNT 0..191 are white (black if the screens are off)
        u32 c = GPU.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000;
        FillLine(dstA, c);
        FillLine(dstB, c);
    }
}

void BananaRenderer::DrawSprites(u32 line)
{
    if (!Thread2D)
    {
        SoftRenderer::DrawSprites(line);
        return;
    }

    if (GPU.CaptureEnable)
    {
        if (Drain2D()) StatWaits++;
        Job& job = NewJob();
        PrepareSpritesJob(job, line);
        Execute(job);
        return;
    }

    Job& job = NewJob();
    PrepareSpritesJob(job, line);
    Submit();
}

// on the 2D thread (or the emulator's thread, while the 2D thread is idle)
void BananaRenderer::Execute(Job& job)
{
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    auto& b = static_cast<BananaRenderer2D&>(*Rend2D_B);

    ApplyLog(job.LogEnd);

    if (job.Kind == Job::Sprites)
    {
        a.DrawSpritesFrom(job.Line, job.Regs[0]);
        b.DrawSpritesFrom(job.Line, job.Regs[1]);
        return;
    }

    if (job.Line < 192)
    {
        a.DrawScanlineFrom(job.Line, job.Regs[0], job.Line3D);
        b.DrawScanlineFrom(job.Line, job.Regs[1], job.Line3D);

        if (job.ScreensEnabled)
        {
            OutputLineA(job.DstA, job.Regs[0].DispCnt, job.BrightnessA, job.HasDisplay ? job.Display : nullptr);
            OutputLineB(job.DstB, job.Regs[1].DispCnt, job.BrightnessB);
        }
        else
        {
            FillLine(job.DstA, 0xFF000000);
            FillLine(job.DstB, 0xFF000000);
        }
    }
    else
    {
        u32 c = job.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000;
        FillLine(job.DstA, c);
        FillLine(job.DstB, c);
    }
}


// ---- the 2D thread ----
//
// Head and Tail count jobs. The emulator's thread fills Jobs[Head % NumJobs] once Head - Tail < NumJobs (the slot's
// last job is drawn: Tail's acquire), then publishes it with Head's release; the 2D thread draws jobs up to Head
// (acquire) and publishes each with Tail's release. The copy log works the same way with LogWrite and LogRead: a
// job's records are written before the job is published, and the space of applied records is reused only after
// LogRead's release. The 2D thread owns the engines' drawing state and copies; the emulator's thread touches them
// only with Tail == Head (Drain2D: capture, Reset, savestates, switching the thread off).
// The 2D thread sleeps on Wake when it has caught up; Kick wakes it every KickBatch jobs, at VBlank, and whenever
// the emulator's thread is about to wait for it.

void BananaRenderer::SetThreaded2D(bool on)
{
    if (on == Thread2D)
        return;
    if (!on)
    {
        StopWorker();
        return;
    }
    if (!Jobs)
        Jobs = std::make_unique<Job[]>(NumJobs);
    if (!Log)
        Log = std::make_unique<u8[]>(LogSize);
    Head.store(0, std::memory_order_relaxed);
    Tail.store(0, std::memory_order_relaxed);
    LogWrite = 0;
    LogRead.store(0, std::memory_order_relaxed);
    Kicked = 0;
    KickPending = false;
    Quit = false;
    static_cast<BananaRenderer2D&>(*Rend2D_A).UseCopies(CopyHook, this);
    static_cast<BananaRenderer2D&>(*Rend2D_B).UseCopies(CopyHook, this);
    Thread2D = true;
    Worker = std::thread([this] { WorkerLoop(); });
}

void BananaRenderer::StopWorker()
{
    if (!Thread2D)
        return;
    Drain2D();
    {
        std::lock_guard<std::mutex> l(M);
        Quit = true;
    }
    Wake.notify_one();
    Worker.join();
    Thread2D = false;
    static_cast<BananaRenderer2D&>(*Rend2D_A).UseCopies(nullptr, nullptr);
    static_cast<BananaRenderer2D&>(*Rend2D_B).UseCopies(nullptr, nullptr);
    Platform::Log(Platform::LogLevel::Info,
                  "[2d] 2D thread: %llu jobs, %llu lines drawn in place (display capture), %.1f MB through the copy "
                  "log, %llu waits for the thread (capture, full ring or log)\n",
                  (unsigned long long)StatJobs, (unsigned long long)StatInPlace, StatLogBytes / 1048576.0,
                  (unsigned long long)StatWaits);
}

// the emulator's thread: a copy for the line being prepared, applied by Execute before it is drawn
void BananaRenderer::LogCopy(void* dst, const void* src, u32 len)
{
    StatLogBytes += len;
    while (len)
    {
        u32 n = len < LogChunk ? len : LogChunk;
        u64 need = sizeof(LogRecord) + ((n + 15) & ~15u);
        u64 pos = LogWrite % LogSize;
        if (LogSize - pos < need)
        {
            // no room before the ring's end: skip to its start
            LogReserve(LogSize - pos);
            *(LogRecord*)&Log[pos] = { nullptr, 0 };
            LogWrite += LogSize - pos;
            pos = 0;
        }
        LogReserve(need);
        *(LogRecord*)&Log[pos] = { (u8*)dst, n };
        memcpy(&Log[pos + sizeof(LogRecord)], src, n);
        LogWrite += need;
        dst = (u8*)dst + n;
        src = (const u8*)src + n;
        len -= n;
    }
}

// room for `bytes` more: the 2D thread frees it as it draws the jobs already handed over. The records of the job
// being prepared are at most the engines' whole copies (a resync), well under LogSize, so this always ends.
void BananaRenderer::LogReserve(u64 bytes)
{
    if (LogWrite + bytes - LogRead.load(std::memory_order_acquire) <= LogSize)
        return;
    StatWaits++;
    Kick();
    while (LogWrite + bytes - LogRead.load(std::memory_order_acquire) > LogSize)
        std::this_thread::yield();
}

// whichever thread draws the job (the 2D thread, or the emulator's while the 2D thread is idle)
void BananaRenderer::ApplyLog(u64 end)
{
    u64 rd = LogRead.load(std::memory_order_relaxed);
    if (rd == end)
        return;
    while (rd < end)
    {
        u64 pos = rd % LogSize;
        const LogRecord& r = *(const LogRecord*)&Log[pos];
        if (!r.Dst)
        {
            rd += LogSize - pos;
            continue;
        }
        memcpy(r.Dst, &Log[pos + sizeof(LogRecord)], r.Len);
        rd += sizeof(LogRecord) + ((r.Len + 15) & ~15ull);
    }
    LogRead.store(rd, std::memory_order_release);
}

void BananaRenderer::WorkerLoop()
{
#ifdef __linux__
    pthread_setname_np(pthread_self(), "bananads-2d");  // for top -H and perf on the handheld
#endif
    u32 tail = Tail.load(std::memory_order_relaxed);
    for (;;)
    {
        if (tail == Head.load(std::memory_order_acquire))
        {
            std::unique_lock<std::mutex> l(M);
            Wake.wait(l, [this] { return Quit || KickPending; });
            if (Quit)
                return;
            KickPending = false;
            continue;
        }
        Execute(Jobs[tail % NumJobs]);
        tail++;
        Tail.store(tail, std::memory_order_release);
    }
}

void BananaRenderer::Kick()
{
    Kicked = Head.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(M);
        KickPending = true;
    }
    Wake.notify_one();
}

BananaRenderer::Job& BananaRenderer::NewJob()
{
    u32 head = Head.load(std::memory_order_relaxed);
    if (head - Tail.load(std::memory_order_acquire) >= NumJobs)
    {
        StatWaits++;
        Kick();
        while (head - Tail.load(std::memory_order_acquire) >= NumJobs)
            std::this_thread::yield();
    }
    return Jobs[head % NumJobs];
}

void BananaRenderer::Submit()
{
    u32 head = Head.load(std::memory_order_relaxed) + 1;
    Head.store(head, std::memory_order_release);
    StatJobs++;
    if (head - Kicked >= KickBatch)
        Kick();
}

bool BananaRenderer::Drain2D()
{
    if (!Thread2D)
        return false;
    u32 head = Head.load(std::memory_order_relaxed);
    if (Tail.load(std::memory_order_acquire) == head)
        return false;
    Kick();
    while (Tail.load(std::memory_order_acquire) != head)
        std::this_thread::yield();
    return true;
}

void BananaRenderer::VBlank()
{
    // the frame's last lines: drawn while the emulator runs VBlank
    if (Thread2D && Head.load(std::memory_order_relaxed) != Kicked)
        Kick();
}

void BananaRenderer::SwapBuffers()
{
    Drain2D();  // the frame's lines are all in the back buffer
    SoftRenderer::SwapBuffers();
}

void BananaRenderer::Reset()
{
    Drain2D();
    SoftRenderer::Reset();
    // GPU::Reset clears the VRAM flats next, without dirty bits
    static_cast<BananaRenderer2D&>(*Rend2D_A).Resync();
    static_cast<BananaRenderer2D&>(*Rend2D_B).Resync();
}

void BananaRenderer::Resync2D()
{
    Drain2D();
    static_cast<BananaRenderer2D&>(*Rend2D_A).Resync();
    static_cast<BananaRenderer2D&>(*Rend2D_B).Resync();
}

void BananaRenderer::PostSavestate()
{
    Resync2D();
    SoftRenderer::PostSavestate();
}

void BananaRenderer::Stop()
{
    Drain2D();
    SoftRenderer::Stop();
}

void BananaRenderer::PreSavestate()
{
    Drain2D();
    SoftRenderer::PreSavestate();
}

}
