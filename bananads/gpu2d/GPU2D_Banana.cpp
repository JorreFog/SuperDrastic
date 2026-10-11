/*
    BananaDS 2D engine: a fast software 2D renderer for melonDS-based emulators.
    Copyright 2026 ROCKNIXDS contributors. Register semantics and address formulas follow the
    melonDS 2D engine, Copyright 2016-2026 melonDS team.

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
#include "GPU2D_Banana.h"
#include "GPU_ColorOp.h"

// ---- four-lane SIMD, just what the compositor needs ------------------------------------------------
#if !defined(BANANA_NO_SIMD) && (defined(__ARM_NEON) || defined(__aarch64__))
#include <arm_neon.h>
#define BANANA_SIMD 1
#ifdef __aarch64__
#define BANANA_NEON 1
#endif
namespace melonDS
{
typedef uint32x4_t V4;
static inline V4 VLoad(const u32* p) { return vld1q_u32(p); }
static inline void VStore(u32* p, V4 v) { vst1q_u32(p, v); }
static inline V4 VDup(u32 x) { return vdupq_n_u32(x); }
static inline V4 VZero() { return vdupq_n_u32(0); }
static inline V4 VNonZero(V4 a) { return vtstq_u32(a, a); }
static inline V4 VEq(V4 a, V4 b) { return vceqq_u32(a, b); }
static inline V4 VAnd(V4 a, V4 b) { return vandq_u32(a, b); }
static inline V4 VOr(V4 a, V4 b) { return vorrq_u32(a, b); }
static inline V4 VNot(V4 a) { return vmvnq_u32(a); }
static inline V4 VLtSmall(V4 a, V4 b) { return vcltq_u32(a, b); }        // a < b, lanes well below 2^31
static inline V4 VSel(V4 m, V4 a, V4 b) { return vbslq_u32(m, a, b); }   // m ? a : b, per bit
static inline V4 VFromBytes4(u32 x) { return vmovl_u16(vget_low_u16(vmovl_u8(vreinterpret_u8_u32(vdup_n_u32(x))))); }
static inline V4 VLoadU16x4(const u16* p) { return vmovl_u16(vld1_u16(p)); }
template<int N> static inline V4 VShl(V4 a) { return vshlq_n_u32(a, N); }
template<int N> static inline V4 VShr(V4 a) { return vshrq_n_u32(a, N); }
static inline u32 VHorOr(V4 a) { uint32x2_t t = vorr_u32(vget_low_u32(a), vget_high_u32(a)); return vget_lane_u32(t, 0) | vget_lane_u32(t, 1); }
static inline u32 VHorAnd(V4 a) { uint32x2_t t = vand_u32(vget_low_u32(a), vget_high_u32(a)); return vget_lane_u32(t, 0) & vget_lane_u32(t, 1); }
}
#elif !defined(BANANA_NO_SIMD) && defined(__SSE2__)
#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#endif
#define BANANA_SIMD 1
namespace melonDS
{
typedef __m128i V4;
static inline V4 VLoad(const u32* p) { return _mm_loadu_si128((const __m128i*)p); }
static inline void VStore(u32* p, V4 v) { _mm_storeu_si128((__m128i*)p, v); }
static inline V4 VDup(u32 x) { return _mm_set1_epi32((int)x); }
static inline V4 VZero() { return _mm_setzero_si128(); }
static inline V4 VNonZero(V4 a) { return _mm_xor_si128(_mm_cmpeq_epi32(a, _mm_setzero_si128()), _mm_set1_epi32(-1)); }
static inline V4 VEq(V4 a, V4 b) { return _mm_cmpeq_epi32(a, b); }
static inline V4 VAnd(V4 a, V4 b) { return _mm_and_si128(a, b); }
static inline V4 VOr(V4 a, V4 b) { return _mm_or_si128(a, b); }
static inline V4 VNot(V4 a) { return _mm_xor_si128(a, _mm_set1_epi32(-1)); }
static inline V4 VLtSmall(V4 a, V4 b) { return _mm_cmplt_epi32(a, b); }   // a < b, lanes well below 2^31
static inline V4 VSel(V4 m, V4 a, V4 b)
{
#ifdef __SSE4_1__
    return _mm_blendv_epi8(b, a, m);
#else
    return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
#endif
}
static inline V4 VFromBytes4(u32 x)
{
    __m128i z = _mm_setzero_si128();
    return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)x), z), z);
}
static inline V4 VLoadU16x4(const u16* p) { return _mm_unpacklo_epi16(_mm_loadl_epi64((const __m128i*)p), _mm_setzero_si128()); }
template<int N> static inline V4 VShl(V4 a) { return _mm_slli_epi32(a, N); }
template<int N> static inline V4 VShr(V4 a) { return _mm_srli_epi32(a, N); }
static inline u32 VHorOr(V4 a)
{
    a = _mm_or_si128(a, _mm_srli_si128(a, 8));
    a = _mm_or_si128(a, _mm_srli_si128(a, 4));
    return (u32)_mm_cvtsi128_si32(a);
}
static inline u32 VHorAnd(V4 a)
{
    a = _mm_and_si128(a, _mm_srli_si128(a, 8));
    a = _mm_and_si128(a, _mm_srli_si128(a, 4));
    return (u32)_mm_cvtsi128_si32(a);
}
}
#else
namespace melonDS
{
struct V4 { u32 v[4]; };
static inline V4 VLoad(const u32* p) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = p[i]; return r; }
static inline void VStore(u32* p, V4 v) { for (int i = 0; i < 4; i++) p[i] = v.v[i]; }
static inline V4 VDup(u32 x) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = x; return r; }
static inline V4 VZero() { return VDup(0); }
static inline V4 VNonZero(V4 a) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] ? ~0u : 0; return r; }
static inline V4 VEq(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = (a.v[i] == b.v[i]) ? ~0u : 0; return r; }
static inline V4 VAnd(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] & b.v[i]; return r; }
static inline V4 VOr(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] | b.v[i]; return r; }
static inline V4 VNot(V4 a) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = ~a.v[i]; return r; }
static inline V4 VLtSmall(V4 a, V4 b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = (a.v[i] < b.v[i]) ? ~0u : 0; return r; }
static inline V4 VSel(V4 m, V4 a, V4 b) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = (m.v[i] & a.v[i]) | (~m.v[i] & b.v[i]); return r; }
static inline V4 VFromBytes4(u32 x) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = (x >> (8*i)) & 0xFF; return r; }
static inline V4 VLoadU16x4(const u16* p) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = p[i]; return r; }
template<int N> static inline V4 VShl(V4 a) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] << N; return r; }
template<int N> static inline V4 VShr(V4 a) { V4 r; for (int i = 0; i < 4; i++) r.v[i] = a.v[i] >> N; return r; }
static inline u32 VHorOr(V4 a) { return a.v[0] | a.v[1] | a.v[2] | a.v[3]; }
static inline u32 VHorAnd(V4 a) { return a.v[0] & a.v[1] & a.v[2] & a.v[3]; }
}
#endif

namespace melonDS
{

bool Banana2DFast = true;

// 15-bit palette colour to the 18-bit internal format; bit 15 is the low green bit
// (melonDS: applies to BG/OBJ palette colours, not to direct colour)
static inline u32 Conv15(u32 c)
{
    u32 r = (c & 0x001F) << 1;
    u32 g = ((c & 0x03E0) >> 4) | ((c & 0x8000) >> 15);
    u32 b = (c & 0x7C00) >> 9;
    return r | (g << 8) | (b << 16);
}

static inline u32 BGFlagMask(u32 bgnum)
{
    return 0x00FFFFFF | (0x01000000u << bgnum);
}

alignas(16) static const u8 AllOnes[256] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
};

// colour effect decision tables (melonDS ColorComposite), indexed by a pixel's flag byte:
// the BLDCNT 2nd-target bit a pixel selects when it is the second layer
struct Target2Table
{
    u16 v[256];
    constexpr Target2Table() : v()
    {
        for (int f = 0; f < 256; f++)
        {
            if      (f & 0x80) v[f] = 0x1000;
            else if (f & 0x40) v[f] = 0x0100;
            else               v[f] = f << 8;
        }
    }
};
static constexpr Target2Table Target2Tab {};

// the BLDCNT 1st-target bit a pixel selects when it is the top layer
struct FirstBitTable
{
    u8 v[256];
    constexpr FirstBitTable() : v()
    {
        for (int f = 0; f < 256; f++)
        {
            if      (f & 0x80) v[f] = 0x10;
            else if (f & 0x40) v[f] = 0x01;
            else               v[f] = f;
        }
    }
};
static constexpr FirstBitTable FirstBitTab {};

template<u32 Size>
static inline bool AnyBits(const NonStupidBitField<Size>& f)
{
    u64 any = 0;
    for (u32 i = 0; i < NonStupidBitField<Size>::DataLength; i++)
        any |= f.Data[i];
    return any != 0;
}

void Banana2DRegs::From(const melonDS::GPU2D& g)
{
    Enabled = g.Enabled;
    LayerEnable = g.LayerEnable;
    OBJEnable = g.OBJEnable;
    ForcedBlank = g.ForcedBlank;
    DispCnt = g.DispCnt;
    memcpy(BGCnt, g.BGCnt, sizeof(BGCnt));
    memcpy(BGXPos, g.BGXPos, sizeof(BGXPos));
    memcpy(BGYPos, g.BGYPos, sizeof(BGYPos));
    memcpy(BGRotA, g.BGRotA, sizeof(BGRotA));
    memcpy(BGRotC, g.BGRotC, sizeof(BGRotC));
    memcpy(BGXRefInternal, g.BGXRefInternal, sizeof(BGXRefInternal));
    memcpy(BGYRefInternal, g.BGYRefInternal, sizeof(BGYRefInternal));
    memcpy(Win0Coords, g.Win0Coords, sizeof(Win0Coords));
    memcpy(Win1Coords, g.Win1Coords, sizeof(Win1Coords));
    memcpy(WinCnt, g.WinCnt, sizeof(WinCnt));
    Win0Active = g.Win0Active;
    Win1Active = g.Win1Active;
    memcpy(BGMosaicSize, g.BGMosaicSize, sizeof(BGMosaicSize));
    memcpy(OBJMosaicSize, g.OBJMosaicSize, sizeof(OBJMosaicSize));
    BGMosaicLine = g.BGMosaicLine;
    OBJMosaicLine = g.OBJMosaicLine;
    BlendCnt = g.BlendCnt;
    EVA = g.EVA;
    EVB = g.EVB;
    EVY = g.EVY;
    PalBG = PalOBJ = PalBGStale = false;
    BGExtChanged = OBJExtChanged = false;
    OAMRebuild = false;
}

// ---- the memory the drawing reads ----

void BananaRenderer2D::PointAtGPU()
{
    if (GPU2D.Num == 0)
    {
        BGVRAM = GPU.VRAMFlat_ABG; BGVRAMMask = 0x7FFFF;
        OBJVRAM = GPU.VRAMFlat_AOBJ; OBJVRAMMask = 0x3FFFF;
        BGExtMem = GPU.VRAMFlat_ABGExtPal;
        OBJExtMem = GPU.VRAMFlat_AOBJExtPal;
        PalMem = &GPU.Palette[0];
    }
    else
    {
        BGVRAM = GPU.VRAMFlat_BBG; BGVRAMMask = 0x1FFFF;
        OBJVRAM = GPU.VRAMFlat_BOBJ; OBJVRAMMask = 0x1FFFF;
        BGExtMem = GPU.VRAMFlat_BBGExtPal;
        OBJExtMem = GPU.VRAMFlat_BOBJExtPal;
        PalMem = &GPU.Palette[0x400];
    }
}

// sizes of the copies (GPU's VRAM flats for this unit) and where each lives in CopyMem
static constexpr u32 ExtBGSize = 32*1024, ExtOBJSize = 8*1024, PalSize = 0x400;

void BananaRenderer2D::UseCopies(CopyFn copy, void* ctx)
{
    PointAtGPU();
    CopyTo = copy;
    CopyCtx = ctx;
    if (!copy)
    {
        CopyMem.reset();
        return;
    }
    u32 bgsize = BGVRAMMask + 1, objsize = OBJVRAMMask + 1;
    CopyMem.reset(new u8[bgsize + objsize + ExtBGSize + ExtOBJSize + PalSize]);
    u8* p = CopyMem.get();
    BGVRAM = p; p += bgsize;
    OBJVRAM = p; p += objsize;
    BGExtMem = p; p += ExtBGSize;
    OBJExtMem = p; p += ExtOBJSize;
    PalMem = p;
    // as GPU's now: nothing is drawn from them before the first job applies this (the palettes follow the next
    // Prepare: they are converted only from what was copied for that line)
    memcpy(BGVRAM, (GPU2D.Num == 0) ? GPU.VRAMFlat_ABG : GPU.VRAMFlat_BBG, bgsize);
    memcpy(OBJVRAM, (GPU2D.Num == 0) ? GPU.VRAMFlat_AOBJ : GPU.VRAMFlat_BOBJ, objsize);
    memcpy(BGExtMem, (GPU2D.Num == 0) ? GPU.VRAMFlat_ABGExtPal : GPU.VRAMFlat_BBGExtPal, ExtBGSize);
    memcpy(OBJExtMem, (GPU2D.Num == 0) ? GPU.VRAMFlat_AOBJExtPal : GPU.VRAMFlat_BOBJExtPal, ExtOBJSize);
    memcpy(CopyMem.get() + bgsize + objsize + ExtBGSize + ExtOBJSize, &GPU.Palette[(GPU2D.Num == 0) ? 0 : 0x400], PalSize);
    NeedResync = false;
}

// every dirty 512-byte block of a VRAM flat, in runs of consecutive blocks
template<u32 Size>
void BananaRenderer2D::CopyDirty(const NonStupidBitField<Size>& dirty, u8* copy, const u8* flat)
{
    for (u32 w = 0; w < NonStupidBitField<Size>::DataLength; w++)
    {
        u64 bits = dirty.Data[w];
        while (bits)
        {
            u32 first = __builtin_ctzll(bits);
            u64 run = bits >> first;
            u32 n = (~run == 0) ? 64 - first : __builtin_ctzll(~run);
            u32 off = (w * 64 + first) * VRAMDirtyGranularity;
            CopyTo(CopyCtx, copy + off, flat + off, n * VRAMDirtyGranularity);
            bits = (n + first >= 64) ? 0 : bits & ~(((1ULL << n) - 1) << first);
        }
    }
}

// after GPU::ResetVRAMCache (reset, savestates) the flats can change without dirty bits: copy them whole
void BananaRenderer2D::DoResync()
{
    NeedResync = false;
    if (!CopyTo)
        return;
    bool a = (GPU2D.Num == 0);
    CopyTo(CopyCtx, BGVRAM, a ? GPU.VRAMFlat_ABG : GPU.VRAMFlat_BBG, BGVRAMMask + 1);
    CopyTo(CopyCtx, OBJVRAM, a ? GPU.VRAMFlat_AOBJ : GPU.VRAMFlat_BOBJ, OBJVRAMMask + 1);
    CopyTo(CopyCtx, BGExtMem, a ? GPU.VRAMFlat_ABGExtPal : GPU.VRAMFlat_BBGExtPal, ExtBGSize);
    CopyTo(CopyCtx, OBJExtMem, a ? GPU.VRAMFlat_AOBJExtPal : GPU.VRAMFlat_BOBJExtPal, ExtOBJSize);
    ExtPalsResynced = true;  // the next drawn scanline drops its converted extended palettes
}


BananaRenderer2D::BananaRenderer2D(melonDS::GPU2D& gpu2D, SoftRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent),
      BGExtPalCache(new u32[4*16*256]), ObjTable(new u32[0x2000])
{
    PointAtGPU();
    Reset();
}

BananaRenderer2D::~BananaRenderer2D()
{
}

void BananaRenderer2D::Reset()
{
    memset(BGLineBuf, 0, sizeof(BGLineBuf));
    memset(OBJLine, 0, sizeof(OBJLine));
    memset(OBJTransAttr, 0, sizeof(OBJTransAttr));
    memset(OBJWindow, 0, sizeof(OBJWindow));
    memset(OBJColor, 0, sizeof(OBJColor));
    memset(OBJPrio, 0, sizeof(OBJPrio));
    memset(OAMCopy, 0, sizeof(OAMCopy));
    memset(&R, 0, sizeof(R));
    memset(WindowMask, 0xFF, sizeof(WindowMask));
    memset(TopLine, 0, sizeof(TopLine));
    memset(SecondLine, 0, sizeof(SecondLine));
    memset(PalBG256, 0, sizeof(PalBG256));
    memset(PalBG16, 0, sizeof(PalBG16));
    memset(PalBG16Planes, 0, sizeof(PalBG16Planes));
    memset(ObjTable.get(), 0, 0x2000 * sizeof(u32));
    Backdrop = 0x20000000;
    for (int i = 0; i < 256; i++) BackdropLine[i] = Backdrop;
    PalBGStale = true;
    PalOBJStale = true;
    BGExtPalValid = 0;
    ObjExtValid = false;
    NumSpriteTab = 0;
    memset(SpriteLines, 0, sizeof(SpriteLines));
    SpriteTabStale = true;
    NumSprites = 0;
    ObjXMin = 256; ObjXMax = 0;
    ObjDirtyMin = 256; ObjDirtyMax = 0;
    ObjWinDirty = false;
    WindowsOn = false;
    Raw3D = false;
    LayerDrawn = 0;
    LayerFull = 0;
    OBJPrioPresent = 0;
}


// ---- palettes ----------------------------------------------------------------------------------

void BananaRenderer2D::UpdatePalettes(bool bg, bool obj, bool bgStale)
{
    if (bg)
    {
        const u16* pal = (const u16*)PalMem;
        for (int i = 0; i < 256; i++)
        {
            u32 c = Conv15(pal[i]) | 0xFF000000;
            PalBG256[i] = i ? c : 0;
            PalBG16[i] = (i & 0xF) ? c : 0;
            // planar copy for the NEON 16-colour text BGs: R, G, B bytes of each 16-colour palette, colour 0 black
            u32 p16 = PalBG16[i];
            PalBG16Planes[i >> 4][(i & 0xF)]      = (u8)p16;
            PalBG16Planes[i >> 4][16 + (i & 0xF)] = (u8)(p16 >> 8);
            PalBG16Planes[i >> 4][32 + (i & 0xF)] = (u8)(p16 >> 16);
        }
        u32 bd = Conv15(pal[0]) | 0x20000000;
        if (bd != Backdrop || bgStale)
        {
            Backdrop = bd;
            for (int i = 0; i < 256; i++) BackdropLine[i] = bd;
        }
    }

    if (obj)
    {
        const u16* pal = (const u16*)(PalMem + 0x200);
        u32* table = &ObjTable[OBJ_StandardPal];
        for (int i = 0; i < 256; i++)
            table[i] = Conv15(pal[i]);
    }
}

// the 16 OBJ extended palettes, as one 4096-entry table indexed by (palette << 8) | colour
void BananaRenderer2D::ConvertOBJExtPals()
{
    const u16* src = (const u16*)OBJExtMem;
    for (int i = 0; i < 4096; i++)
        ObjTable[i] = Conv15(src[i]);
    ObjExtValid = true;
}

void BananaRenderer2D::ConvertBGExtPal(u32 idx)
{
    u32* table = &BGExtPalCache[idx << 8];
    const u16* src = (const u16*)&BGExtMem[idx << 9];
    table[0] = 0;
    for (int i = 1; i < 256; i++)
        table[i] = Conv15(src[i]) | 0xFF000000;
    BGExtPalValid |= (1ULL << idx);
}



// ---- windows and mosaic ------------------------------------------------------------------------

// Same semantics as GPU2D::CalculateWindowMask (including the horizontal window state that is
// carried from one scanline to the next in Win0Active/Win1Active bit 1), done with range fills.
void BananaRenderer2D::ApplyWindowEdges(const u8* coords, u8& active, u8 cnt)
{
    u8 x1 = coords[0];
    u8 x2 = coords[1];
    bool v = active & 0x1;
    bool h = active & 0x2;

    auto fill = [&](int a, int b)
    {
        if (h && v && b > a)
            memset(&WindowMask[a], cnt, b - a);
    };

    // per pixel: if (i == x2) inactive; else if (i == x1) active; then test
    if (x1 < x2)
    {
        fill(0, x1);  h = true;
        fill(x1, x2); h = false;
        fill(x2, 256);
    }
    else if (x2 < x1)
    {
        fill(0, x2);  h = false;
        fill(x2, x1); h = true;
        fill(x1, 256);
    }
    else
    {
        fill(0, x2);  h = false;
        fill(x2, 256);
    }

    active = (active & 0x1) | (h ? 0x2 : 0);
}

void BananaRenderer2D::CalcWindowMask()
{
    u32 dispcnt = R.DispCnt;

    memset(WindowMask, R.WinCnt[2], 256);

    if (dispcnt & (1<<15))
    {
        u8 c = R.WinCnt[3];
        for (int i = 0; i < 256; i++)
            if (OBJWindow[i]) WindowMask[i] = c;
    }

    if (dispcnt & (1<<14))
        ApplyWindowEdges(R.Win1Coords, R.Win1Active, R.WinCnt[1]);

    if (dispcnt & (1<<13))
        ApplyWindowEdges(R.Win0Coords, R.Win0Active, R.WinCnt[0]);
}

void BananaRenderer2D::ApplyWindow(u32* line, u8 bit)
{
    for (int i = 0; i < 256; i++)
        line[i] = (WindowMask[i] & bit) ? line[i] : 0;
}

// horizontal mosaic: every pixel of a block repeats the block's first pixel
// (equivalent to melonDS sampling the BG at x - (x % (size+1)))
void BananaRenderer2D::ApplyMosaicX(u32* line, u32 size)
{
    u32 step = size + 1;
    for (u32 b = 0; b < 256; b += step)
    {
        u32 v = line[b];
        u32 end = b + step; if (end > 256) end = 256;
        for (u32 i = b + 1; i < end; i++)
            line[i] = v;
    }
}


// ---- BG layers ---------------------------------------------------------------------------------

// eight 256-colour pixels of one tile row
static inline void Expand8(u32* out, u64 pix, const u32* pal, u32 flagmask, bool hflip)
{
    if (hflip)
    {
        out[0] = pal[(pix >> 56) & 0xFF] & flagmask;
        out[1] = pal[(pix >> 48) & 0xFF] & flagmask;
        out[2] = pal[(pix >> 40) & 0xFF] & flagmask;
        out[3] = pal[(pix >> 32) & 0xFF] & flagmask;
        out[4] = pal[(pix >> 24) & 0xFF] & flagmask;
        out[5] = pal[(pix >> 16) & 0xFF] & flagmask;
        out[6] = pal[(pix >>  8) & 0xFF] & flagmask;
        out[7] = pal[(pix      ) & 0xFF] & flagmask;
    }
    else
    {
        out[0] = pal[(pix      ) & 0xFF] & flagmask;
        out[1] = pal[(pix >>  8) & 0xFF] & flagmask;
        out[2] = pal[(pix >> 16) & 0xFF] & flagmask;
        out[3] = pal[(pix >> 24) & 0xFF] & flagmask;
        out[4] = pal[(pix >> 32) & 0xFF] & flagmask;
        out[5] = pal[(pix >> 40) & 0xFF] & flagmask;
        out[6] = pal[(pix >> 48) & 0xFF] & flagmask;
        out[7] = pal[(pix >> 56) & 0xFF] & flagmask;
    }
}

// eight 16-colour pixels of one tile row; pal already points at the tile's 16-colour palette
static inline void Expand4(u32* out, u32 pix, const u32* pal, u32 flagmask, bool hflip)
{
    if (hflip)
    {
        out[0] = pal[(pix >> 28) & 0xF] & flagmask;
        out[1] = pal[(pix >> 24) & 0xF] & flagmask;
        out[2] = pal[(pix >> 20) & 0xF] & flagmask;
        out[3] = pal[(pix >> 16) & 0xF] & flagmask;
        out[4] = pal[(pix >> 12) & 0xF] & flagmask;
        out[5] = pal[(pix >>  8) & 0xF] & flagmask;
        out[6] = pal[(pix >>  4) & 0xF] & flagmask;
        out[7] = pal[(pix      ) & 0xF] & flagmask;
    }
    else
    {
        out[0] = pal[(pix      ) & 0xF] & flagmask;
        out[1] = pal[(pix >>  4) & 0xF] & flagmask;
        out[2] = pal[(pix >>  8) & 0xF] & flagmask;
        out[3] = pal[(pix >> 12) & 0xF] & flagmask;
        out[4] = pal[(pix >> 16) & 0xF] & flagmask;
        out[5] = pal[(pix >> 20) & 0xF] & flagmask;
        out[6] = pal[(pix >> 24) & 0xF] & flagmask;
        out[7] = pal[(pix >> 28) & 0xF] & flagmask;
    }
}

static inline void Zero8(u32* out)
{
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    out[4] = 0; out[5] = 0; out[6] = 0; out[7] = 0;
}

static inline bool HasZeroByte(u64 pix) { return ((pix - 0x0101010101010101ULL) & ~pix & 0x8080808080808080ULL) != 0; }
static inline bool HasZeroNibble(u32 pix) { return ((pix - 0x11111111u) & ~pix & 0x88888888u) != 0; }

void BananaRenderer2D::DrawBG_3D()
{
    const u32* src = Line3D;
    u32* out = BGLine(0);
    for (int i = 0; i < 256; i++)
    {
        u32 c = src[i];
        out[i] = (c >> 24) ? (c | 0x40000000) : 0;
    }
}

template<bool bpp8>
void BananaRenderer2D::DrawBG_Text(u32 line, u32 bgnum)
{
    u16 bgcnt = R.BGCnt[bgnum];
    u32 dispcnt = R.DispCnt;

    u32 xoff = R.BGXPos[bgnum];
    u32 yoff = R.BGYPos[bgnum];
    if (bgcnt & (1<<6))
        yoff += R.BGMosaicLine;
    else
        yoff += line;
    yoff &= 0xFFFF;

    u32 widexmask = (bgcnt & (1<<14)) ? 0x100 : 0;

    bool extpal = bpp8 && (dispcnt & (1<<30));
    u32 extpalslot = ((bgnum < 2) && (bgcnt & 0x2000)) ? (2 + bgnum) : bgnum;

    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    u32 tilesetaddr, tilemapaddr;
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);
    }
    else
    {
        tilesetaddr = ((dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
    }

    if (bgcnt & (1<<15))
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & (1<<14))
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u32 flagmask = BGFlagMask(bgnum);
    u32 row = yoff & 0x7;
    u32 xt = xoff & ~0x7u;
    u32* out = BGLine(bgnum) - (xoff & 0x7);
    bool full = true;
    bool drawn = false;

#ifdef BANANA_NEON
    if (!bpp8 && Banana2DFast)
    {
        // 16-colour tiles, 8 pixels per tile row: the 8 nibbles spread to 8 index bytes (reversed for an X flip), looked up
        // in the tile palette's R, G and B planes and in a flag plane (0 for colour 0, else this BG's flag), and stored
        // interleaved as 8 pixels (st4). The scalar loop below does the same with 8 loads and 8 stores per tile.
        const uint8x8_t m0f = vdup_n_u8(0x0F);
        const uint8x8_t perm[2] = { vcreate_u8(0x0706050403020100ULL), vcreate_u8(0x0001020304050607ULL) };
        const uint8x16_t flagplane = vsetq_lane_u8(0, vdupq_n_u8((u8)(1u << bgnum)), 0);
        const uint8x16_t zero = vdupq_n_u8(0);
        uint8x8_t minidx = vdup_n_u8(0xFF);
        // the line's (at most two) 32-entry map blocks: 64-byte aligned, so each is contiguous in VRAM
        const u16* block[2] = { (const u16*)&vram[tilemapaddr & vrammask],
                                (const u16*)&vram[(tilemapaddr + (widexmask << 3)) & vrammask] };
        const u32 rowoff[2] = { tilesetaddr + (row << 2), tilesetaddr + ((7 - row) << 2) };
        for (int t = 0; t < 33; t++, xt += 8, out += 8)
        {
            u16 tile = block[(xt & widexmask) >> 8][(xt >> 3) & 31];
            u32 pix = *(u32*)&vram[(rowoff[(tile >> 11) & 1] + ((tile & 0x03FF) << 5)) & vrammask];
            if (pix == 0)
            {
                vst1q_u8((u8*)out, zero);
                vst1q_u8((u8*)(out + 4), zero);
                full = false;
                continue;
            }
            drawn = true;
            uint8x8_t b = vreinterpret_u8_u32(vdup_n_u32(pix));
            uint8x8_t idx = vzip1_u8(vand_u8(b, m0f), vshr_n_u8(b, 4));
            idx = vtbl1_u8(idx, perm[(tile >> 10) & 1]);
            minidx = vmin_u8(minidx, idx);
            const u8* pl = PalBG16Planes[tile >> 12];
            uint8x8x4_t o;
            o.val[0] = vqtbl1_u8(vld1q_u8(pl), idx);
            o.val[1] = vqtbl1_u8(vld1q_u8(pl + 16), idx);
            o.val[2] = vqtbl1_u8(vld1q_u8(pl + 32), idx);
            o.val[3] = vqtbl1_u8(flagplane, idx);
            vst4_u8((u8*)out, o);
        }
        if (vminv_u8(minidx) == 0) full = false;
        if (drawn) LayerDrawn |= (1 << bgnum);
        if (full) LayerFull |= (1 << bgnum);
        return;
    }
#endif

    for (int t = 0; t < 33; t++, xt += 8, out += 8)
    {
        u16 tile = *(u16*)&vram[(tilemapaddr + ((xt & 0xF8) >> 2) + ((xt & widexmask) << 3)) & vrammask];
        u32 r = (tile & (1<<11)) ? (7 - row) : row;

        if (bpp8)
        {
            u64 pix = *(u64*)&vram[(tilesetaddr + ((tile & 0x03FF) << 6) + (r << 3)) & vrammask];
            if (pix == 0)
            {
                Zero8(out);
                full = false;
                continue;
            }
            drawn = true;
            if (HasZeroByte(pix)) full = false;
            const u32* pal = extpal ? GetBGExtPal(extpalslot, tile >> 12) : PalBG256;
            Expand8(out, pix, pal, flagmask, tile & (1<<10));
        }
        else
        {
            u32 pix = *(u32*)&vram[(tilesetaddr + ((tile & 0x03FF) << 5) + (r << 2)) & vrammask];
            if (pix == 0)
            {
                Zero8(out);
                full = false;
                continue;
            }
            drawn = true;
            if (HasZeroNibble(pix)) full = false;
            Expand4(out, pix, PalBG16 + ((tile & 0xF000) >> 8), flagmask, tile & (1<<10));
        }
    }

    if (drawn) LayerDrawn |= (1 << bgnum);
    if (full) LayerFull |= (1 << bgnum);
}

// a rotscale BG with no rotation or scaling along the line (rotA = 1.0, rotC = 0): the line is one
// row of tiles, drawn tile by tile like a text BG. map16: 16-bit map entries with flips and extended
// palettes (mixed affine/text), otherwise 8-bit entries (affine)
template<bool map16>
void BananaRenderer2D::DrawTiledScroll(u32 bgnum, u32 tilemapaddr, u32 tilesetaddr, u32 coordmask, u32 yshift,
                                       bool wrap, bool extpal, s32 rotX, s32 rotY)
{
    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    u32 overflowmask = wrap ? 0 : ~(coordmask | 0x7FF);
    u32 flagmask = BGFlagMask(bgnum);
    u32* line = BGLine(bgnum);

    if (rotY & overflowmask)
    {
        memset(line, 0, 256*4);
        return;
    }

    u32 size = (coordmask >> 8) + 8;      // map size in pixels (128..1024)
    u32 sizemask = size - 1;
    u32 rowbase = ((rotY & coordmask) >> 11) << (yshift - 3);
    u32 tileyoff = (rotY >> 8) & 0x7;

    s32 x0 = rotX >> 8;
    u32* out = line - (x0 & 0x7);
    s32 xt = x0 & ~0x7;
    bool full = true;
    bool drawn = false;

    for (int t = 0; t < 33; t++, xt += 8, out += 8)
    {
        if (!wrap && (u32)xt >= size)
        {
            Zero8(out);
            full = false;
            continue;
        }
        u32 tx = ((u32)xt & sizemask) >> 3;
        u32 tile;
        u32 pixaddr;
        bool hflip = false;
        if (map16)
        {
            tile = *(u16*)&vram[(tilemapaddr + ((rowbase + tx) << 1)) & vrammask];
            u32 ty = (tile & (1<<11)) ? (7 - tileyoff) : tileyoff;
            hflip = tile & (1<<10);
            pixaddr = tilesetaddr + ((tile & 0x03FF) << 6) + (ty << 3);
        }
        else
        {
            tile = vram[(tilemapaddr + rowbase + tx) & vrammask];
            pixaddr = tilesetaddr + (tile << 6) + (tileyoff << 3);
        }

        u64 pix = *(u64*)&vram[pixaddr & vrammask];
        if (pix == 0)
        {
            Zero8(out);
            full = false;
            continue;
        }
        drawn = true;
        if (HasZeroByte(pix)) full = false;
        const u32* pal = (map16 && extpal) ? GetBGExtPal(bgnum, tile >> 12) : PalBG256;
        Expand8(out, pix, pal, flagmask, hflip);
    }

    if (drawn) LayerDrawn |= (1 << bgnum);
    if (full) LayerFull |= (1 << bgnum);
}

void BananaRenderer2D::DrawBG_Affine(u32 line, u32 bgnum)
{
    u16 bgcnt = R.BGCnt[bgnum];
    u32 dispcnt = R.DispCnt;

    u32 coordmask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
    case 0: coordmask = 0x07800; yshift = 7; break;
    case 1: coordmask = 0x0F800; yshift = 8; break;
    case 2: coordmask = 0x1F800; yshift = 9; break;
    default: coordmask = 0x3F800; yshift = 10; break;
    }

    bool wrap = bgcnt & (1<<13);
    u32 overflowmask = wrap ? 0 : ~(coordmask | 0x7FF);

    s32 rotA = R.BGRotA[bgnum-2];
    s32 rotC = R.BGRotC[bgnum-2];
    s32 rotX = R.BGXRefInternal[bgnum-2];
    s32 rotY = R.BGYRefInternal[bgnum-2];

    u32 tilesetaddr, tilemapaddr;
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);
    }
    else
    {
        tilesetaddr = ((dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
    }

    if (rotA == 0x100 && rotC == 0)
    {
        DrawTiledScroll<false>(bgnum, tilemapaddr, tilesetaddr, coordmask, yshift, wrap, false, rotX, rotY);
        return;
    }

    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    yshift -= 3;

    u32 flagmask = BGFlagMask(bgnum);
    u32* out = BGLine(bgnum);
    u32 acc = 0;   // OR of outputs, for emptiness
    u32 all = ~0u; // AND of outputs, for fullness

    for (int i = 0; i < 256; i++)
    {
        u32 c = 0;
        if (!((rotX | rotY) & overflowmask))
        {
            u8 tile = vram[(tilemapaddr + ((((rotY & coordmask) >> 11) << yshift) + ((rotX & coordmask) >> 11))) & vrammask];
            u32 tilexoff = (rotX >> 8) & 0x7;
            u32 tileyoff = (rotY >> 8) & 0x7;
            u8 color = vram[(tilesetaddr + (tile << 6) + (tileyoff << 3) + tilexoff) & vrammask];
            c = PalBG256[color] & flagmask;
        }
        out[i] = c;
        acc |= c;
        all &= c;

        rotX += rotA;
        rotY += rotC;
    }

    if (acc) LayerDrawn |= (1 << bgnum);
    if (all) LayerFull |= (1 << bgnum);
}

// direct-colour pixels, contiguous in VRAM and in the output
#ifdef BANANA_NEON
// AND / OR of the 32-bit pixels whose low and high halves are the lanes of lo and hi
static inline u32 HorAnd16(uint16x8_t lo, uint16x8_t hi)
{
    uint64x2_t l = vreinterpretq_u64_u16(lo), h = vreinterpretq_u64_u16(hi);
    u64 a = vgetq_lane_u64(l, 0) & vgetq_lane_u64(l, 1), b = vgetq_lane_u64(h, 0) & vgetq_lane_u64(h, 1);
    a &= a >> 32; a &= a >> 16; b &= b >> 32; b &= b >> 16;
    return (u32)(a & 0xFFFF) | ((u32)(b & 0xFFFF) << 16);
}
static inline u32 HorOr16(uint16x8_t lo, uint16x8_t hi)
{
    uint64x2_t l = vreinterpretq_u64_u16(lo), h = vreinterpretq_u64_u16(hi);
    u64 a = vgetq_lane_u64(l, 0) | vgetq_lane_u64(l, 1), b = vgetq_lane_u64(h, 0) | vgetq_lane_u64(h, 1);
    a |= a >> 32; a |= a >> 16; b |= b >> 32; b |= b >> 16;
    return (u32)(a & 0xFFFF) | ((u32)(b & 0xFFFF) << 16);
}
#endif

static inline void ConvertRun16(u32* out, const u16* src, int n, u32 flag, u32& acc, u32& all)
{
    int j = 0;
#ifdef BANANA_NEON
    if (Banana2DFast && n >= 8)
    {
        // 8 pixels in 16-bit lanes: low halves R | G << 8, high halves B | the flag, zeroed where bit 15 is clear,
        // stored interleaved (st2)
        const uint16x8_t m3e = vdupq_n_u16(0x3E), vflag = vdupq_n_u16((u16)(flag >> 16));
        uint16x8_t accLo = vdupq_n_u16(0), accHi = accLo, allLo = vdupq_n_u16(0xFFFF), allHi = allLo;
        for (; j + 8 <= n; j += 8)
        {
            uint16x8_t c = vld1q_u16(src + j);
            uint16x8_t a = vreinterpretq_u16_s16(vshrq_n_s16(vreinterpretq_s16_u16(c), 15));
            uint16x8_t r = vandq_u16(vshlq_n_u16(c, 1), m3e);
            uint16x8_t g = vandq_u16(vshrq_n_u16(c, 4), m3e);
            uint16x8_t b = vandq_u16(vshrq_n_u16(c, 9), m3e);
            uint16x8x2_t o;
            o.val[0] = vandq_u16(vsliq_n_u16(r, g, 8), a);
            o.val[1] = vandq_u16(vorrq_u16(b, vflag), a);
            vst2q_u16((u16*)(out + j), o);
            accLo = vorrq_u16(accLo, o.val[0]); accHi = vorrq_u16(accHi, o.val[1]);
            allLo = vandq_u16(allLo, o.val[0]); allHi = vandq_u16(allHi, o.val[1]);
        }
        acc |= HorOr16(accLo, accHi);
        all &= HorAnd16(allLo, allHi);
    }
#endif
    if (n >= 4)
    {
        V4 vacc = VZero(), vall = VDup(~0u);
        V4 vflag = VDup(flag);
        V4 m1f = VDup(0x1F), m3e0 = VDup(0x3E0), m7c00 = VDup(0x7C00), m8000 = VDup(0x8000);
        for (; j + 4 <= n; j += 4)
        {
            V4 c = VLoadU16x4(src + j);
            V4 alpha = VNonZero(VAnd(c, m8000));
            V4 col = VOr(VOr(VShl<1>(VAnd(c, m1f)), VShl<4>(VAnd(c, m3e0))), VOr(VShl<7>(VAnd(c, m7c00)), vflag));
            col = VAnd(col, alpha);
            VStore(out + j, col);
            vacc = VOr(vacc, col);
            vall = VAnd(vall, col);
        }
        acc |= VHorOr(vacc);
        all &= VHorAnd(vall);
    }
    for (; j < n; j++)
    {
        u32 color = src[j];
        u32 c = (color & 0x8000) ? (Conv15(color & 0x7FFF) | flag) : 0;
        out[j] = c;
        acc |= c;
        all &= c;
    }
}

static inline void ConvertRun8(u32* out, const u8* src, int n, const u32* pal, u32 flagmask, u32& acc, u32& all)
{
    for (int j = 0; j < n; j++)
    {
        u32 c = pal[src[j]] & flagmask;
        out[j] = c;
        acc |= c;
        all &= c;
    }
}

// direct-colour bitmap BG (extended rotscale BG with BGCNT bit 7 and bit 2)
void BananaRenderer2D::DrawBitmap16(u32* out, u32 flag, u32 base, u32 xmask, u32 ymask, u32 yshift, bool wrap,
                                    s32 rotA, s32 rotC, s32 rotX, s32 rotY, u32 bgnum)
{
    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    u32 ofxmask = wrap ? 0 : ~xmask;
    u32 ofymask = wrap ? 0 : ~ymask;
    u32 acc = 0, all = ~0u;
    u32 width = (xmask >> 8) + 1;
    u32 rowaddr = base + ((((rotY & ymask) >> 8) << yshift) << 1);

    if (rotA == 0x100 && rotC == 0 && (rowaddr & vrammask) + width * 2 <= vrammask + 1)
    {
        // scroll only: one row of the bitmap, read left to right in contiguous runs
        if (rotY & ofymask)
        {
            memset(out, 0, 256*4);
            return;
        }
        const u16* row = (const u16*)&vram[rowaddr & vrammask];
        s32 x0 = rotX >> 8;
        if (wrap)
        {
            u32 x = (u32)x0 & (width - 1);
            int i = 0;
            while (i < 256)
            {
                int n = 256 - i;
                if (n > (int)(width - x)) n = width - x;
                ConvertRun16(out + i, row + x, n, flag, acc, all);
                i += n;
                x = 0;
            }
        }
        else
        {
            s32 first = -x0; if (first < 0) first = 0; if (first > 256) first = 256;
            s32 last = (s32)width - x0; if (last < 0) last = 0; if (last > 256) last = 256;
            if (last < first) last = first;
            if (first > 0) { memset(out, 0, first * 4); all = 0; }
            if (last > first) ConvertRun16(out + first, row + (x0 + first), last - first, flag, acc, all);
            if (last < 256) { memset(out + last, 0, (256 - last) * 4); all = 0; }
        }
    }
    else
    {
        for (int i = 0; i < 256; i++)
        {
            u32 c = 0;
            if (!(rotX & ofxmask) && !(rotY & ofymask))
            {
                u16 color = *(u16*)&vram[(base + (((((rotY & ymask) >> 8) << yshift) + ((rotX & xmask) >> 8)) << 1)) & vrammask];
                c = (color & 0x8000) ? (Conv15(color & 0x7FFF) | flag) : 0;
            }
            out[i] = c;
            acc |= c;
            all &= c;
            rotX += rotA;
            rotY += rotC;
        }
    }

    if (acc) LayerDrawn |= (1 << bgnum);
    if (all) LayerFull |= (1 << bgnum);
}

// 256-colour bitmap BG (extended rotscale BG with bit 7 set and bit 2 clear, or the mode 6 large BG)
void BananaRenderer2D::DrawBitmap8(u32* out, u32 flagmask, u32 base, u32 xmask, u32 ymask, u32 yshift, bool wrap,
                                   s32 rotA, s32 rotC, s32 rotX, s32 rotY, u32 bgnum)
{
    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    u32 ofxmask = wrap ? 0 : ~xmask;
    u32 ofymask = wrap ? 0 : ~ymask;
    u32 acc = 0, all = ~0u;
    u32 width = (xmask >> 8) + 1;
    u32 rowaddr = base + (((rotY & ymask) >> 8) << yshift);

    if (rotA == 0x100 && rotC == 0 && (rowaddr & vrammask) + width <= vrammask + 1)
    {
        if (rotY & ofymask)
        {
            memset(out, 0, 256*4);
            return;
        }
        const u8* row = &vram[rowaddr & vrammask];
        s32 x0 = rotX >> 8;
        if (wrap)
        {
            u32 x = (u32)x0 & (width - 1);
            int i = 0;
            while (i < 256)
            {
                int n = 256 - i;
                if (n > (int)(width - x)) n = width - x;
                ConvertRun8(out + i, row + x, n, PalBG256, flagmask, acc, all);
                i += n;
                x = 0;
            }
        }
        else
        {
            s32 first = -x0; if (first < 0) first = 0; if (first > 256) first = 256;
            s32 last = (s32)width - x0; if (last < 0) last = 0; if (last > 256) last = 256;
            if (last < first) last = first;
            if (first > 0) { memset(out, 0, first * 4); all = 0; }
            if (last > first) ConvertRun8(out + first, row + (x0 + first), last - first, PalBG256, flagmask, acc, all);
            if (last < 256) { memset(out + last, 0, (256 - last) * 4); all = 0; }
        }
    }
    else
    {
        for (int i = 0; i < 256; i++)
        {
            u32 c = 0;
            if (!(rotX & ofxmask) && !(rotY & ofymask))
                c = PalBG256[vram[(base + (((rotY & ymask) >> 8) << yshift) + ((rotX & xmask) >> 8)) & vrammask]] & flagmask;
            out[i] = c;
            acc |= c;
            all &= c;
            rotX += rotA;
            rotY += rotC;
        }
    }

    if (acc) LayerDrawn |= (1 << bgnum);
    if (all) LayerFull |= (1 << bgnum);
}

void BananaRenderer2D::DrawBG_Extended(u32 line, u32 bgnum)
{
    u16 bgcnt = R.BGCnt[bgnum];
    u32 dispcnt = R.DispCnt;

    s32 rotA = R.BGRotA[bgnum-2];
    s32 rotC = R.BGRotC[bgnum-2];
    s32 rotX = R.BGXRefInternal[bgnum-2];
    s32 rotY = R.BGYRefInternal[bgnum-2];

    u32* out = BGLine(bgnum);

    if (bgcnt & (1<<7))
    {
        // bitmap modes
        u32 xmask, ymask;
        u32 yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
        case 0: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
        case 1: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
        case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        bool wrap = bgcnt & (1<<13);
        u32 base = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & (1<<2))
            DrawBitmap16(out, 0x01000000u << bgnum, base, xmask, ymask, yshift, wrap, rotA, rotC, rotX, rotY, bgnum);
        else
            DrawBitmap8(out, BGFlagMask(bgnum), base, xmask, ymask, yshift, wrap, rotA, rotC, rotX, rotY, bgnum);
        return;
    }

    // mixed affine/text mode: 16-bit map entries, 256-colour tiles with flips, extended palettes
    u32 coordmask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
    case 0: coordmask = 0x07800; yshift = 7; break;
    case 1: coordmask = 0x0F800; yshift = 8; break;
    case 2: coordmask = 0x1F800; yshift = 9; break;
    default: coordmask = 0x3F800; yshift = 10; break;
    }

    bool wrap = bgcnt & (1<<13);
    u32 overflowmask = wrap ? 0 : ~(coordmask | 0x7FF);
    bool extpal = dispcnt & (1<<30);

    u32 tilesetaddr, tilemapaddr;
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);
    }
    else
    {
        tilesetaddr = ((dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
    }

    if (rotA == 0x100 && rotC == 0)
    {
        DrawTiledScroll<true>(bgnum, tilemapaddr, tilesetaddr, coordmask, yshift, wrap, extpal, rotX, rotY);
        return;
    }

    u8* vram;
    u32 vrammask;
    GetBGVRAM(vram, vrammask);

    yshift -= 3;

    u32 flagmask = BGFlagMask(bgnum);
    u32 acc = 0, all = ~0u;

    for (int i = 0; i < 256; i++)
    {
        u32 c = 0;
        if (!((rotX | rotY) & overflowmask))
        {
            u16 tile = *(u16*)&vram[(tilemapaddr + (((((rotY & coordmask) >> 11) << yshift) + ((rotX & coordmask) >> 11)) << 1)) & vrammask];

            u32 tilexoff = (rotX >> 8) & 0x7;
            u32 tileyoff = (rotY >> 8) & 0x7;
            if (tile & (1<<10)) tilexoff = 7 - tilexoff;
            if (tile & (1<<11)) tileyoff = 7 - tileyoff;

            u8 color = vram[(tilesetaddr + ((tile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & vrammask];
            const u32* pal = extpal ? GetBGExtPal(bgnum, tile >> 12) : PalBG256;
            c = pal[color] & flagmask;
        }
        out[i] = c;
        acc |= c;
        all &= c;
        rotX += rotA;
        rotY += rotC;
    }

    if (acc) LayerDrawn |= (1 << bgnum);
    if (all) LayerFull |= (1 << bgnum);
}

void BananaRenderer2D::DrawBG_Large(u32 line)
{
    u16 bgcnt = R.BGCnt[2];

    // large BG sizes: 0: 512x1024, 1: 1024x512, 2: 512x256, 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
    case 0: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
    case 1: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
    case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
    default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    bool wrap = bgcnt & (1<<13);

    DrawBitmap8(BGLine(2), BGFlagMask(2), 0, xmask, ymask, yshift, wrap,
                R.BGRotA[0], R.BGRotC[0], R.BGXRefInternal[0], R.BGYRefInternal[0], 2);
}


// ---- sprites -----------------------------------------------------------------------------------

template<bool window>
inline void BananaRenderer2D::PutOBJ(s32 x, u32 color, u32 attr)
{
    if (window)
    {
        if (color) OBJWindow[x] = 1;
        return;
    }

    // melonDS DrawSpritePixel: an opaque pixel replaces a transparent one, or an opaque one of a
    // worse (higher) BG priority; a transparent pixel over a transparent one only updates the
    // attribute bits (recorded in the side table, reconstructed by the OBJ mosaic pass)
    u32 old = OBJLine[x];
    bool write = color && (!(old & OBJ_IsOpaque) || ((attr & OBJ_BGPrioMask) < (old & OBJ_BGPrioMask)));
    OBJLine[x] = write ? (color | attr) : old;
    OBJTransAttr[x] = (u8)(attr >> 16);
}

// four 4-bit pixels (one byte each, in pixel order) from four nibbles, forward and reversed
static inline u32 SpreadNibbles(u32 v)
{
    return (v & 0x000F) | ((v & 0x00F0) << 4) | ((v & 0x0F00) << 8) | ((v & 0xF000) << 12);
}
static inline u32 SpreadNibblesRev(u32 v)
{
    return ((v & 0xF000) >> 12) | ((v & 0x0F00) >> 0) | ((v & 0x00F0) << 12) | ((v & 0x000F) << 24);
}

// four consecutive sprite pixels (colour indices in the four bytes of 'idx4') with melonDS's
// per-pixel rules, four lanes at a time
inline void BananaRenderer2D::PutOBJ4(s32 x, u32 idx4, u32 attr)
{
    V4 idx = VFromBytes4(idx4);
    V4 old = VLoad(&OBJLine[x]);
    V4 opaque = VNonZero(idx);
    V4 oldOpaque = VNonZero(VAnd(old, VDup(OBJ_IsOpaque)));
    V4 better = VLtSmall(VDup(attr & OBJ_BGPrioMask), VAnd(old, VDup(OBJ_BGPrioMask)));
    V4 write = VAnd(opaque, VOr(VNot(oldOpaque), better));
    VStore(&OBJLine[x], VSel(write, VOr(idx, VDup(attr)), old));
    *(u32*)&OBJTransAttr[x] = ((attr >> 16) & 0xFF) * 0x01010101u;
}

// the per-sprite values melonDS's DrawSprites derives from OAM on every scanline
void BananaRenderer2D::RebuildSpriteTable()
{
    static const u8 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    static const u8 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    const u16* oam = OAMCopy;
    int n = 0;
    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        const u16* attrib = &oam[sprnum*4];

        u16 sprtype = (attrib[0] >> 8) & 0x3;
        if (sprtype == 2) // disabled
            continue;

        bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

        u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
        u32 width = spritewidth[sizeparam];
        u32 height = spriteheight[sizeparam];
        u32 boundwidth = width;
        u32 boundheight = height;

        if (sprtype == 3) // double-size rotscale sprite
        {
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        s32 xpos = (s32)(attrib[1] << 23) >> 23;
        if (xpos <= -(s32)boundwidth)
            continue;

        SpriteEntry& e = SpriteTab[n++];
        e.num = sprnum;
        e.ypos = attrib[0] & 0xFF;
        e.boundheight = boundheight;
        e.flags = (sprtype & 1) | (iswin ? 2 : 0) | ((attrib[0] & (1<<12)) ? 4 : 0);
        e.xpos = xpos;
        e.width = width;
        e.height = height;
        e.boundwidth = boundwidth;
    }
    NumSpriteTab = n;

    // render.2d_fast: the entries each sprite line can draw, as bits in table order
    if (Banana2DFast)
    {
        memset(SpriteLines, 0, sizeof(SpriteLines));
        for (int i = 0; i < n; i++)
        {
            const SpriteEntry& e = SpriteTab[i];
            u64 bit = 1ULL << (i & 63);
            for (u32 h = 0; h < e.boundheight; h++)
                SpriteLines[(e.ypos + h) & 0xFF][i >> 6] |= bit;
        }
    }
}

void BananaRenderer2D::DrawSprites(u32 line)
{
    Banana2DRegs regs;
    PrepareSprites(regs);
    DrawSpritesFrom(line, regs);
}

// what a sprite line reads of the registers (DrawSpritesFrom and the sprite drawing)
void Banana2DRegs::FromSprites(const melonDS::GPU2D& g)
{
    Enabled = g.Enabled;
    OBJEnable = g.OBJEnable;
    DispCnt = g.DispCnt;
    OBJMosaicLine = g.OBJMosaicLine;
    OAMRebuild = false;
}

void BananaRenderer2D::PrepareSprites(Banana2DRegs& regs)
{
    if (Banana2DFast) regs.FromSprites(GPU2D);
    else              regs.From(GPU2D);
    if (NeedResync)
        DoResync();

    // the OBJ buffers don't get updated at all if the 2D engine is disabled
    if (!GPU2D.Enabled)
        return;

    if (GPU2D.Num == 0)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
        if (CopyTo) CopyDirty(objDirty, OBJVRAM, GPU.VRAMFlat_AOBJ);
    }
    else
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
        if (CopyTo) CopyDirty(objDirty, OBJVRAM, GPU.VRAMFlat_BOBJ);
    }

    u32 oambit = 1u << GPU2D.Num;
    if (GPU2D.OBJEnable && ((GPU.OAMDirty & oambit) || SpriteTabStale))
    {
        const u8* oam = &GPU.OAM[GPU2D.Num ? 0x400 : 0];
        if (CopyTo) CopyTo(CopyCtx, OAMCopy, oam, sizeof(OAMCopy));
        else        memcpy(OAMCopy, oam, sizeof(OAMCopy));
        regs.OAMRebuild = true;
        GPU.OAMDirty &= ~oambit;
        SpriteTabStale = false;
    }
}

void BananaRenderer2D::DrawSpritesFrom(u32 line, const Banana2DRegs& regs)
{
    if (Banana2DFast)
    {
        // (only what PrepareSprites copied: the next scanline sets the whole of R)
        R.Enabled = regs.Enabled;
        R.OBJEnable = regs.OBJEnable;
        R.DispCnt = regs.DispCnt;
        R.OBJMosaicLine = regs.OBJMosaicLine;
        R.OAMRebuild = regs.OAMRebuild;
    }
    else
        R = regs;

    if (!R.Enabled)
        return;

    NumSprites = 0;
    ObjXMin = 256;
    ObjXMax = 0;
    if (Banana2DFast)
    {
        // only what the last sprite line wrote (its sprites' extent, the whole line after OBJ mosaic)
        if (ObjDirtyMax > ObjDirtyMin)
        {
            memset(&OBJLine[ObjDirtyMin], 0, (ObjDirtyMax - ObjDirtyMin) * 4);
            memset(&OBJTransAttr[ObjDirtyMin], 0, ObjDirtyMax - ObjDirtyMin);
        }
        if (ObjWinDirty)
            memset(OBJWindow, 0, sizeof(OBJWindow));
        ObjDirtyMin = 256;
        ObjDirtyMax = 0;
        ObjWinDirty = false;
    }
    else
    {
        memset(OBJLine, 0, sizeof(OBJLine));
        memset(OBJTransAttr, 0, sizeof(OBJTransAttr));
        memset(OBJWindow, 0, sizeof(OBJWindow));
    }

    if (!R.OBJEnable)
        return;

    if (R.OAMRebuild)
        RebuildSpriteTable();

    // render.2d_fast: only the entries on this line (the same ones, in the same order)
    u64 onLine[2] = { ~0ULL, ~0ULL };
    if (Banana2DFast)
    {
        onLine[0] = SpriteLines[line & 0xFF][0];
        onLine[1] = SpriteLines[line & 0xFF][1];
    }
    for (int w = 0; w < 2; w++)
    for (u64 bits = onLine[w]; bits; bits &= bits - 1)
    {
        int i = w * 64 + __builtin_ctzll(bits);
        if (i >= NumSpriteTab)
            break;
        const SpriteEntry& e = SpriteTab[i];

        if (((line - e.ypos) & 0xFF) >= e.boundheight)
            continue;

        s32 ypos;
        if ((e.flags & 6) == 4)
        {
            // sprite mosaic (not for OBJ-window sprites): Y position latched by the register side
            ypos = (R.OBJMosaicLine - e.ypos) & 0xFF;
            if (ypos >= e.boundheight) ypos = 0;
        }
        else
            ypos = (line - e.ypos) & 0xFF;

        s32 xpos = e.xpos;
        if (!(e.flags & 2))
        {
            s32 x0 = xpos < 0 ? 0 : xpos;
            s32 x1 = xpos + e.boundwidth; if (x1 > 256) x1 = 256;
            if (x0 < ObjXMin) ObjXMin = x0;
            if (x1 > ObjXMax) ObjXMax = x1;
        }
        else
            ObjWinDirty = true;

        if (e.flags & 1)
        {
            if (e.flags & 2) DrawSprite_Rotscale<true>(e.num, e.boundwidth, e.boundheight, e.width, e.height, xpos, ypos);
            else             DrawSprite_Rotscale<false>(e.num, e.boundwidth, e.boundheight, e.width, e.height, xpos, ypos);
        }
        else
        {
            if (e.flags & 2) DrawSprite_Normal<true>(e.num, e.width, e.height, xpos, ypos);
            else             DrawSprite_Normal<false>(e.num, e.width, e.height, xpos, ypos);
        }

        NumSprites++;
    }
    ObjDirtyMin = ObjXMin;
    ObjDirtyMax = ObjXMax;
}

template<bool window>
void BananaRenderer2D::DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = OAMCopy;
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];
    u32 dispcnt = R.DispCnt;

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(objvram, objvrammask);

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & (1<<12)) && !window)
        pixelattr |= OBJ_Mosaic;

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((xoff-centerX) * rotA) + ((ypos-centerY) * rotB) + (width << 7);
    s32 rotY = ((xoff-centerX) * rotC) + ((ypos-centerY) * rotD) + (height << 7);

    width <<= 8;
    height <<= 8;

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (dispcnt & 0x40)
        {
            if (dispcnt & 0x20)
            {
                // 'reserved': draws nothing
                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((dispcnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (dispcnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < boundwidth;)
        {
            if ((u32)rotX < width && (u32)rotY < height)
            {
                u16 color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];
                PutOBJ<window>(xpos, (color & 0x8000) ? color : 0, pixelattr);
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (dispcnt & (1<<4))
        {
            pixelsaddr <<= ((dispcnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & (1<<13))
        {
            // 256-color
            if (!window)
            {
                if (!(dispcnt & (1<<31)))
                    pixelattr |= OBJ_StandardPal;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    u8 color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];
                    PutOBJ<window>(xpos, color, pixelattr);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= OBJ_StandardPal;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    u8 color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;
                    PutOBJ<window>(xpos, color, pixelattr);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void BananaRenderer2D::DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = OAMCopy;
    u16* attrib = &oam[num * 4];
    u32 dispcnt = R.DispCnt;

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    if ((attrib[0] & (1<<12)) && !window)
        pixelattr |= OBJ_Mosaic;

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(objvram, objvrammask);

    // yflip
    if (attrib[1] & (1<<13))
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    bool xflip = attrib[1] & (1<<12);

    if (spritemode == 3)
    {
        // bitmap sprite
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        if (dispcnt & 0x40)
        {
            if (dispcnt & 0x20)
            {
                // 'reserved': draws nothing
                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((dispcnt >> 22) & 0x1));
                pixelsaddr += (ypos * width * 2);
            }
        }
        else
        {
            if (dispcnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (ypos * 128 * 2);
            }
        }

        s32 pixelstride;
        if (xflip)
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend; xoff++, xpos++)
        {
            u16 color = *(u16*)&objvram[pixelsaddr & objvrammask];
            pixelsaddr += pixelstride;
            PutOBJ<window>(xpos, (color & 0x8000) ? color : 0, pixelattr);
        }
        return;
    }

    // tiled sprite: byte address of the tile row this scanline uses, tile column 0
    u32 tilebase = tilenum;
    if (dispcnt & (1<<4))
    {
        tilebase <<= ((dispcnt >> 20) & 0x3);
        tilebase += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
    }
    else
    {
        tilebase += ((ypos >> 3) * 0x20);
    }
    tilebase <<= 5;

    if (spritemode == 1) pixelattr |= 0x80000000;
    else                 pixelattr |= 0x10000000;

    u32 ntiles = width >> 3;

    if (attrib[0] & (1<<13))
    {
        // 256-color: 64 bytes per tile, 8 bytes per row
        tilebase += ((ypos & 0x7) << 3);

        if (!window)
        {
            if (!(dispcnt & (1<<31)))
                pixelattr |= OBJ_StandardPal;
            else
                pixelattr |= ((attrib[2] & 0xF000) >> 4);
        }

        u32 x = xoff;
        while (x < xend)
        {
            u32 j = x >> 3;
            u32 k0 = x & 0x7;
            u32 k1 = xend - (j << 3); if (k1 > 8) k1 = 8;
            u32 sj = xflip ? (ntiles - 1 - j) : j;
            u64 pix = *(u64*)&objvram[(tilebase + (sj << 6)) & objvrammask];

            if (pix == 0 && !window)
            {
                u8 a = (u8)(pixelattr >> 16);
                for (u32 k = k0; k < k1; k++)
                    OBJTransAttr[xpos + (k - k0)] = a;
            }
            else if (!window && k0 == 0 && k1 == 8)
            {
                u32 lo = (u32)pix, hi = (u32)(pix >> 32);
                if (xflip) { u32 t = __builtin_bswap32(hi); hi = __builtin_bswap32(lo); lo = t; }
                PutOBJ4(xpos, lo, pixelattr);
                PutOBJ4(xpos + 4, hi, pixelattr);
            }
            else if (xflip)
            {
                for (u32 k = k0; k < k1; k++)
                    PutOBJ<window>(xpos + (k - k0), (pix >> ((7 - k) << 3)) & 0xFF, pixelattr);
            }
            else
            {
                for (u32 k = k0; k < k1; k++)
                    PutOBJ<window>(xpos + (k - k0), (pix >> (k << 3)) & 0xFF, pixelattr);
            }

            xpos += k1 - k0;
            x = (j << 3) + k1;
        }
    }
    else
    {
        // 16-color: 32 bytes per tile, 4 bytes per row
        tilebase += ((ypos & 0x7) << 2);

        if (!window)
        {
            pixelattr |= OBJ_StandardPal;
            pixelattr |= ((attrib[2] & 0xF000) >> 8);
        }

        u32 x = xoff;
        while (x < xend)
        {
            u32 j = x >> 3;
            u32 k0 = x & 0x7;
            u32 k1 = xend - (j << 3); if (k1 > 8) k1 = 8;
            u32 sj = xflip ? (ntiles - 1 - j) : j;
            u32 pix = *(u32*)&objvram[(tilebase + (sj << 5)) & objvrammask];

            if (pix == 0 && !window)
            {
                u8 a = (u8)(pixelattr >> 16);
                for (u32 k = k0; k < k1; k++)
                    OBJTransAttr[xpos + (k - k0)] = a;
            }
            else if (!window && k0 == 0 && k1 == 8)
            {
                u32 lo, hi;
                if (xflip) { lo = SpreadNibblesRev(pix >> 16); hi = SpreadNibblesRev(pix & 0xFFFF); }
                else       { lo = SpreadNibbles(pix & 0xFFFF); hi = SpreadNibbles(pix >> 16); }
                PutOBJ4(xpos, lo, pixelattr);
                PutOBJ4(xpos + 4, hi, pixelattr);
            }
            else if (xflip)
            {
                for (u32 k = k0; k < k1; k++)
                    PutOBJ<window>(xpos + (k - k0), (pix >> ((7 - k) << 2)) & 0xF, pixelattr);
            }
            else
            {
                for (u32 k = k0; k < k1; k++)
                    PutOBJ<window>(xpos + (k - k0), (pix >> (k << 2)) & 0xF, pixelattr);
            }

            xpos += k1 - k0;
            x = (j << 3) + k1;
        }
    }
}

// melonDS ApplySpriteMosaicX, with the transparent pixels' attribute words rebuilt from the side table
void BananaRenderer2D::ApplySpriteMosaicX()
{
    u8 mosw = R.OBJMosaicSize[0];
    if (mosw == 0) return;

    u8 mosx = 0;
    u32 latchcolor = 0;
    for (int i = 0; i < 256; i++)
    {
        u32 curcolor = OBJLine[i];
        if (!(curcolor & OBJ_IsOpaque))
            curcolor = ((u32)OBJTransAttr[i] << 16) & (OBJ_IsSprite | OBJ_Mosaic | OBJ_BGPrioMask);

        bool latch = false;

        if (mosx == 0)
            latch = true;
        else if (!(curcolor & OBJ_Mosaic))
            latch = true;
        else if (!(latchcolor & OBJ_Mosaic))
            latch = true;
        else if ((curcolor & OBJ_BGPrioMask) < (latchcolor & OBJ_BGPrioMask))
            latch = true;

        if (latch)
            latchcolor = curcolor;

        OBJLine[i] = latchcolor;

        if (mosx == mosw)
            mosx = 0;
        else
            mosx++;
    }

    // the mosaic can move opaque pixels anywhere along the line
    ObjXMin = 0;
    ObjXMax = 256;
    ObjDirtyMin = 0;
    ObjDirtyMax = 256;
}

// resolve the OBJ line into colours (melonDS InterleaveSprites, all priorities at once). The colour
// of a palette pixel comes from one table: standard palette entries at OBJ_StandardPal, the 16
// extended palettes below it, both exactly where the OBJ pixel's low 13 bits point.
template<bool windows>
void BananaRenderer2D::ResolveSpritesImpl()
{
    u8 present = 0;
    const u32* table = ObjTable.get();

    for (int i = ObjXMin; i < ObjXMax; i++)
    {
        u32 p = OBJLine[i];
        if (!(p & OBJ_IsOpaque) || (windows && !(WindowMask[i] & 0x10)))
        {
            OBJColor[i] = 0;
            continue;
        }

        u32 c = (p & OBJ_DirectColor) ? Conv15(p & 0x7FFF) : table[p & 0x1FFF];
        OBJColor[i] = c | (p & 0xFF000000);
        u32 prio = (p >> 16) & 0x3;
        OBJPrio[i] = prio;
        present |= (1 << prio);
    }

    OBJPrioPresent = present;
}

#ifdef BANANA_NEON
// ResolveSpritesImpl four pixels at a time: palette colours gathered with four scalar loads, direct colours
// converted in the lanes, no OBJPrio line (the compositor reads the priority from OBJLine)
template<bool windows>
void BananaRenderer2D::ResolveSpritesNeon(int x0, int x1)
{
    const u32* table = ObjTable.get();
    const uint32x4_t vopaque = vdupq_n_u32(OBJ_IsOpaque), vdirect = vdupq_n_u32(OBJ_DirectColor);
    const uint32x4_t one = vdupq_n_u32(1), m3 = vdupq_n_u32(3), zero = vdupq_n_u32(0);
    uint32x4_t present = zero;
    for (int i = x0; i < x1; i += 4)
    {
        uint32x4_t p = vld1q_u32(&OBJLine[i]);
        uint32x4_t op = vtstq_u32(p, vopaque);
        if (windows)
            op = vandq_u32(op, vtstq_u32(VFromBytes4(*(const u32*)&WindowMask[i]), vdupq_n_u32(0x10)));
        if (vmaxvq_u32(op) == 0)
        {
            vst1q_u32(&OBJColor[i], zero);
            continue;
        }
        uint32x4_t pal = vdupq_n_u32(table[vgetq_lane_u32(p, 0) & 0x1FFF]);
        pal = vsetq_lane_u32(table[vgetq_lane_u32(p, 1) & 0x1FFF], pal, 1);
        pal = vsetq_lane_u32(table[vgetq_lane_u32(p, 2) & 0x1FFF], pal, 2);
        pal = vsetq_lane_u32(table[vgetq_lane_u32(p, 3) & 0x1FFF], pal, 3);
        uint32x4_t dc = vorrq_u32(vorrq_u32(vshlq_n_u32(vandq_u32(p, vdupq_n_u32(0x1F)), 1),
                                            vshlq_n_u32(vandq_u32(p, vdupq_n_u32(0x3E0)), 4)),
                                  vshlq_n_u32(vandq_u32(p, vdupq_n_u32(0x7C00)), 7));
        uint32x4_t c = vbslq_u32(vtstq_u32(p, vdirect), dc, pal);
        c = vorrq_u32(c, vandq_u32(p, vdupq_n_u32(0xFF000000)));
        vst1q_u32(&OBJColor[i], vandq_u32(c, op));
        uint32x4_t prio = vandq_u32(vshrq_n_u32(p, 16), m3);
        present = vorrq_u32(present, vandq_u32(vshlq_u32(one, vreinterpretq_s32_u32(prio)), op));
    }
    OBJPrioPresent = (u8)VHorOr(present);
}
#endif

void BananaRenderer2D::ResolveSprites()
{
    int x0 = ObjXMin, x1 = ObjXMax;
    if (x1 <= x0)
    {
        memset(OBJColor, 0, sizeof(OBJColor));
        OBJPrioPresent = 0;
        return;
    }
#ifdef BANANA_NEON
    // (OBJLine is transparent outside the sprites' extent: the extent can be rounded out to 4 pixels)
    if (Banana2DFast) { x0 &= ~3; x1 = (x1 + 3) & ~3; }
#endif
    if (x0 > 0) memset(OBJColor, 0, x0 * 4);
    if (x1 < 256) memset(OBJColor + x1, 0, (256 - x1) * 4);

    if (!ObjExtValid && (R.DispCnt & (1u << 31)))
        ConvertOBJExtPals();

#ifdef BANANA_NEON
    if (Banana2DFast)
    {
        if (WindowsOn) ResolveSpritesNeon<true>(x0, x1);
        else           ResolveSpritesNeon<false>(x0, x1);
        return;
    }
#endif

    if (WindowsOn) ResolveSpritesImpl<true>();
    else           ResolveSpritesImpl<false>();
}


// ---- compositing -------------------------------------------------------------------------------

struct CompStep
{
    const u32* line;
    const u32* prio;   // null for a BG; the OBJ priority line for a sprite step
    u32 prioval;       // compared with the prio line's pixels and priomask
    u32 priomask;
    bool raw3d;        // line is the 3D layer's own line, converted here (DrawBG_3D's conversion)
};

// painter's algorithm over N layer lines, four pixels at a time with the top (and second) pixel kept
// in registers; startLine is the layer everything is painted onto (the backdrop, or a layer that
// covers the whole line). has3d: a step is the 3D layer's own line, made into DrawBG_3D's pixels here
// (opaque where the alpha byte is nonzero, with the 3D flag added); every other step's pixels are
// opaque exactly where their flag byte is nonzero, so the same test serves all steps
template<int N, bool needSecond, bool has3d>
static void CompositeN(const CompStep* steps, const u32* startLine, u32* outTop, u32* outSecond)
{
    // (the step pointers and priority values in registers for the whole line)
    const u32* line[N > 0 ? N : 1];
    const u32* prio[N > 0 ? N : 1];
    V4 prioval[N > 0 ? N : 1];
    V4 priomask[N > 0 ? N : 1];
    V4 orv[N > 0 ? N : 1];
    for (int k = 0; k < N; k++)
    {
        line[k] = steps[k].line;
        prio[k] = steps[k].prio;
        prioval[k] = VDup(steps[k].prioval);
        priomask[k] = VDup(steps[k].priomask);
        orv[k] = VDup(steps[k].raw3d ? 0x40000000 : 0);
    }
    const V4 flagbyte = VDup(0xFF000000);
    for (int i = 0; i < 256; i += 4)
    {
        V4 top = VLoad(startLine + i);
        V4 sec = VZero();
        for (int k = 0; k < N; k++)
        {
            V4 c = VLoad(line[k] + i);
            V4 m;
            if (has3d)
            {
                m = VNonZero(VAnd(c, flagbyte));
                c = VOr(c, orv[k]);
            }
            else
                m = VNonZero(c);
            if (prio[k])
                m = VAnd(m, VEq(VAnd(VLoad(prio[k] + i), priomask[k]), prioval[k]));
            if (needSecond)
                sec = VSel(m, top, sec);
            top = VSel(m, c, top);
        }
        VStore(outTop + i, top);
        if (needSecond)
            VStore(outSecond + i, sec);
    }
}

void BananaRenderer2D::Composite(u32* outTop, u32* outSecond, bool needSecond)
{
    // painter's order, back to front: for each priority 3..0, BG3, BG2, BG1, BG0 of that priority,
    // then the sprites of that priority (the same order as melonDS)
    CompStep steps[8];
    u8 full[8];
    int n = 0;
    // sprites of a single priority: every opaque OBJColor pixel has it, no priority test
    // (fast: the priority is read from OBJLine's bits 16-17, ResolveSpritesNeon doesn't write OBJPrio)
    const u32* objprio = !Banana2DFast ? OBJPrio : !(OBJPrioPresent & (OBJPrioPresent - 1)) ? nullptr : OBJLine;
    u32 priomask = Banana2DFast ? OBJ_BGPrioMask : ~0u;
    u32 prioshift = Banana2DFast ? 16 : 0;
    for (int p = 3; p >= 0; p--)
    {
        for (int bg = 3; bg >= 0; bg--)
        {
            if ((LayerDrawn & (1<<bg)) && ((R.BGCnt[bg] & 0x3) == p))
            {
                bool raw = (bg == 0) && Raw3D;
                steps[n] = { raw ? Line3D : BGLine(bg), nullptr, 0, 0, raw };
                full[n] = (LayerFull >> bg) & 1;
                n++;
            }
        }
        if (OBJPrioPresent & (1<<p))
        {
            steps[n] = { OBJColor, objprio, (u32)p << prioshift, priomask, false };
            full[n] = 0;
            n++;
        }
    }

    // layers under the top-most fully opaque layer (the two top-most when the second layer is
    // needed for blending) can't show through: paint onto that layer and skip what is below
    int start = 0;
    int wanted = needSecond ? 2 : 1;
    int found = 0;
    const u32* startLine = BackdropLine;
    for (int k = n - 1; k >= 0; k--)
    {
        if (full[k])
        {
            found++;
            if (found == wanted)
            {
                start = k + 1;
                startLine = steps[k].line;
                break;
            }
        }
    }

    const CompStep* s = steps + start;
    bool has3d = false;
    for (int k = start; k < n; k++) has3d |= steps[k].raw3d;
    switch (n - start)
    {
#define BANANA_COMP(N) case N: \
        if (has3d) { if (needSecond) CompositeN<N, true, true>(s, startLine, outTop, outSecond); else CompositeN<N, false, true>(s, startLine, outTop, outSecond); } \
        else       { if (needSecond) CompositeN<N, true, false>(s, startLine, outTop, outSecond); else CompositeN<N, false, false>(s, startLine, outTop, outSecond); } \
        break;
    BANANA_COMP(0) BANANA_COMP(1) BANANA_COMP(2) BANANA_COMP(3)
    BANANA_COMP(4) BANANA_COMP(5) BANANA_COMP(6) BANANA_COMP(7) BANANA_COMP(8)   // at most 4 BGs + 4 OBJ steps
    default: break;
#undef BANANA_COMP
    }
}

#ifdef BANANA_NEON
// melonDS's colour operations (GPU_ColorOp.h) on four pixels, exactly: red and blue are computed packed in one lane
// (each channel's sum stays below 2^16 before the shift, so neither reaches the other)
static inline uint32x4_t VBlendN(uint32x4_t v1, uint32x4_t v2, uint32x4_t eva, uint32x4_t evb, int shift)
{
    const uint32x4_t mrb = vdupq_n_u32(0x3F003F), mg = vdupq_n_u32(0x3F00);
    uint32x4_t rb = vdupq_n_u32(shift == 4 ? 0x080008 : 0x100010);
    uint32x4_t g = vdupq_n_u32(shift == 4 ? 0x000800 : 0x001000);
    rb = vmlaq_u32(rb, vandq_u32(v1, mrb), eva);
    rb = vmlaq_u32(rb, vandq_u32(v2, mrb), evb);
    g = vmlaq_u32(g, vandq_u32(v1, mg), eva);
    g = vmlaq_u32(g, vandq_u32(v2, mg), evb);
    if (shift == 4) { rb = vshrq_n_u32(rb, 4); g = vshrq_n_u32(g, 4); }
    else            { rb = vshrq_n_u32(rb, 5); g = vshrq_n_u32(g, 5); }
    uint32x4_t r = vminq_u32(vandq_u32(rb, vdupq_n_u32(0xFF)), vdupq_n_u32(0x3F));
    uint32x4_t b = vminq_u32(vandq_u32(rb, vdupq_n_u32(0x7F0000)), vdupq_n_u32(0x3F0000));
    g = vminq_u32(vandq_u32(g, vdupq_n_u32(0x7F00)), mg);
    return vorrq_u32(vorrq_u32(r, g), vorrq_u32(b, vdupq_n_u32(0xFF000000)));
}

template<bool up>
static inline uint32x4_t VBright(uint32x4_t val, uint32x4_t evy)
{
    uint32x4_t rb = vandq_u32(val, vdupq_n_u32(0x3F003F));
    uint32x4_t g = vandq_u32(val, vdupq_n_u32(0x3F00));
    if (up)
    {
        rb = vaddq_u32(rb, vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(8 * 0x010001), vsubq_u32(vdupq_n_u32(0x3F003F), rb), evy), 4), vdupq_n_u32(0x3F003F)));
        g = vaddq_u32(g, vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(8 * 0x000100), vsubq_u32(vdupq_n_u32(0x3F00), g), evy), 4), vdupq_n_u32(0x3F00)));
    }
    else
    {
        rb = vsubq_u32(rb, vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(7 * 0x010001), rb, evy), 4), vdupq_n_u32(0x3F003F)));
        g = vsubq_u32(g, vandq_u32(vshrq_n_u32(vmlaq_u32(vdupq_n_u32(7 * 0x000100), g, evy), 4), vdupq_n_u32(0x3F00)));
    }
    return vorrq_u32(vorrq_u32(rb, g), vdupq_n_u32(0xFF000000));
}

// a pixel's BLDCNT 1st-target bit (FirstBitTab) from its flag byte f
static inline uint32x4_t VFirstBit(uint32x4_t f)
{
    uint32x4_t r = vbslq_u32(vtstq_u32(f, vdupq_n_u32(0x40)), vdupq_n_u32(0x01), f);
    return vbslq_u32(vtstq_u32(f, vdupq_n_u32(0x80)), vdupq_n_u32(0x10), r);
}

template<bool up>
void BananaRenderer2D::BrightnessPassNeon(u32* dst)
{
    const uint32x4_t bc = vdupq_n_u32(R.BlendCnt), evy = vdupq_n_u32(R.EVY), m20 = vdupq_n_u32(0x20);
    const u8* wm = WindowsOn ? WindowMask : AllOnes;
    for (int i = 0; i < 256; i += 4)
    {
        uint32x4_t val = vld1q_u32(&TopLine[i]);
        uint32x4_t hit = vtstq_u32(VFirstBit(vshrq_n_u32(val, 24)), bc);
        hit = vandq_u32(hit, vtstq_u32(VFromBytes4(*(const u32*)&wm[i]), m20));
        vst1q_u32(&dst[i], vbslq_u32(hit, VBright<up>(val, evy), val));
    }
}

// EffectsPass four pixels at a time: every result the lanes can need, then the same choices as masks
template<u32 effect>
void BananaRenderer2D::EffectsPassNeon(u32* dst)
{
    const u32 blendCnt = R.BlendCnt;
    const uint32x4_t bc = vdupq_n_u32(blendCnt), evy = vdupq_n_u32(R.EVY);
    const uint32x4_t EVA = vdupq_n_u32(R.EVA), EVB = vdupq_n_u32(R.EVB);
    const uint32x4_t m1f = vdupq_n_u32(0x1F), m40 = vdupq_n_u32(0x40), m80 = vdupq_n_u32(0x80), m20 = vdupq_n_u32(0x20);
    const uint32x4_t sixteen = vdupq_n_u32(16), one = vdupq_n_u32(1), thirtytwo = vdupq_n_u32(32);
    const uint32x4_t zero = vdupq_n_u32(0);
    // brightness for sprite / 3D / BG pixels that aren't blended: BLDCNT bit 4 / bit 0 / the pixel's own bit
    const uint32x4_t sprBright = vdupq_n_u32((effect >= 2 && (blendCnt & 0x10)) ? ~0u : 0);
    const uint32x4_t d3Bright = vdupq_n_u32((effect >= 2 && (blendCnt & 0x01)) ? ~0u : 0);
    const u8* wm = WindowsOn ? WindowMask : AllOnes;

    for (int i = 0; i < 256; i += 4)
    {
        uint32x4_t val1 = vld1q_u32(&TopLine[i]);
        uint32x4_t val2 = vld1q_u32(&SecondLine[i]);
        uint32x4_t f1 = vshrq_n_u32(val1, 24);
        uint32x4_t f2 = vshrq_n_u32(val2, 24);
        // Target2Tab
        uint32x4_t t2 = vbslq_u32(vtstq_u32(f2, m40), vdupq_n_u32(0x100), vshlq_n_u32(f2, 8));
        t2 = vbslq_u32(vtstq_u32(f2, m80), vdupq_n_u32(0x1000), t2);
        uint32x4_t second = vtstq_u32(t2, bc);
        uint32x4_t inwin = vtstq_u32(VFromBytes4(*(const u32*)&wm[i]), m20);

        uint32x4_t spr = vtstq_u32(f1, m80);
        uint32x4_t d3 = vbicq_u32(vtstq_u32(f1, m40), spr);
        uint32x4_t bg = vmvnq_u32(vorrq_u32(spr, d3));

        uint32x4_t out = val1;
        uint32x4_t useBright;   // lanes that take the brightness result
        uint32x4_t useB4;       // lanes that take ColorBlend4

        // sprites: blended over a 2nd target (their own alpha for bitmap sprites), else maybe brightness
        useB4 = vandq_u32(spr, second);
        useBright = vandq_u32(vbicq_u32(spr, second), vandq_u32(sprBright, inwin));
        // BG pixels (and the backdrop): the effect where the pixel is a 1st target inside the effect window
        if (effect != 0)
        {
            uint32x4_t cond = vandq_u32(vandq_u32(bg, vtstq_u32(f1, bc)), inwin);
            if (effect == 1) useB4 = vorrq_u32(useB4, vandq_u32(cond, second));
            else             useBright = vorrq_u32(useBright, cond);
        }
        // 3D pixels: ColorBlend5 over a 2nd target, else maybe brightness
        uint32x4_t useB5 = vandq_u32(d3, second);
        useBright = vorrq_u32(useBright, vandq_u32(vbicq_u32(d3, second), vandq_u32(d3Bright, inwin)));

        if (vmaxvq_u32(useB4))
        {
            uint32x4_t own = vandq_u32(spr, vtstq_u32(f1, m40));
            uint32x4_t eva = vbslq_u32(own, vandq_u32(f1, m1f), EVA);
            uint32x4_t evb = vbslq_u32(own, vsubq_u32(sixteen, vandq_u32(f1, m1f)), EVB);
            out = vbslq_u32(useB4, VBlendN(val1, val2, eva, evb, 4), out);
        }
        if (vmaxvq_u32(useB5))
        {
            uint32x4_t eva = vaddq_u32(vandq_u32(f1, m1f), one);
            uint32x4_t b5 = VBlendN(val1, val2, eva, vsubq_u32(thirtytwo, eva), 5);
            b5 = vbslq_u32(vceqq_u32(eva, thirtytwo), val1, b5);
            out = vbslq_u32(useB5, b5, out);
        }
        if (effect >= 2 && vmaxvq_u32(useBright))
            out = vbslq_u32(useBright, VBright<effect == 2>(val1, evy), out);
        (void)zero;
        vst1q_u32(&dst[i], out);
    }
}
#endif

// brightness up/down with no 2nd targets in BLDCNT: the only effect that can apply is on 1st-target
// pixels inside the effect window
template<bool up>
void BananaRenderer2D::BrightnessPass(u32* dst)
{
    u32 blendCnt = R.BlendCnt;
    u32 evy = R.EVY;
    const u8* wm = WindowsOn ? WindowMask : AllOnes;

    for (int i = 0; i < 256; i++)
    {
        u32 val = TopLine[i];
        u32 f1 = val >> 24;
        u32 fb = (f1 & 0x40) ? 0x01 : f1;
        fb = (f1 & 0x80) ? 0x10 : fb;
        bool hit = (blendCnt & fb) && (wm[i] & 0x20);
        u32 e = up ? ColorBrightnessUp(val, evy, 0x8) : ColorBrightnessDown(val, evy, 0x7);
        dst[i] = hit ? e : val;
    }
}

// melonDS ColorComposite with the 2nd-target bits set: sprite and 3D blending, alpha, brightness
template<u32 effect>
void BananaRenderer2D::EffectsPass(u32* dst)
{
    u32 blendCnt = R.BlendCnt;
    const u8* wm = WindowsOn ? WindowMask : AllOnes;
    u32 EVA = R.EVA, EVB = R.EVB, EVY = R.EVY;

    for (int i = 0; i < 256; i++)
    {
        u32 val1 = TopLine[i];
        u32 val2 = SecondLine[i];
        u32 flag1 = val1 >> 24;
        bool second = blendCnt & Target2Tab.v[val2 >> 24];

        u32 out;
        if (flag1 & 0x80)
        {
            // sprite blending (semi-transparent or bitmap sprite over a 2nd target)
            if (second)
            {
                u32 eva, evb;
                if (flag1 & 0x40)
                {
                    eva = flag1 & 0x1F;
                    evb = 16 - eva;
                }
                else
                {
                    eva = EVA;
                    evb = EVB;
                }
                out = ColorBlend4(val1, val2, eva, evb);
            }
            else if (effect >= 2 && (blendCnt & 0x10) && (wm[i] & 0x20))
                out = (effect == 2) ? ColorBrightnessUp(val1, EVY, 0x8) : ColorBrightnessDown(val1, EVY, 0x7);
            else
                out = val1;
        }
        else if (flag1 & 0x40)
        {
            // 3D layer over a 2nd target
            if (second)
                out = ColorBlend5(val1, val2);
            else if (effect >= 2 && (blendCnt & 0x01) && (wm[i] & 0x20))
                out = (effect == 2) ? ColorBrightnessUp(val1, EVY, 0x8) : ColorBrightnessDown(val1, EVY, 0x7);
            else
                out = val1;
        }
        else
        {
            out = val1;
            if (effect != 0 && (blendCnt & flag1) && (wm[i] & 0x20))
            {
                if (effect == 1)
                {
                    if (second) out = ColorBlend4(val1, val2, EVA, EVB);
                }
                else if (effect == 2)
                    out = ColorBrightnessUp(val1, EVY, 0x8);
                else
                    out = ColorBrightnessDown(val1, EVY, 0x7);
            }
        }
        dst[i] = out;
    }
}


// ---- scanline ----------------------------------------------------------------------------------

void BananaRenderer2D::DrawScanline(u32 line)
{
    Banana2DRegs regs;
    PrepareScanline(regs);
    DrawScanlineFrom(line, regs, Parent.Output3D);
}

void BananaRenderer2D::PrepareScanline(Banana2DRegs& regs)
{
    regs.From(GPU2D);
    if (NeedResync)
        DoResync();

    if (!GPU2D.Enabled || GPU2D.ForcedBlank)
        return;

    // the windows' horizontal state after the line, as CalcWindowMask leaves it: whatever it was before the line,
    // the last of the two edges wins (x2 when they are equal)
    u32 dispcnt = GPU2D.DispCnt;
    if (dispcnt & (1<<14))
        GPU2D.Win1Active = (GPU2D.Win1Active & 0x1) | ((GPU2D.Win1Coords[1] < GPU2D.Win1Coords[0]) ? 0x2 : 0);
    if (dispcnt & (1<<13))
        GPU2D.Win0Active = (GPU2D.Win0Active & 0x1) | ((GPU2D.Win0Coords[1] < GPU2D.Win0Coords[0]) ? 0x2 : 0);

    if (ExtPalsResynced)
    {
        regs.BGExtChanged = regs.OBJExtChanged = true;
        ExtPalsResynced = false;
    }

    if (GPU2D.Num == 0)
    {
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
        regs.BGExtChanged |= GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
        regs.OBJExtChanged |= GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
        if (CopyTo)
        {
            CopyDirty(bgDirty, BGVRAM, GPU.VRAMFlat_ABG);
            CopyDirty(bgExtPalDirty, BGExtMem, GPU.VRAMFlat_ABGExtPal);
            CopyDirty(objExtPalDirty, OBJExtMem, GPU.VRAMFlat_AOBJExtPal);
        }
    }
    else
    {
        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
        regs.BGExtChanged |= GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
        regs.OBJExtChanged |= GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
        if (CopyTo)
        {
            CopyDirty(bgDirty, BGVRAM, GPU.VRAMFlat_BBG);
            CopyDirty(bgExtPalDirty, BGExtMem, GPU.VRAMFlat_BBGExtPal);
            CopyDirty(objExtPalDirty, OBJExtMem, GPU.VRAMFlat_BOBJExtPal);
        }
    }

    // the palettes: converted by the drawing when changed (bgbits: the BG half and its colour 0, the backdrop)
    u32 shift = GPU2D.Num * 2;
    u32 bgbits = (0x01 | 0x10) << shift;
    u32 objbits = 0x02 << shift;
    u32 paloff = GPU2D.Num ? 0x400 : 0;
    if ((GPU.PaletteDirty & bgbits) || PalBGStale)
    {
        regs.PalBG = true;
        regs.PalBGStale = PalBGStale;
        if (CopyTo) CopyTo(CopyCtx, (void*)PalMem, &GPU.Palette[paloff], 0x200);
        GPU.PaletteDirty &= ~bgbits;
        PalBGStale = false;
    }
    if ((GPU.PaletteDirty & objbits) || PalOBJStale)
    {
        regs.PalOBJ = true;
        if (CopyTo) CopyTo(CopyCtx, (void*)(PalMem + 0x200), &GPU.Palette[paloff + 0x200], 0x200);
        GPU.PaletteDirty &= ~objbits;
        PalOBJStale = false;
    }
}

void BananaRenderer2D::DrawScanlineFrom(u32 line, const Banana2DRegs& regs, const u32* line3d)
{
    R = regs;
    Line3D = line3d;
    u32* dst = Parent.Output2D[GPU2D.Num];

    if (!R.Enabled)
    {
        // if this 2D unit is disabled in POWCNT, the output is a fixed color
        // (black for unit A, white for unit B)
        u32 fillcolor = (GPU2D.Num == 0) ? 0xFF000000 : 0xFF3F3F3F;
        for (int i = 0; i < 256; i++)
            dst[i] = fillcolor;
        return;
    }

    if (R.ForcedBlank)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFF3F3F3F;
        return;
    }

    if (R.BGExtChanged) BGExtPalValid = 0;
    if (R.OBJExtChanged) ObjExtValid = false;
    if (R.PalBG || R.PalOBJ)
        UpdatePalettes(R.PalBG, R.PalOBJ, R.PalBGStale);

    u32 dispcnt = R.DispCnt;
    u8 enable = R.LayerEnable;

    WindowsOn = (dispcnt & 0xE000) != 0;
    if (WindowsOn)
        CalcWindowMask();

    LayerDrawn = 0;
    LayerFull = 0;
    Raw3D = false;

    // BG types per mode: 0 none, 1 text, 2 affine, 3 extended, 4 large, 5 3D
    enum { T_None, T_Text, T_Affine, T_Ext, T_Large, T_3D };
    u8 type[4] = { T_None, T_None, T_None, T_None };
    bool bg0_3d = (!GPU2D.Num) && (dispcnt & 0x8);
    switch (dispcnt & 0x7)
    {
    case 0: type[0] = T_Text; type[1] = T_Text; type[2] = T_Text;   type[3] = T_Text;   break;
    case 1: type[0] = T_Text; type[1] = T_Text; type[2] = T_Text;   type[3] = T_Affine; break;
    case 2: type[0] = T_Text; type[1] = T_Text; type[2] = T_Affine; type[3] = T_Affine; break;
    case 3: type[0] = T_Text; type[1] = T_Text; type[2] = T_Text;   type[3] = T_Ext;    break;
    case 4: type[0] = T_Text; type[1] = T_Text; type[2] = T_Affine; type[3] = T_Ext;    break;
    case 5: type[0] = T_Text; type[1] = T_Text; type[2] = T_Ext;    type[3] = T_Ext;    break;
    case 6: type[0] = bg0_3d ? T_3D : T_None; type[2] = T_Large; break;
    case 7: type[0] = T_Text; type[1] = T_Text; break;
    }
    if (bg0_3d && type[0] == T_Text) type[0] = T_3D;

    u32 bgmosaic = R.BGMosaicSize[0];

    for (u32 bg = 0; bg < 4; bg++)
    {
        if (!(enable & (1<<bg)) || type[bg] == T_None)
            continue;

        u16 bgcnt = R.BGCnt[bg];
        u32* L = BGLine(bg);

        switch (type[bg])
        {
        case T_3D:
            // with no window to apply, the compositor converts the 3D line itself (one pass less)
            if (Banana2DFast && !WindowsOn) Raw3D = true;
            else                            DrawBG_3D();
            LayerDrawn |= (1<<bg);
            break;
        case T_Text:
            if (bgcnt & (1<<7)) DrawBG_Text<true>(line, bg);
            else                DrawBG_Text<false>(line, bg);
            break;
        case T_Affine:
            DrawBG_Affine(line, bg);
            break;
        case T_Ext:
            DrawBG_Extended(line, bg);
            break;
        case T_Large:
            DrawBG_Large(line);
            break;
        }

        if (!(LayerDrawn & (1<<bg)))
            continue;

        if (type[bg] != T_3D && (bgcnt & (1<<6)) && bgmosaic > 0)
            ApplyMosaicX(L, bgmosaic);

        if (WindowsOn)
            ApplyWindow(L, 1 << bg);
    }
    if (WindowsOn)
        LayerFull = 0;

    if ((enable & (1<<4)) && NumSprites)
    {
        ApplySpriteMosaicX();
        ResolveSprites();
    }
    else
        OBJPrioPresent = 0;

    // which colour effects can apply on this line? blending (sprite, 3D or the alpha effect) needs
    // a 2nd target in BLDCNT; brightness needs 1st targets
    u32 blendCnt = R.BlendCnt;
    u32 effect = (blendCnt >> 6) & 0x3;
    bool secondTargets = (blendCnt & 0x3F00) != 0;
    bool possible = secondTargets || ((effect >= 2) && (blendCnt & 0x3F));

    if (!possible)
    {
        Composite(dst, nullptr, false);
        return;
    }

    if (!secondTargets)
    {
        Composite(TopLine, nullptr, false);
#ifdef BANANA_NEON
        if (Banana2DFast)
        {
            if (effect == 2) BrightnessPassNeon<true>(dst);
            else             BrightnessPassNeon<false>(dst);
            return;
        }
#endif
        if (effect == 2) BrightnessPass<true>(dst);
        else             BrightnessPass<false>(dst);
        return;
    }

    Composite(TopLine, SecondLine, true);
#ifdef BANANA_NEON
    if (Banana2DFast)
    {
        switch (effect)
        {
        case 0: EffectsPassNeon<0>(dst); break;
        case 1: EffectsPassNeon<1>(dst); break;
        case 2: EffectsPassNeon<2>(dst); break;
        default: EffectsPassNeon<3>(dst); break;
        }
        return;
    }
#endif
    switch (effect)
    {
    case 0: EffectsPass<0>(dst); break;
    case 1: EffectsPass<1>(dst); break;
    case 2: EffectsPass<2>(dst); break;
    default: EffectsPass<3>(dst); break;
    }
}

}
