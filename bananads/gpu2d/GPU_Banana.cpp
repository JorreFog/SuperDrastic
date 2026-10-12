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
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
#include <arm_neon.h>
#endif

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

void BananaRenderer::SetHires(QuarterFn fn, void* ctx)
{
    Drain2D();
    Quarter = fn;
    QuarterCtx = ctx;
    for (auto& b : HiFB)
        for (auto& f : b)
        {
            f = std::make_unique<u32[]>(512 * 384);
            memset(f.get(), 0, 512 * 384 * sizeof(u32));
        }
}

bool BananaRenderer::GetFramebuffers(void** top, void** bottom)
{
    if (!Quarter)
        return SoftRenderer::GetFramebuffers(top, bottom);
    int frontbuf = BackBuffer ^ 1;
    *top = HiFB[frontbuf][0].get();
    *bottom = HiFB[frontbuf][1].get();
    return true;
}

u32* BananaRenderer::LineDst(int screen, u32 line)
{
    if (Quarter) return &HiFB[BackBuffer][screen][line * 1024];
    return &Framebuffer[BackBuffer][screen][line * 256];
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

#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
// 16 pixels at a time, one plane per byte (ld4/st4): out byte = (c << 2) | (c >> 4), the top two bits of the 6-bit
// channel repeated (Expand); with master brightness on the 6-bit channels first, exactly ColorBrightnessUp(c, f, 0)
// and ColorBrightnessDown(c, f, 0xF) (each channel's result fits in its 6 bits, so the packed masks drop nothing)
static inline uint8x16_t Expand6(uint8x16_t c)
{
    uint8x16_t t = vshlq_n_u8(c, 2);
    return vsriq_n_u8(t, t, 6);
}

template<int mode>
static inline uint8x16_t Bright6(uint8x16_t c, uint8x8_t f)
{
    c = vandq_u8(c, vdupq_n_u8(0x3F));
    if (mode == 1)
    {
        // c + (((63 - c) * f) >> 4)
        uint8x16_t d = vsubq_u8(vdupq_n_u8(0x3F), c);
        uint8x8_t lo = vshrn_n_u16(vmull_u8(vget_low_u8(d), f), 4);
        uint8x16_t r = vshrn_high_n_u16(lo, vmull_high_u8(d, vcombine_u8(f, f)), 4);
        return vaddq_u8(c, r);
    }
    else
    {
        // c - ((c * f + 15) >> 4)
        uint16x8_t bias = vdupq_n_u16(15);
        uint8x8_t lo = vshrn_n_u16(vmlal_u8(bias, vget_low_u8(c), f), 4);
        uint8x16_t r = vshrn_high_n_u16(lo, vmlal_high_u8(bias, c, vcombine_u8(f, f)), 4);
        return vsubq_u8(c, r);
    }
}

template<int mode>
static void WriteLineNeon(u32* dst, const u32* src, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    for (int i = 0; i < 256; i += 16)
    {
        uint8x16x4_t s = vld4q_u8((const u8*)(src + i));
        uint8x16_t r = s.val[0], g = s.val[1], b = s.val[2];
        if (mode)
        {
            r = Bright6<mode>(r, f);
            g = Bright6<mode>(g, f);
            b = Bright6<mode>(b, f);
        }
        uint8x16x4_t o;
        o.val[0] = Expand6(b);
        o.val[1] = Expand6(g);
        o.val[2] = Expand6(r);
        o.val[3] = vdupq_n_u8(0xFF);
        vst4q_u8((u8*)(dst + i), o);
    }
}

// 16 direct-colour pixels (display modes 2 and 3) to the three 6-bit channel planes
static inline void Planes15(const u16* p, uint8x16_t& r, uint8x16_t& g, uint8x16_t& b)
{
    uint16x8_t c0 = vld1q_u16(p), c1 = vld1q_u16(p + 8);
    uint16x8_t m = vdupq_n_u16(0x3E);
    r = vcombine_u8(vmovn_u16(vandq_u16(vshlq_n_u16(c0, 1), m)), vmovn_u16(vandq_u16(vshlq_n_u16(c1, 1), m)));
    g = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(c0, 4), m)), vmovn_u16(vandq_u16(vshrq_n_u16(c1, 4), m)));
    b = vcombine_u8(vmovn_u16(vandq_u16(vshrq_n_u16(c0, 9), m)), vmovn_u16(vandq_u16(vshrq_n_u16(c1, 9), m)));
}

template<int mode>
static void OutputDisplayNeonT(u32* dst, const u16* display, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    for (int i = 0; i < 256; i += 16)
    {
        uint8x16_t r, g, b;
        Planes15(display + i, r, g, b);
        if (mode)
        {
            r = Bright6<mode>(r, f);
            g = Bright6<mode>(g, f);
            b = Bright6<mode>(b, f);
        }
        uint8x16x4_t o;
        o.val[0] = Expand6(b);
        o.val[1] = Expand6(g);
        o.val[2] = Expand6(r);
        o.val[3] = vdupq_n_u8(0xFF);
        vst4q_u8((u8*)(dst + i), o);
    }
}

// display modes 2 and 3: the conversion to the 18-bit format fused with WriteLine
static void OutputDisplayNeon(u32* dst, const u16* display, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1 && factor)      OutputDisplayNeonT<1>(dst, display, factor);
    else if (mode == 2 && factor) OutputDisplayNeonT<2>(dst, display, factor);
    else                          OutputDisplayNeonT<0>(dst, display, 0);
}
#endif

// master brightness and colour expansion in one pass
void BananaRenderer::WriteLine(u32* dst, const u32* src, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;

#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    if (Banana2DFast)
    {
        if (mode == 1 && factor)      WriteLineNeon<1>(dst, src, factor);
        else if (mode == 2 && factor) WriteLineNeon<2>(dst, src, factor);
        else                          WriteLineNeon<0>(dst, src, 0);
        return;
    }
#endif

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
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
            if (Banana2DFast && display)
            {
                OutputDisplayNeon(dst, display, brightness);
                return;
            }
#endif
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

// ---- 2x: a DS line is two framebuffer rows of 512 pixels ----

// a 256-pixel line with each pixel twice, on both rows
static void DoubleLine(u32* dst, const u32* src)
{
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    for (int i = 0; i < 256; i += 4)
    {
        uint32x4_t v = vld1q_u32(src + i);
        uint32x4x2_t z = {{ v, v }};
        vst2q_u32(dst + 2 * i, z);
        vst2q_u32(dst + 512 + 2 * i, z);
    }
#else
    for (int i = 0; i < 256; i++)
        dst[2 * i] = dst[2 * i + 1] = dst[512 + 2 * i] = dst[512 + 2 * i + 1] = src[i];
#endif
}

// a 512-pixel row from its even and odd pixels
static void ZipLine(u32* dst, const u32* even, const u32* odd)
{
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    for (int i = 0; i < 256; i += 4)
    {
        uint32x4x2_t z = {{ vld1q_u32(even + i), vld1q_u32(odd + i) }};
        vst2q_u32(dst + 2 * i, z);
    }
#else
    for (int i = 0; i < 256; i++)
    {
        dst[2 * i] = even[i];
        dst[2 * i + 1] = odd[i];
    }
#endif
}

// unit A's line: at 2x its four composited quarters when the line showed the 3D layer (HiLines 4), else doubled
void BananaRenderer::EmitA(u32* dst, u32 dispcnt, u16 brightness, const u16* display)
{
    if (!Quarter)
    {
        OutputLineA(dst, dispcnt, brightness, display);
        return;
    }
    alignas(16) u32 q[4][256];
    OutputLineA(q[0], dispcnt, brightness, display);
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    if (((dispcnt >> 16) & 0x3) != 1 || a.HiLines == 1)
    {
        DoubleLine(dst, q[0]);
        return;
    }
    for (int k = 1; k < 4; k++)
        WriteLine(q[k], a.HiOut[k - 1], brightness);
    ZipLine(dst, q[0], q[1]);
    ZipLine(dst + 512, q[2], q[3]);
}

void BananaRenderer::EmitB(u32* dst, u32 dispcnt, u16 brightness)
{
    if (!Quarter)
    {
        OutputLineB(dst, dispcnt, brightness);
        return;
    }
    alignas(16) u32 t[256];
    OutputLineB(t, dispcnt, brightness);
    DoubleLine(dst, t);
}

void BananaRenderer::Fill(u32* dst, u32 val)
{
    for (int k = 0; k < (Quarter ? 4 : 1); k++)
        FillLine(dst + 256 * k, val);
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

    // display capture writes unit A's lines back into VRAM at their time: unit A is drawn here and now
    SetAInPlace(GPU.CaptureEnable);

    Job& job = NewJob();
    u32* line3d;
    PrepareScanlineJob(job, line, line3d);
    job.OnlyB = AInPlace;
    Submit();

    if (AInPlace)
    {
        // (the 2D thread reads this job too, for unit B; neither writes it)
        StatInPlace++;
        if (job.Line < 192)
        {
            const u32* q[4] = { job.Line3D[0], job.Line3D[1], job.Line3D[2], job.Line3D[3] };
            static_cast<BananaRenderer2D&>(*Rend2D_A).DrawScanlineFrom(job.Line, job.Regs[0], job.Line3D[0], Quarter ? q : nullptr);
            if (job.ScreensEnabled)
                EmitA(job.DstA, job.Regs[0].DispCnt, job.BrightnessA, job.HasDisplay ? job.Display : nullptr);
            else
                Fill(job.DstA, 0xFF000000);
            Output3D = line3d;
            DoCapture(job.Line);
        }
        else
            Fill(job.DstA, job.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000);
    }
}

// unit A on the emulator's thread or not: switched with the 2D thread done with unit A's jobs
void BananaRenderer::SetAInPlace(bool on)
{
    if (on == AInPlace)
        return;
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    if (on)
    {
        if (Drain2D()) StatWaits++;
        a.SetCopyFn(DirectCopy, nullptr);
    }
    else
        a.SetCopyFn(CopyHook, this);  // (the 2D thread sees what was drawn here through the next job's release)
    AInPlace = on;
}

void BananaRenderer::PrepareScanlineJob(Job& job, u32 line, u32*& line3d)
{
    job.Kind = Job::Scanline;
    job.DstA = LineDst(GPU.ScreenSwap ? 0 : 1, line);
    job.DstB = LineDst(GPU.ScreenSwap ? 1 : 0, line);
    job.ScreensEnabled = GPU.ScreensEnabled;

    // the position used for drawing operations is based on VCOUNT
    line = GPU.VCount;
    job.Line = line;
    line3d = nullptr;
    if (line < 192)
    {
        line3d = Rend3D->GetLine(line);
        // (only read when BG0 is the 3D layer and enabled; render.2d_fast skips the 1 KB copy otherwise)
        if ((GPU.GPU2D_A.DispCnt & (1<<3)) && (!Banana2DFast || (GPU.GPU2D_A.LayerEnable & 1)))
        {
            if (Quarter)
                for (int q = 0; q < 4; q++)
                    memcpy(job.Line3D[q], Quarter(QuarterCtx, line, q), sizeof(job.Line3D[q]));
            else
                memcpy(job.Line3D[0], line3d, sizeof(job.Line3D[0]));
        }

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
    u32* dstA = LineDst(GPU.ScreenSwap ? 0 : 1, line);
    u32* dstB = LineDst(GPU.ScreenSwap ? 1 : 0, line);

    // the position used for drawing operations is based on VCOUNT
    line = GPU.VCount;
    if (line < 192)
    {
        if (Quarter)
        {
            const u32* q[4];
            for (int k = 0; k < 4; k++) q[k] = Quarter(QuarterCtx, line, k);
            Output3D = const_cast<u32*>(q[0]);  // (display capture: quarter 0)
            static_cast<BananaRenderer2D&>(*Rend2D_A).DrawScanlineHires(line, q);
        }
        else
        {
            Output3D = Rend3D->GetLine(line);
            Rend2D_A->DrawScanline(line);
        }
        Rend2D_B->DrawScanline(line);

        if (GPU.ScreensEnabled)
        {
            EmitA(dstA, GPU.GPU2D_A.DispCnt, GPU.MasterBrightnessA, DisplaySource(GPU, line));
            EmitB(dstB, GPU.GPU2D_B.DispCnt, GPU.MasterBrightnessB);
        }
        else
        {
            Fill(dstA, 0xFF000000);
            Fill(dstB, 0xFF000000);
        }

        if (GPU.CaptureEnable)
            DoCapture(line);
    }
    else
    {
        // scanlines outside VCOUNT 0..191 are white (black if the screens are off)
        u32 c = GPU.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000;
        Fill(dstA, c);
        Fill(dstB, c);
    }
}

void BananaRenderer::DrawSprites(u32 line)
{
    if (!Thread2D)
    {
        SoftRenderer::DrawSprites(line);
        return;
    }

    SetAInPlace(GPU.CaptureEnable);

    Job& job = NewJob();
    PrepareSpritesJob(job, line);
    job.OnlyB = AInPlace;
    Submit();

    if (AInPlace)
        static_cast<BananaRenderer2D&>(*Rend2D_A).DrawSpritesFrom(job.Line, job.Regs[0]);
}

// on the 2D thread
void BananaRenderer::Execute(Job& job)
{
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    auto& b = static_cast<BananaRenderer2D&>(*Rend2D_B);

    ApplyLog(job.LogEnd);

    bool doA = !job.OnlyB;

    if (job.Kind == Job::Sprites)
    {
        if (doA) a.DrawSpritesFrom(job.Line, job.Regs[0]);
        b.DrawSpritesFrom(job.Line, job.Regs[1]);
        return;
    }

    if (job.Line < 192)
    {
        const u32* q[4] = { job.Line3D[0], job.Line3D[1], job.Line3D[2], job.Line3D[3] };
        if (doA) a.DrawScanlineFrom(job.Line, job.Regs[0], job.Line3D[0], Quarter ? q : nullptr);
        b.DrawScanlineFrom(job.Line, job.Regs[1], job.Line3D[0]);

        if (job.ScreensEnabled)
        {
            if (doA) EmitA(job.DstA, job.Regs[0].DispCnt, job.BrightnessA, job.HasDisplay ? job.Display : nullptr);
            EmitB(job.DstB, job.Regs[1].DispCnt, job.BrightnessB);
        }
        else
        {
            if (doA) Fill(job.DstA, 0xFF000000);
            Fill(job.DstB, 0xFF000000);
        }
    }
    else
    {
        u32 c = job.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000;
        if (doA) Fill(job.DstA, c);
        Fill(job.DstB, c);
    }
}


// ---- the 2D thread ----
//
// Head and Tail count jobs. The emulator's thread fills Jobs[Head % NumJobs] once Head - Tail < NumJobs (the slot's
// last job is drawn: Tail's acquire), then publishes it with Head's release; the 2D thread draws jobs up to Head
// (acquire) and publishes each with Tail's release. The copy log works the same way with LogWrite and LogRead: a
// job's records are written before the job is published, and the space of applied records is reused only after
// LogRead's release. The 2D thread owns the engines' drawing state and copies; the emulator's thread touches them
// only with Tail == Head (Drain2D: Reset, savestates, switching the thread off), except unit A's while display
// capture is on (SetAInPlace waits for the 2D thread first; its jobs then leave unit A alone).
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
    AInPlace = false;
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
    AInPlace = false;
    Platform::Log(Platform::LogLevel::Info,
                  "[2d] 2D thread: %llu jobs, %llu lines with unit A drawn in place (display capture), %.1f MB through the copy "
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

// the 2D thread, before it draws a job
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
    for (auto& b : HiFB)
        for (auto& f : b)
            if (f) memset(f.get(), 0, 512 * 384 * sizeof(u32));
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
    for (auto& b : HiFB)
        for (auto& f : b)
            if (f) memset(f.get(), 0, 512 * 384 * sizeof(u32));
}

void BananaRenderer::PreSavestate()
{
    Drain2D();
    SoftRenderer::PreSavestate();
}

}
