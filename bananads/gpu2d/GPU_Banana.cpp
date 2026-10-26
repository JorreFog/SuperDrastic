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
#include <sys/resource.h>
#endif
#include "NDS.h"
#include "Platform.h"
#include "GPU_Banana.h"

#include <chrono>
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

void BananaRenderer::SetHires(QuarterFn fn, void* ctx, QuarterRowsFn rows, int scale)
{
    Drain2D();
    Quarter = fn;
    QuarterCtx = ctx;
    QuarterRows = rows;
    Sc = scale == 3 || scale == 4 ? scale : 2;
    NQ = Sc * Sc;
    static_cast<BananaRenderer2D&>(*Rend2D_A).HiN = NQ;
    for (auto& b : HiFB)
        for (auto& f : b)
        {
            f = std::make_unique<u32[]>(256 * 192 * NQ);
            memset(f.get(), 0, 256 * 192 * NQ * sizeof(u32));
        }
}

bool BananaRenderer::GetFramebuffers(void** top, void** bottom)
{
    if (LazyFrames)
        WaitFence(FenceCur);  // (render.2d_lazy: the front frame's last lines may still be being drawn)
    if (Front[0])
    {
        *top = Front[0];
        *bottom = Front[1];
        return true;
    }
    if (!Quarter)
        return SoftRenderer::GetFramebuffers(top, bottom);
    int frontbuf = BackBuffer ^ 1;
    *top = HiFB[frontbuf][0].get();
    *bottom = HiFB[frontbuf][1].get();
    return true;
}

void BananaRenderer::SetOutput(u32* top, u32* bottom)
{
    // the frontend's buffers going away (null): with render.2d_lazy the 2D thread may still be drawing the last
    // frame into them, so it finishes first (a quit without the resume state's drain wrote into unmapped memory)
    if (!top)
    {
        Drain2D();
        if (OutSkipping) OutReset();   // (the 2D thread is idle: drained)
    }
    Out[0] = top;
    Out[1] = bottom;
    if (!top)
        Front[0] = Front[1] = nullptr;
}

u32* BananaRenderer::LineDst(int screen, u32 line)
{
    if (Out[screen]) return Out[screen] + line * (Quarter ? 256 * NQ : 256);
    if (Quarter) return &HiFB[BackBuffer][screen][line * 256 * NQ];
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

// 2x: WriteLine of two 256-pixel lines and ZipLine of the results (a 512-pixel row of their pixels in turn), in one
// pass: the same bytes as the two passes, without the 2 KB in between
template<int mode>
static void WriteZipNeon(u32* dst, const u32* even, const u32* odd, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    const uint8x16_t ff = vdupq_n_u8(0xFF);
    for (int i = 0; i < 256; i += 16)
    {
        uint8x16x4_t e = vld4q_u8((const u8*)(even + i));
        uint8x16x4_t o = vld4q_u8((const u8*)(odd + i));
        uint8x16_t er = e.val[0], eg = e.val[1], eb = e.val[2];
        uint8x16_t orr = o.val[0], og = o.val[1], ob = o.val[2];
        if (mode)
        {
            er = Bright6<mode>(er, f); eg = Bright6<mode>(eg, f); eb = Bright6<mode>(eb, f);
            orr = Bright6<mode>(orr, f); og = Bright6<mode>(og, f); ob = Bright6<mode>(ob, f);
        }
        er = Expand6(er); eg = Expand6(eg); eb = Expand6(eb);
        orr = Expand6(orr); og = Expand6(og); ob = Expand6(ob);
        uint8x16x4_t lo, hi;
        lo.val[0] = vzip1q_u8(eb, ob); hi.val[0] = vzip2q_u8(eb, ob);
        lo.val[1] = vzip1q_u8(eg, og); hi.val[1] = vzip2q_u8(eg, og);
        lo.val[2] = vzip1q_u8(er, orr); hi.val[2] = vzip2q_u8(er, orr);
        lo.val[3] = ff; hi.val[3] = ff;
        vst4q_u8((u8*)(dst + 2 * i), lo);
        vst4q_u8((u8*)(dst + 2 * i + 16), hi);
    }
}

// 2x: WriteLine and DoubleLine in one pass (each pixel twice, on both rows)
template<int mode>
static void WriteDoubleNeon(u32* dst, const u32* src, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    const uint8x16_t ff = vdupq_n_u8(0xFF);
    for (int i = 0; i < 256; i += 16)
    {
        uint8x16x4_t s = vld4q_u8((const u8*)(src + i));
        uint8x16_t r = s.val[0], g = s.val[1], b = s.val[2];
        if (mode)
        {
            r = Bright6<mode>(r, f); g = Bright6<mode>(g, f); b = Bright6<mode>(b, f);
        }
        r = Expand6(r); g = Expand6(g); b = Expand6(b);
        uint8x16x4_t lo, hi;
        lo.val[0] = vzip1q_u8(b, b); hi.val[0] = vzip2q_u8(b, b);
        lo.val[1] = vzip1q_u8(g, g); hi.val[1] = vzip2q_u8(g, g);
        lo.val[2] = vzip1q_u8(r, r); hi.val[2] = vzip2q_u8(r, r);
        lo.val[3] = ff; hi.val[3] = ff;
        vst4q_u8((u8*)(dst + 2 * i), lo);
        vst4q_u8((u8*)(dst + 2 * i + 16), hi);
        vst4q_u8((u8*)(dst + 512 + 2 * i), lo);
        vst4q_u8((u8*)(dst + 512 + 2 * i + 16), hi);
    }
}

static void WriteZip(u32* dst, const u32* even, const u32* odd, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1 && factor)      WriteZipNeon<1>(dst, even, odd, factor);
    else if (mode == 2 && factor) WriteZipNeon<2>(dst, even, odd, factor);
    else                          WriteZipNeon<0>(dst, even, odd, 0);
}

static void WriteDouble(u32* dst, const u32* src, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1 && factor)      WriteDoubleNeon<1>(dst, src, factor);
    else if (mode == 2 && factor) WriteDoubleNeon<2>(dst, src, factor);
    else                          WriteDoubleNeon<0>(dst, src, 0);
}

// ---- 3x (render.scale=3, experimental): a DS line is three rows of 768 pixels ----

// 16 pixels' expanded channel planes as four vectors of BGRA pixels (what vst4q_u8 of b, g, r, 0xFF stores)
static inline uint32x4x4_t Pixels16(uint8x16_t r, uint8x16_t g, uint8x16_t b)
{
    const uint8x16_t ff = vdupq_n_u8(0xFF);
    uint16x8_t bglo = vreinterpretq_u16_u8(vzip1q_u8(b, g)), bghi = vreinterpretq_u16_u8(vzip2q_u8(b, g));
    uint16x8_t rflo = vreinterpretq_u16_u8(vzip1q_u8(r, ff)), rfhi = vreinterpretq_u16_u8(vzip2q_u8(r, ff));
    uint32x4x4_t o;
    o.val[0] = vreinterpretq_u32_u16(vzip1q_u16(bglo, rflo));
    o.val[1] = vreinterpretq_u32_u16(vzip2q_u16(bglo, rflo));
    o.val[2] = vreinterpretq_u32_u16(vzip1q_u16(bghi, rfhi));
    o.val[3] = vreinterpretq_u32_u16(vzip2q_u16(bghi, rfhi));
    return o;
}

template<int mode>
static inline uint32x4x4_t Load16(const u32* src, uint8x8_t f)
{
    uint8x16x4_t s = vld4q_u8((const u8*)src);
    uint8x16_t r = s.val[0], g = s.val[1], b = s.val[2];
    if (mode)
    {
        r = Bright6<mode>(r, f); g = Bright6<mode>(g, f); b = Bright6<mode>(b, f);
    }
    return Pixels16(Expand6(r), Expand6(g), Expand6(b));
}

// WriteLine of N 256-pixel lines (N = 3, 4: pixels Nk .. Nk + N - 1 of a row of 256 N) and the row from them
template<int mode, int N>
static void WriteZipNNeon(u32* dst, const u32* const* src, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    for (int i = 0; i < 256; i += 16)
    {
        uint32x4x4_t p[N];
        for (int n = 0; n < N; n++) p[n] = Load16<mode>(src[n] + i, f);
        for (int k = 0; k < 4; k++)
        {
            if (N == 3)
            {
                uint32x4x3_t z = {{ p[0].val[k], p[1].val[k], p[2].val[k] }};
                vst3q_u32(dst + 3 * i + 12 * k, z);
            }
            else
            {
                uint32x4x4_t z = {{ p[0].val[k], p[1].val[k], p[2].val[k], p[N - 1].val[k] }};
                vst4q_u32(dst + 4 * i + 16 * k, z);
            }
        }
    }
}

// WriteLine with each pixel N times on N rows of 256 N
template<int mode, int N>
static void WriteRepNNeon(u32* dst, const u32* src, u32 factor)
{
    uint8x8_t f = vdup_n_u8((u8)factor);
    for (int i = 0; i < 256; i += 16)
    {
        uint32x4x4_t p = Load16<mode>(src + i, f);
        for (int k = 0; k < 4; k++)
            for (int r = 0; r < N; r++)
            {
                if (N == 3)
                {
                    uint32x4x3_t z = {{ p.val[k], p.val[k], p.val[k] }};
                    vst3q_u32(dst + 768 * r + 3 * i + 12 * k, z);
                }
                else
                {
                    uint32x4x4_t z = {{ p.val[k], p.val[k], p.val[k], p.val[k] }};
                    vst4q_u32(dst + 1024 * r + 4 * i + 16 * k, z);
                }
            }
    }
}

template<int N>
static void WriteZipN(u32* dst, const u32* const* src, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1 && factor)      WriteZipNNeon<1, N>(dst, src, factor);
    else if (mode == 2 && factor) WriteZipNNeon<2, N>(dst, src, factor);
    else                          WriteZipNNeon<0, N>(dst, src, 0);
}

template<int N>
static void WriteRepN(u32* dst, const u32* src, u16 brightness)
{
    u32 mode = brightness >> 14;
    u32 factor = brightness & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1 && factor)      WriteRepNNeon<1, N>(dst, src, factor);
    else if (mode == 2 && factor) WriteRepNNeon<2, N>(dst, src, factor);
    else                          WriteRepNNeon<0, N>(dst, src, 0);
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

void BananaRenderer::OutputLineB(u32* dst, u32 dispcnt, u16 brightness, const u32* src)
{
    if (!((dispcnt >> 16) & 0x1))
    {
        FillLine(dst, 0xFFFFFFFF);
        return;
    }
    WriteLine(dst, src ? src : Output2D[1], brightness);
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

// 3x, 4x: a 256-pixel line with each pixel n times, on n rows of 256 n
static void RepLine(u32* dst, const u32* src, int n)
{
    for (int i = 0; i < 256; i++)
        for (int r = 0; r < n; r++)
            for (int c = 0; c < n; c++)
                dst[256 * n * r + n * i + c] = src[i];
}

// 3x, 4x: a row of 256 n pixels from its pixels nk + c (src[c])
static void ZipNLine(u32* dst, const u32* const* src, int n)
{
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < n; c++)
            dst[n * i + c] = src[c][i];
}

// unit A's line at 3x or 4x: its Sc * Sc composited quarters (HiLines NQ), else each pixel repeated
void BananaRenderer::EmitAN(u32* dst, u32 dispcnt, u16 brightness, const u16* display)
{
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    const int w = 256 * Sc;
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    if (Banana2DFast && ((dispcnt >> 16) & 0x3) == 1)
    {
        if (a.HiLines == 1)
        {
            if (Sc == 3) WriteRepN<3>(dst, Output2D[0], brightness);
            else         WriteRepN<4>(dst, Output2D[0], brightness);
            return;
        }
        const u32* src[16];
        src[0] = Output2D[0];
        for (int k = 1; k < NQ; k++) src[k] = a.HiOut[k - 1];
        for (int r = 0; r < Sc; r++)
        {
            if (Sc == 3) WriteZipN<3>(dst + w * r, src + 3 * r, brightness);
            else         WriteZipN<4>(dst + w * r, src + 4 * r, brightness);
        }
        return;
    }
#endif
    alignas(16) u32 q[16][256];
    OutputLineA(q[0], dispcnt, brightness, display);
    if (((dispcnt >> 16) & 0x3) != 1 || a.HiLines == 1)
    {
        RepLine(dst, q[0], Sc);
        return;
    }
    const u32* src[16];
    for (int k = 0; k < NQ; k++)
    {
        if (k) WriteLine(q[k], a.HiOut[k - 1], brightness);
        src[k] = q[k];
    }
    for (int r = 0; r < Sc; r++)
        ZipNLine(dst + w * r, src + Sc * r, Sc);
}

// unit A's line: at 2x its four composited quarters when the line showed the 3D layer (HiLines 4), else doubled
void BananaRenderer::EmitA(u32* dst, u32 dispcnt, u16 brightness, const u16* display)
{
    if (!Quarter)
    {
        OutputLineA(dst, dispcnt, brightness, display);
        return;
    }
    if (Sc >= 3)
    {
        EmitAN(dst, dispcnt, brightness, display);
        return;
    }
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    // (render.2d_fast: the output stage and the 2x rows in one pass: WriteZip, WriteDouble)
    if (Banana2DFast && ((dispcnt >> 16) & 0x3) == 1)
    {
        if (a.HiLines == 1)
            WriteDouble(dst, Output2D[0], brightness);
        else
        {
            WriteZip(dst, Output2D[0], a.HiOut[0], brightness);
            WriteZip(dst + 512, a.HiOut[1], a.HiOut[2], brightness);
        }
        return;
    }
#endif
    alignas(16) u32 q[4][256];
    OutputLineA(q[0], dispcnt, brightness, display);
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

void BananaRenderer::EmitB(u32* dst, u32 dispcnt, u16 brightness, const u32* src)
{
    if (!Quarter)
    {
        OutputLineB(dst, dispcnt, brightness, src);
        return;
    }
#if defined(__aarch64__) && !defined(BANANA_NO_SIMD)
    if (Banana2DFast && ((dispcnt >> 16) & 0x1))
    {
        if (Sc == 3) WriteRepN<3>(dst, src ? src : Output2D[1], brightness);
        else if (Sc == 4) WriteRepN<4>(dst, src ? src : Output2D[1], brightness);
        else WriteDouble(dst, src ? src : Output2D[1], brightness);
        return;
    }
#endif
    alignas(16) u32 t[256];
    OutputLineB(t, dispcnt, brightness, src);
    if (Sc >= 3) RepLine(dst, t, Sc);
    else DoubleLine(dst, t);
}

void BananaRenderer::Fill(u32* dst, u32 val)
{
    for (int k = 0; k < (Quarter ? NQ : 1); k++)
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

    FlushPending();

    // display capture writes unit A's lines back into VRAM at their time: unit A is drawn here and now (when the
    // capture reads them)
    SetAInPlace(CaptureNeedsA(GPU.VCount));

    Job& job = NewJob();
    u32* line3d;
    PrepareScanlineJob(job, line, line3d);
    job.OnlyB = AInPlace;
    // render.2d_merge: GPU::StartHBlank prepares the next sprite line right after this one (line < 191): that goes
    // into this job, which is handed over then. Not while display capture is on (capture's lines go as before)
    if (Merging && line < 191 && !AInPlace && !GPU.CaptureEnable)
    {
        Pending = true;
        return;
    }
    Submit(1);

    if (AInPlace)
    {
        // (the 2D thread reads this job too, for unit B; neither writes it)
        StatInPlace++;
        if (job.Line < 192)
        {
            static_cast<BananaRenderer2D&>(*Rend2D_A).DrawScanlineFrom(job.Line, job.Regs[0], job.Q3D[0], Quarter ? job.Q3D : nullptr);
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
    else if (GPU.CaptureEnable && job.Line < CaptureHeight())
    {
        // a capture of the 3D layer and/or source B: unit A stays on the 2D thread (Output2D isn't read)
        Output3D = line3d ? line3d : Rend3D->GetLine(job.Line);
        DoCapture(job.Line);
    }
}

// does this line's display capture read unit A's 2D line (Output2D[0])? With render.cap_unit_a, only a capture with
// source A (modes 0, 2, 3) of the graphics screen (DISPCAPCNT bit 24 clear) on a line it covers; source B (VRAM or the
// main memory FIFO) and the 3D layer are read on this thread without unit A's drawing
bool BananaRenderer::CaptureNeedsA(u32 line) const
{
    if (!GPU.CaptureEnable)
        return false;
    if (!BananaCapNeedA)
        return true;
    u32 cnt = GPU.CaptureCnt;
    if (((cnt >> 29) & 0x3) == 1 || (cnt & (1<<24)))
        return false;
    return line < CaptureHeight();
}

// the lines DISPCAPCNT's size covers (DoCapture's height)
u32 BananaRenderer::CaptureHeight() const
{
    u32 sz = (GPU.CaptureCnt >> 20) & 0x3;
    return sz ? 64 * sz : 128;
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
    job.Kind = ScanBit;
    job.DstA = LineDst(GPU.ScreenSwap ? 0 : 1, line);
    job.DstB = LineDst(GPU.ScreenSwap ? 1 : 0, line);
    job.BaseA = Out[GPU.ScreenSwap ? 0 : 1];
    job.BaseB = Out[GPU.ScreenSwap ? 1 : 0];
    job.Row = line;
    job.ScreensEnabled = GPU.ScreensEnabled;

    // the position used for drawing operations is based on VCOUNT
    line = GPU.VCount;
    job.Line = line;
    job.WaitRows = false;
    line3d = nullptr;
    if (line < 192)
    {
        // render.3d_wait2d: with the 3D frame's own rows and unit A on the 2D thread, the emulator's thread doesn't
        // wait for them (GetLine would): the 2D thread does, before it composites the line
        const bool rows = Quarter && Banana3DShare && Banana3DWait2D && QuarterRow && !AInPlace && QuarterRows &&
            QuarterRows(QuarterCtx);
        if (!rows)
            line3d = Rend3D->GetLine(line);
        for (int q = 0; q < NQ; q++) job.Q3D[q] = job.Line3D[q];
        // (only read when BG0 is the 3D layer and enabled; render.2d_fast skips the 1 KB copy otherwise)
        if ((GPU.GPU2D_A.DispCnt & (1<<3)) && (!Banana2DFast || (GPU.GPU2D_A.LayerEnable & 1)))
        {
            if (rows)
            {
                for (int q = 0; q < NQ; q++) job.Q3D[q] = QuarterRow(QuarterCtx, line, q);
                job.WaitRows = true;
                job.RowsGen = RowsGen(QuarterCtx);
            }
            else if (Quarter && Banana3DShare && QuarterRows && QuarterRows(QuarterCtx))
            {
                // render.3d_share: the 3D frame's own rows, not a 4 KB copy
                for (int q = 0; q < NQ; q++) job.Q3D[q] = Quarter(QuarterCtx, line, q);
            }
            else if (Quarter)
                for (int q = 0; q < NQ; q++)
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
            memcpy(job.Display, display, 256 * sizeof(u16));
    }
    job.LogEndScan = job.LogEnd = LogWrite;
}

// a sprite line: a job of its own, or the second half of the pending merged job (render.2d_merge)
void BananaRenderer::PrepareSpritesJob(Job& job, u32 line)
{
    job.Kind |= SpriteBit;
    job.SpriteLine = line;
    static_cast<BananaRenderer2D&>(*Rend2D_A).PrepareSprites(job.SRegs[0]);
    static_cast<BananaRenderer2D&>(*Rend2D_B).PrepareSprites(job.SRegs[1]);
    job.LogEnd = LogWrite;
}

// a merged job's scanline half handed over alone (anything but its sprite line came next)
void BananaRenderer::FlushPending()
{
    if (!Pending)
        return;
    Pending = false;
    Submit(1);
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
            const u32* q[16];
            for (int k = 0; k < NQ; k++) q[k] = Quarter(QuarterCtx, line, k);
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

    if (Pending && !CaptureNeedsA(line))
    {
        // render.2d_merge: the scanline prepared just before, in the same job (its OnlyB: unit A not in place)
        Pending = false;
        Job& job = Jobs[LocalHead % NumJobs];
        PrepareSpritesJob(job, line);
        Submit(2);
        return;
    }
    FlushPending();

    SetAInPlace(CaptureNeedsA(line));

    Job& job = NewJob();
    job.Kind = 0;
    PrepareSpritesJob(job, line);
    job.OnlyB = AInPlace;
    Submit(1);

    if (AInPlace)
        static_cast<BananaRenderer2D&>(*Rend2D_A).DrawSpritesFrom(job.SpriteLine, job.SRegs[0]);
}

// on the 2D thread
void BananaRenderer::Execute(Job& job)
{
    auto& a = static_cast<BananaRenderer2D&>(*Rend2D_A);
    auto& b = static_cast<BananaRenderer2D&>(*Rend2D_B);

    bool doA = !job.OnlyB;

    if (job.Kind & ScanBit)
    {
        ApplyLog(job.LogEndScan);
        ExecuteScanline(job, a, b, doA);
    }
    if (job.Kind & SpriteBit)
    {
        ApplyLog(job.LogEnd);
        if (doA) a.DrawSpritesFrom(job.SpriteLine, job.SRegs[0]);
        b.DrawSpritesFrom(job.SpriteLine, job.SRegs[1]);
        if (Skipping)
        {
            // (render.2d_skip: what unit B's next scanline reads of the sprites: this line's OBJ line, made from these
            // registers and memory)
            SprKeyB.Line = job.SpriteLine;
            SprKeyB.Gen = GenB;
            SprKeyB.Regs = job.SRegs[1];
        }
    }
}

// render.2d_skip (the 2D thread): unit B's scanline from its line cache when everything the drawing reads is as it was
// when the cached line was drawn: the same registers (with what Prepare* found changed), the same sprite line before
// it (its registers, and memory as then), no change to the unit's memory copies since (GenB counts the copy log's
// records that changed them, byte for byte), and the same engine state (CacheEpoch: reset, savestate, new copies).
// The drawing is a function of exactly these, so the cached line is the line it would draw. Returns the line.
const u32* BananaRenderer::DrawBCached(Job& job, BananaRenderer2D& b)
{
    u32* dst = &CacheB[job.Line * 256];
    LineKey& k = KeysB[job.Line];
    LineKey cur;
    memset(&cur, 0, sizeof(cur));
    cur.Epoch = b.CacheEpoch;
    cur.Valid = 1;
    cur.GenScan = GenB;
    cur.Spr = SprKeyB;
    cur.Regs = job.Regs[1];
    // (OBJ mosaic rewrites the whole sprite line in place as the line is drawn: such lines are always drawn)
    if (!job.Regs[1].OBJMosaicSize[0] && memcmp(&cur, &k, sizeof(cur)) == 0)
    {
        b.ScanlineState(job.Regs[1]);
        StatSkipped++;
        return dst;
    }
    b.DrawScanlineFrom(job.Line, job.Regs[1], job.Q3D[0], nullptr, dst);
    memcpy(&k, &cur, sizeof(k));
    if (VerB) VerB[job.Line] = ++VerCtr;
    return dst;
}

// ---- render.2d_out_skip (the 2D thread, but OutputTouched) ----

void BananaRenderer::OutReset()
{
    if (!OutSlots)
        return;
    for (int i = 0; i < 8; i++)
    {
        OutSlots[i].Base = nullptr;
        OutSlots[i].Seen = OutSlots[i].Touch.load(std::memory_order_acquire);
        memset(OutSlots[i].K, 0, sizeof(OutSlots[i].K));
    }
    OutNext = 0;
}

BananaRenderer::OutSlot* BananaRenderer::FindOut(u32* base, bool make)
{
    for (int i = 0; i < 8; i++)
        if (OutSlots[i].Base == base)
            return &OutSlots[i];
    if (!make)
        return nullptr;
    OutSlot* s = &OutSlots[OutNext++ & 7];   // (more buffers than 8: the oldest forgotten, its rows drawn again)
    s->Base = base;
    s->Seen = s->Touch.load(std::memory_order_acquire);
    memset(s->K, 0, sizeof(s->K));
    return s;
}

void BananaRenderer::OutForget(u32* base, u32 row)
{
    if (!OutSkipping || Sc < 3 || !base || row >= 192)
        return;
    if (OutSlot* s = FindOut(base, false))
        s->K[row].Valid = 0;
}

// true: unit B's line is already in the buffer's row (written from the same cached line, brightness and display bit,
// with nothing else written there since): EmitB isn't needed. Else the key is recorded for the EmitB that follows
bool BananaRenderer::OutSkipB(const Job& job, const u32* srcB)
{
    if (!OutSkipping || Sc < 3 || !job.BaseB || job.Row >= 192)
        return false;
    OutSlot* s = FindOut(job.BaseB, true);
    u32 t = s->Touch.load(std::memory_order_acquire);
    if (t != s->Seen)
    {
        memset(s->K, 0, sizeof(s->K));
        s->Seen = t;
    }
    OutKey& k = s->K[job.Row];
    if (!srcB || job.Row != job.Line)
    {
        k.Valid = 0;
        return false;
    }
    OutKey cur{ VerB[job.Line], job.BrightnessB, (u8)((job.Regs[1].DispCnt >> 16) & 1), 1 };
    if (k.Valid && k.Ver == cur.Ver && k.Bright == cur.Bright && k.On == cur.On)
    {
        StatOutSkipped++;
        return true;
    }
    k = cur;
    return false;
}

void BananaRenderer::OutputTouched(const void* base)
{
    if (!OutSlots)
        return;
    for (int i = 0; i < 8; i++)
        if ((const void*)OutSlots[i].Base == base)
            OutSlots[i].Touch.fetch_add(1, std::memory_order_acq_rel);
}

void BananaRenderer::ExecuteScanline(Job& job, BananaRenderer2D& a, BananaRenderer2D& b, bool doA)
{
    if (job.Line < 192)
    {
        if (doA && job.WaitRows) WaitRows(QuarterCtx, job.Line, job.RowsGen);
        if (doA) a.DrawScanlineFrom(job.Line, job.Regs[0], job.Q3D[0], Quarter ? job.Q3D : nullptr);
        const u32* srcB = nullptr;
        if (Skipping)
            srcB = DrawBCached(job, b);
        else
            b.DrawScanlineFrom(job.Line, job.Regs[1], job.Q3D[0]);

        OutForget(job.BaseA, job.Row);   // (unit A's row, drawn here or on the emulator's thread)
        if (job.ScreensEnabled)
        {
            if (doA) EmitA(job.DstA, job.Regs[0].DispCnt, job.BrightnessA, job.HasDisplay ? job.Display : nullptr);
            if (!OutSkipB(job, srcB))
                EmitB(job.DstB, job.Regs[1].DispCnt, job.BrightnessB, srcB);
            else if (Banana2DVerify)
            {
                // (render.2d_verify: the row as EmitB would write it, against what the buffer holds)
                static thread_local std::unique_ptr<u32[]> ref;
                if (!ref) ref = std::make_unique<u32[]>(256 * 16);
                EmitB(ref.get(), job.Regs[1].DispCnt, job.BrightnessB, srcB);
                Banana2DVerifyCount(2, memcmp(ref.get(), job.DstB, 256 * NQ * sizeof(u32)) == 0);
            }
        }
        else
        {
            if (doA) Fill(job.DstA, 0xFF000000);
            OutForget(job.BaseB, job.Row);
            Fill(job.DstB, 0xFF000000);
        }
    }
    else
    {
        u32 c = job.ScreensEnabled ? 0xFFFFFFFF : 0xFF000000;
        OutForget(job.BaseA, job.Row);
        OutForget(job.BaseB, job.Row);
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
// render.2d_ring=split (SetSplitRing): Head, Tail and LogRead are the ones on PLine and CLine (cache lines of their
// own), and the emulator's thread keeps the last Tail and LogRead it read (TailSeen, LogReadSeen) and reads them
// again only when the ring or the log looks full by those: they only grow, so a stale value only says "full" too
// early, and the acquire that read it still orders the slots and the log space it freed. Shared: the members
// next to Kicked and the mutex, read every time (the old code).

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
    {
        Jobs = std::make_unique<Job[]>(NumJobs);
        JobLine3D = std::make_unique<u32[]>(NumJobs * NQ * 256);
        JobDisplay = std::make_unique<u16[]>(NumJobs * 256);
        for (u32 i = 0; i < NumJobs; i++)
        {
            Jobs[i].Line3D = (u32 (*)[256])&JobLine3D[i * NQ * 256];
            Jobs[i].Display = &JobDisplay[i * 256];
        }
    }
    if (!Log)
        Log = std::make_unique<u8[]>(LogSize);
    RingSplit = SplitRing;
    HeadP = RingSplit ? &PLine.Head : &Head;
    TailP = RingSplit ? &CLine.Tail : &Tail;
    LogReadP = RingSplit ? &CLine.LogRead : &LogRead;
    HeadP->store(0, std::memory_order_relaxed);
    TailP->store(0, std::memory_order_relaxed);
    TailSeen = 0;
    LogWrite = 0;
    LogReadP->store(0, std::memory_order_relaxed);
    LogReadSeen = 0;
    Kicked = 0;
    LocalHead = 0;
    Units = KickedUnits = 0;
    Pending = false;
    Merging = Merge;
    Skipping = SkipLines;
    if (Skipping && !CacheB)
    {
        CacheB = std::make_unique<u32[]>(192 * 256);
        KeysB = std::make_unique<LineKey[]>(192);
    }
    if (Skipping)
        memset(KeysB.get(), 0, 192 * sizeof(LineKey));  // (Valid 0: nothing cached)
    OutSkipping = Skipping && OutSkipOn;
    if (OutSkipping && !OutSlots)
    {
        OutSlots = std::make_unique<OutSlot[]>(8);
        VerB = std::make_unique<u64[]>(192);
    }
    if (OutSkipping)
    {
        OutReset();
        memset(VerB.get(), 0, 192 * sizeof(u64));
    }
    memset(&SprKeyB, 0, sizeof(SprKeyB));
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
                  "log, %llu waits for the thread (capture, full ring or log), %llu of unit B's lines from its cache (render.2d_skip)\n",
                  (unsigned long long)StatJobs, (unsigned long long)StatInPlace, StatLogBytes / 1048576.0,
                  (unsigned long long)StatWaits, (unsigned long long)StatSkipped);
    if (OutSkipping)
        Platform::Log(Platform::LogLevel::Info, "[2d] %llu of unit B's lines already in the frontend's buffer, not written "
                      "again (render.2d_out_skip)\n", (unsigned long long)StatOutSkipped);
    OutSkipping = false;
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
    if (RingSplit)
    {
        if (LogWrite + bytes - LogReadSeen <= LogSize)
            return;
        LogReadSeen = LogReadP->load(std::memory_order_acquire);
        if (LogWrite + bytes - LogReadSeen <= LogSize)
            return;
    }
    else if (LogWrite + bytes - LogRead.load(std::memory_order_acquire) <= LogSize)
        return;
    StatWaits++;
    Kick();
    WaitForWorker([&] { return LogWrite + bytes - LogReadP->load(std::memory_order_acquire) <= LogSize; });
}

// the 2D thread, before it draws a job
void BananaRenderer::ApplyLog(u64 end)
{
    u64 rd = LogReadP->load(std::memory_order_relaxed);
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
        const u8* src = &Log[pos + sizeof(LogRecord)];
        // (render.2d_skip: a record that changes unit B's copies)
        if (Skipping && static_cast<BananaRenderer2D&>(*Rend2D_B).InCopies(r.Dst) && memcmp(r.Dst, src, r.Len))
            GenB++;
        memcpy(r.Dst, src, r.Len);
        rd += sizeof(LogRecord) + ((r.Len + 15) & ~15ull);
    }
    LogReadP->store(rd, std::memory_order_release);
}

void BananaRenderer::WorkerLoop()
{
#ifdef __linux__
    pthread_setname_np(pthread_self(), "bananads-2d");  // for top -H and perf on the handheld
    if (Nice) setpriority(PRIO_PROCESS, 0, Nice);
#endif
    std::atomic<u32>* const headp = HeadP;
    std::atomic<u32>* const tailp = TailP;
    u32 tail = tailp->load(std::memory_order_relaxed);
    for (;;)
    {
        if (tail == headp->load(std::memory_order_acquire))
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
        tailp->store(tail, std::memory_order_seq_cst);
        if (FenceWaiting.load(std::memory_order_seq_cst))
        {
            // (render.2d_fence_wait: a waiter; the flag is cleared here and set again by a waiter that still waits)
            FenceWaiting.store(false, std::memory_order_relaxed);
            { std::lock_guard<std::mutex> l(FenceM); }
            FenceCv.notify_all();
        }
        if (TestDelayUs) [[unlikely]]
            std::this_thread::sleep_for(std::chrono::microseconds(TestDelayUs));
    }
}

void BananaRenderer::Kick()
{
    if (Merging)
        HeadP->store(LocalHead, std::memory_order_release);  // (render.2d_merge: the jobs handed over here)
    Kicked = LocalHead;
    KickedUnits = Units;
    {
        std::lock_guard<std::mutex> l(M);
        KickPending = true;
    }
    Wake.notify_one();
}

BananaRenderer::Job& BananaRenderer::NewJob()
{
    u32 head = LocalHead;
    if (RingSplit)
    {
        if (head - TailSeen < NumJobs)
            return Jobs[head % NumJobs];
        TailSeen = TailP->load(std::memory_order_acquire);
        if (head - TailSeen < NumJobs)
            return Jobs[head % NumJobs];
    }
    else if (head - Tail.load(std::memory_order_acquire) < NumJobs)
        return Jobs[head % NumJobs];
    StatWaits++;
    Kick();
    WaitForWorker([&] { return head - TailP->load(std::memory_order_acquire) < NumJobs; });
    return Jobs[head % NumJobs];
}

// units: the lines in the job (a merged job is two), counted for render.2d_kick as the jobs before merging were
void BananaRenderer::Submit(u32 units)
{
    u32 head = ++LocalHead;
    // (render.2d_merge: every 8th job too, without a wake-up, so a 2D thread still awake goes on with them as it did
    // with a store per job: the frame's last lines aren't all left for the wake-up at VBlank)
    if (!Merging || !(head & 7))
        HeadP->store(head, std::memory_order_release);
    StatJobs += units;
    Units += units;
    if (Units - KickedUnits >= KickBatch)
        Kick();
}

// a wait for the 2D thread (render.2d_fence_wait). Blocking: the flag is set with sequential consistency before the
// condition is tested again, and the 2D thread publishes Tail the same way before it reads the flag, so either the
// re-test sees the new Tail or the 2D thread sees the flag and notifies under the mutex; the timed wait is a safety
// net only. Spinning (the default): as before
template<class Pred> void BananaRenderer::WaitForWorker(Pred pred) const
{
    if (!FenceBlock)
    {
        while (!pred())
            std::this_thread::yield();
        return;
    }
    while (!pred())
    {
        FenceWaiting.store(true, std::memory_order_seq_cst);
        std::unique_lock<std::mutex> l(FenceM);
        if (pred())
            break;
        FenceCv.wait_for(l, std::chrono::milliseconds(1));
    }
}

bool BananaRenderer::Drain2D()
{
    if (!Thread2D)
        return false;
    FlushPending();
    u32 head = LocalHead;
    if (TailP->load(std::memory_order_acquire) == head)
        return false;
    Kick();
    WaitForWorker([&] { return TailP->load(std::memory_order_acquire) == head; });
    return true;
}

void BananaRenderer::WaitFence(u32 fence) const
{
    WaitForWorker([&] { return (s32)(TailP->load(std::memory_order_acquire) - fence) >= 0; });
}

// the next frame's 3D: Gengis draws it into the rows the frame before last showed (render.3d_share), which that
// frame's jobs may still read with render.2d_lazy
void BananaRenderer::Start3DRendering()
{
    FlushPending();
    if (LazyFrames)
        WaitFence(FenceCur);
    SoftRenderer::Start3DRendering();
}

void BananaRenderer::VBlank()
{
    // the frame's last lines: drawn while the emulator runs VBlank
    FlushPending();
    if (Thread2D && LocalHead != Kicked)
        Kick();
}

void BananaRenderer::SwapBuffers()
{
    if (LazyFrames && Thread2D && !AInPlace)
    {
        // render.2d_lazy: this frame's last jobs handed over, and only the frame before waited for (the buffers the
        // next frame's lines go into: the other back buffer, or a scanout slot the frontend frees after its fence)
        FlushPending();
        if (LocalHead != Kicked)
            Kick();
        FencePrev = FenceCur;
        FenceCur = LocalHead;
        WaitFence(FencePrev);
    }
    else
    {
        Drain2D();  // the frame's lines are all in the back buffer (or SetOutput's)
        FencePrev = FenceCur = LocalHead;
    }
    Front[0] = Out[0];
    Front[1] = Out[1];
    SoftRenderer::SwapBuffers();
}

void BananaRenderer::Reset()
{
    Drain2D();
    SoftRenderer::Reset();
    for (auto& b : HiFB)
        for (auto& f : b)
            if (f) memset(f.get(), 0, 256 * 192 * NQ * sizeof(u32));
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
            if (f) memset(f.get(), 0, 256 * 192 * NQ * sizeof(u32));
}

void BananaRenderer::PreSavestate()
{
    Drain2D();
    SoftRenderer::PreSavestate();
}

}
