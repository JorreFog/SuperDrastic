// Gengis Engine as melonDS's 3D renderer: see gengis_renderer.h.
#include <pthread.h>
#include <sys/resource.h>
#include "gengis_renderer.h"
#include "emu.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <time.h>

#include "GPU.h"
#include "NDS.h"
#include "Platform.h"

using namespace melonDS;

namespace bananads {

static inline uint64_t ThreadNs() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static inline uint64_t MonoNs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

GengisRenderer3D::GengisRenderer3D(melonDS::GPU3D& gpu3D, int threads, bool defer, int scale)
    : Renderer3D(gpu3D), Scale_(scale >= 2 && scale <= 4 ? scale : 1) {
    gx_zva = Zva;
    gx_stats = Stats != 0;
    gx_trim = Trim;
    gx_hrneon = HrNeon < 0 ? Scale_ >= 3 : HrNeon != 0;
    gx_lazy = Lazy < 0 ? Scale_ >= 3 : Lazy != 0;
    Sub = Scale_ == 4 && Units ? (Units == 2 ? 4 : 2) : 1;
    VbConvOnly = VbWait < 0 ? Scale_ >= 3 : VbWait == 0;
    NU = GX_NBINS * Sub;
    Next.store(NU);
    gx_init();
    gx_defer = defer;
    for (auto& o : OutBuf) {
        o = (uint32_t*)aligned_alloc(64, OutWords() * sizeof(uint32_t));
        memset(o, 0, OutWords() * sizeof(uint32_t));
    }
    Out = OutBuf[0];
    for (auto& d : Done) d.store(0);
    if (threads < 0) threads = 0;
    Active = WakeN < 1 ? 1 : WakeN;
    for (int i = 0; i < threads; i++) Threads.emplace_back([this, i] { Worker(i); });
    Platform::Log(Platform::LogLevel::Info, "[gengis] Gengis Engine at %dx, %d render threads%s\n", Scale_, threads,
                  defer ? ", deferred shading" : "");
}

GengisRenderer3D::~GengisRenderer3D() {
    WaitAll();
    if (Verify)
        Platform::Log(Platform::LogLevel::Info, "[gengis] verify: %lu bins rendered again on the reference paths, %lu differ (hrneon %d); CPU %.3f ms a bin, reference %.3f\n",
                      (unsigned long)VerBins.load(), (unsigned long)VerBad.load(), gx_hrneon,
                      VerNewNs.load() / 1e6 / (VerBins.load() + 1e-9), VerRefNs.load() / 1e6 / (VerBins.load() + 1e-9));
    Platform::Log(Platform::LogLevel::Info, "[gengis] the emulator's thread drew %lu of %lu bins and converted %lu frames; %lu late frames (render.gengis_wake=%d of %zu)\n",
                  EmuBins, AllBins.load(), EmuConverts, LateFrames.load(), Active, Threads.size());
    {
        std::lock_guard<std::mutex> l(M);
        Quit = true;
    }
    Wake.notify_all();
    WakeLate.notify_all();
    for (auto& t : Threads) t.join();
    for (auto& o : OutBuf) free(o);
}

void GengisRenderer3D::Reset() {
    WaitAll();
    Textures.clear();
    Stale.clear();
    StaleBytes = 0;
    for (auto& o : OutBuf) memset(o, 0, OutWords() * sizeof(uint32_t));
    for (auto& k : BinKey) k.clear();
}

// ---- textures: decoded once into the rasterizer's formats (u32 r6 | g6 << 8 | b6 << 16 | a5 << 24, or u8 indices
// into a u32 palette), kept until texture or palette VRAM changes ----

static uint32_t Texel(uint16_t c, uint32_t a) { return gx_expand555(c) | a << 24; }

// the blocks of [start, start + len) (wrapping at the VRAM's size: the accessors mask the address) into a bitmap of
// nblk 512-byte blocks
static void MarkRange(uint64_t* bits, uint32_t nblk, uint32_t start, uint32_t len) {
    if (!len) return;
    const uint32_t b0 = start >> 9, b1 = (start + len - 1) >> 9;
    if (b1 - b0 + 1 >= nblk) {
        for (uint32_t i = 0; i < nblk / 64; i++) bits[i] = ~0ull;
        return;
    }
    for (uint32_t b = b0; b <= b1; b++) {
        const uint32_t k = b & (nblk - 1);
        bits[k >> 6] |= 1ull << (k & 63);
    }
}

void GengisRenderer3D::Decode(Texture& tx, uint32_t texparam, uint32_t texpal) {
    const u8* vram = GPU.VRAMFlat_Texture;
    const u8* pram = GPU.VRAMFlat_TexPal;
    auto col = [&](uint32_t a) { uint16_t v; memcpy(&v, &pram[a & 0x1FFFF], 2); return v; };
    auto byte = [&](uint32_t a) { return vram[a & 0x7FFFF]; };
    auto half = [&](uint32_t a) { uint16_t v; memcpy(&v, &vram[a & 0x7FFFE], 2); return v; };
    const uint32_t addr = (texparam & 0xFFFF) << 3;
    const uint32_t w = 8u << ((texparam >> 20) & 7), h = 8u << ((texparam >> 23) & 7), n = w * h;
    const uint32_t fmt = (texparam >> 26) & 7, alpha0 = (texparam & (1u << 29)) ? 0 : 31;
    // what the decode reads (byte() and half() mask with 0x7FFFF, col() with 0x1FFFF): the formats' texel bytes
    // and palette entries; the 4x4 compressed format's slot-1 words and palette entries are marked as read
    switch (fmt) {
    case 1: case 4: case 6: MarkRange(tx.texblk, 1024, addr, n); break;
    case 2: MarkRange(tx.texblk, 1024, addr, n >> 2); break;
    case 3: MarkRange(tx.texblk, 1024, addr, n >> 1); break;
    case 7: MarkRange(tx.texblk, 1024, addr, n << 1); break;
    default: break;
    }
    switch (fmt) {
    case 1: MarkRange(tx.palblk, 256, (texpal << 4) & 0x1FFFF, 64); break;
    case 2: MarkRange(tx.palblk, 256, (texpal << 3) & 0x1FFFF, 8); break;
    case 3: MarkRange(tx.palblk, 256, (texpal << 4) & 0x1FFFF, 32); break;
    case 4: MarkRange(tx.palblk, 256, (texpal << 4) & 0x1FFFF, 512); break;
    case 6: MarkRange(tx.palblk, 256, (texpal << 4) & 0x1FFFF, 16); break;
    default: break;
    }
    gx_texture& t = tx.t;
    t.params = texparam;
    t.width = (uint16_t)w;
    t.height = (uint16_t)h;
    t.format = (uint8_t)fmt;
    t.paletted = fmt != 5 && fmt != 7;
    if (t.paletted) {
        tx.idx.resize(n);
        tx.pal.assign(256, 0);
        switch (fmt) {
        case 1:  // A3I5
            for (uint32_t i = 0; i < n; i++) tx.idx[i] = byte(addr + i);
            for (uint32_t b = 0; b < 256; b++) {
                const uint32_t a3 = b >> 5;
                tx.pal[b] = Texel(col((texpal << 4) + ((b & 31) << 1)), (a3 << 2) + (a3 >> 1));
            }
            break;
        case 2:  // 4 colours
            for (uint32_t i = 0; i < n; i++) tx.idx[i] = (byte(addr + (i >> 2)) >> ((i & 3) << 1)) & 3;
            for (uint32_t b = 0; b < 4; b++) tx.pal[b] = Texel(col((texpal << 3) + (b << 1)), b ? 31 : alpha0);
            break;
        case 3:  // 16 colours
            for (uint32_t i = 0; i < n; i++) tx.idx[i] = (byte(addr + (i >> 1)) >> ((i & 1) << 2)) & 15;
            for (uint32_t b = 0; b < 16; b++) tx.pal[b] = Texel(col((texpal << 4) + (b << 1)), b ? 31 : alpha0);
            break;
        case 4:  // 256 colours
            for (uint32_t i = 0; i < n; i++) tx.idx[i] = byte(addr + i);
            for (uint32_t b = 0; b < 256; b++) tx.pal[b] = Texel(col((texpal << 4) + (b << 1)), b ? 31 : alpha0);
            break;
        default:  // 6: A5I3
            for (uint32_t i = 0; i < n; i++) tx.idx[i] = byte(addr + i);
            for (uint32_t b = 0; b < 256; b++) tx.pal[b] = Texel(col((texpal << 4) + ((b & 7) << 1)), b >> 3);
            break;
        }
        t.texels = tx.idx.data();
        t.palette = tx.pal.data();
        return;
    }
    tx.texels.resize(n);
    if (fmt == 7) {  // direct colour
        for (uint32_t i = 0; i < n; i++) {
            const uint16_t c = half(addr + 2 * i);
            tx.texels[i] = Texel(c, (c & 0x8000) ? 31 : 0);
        }
    } else {  // 5: 4x4 compressed (melonDS's TextureLookup, texel by texel)
        const uint32_t pbase = texpal << 4;
        auto mix = [](uint16_t c0, uint16_t c1, uint32_t k0, uint32_t k1, uint32_t sh) {
            const uint32_t r = ((c0 & 0x1F) * k0 + (c1 & 0x1F) * k1) >> sh;
            const uint32_t g = (((c0 & 0x3E0) * k0 + (c1 & 0x3E0) * k1) >> sh) & 0x3E0;
            const uint32_t b = (((c0 & 0x7C00) * k0 + (c1 & 0x7C00) * k1) >> sh) & 0x7C00;
            return (uint16_t)(r | g | b);
        };
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t va = addr + ((y & 0x3FC) * (w >> 2)) + (x & 0x3FC) + (y & 3);
                va &= 0x7FFFF;
                uint32_t slot1 = 0x20000 + ((va & 0x1FFFC) >> 1);
                if (va >= 0x40000) slot1 += 0x10000;
                const uint32_t val = (va >= 0x20000 && va < 0x40000) ? 0 : (byte(va) >> (2 * (x & 3))) & 3;
                const uint16_t info = half(slot1);
                const uint32_t po = pbase + ((info & 0x3FFF) << 2), mode = info >> 14;
                {   // (all the reads below: va's byte, slot1's half, palette entries po..po + 7 masked)
                    const uint32_t vb = va >> 9, sb = (slot1 & 0x7FFFE) >> 9, pb0 = (po & 0x1FFFF) >> 9, pb1 = ((po + 7) & 0x1FFFF) >> 9;
                    tx.texblk[vb >> 6] |= 1ull << (vb & 63);
                    tx.texblk[sb >> 6] |= 1ull << (sb & 63);
                    tx.palblk[pb0 >> 6] |= 1ull << (pb0 & 63);
                    tx.palblk[pb1 >> 6] |= 1ull << (pb1 & 63);
                }
                uint16_t c = 0;
                uint32_t a = 31;
                switch (val) {
                case 0: c = col(po); break;
                case 1: c = col(po + 2); break;
                case 2:
                    if (mode == 1) c = mix(col(po), col(po + 2), 1, 1, 1);
                    else if (mode == 3) c = mix(col(po), col(po + 2), 5, 3, 3);
                    else c = col(po + 4);
                    break;
                default:
                    if (mode == 2) c = col(po + 6);
                    else if (mode == 3) c = mix(col(po), col(po + 2), 3, 5, 3);
                    else a = 0;
                    break;
                }
                tx.texels[y * w + x] = Texel(c, a);
            }
    }
    t.texels = tx.texels.data();
    t.palette = nullptr;
}

const gx_texture* GengisRenderer3D::GetTexture(uint32_t texparam, uint32_t texpal) {
    const uint32_t fmt = (texparam >> 26) & 7;
    const uint64_t key = (uint64_t)(texparam & 0x3FF0FFFF) | (fmt == 7 ? 0 : (uint64_t)(texpal & 0x1FFF) << 32);
    auto& e = Textures[key];
    if (!e) {
        e = std::make_unique<Texture>();
        const uint64_t d0 = Stats ? ThreadNs() : 0;
        Decode(*e, texparam, texpal);
        // render.gengis_reuse: the decode's identity (in the unused +8), kept from the dropped decode of the same
        // texture when the new one has the same texels and palette (a VRAM write of the same bytes, or an
        // animation back to a frame it had): bins that use it can still be reused
        uint64_t serial = 0;
        auto st = Stale.find(key);
        if (st != Stale.end()) {
            const Texture& o = *st->second;
            if (o.texels == e->texels && o.idx == e->idx && o.pal == e->pal) memcpy(&serial, o.t.pad0, sizeof serial);
            StaleBytes -= o.texels.size() * 4 + o.idx.size() + o.pal.size() * 4;
            Stale.erase(st);
            if (Stats) StSameTex += serial != 0;
        }
        if (!serial) serial = ++TexSerial;
        memcpy(e->t.pad0, &serial, sizeof serial);
        if (Scale_ >= 3) gx_tex_minalpha_store((uint8_t*)&e->t);   // (fused.c's tex_min_alpha: once per decode)
        if (Stats) { StDecNs += ThreadNs() - d0; StDecoded++; StDecTexels += (uint64_t)e->t.width * e->t.height; }
    }
    return &e->t;
}

// texture or palette VRAM changed (melonDS's flat copies: only their dirty blocks were copied): the entries that read
// a dirty block are decoded again when next used, the others are as they were
void GengisRenderer3D::InvalidateTextures(const uint64_t* texdirty, const uint64_t* paldirty, bool tex, bool pal) {
    for (auto it = Textures.begin(); it != Textures.end();) {
        const Texture& e = *it->second;
        uint64_t hit = 0;
        if (tex) for (int i = 0; i < 1024 / 64; i++) hit |= e.texblk[i] & texdirty[i];
        if (pal) for (int i = 0; i < 256 / 64; i++) hit |= e.palblk[i] & paldirty[i];
        if (hit) {
            // (GetTexture compares the next decode with it; at most 512 entries and ~16 MB of decodes kept)
            const Texture& t = *it->second;
            const size_t bytes = t.texels.size() * 4 + t.idx.size() + t.pal.size() * 4;
            if (Stale.size() >= 512 || StaleBytes + bytes > (16u << 20)) { Stale.clear(); StaleBytes = 0; }
            if (bytes <= (16u << 20)) { StaleBytes += bytes; Stale[it->first] = std::move(it->second); }
            it = Textures.erase(it);
        } else ++it;
    }
}

// ---- melonDS's polygons as the rasterizer's ----

melonDS::u32 GengisRenderer3D::DispMask = ~0u;

void GengisRenderer3D::Convert() {
    const u32 d3 = GPU3D.RenderDispCnt;
    Polys.clear();
    std::vector<gx_poly>& trl = Trl;
    trl.clear();
    for (u32 i = 0; i < GPU3D.RenderNumPolygons; i++) {
        const Polygon* p = GPU3D.RenderPolygonRAM[i];
        if (p->Degenerate) continue;
        const unsigned n = p->NumVertices;
        if (n < 1 || n > 10) continue;
        gx_poly g{};
        unsigned ytop = 0xFFFF, ybot = 0;
        uint32_t flags = 0;
        if (p->Translucent) flags |= GXF_TRANSLUCENT;
        const u32 fmt = (p->TexParam >> 26) & 7;
        if (fmt && (d3 & 1)) flags |= GXF_TEXTURED;
        if (p->WBuffer) flags |= GXF_WBUFFER;
        bool flat = true, constdepth = true, affine = true;
        // the walker starts at vertex 0, which must be the top one. melonDS's VTop is the top at 1x; at 2x two
        // vertices on one 1x line can be a 2x line apart, and starting at the lower one walked no edge for the
        // lines between: the spans there kept another polygon's x (a thin quad's line drawn from x 9 to 273,
        // HeartGold's wind turbine). So at 2x the first vertex from VTop with the least 2x y (VTop itself when it is
        // still the top: the same order as before)
        unsigned top = p->VTop;
        if (Scale_ == 2)
            for (unsigned k = 1; k < n; k++) {
                const unsigned vi = (p->VTop + k) % n;
                if (p->Vertices[vi]->HiresPosition[1] >> 3 < p->Vertices[top]->HiresPosition[1] >> 3) top = vi;
            }
        else if (Scale_ >= 3)  // (3x, 4x: the same with floor(3 y), floor(4 y))
            for (unsigned k = 1; k < n; k++) {
                const unsigned vi = (p->VTop + k) % n;
                if ((p->Vertices[vi]->HiresPosition[1] * Scale_) >> 4 < (p->Vertices[top]->HiresPosition[1] * Scale_) >> 4) top = vi;
            }
        for (unsigned k = 0; k < n; k++) {
            const unsigned vi = (top + k) % n;
            const Vertex* v = p->Vertices[vi];
            uint8_t* r = g.vtx[k];
            const int32_t w = p->WBuffer ? p->FinalZ[vi] : p->FinalW[vi];
            // at 2x the position from melonDS's sub-pixel one (4 fraction bits): floor(2 x), as 1x is floor(x)
            // (3x, 4x, experimental: floor(3 x), floor(4 x) likewise)
            const uint16_t x = (uint16_t)(Scale_ == 2 ? v->HiresPosition[0] >> 3 : Scale_ >= 3 ? (v->HiresPosition[0] * Scale_) >> 4 : v->FinalPosition[0]);
            const uint16_t y = (uint16_t)(Scale_ == 2 ? v->HiresPosition[1] >> 3 : Scale_ >= 3 ? (v->HiresPosition[1] * Scale_) >> 4 : v->FinalPosition[1]);
            const uint16_t z = (uint16_t)(p->FinalZ[vi] >> 9);
            const uint16_t c = (uint16_t)((v->FinalColor[0] >> 4) | (v->FinalColor[1] >> 4) << 5 | (v->FinalColor[2] >> 4) << 10);
            memcpy(r + 0, &w, 4);
            memcpy(r + 4, &x, 2);
            memcpy(r + 6, &y, 2);
            memcpy(r + 8, &z, 2);
            memcpy(r + 10, &c, 2);
            memcpy(r + 12, &v->TexCoords[0], 2);
            memcpy(r + 14, &v->TexCoords[1], 2);
            if (k) {
                flat &= !memcmp(r + 10, g.vtx[0] + 10, 2);
                constdepth &= p->WBuffer ? !memcmp(r, g.vtx[0], 4) : !memcmp(r + 8, g.vtx[0] + 8, 2);
                affine &= !memcmp(r, g.vtx[0], 4);
            }
            if (y < ytop) ytop = y;
            if (y > ybot) ybot = y;
        }
        if (flat) flags |= GXF_FLAT;
        if (constdepth) flags |= GXF_CONSTDEPTH;
        if (affine) flags |= GXF_AFFINE;
        g.ytop = (uint16_t)ytop;
        g.ybot = (uint16_t)ybot;
        g.count = (uint8_t)n;
        g.flags = (uint8_t)flags;
        const uint16_t wrap = (uint16_t)((p->TexParam >> 16) & 0xF);
        const uint32_t a8 = n | flags << 8 | (ybot & 0x1FF) << 23;   // (2x: ybot <= 384)
        const gx_texture* tex = (flags & GXF_TEXTURED) ? GetTexture(p->TexParam, p->TexPalette) : nullptr;
        memcpy(g.rec + 2, &wrap, 2);
        memcpy(g.rec + 4, &p->Attr, 4);
        memcpy(g.rec + 8, &a8, 4);
        memcpy(g.rec + 0x10, &tex, sizeof tex);
        (p->Translucent ? trl : Polys).push_back(g);
    }
    Frame.ntranslucent = (unsigned)trl.size();
    if (Stats) { StOpa += Polys.size(); StTrl += trl.size(); }
    Polys.insert(Polys.end(), trl.begin(), trl.end());
    Frame.polys = Polys.data();
    Frame.npolys = (unsigned)Polys.size();
    YRange.resize(Polys.size());
    for (size_t i = 0; i < Polys.size(); i++) YRange[i] = Polys[i].ytop | (uint32_t)Polys[i].ybot << 16;
    Frame.yr = YRange.data();

    Frame.d3 = d3 & DispMask;	// edge marking (bit 5), fog (bit 7): render.edge_marking, render.fog
    Frame.aref = GPU3D.RenderAlphaRef;
    const u32 a1 = GPU3D.RenderClearAttr1, a2 = GPU3D.RenderClearAttr2;
    Frame.clear_color = gx_expand555(a1 & 0x7FFF) | ((a1 >> 16) & 0x1F) << 24 | ((a1 >> 15) & 1) << 31;
    Frame.clear_attr = ((a2 & 0x7FFF) * 0x200 + 0x1FF) | (a1 & 0x3F000000);
    Frame.rear_col = (const uint16_t*)&GPU.VRAMFlat_Texture[0x40000];
    Frame.rear_dep = (const uint16_t*)&GPU.VRAMFlat_Texture[0x60000];
    Frame.rear_x = (uint8_t)(a2 >> 16);
    Frame.rear_y = (uint8_t)(a2 >> 24);
    for (int i = 0; i < 32; i++) {
        const uint8_t d0 = GPU3D.RenderFogDensityTable[i + 1], d1 = GPU3D.RenderFogDensityTable[i + 2];
        Frame.fog_table[i] = d0;
        Frame.fog_table[32 + i] = (uint8_t)(int8_t)(d1 - d0);
    }
    Frame.fog_color = gx_expand555(GPU3D.RenderFogColor & 0x7FFF) | ((GPU3D.RenderFogColor >> 16) & 0x1F) << 24;
    Frame.fog_offset = GPU3D.RenderFogOffset;
    for (int i = 0; i < 8; i++) {
        const uint32_t e = gx_expand555(GPU3D.RenderEdgeTable[i]);
        Frame.edge[i] = (uint8_t)e;
        Frame.edge[8 + i] = (uint8_t)(e >> 8);
        Frame.edge[16 + i] = (uint8_t)(e >> 16);
    }
    for (int i = 0; i < 32; i++) {
        const uint32_t t = gx_expand555(GPU3D.RenderToonTable[i]);
        Frame.toon[i] = (uint8_t)t;
        Frame.toon[32 + i] = (uint8_t)(t >> 8);
        Frame.toon[64 + i] = (uint8_t)(t >> 16);
    }
    // render.gengis_keyskip: while no bin was reusable for 32 rendered frames (a moving camera: a race), the keys are
    // built on 2 frames of every 16 only (enough to see the scene stop); a frame without keys reuses nothing and
    // leaves no keys for the next one to match
    if (KeySkip && Scale_ >= 3 && ReuseMiss > 32 && (KeyFrames++ & 15) > 1) {
        for (unsigned b = 0; b < GX_NBINS; b++) { BinKey[b].clear(); BinReuse[b] = false; }
        ReuseMiss++;
    } else {
        BinKeys();
        bool any = false;
        for (unsigned b = 0; b < GX_NBINS; b++) any |= BinReuse[b];
        ReuseMiss = any ? 0 : ReuseMiss + 1;
        if (any) KeyFrames = 0;
    }
}

// render.gengis_reuse: a bin is a pure function of the frame state the rasterizer reads and of the polygons that
// reach its window (the bin's lines and one above and below), in order, with their textures' contents. Each bin's
// inputs are kept as bytes (each texture by the serial of its decode: a texture decoded again counts as changed);
// a bin whose inputs equal the last rendered frame's is copied from that frame's rows (the other buffer, complete:
// every rendered frame renders or copies all its bins) instead of rendered. Not with the rear-plane bitmap (VRAM
// the bytes do not cover)
void GengisRenderer3D::BinKeys() {
    const unsigned BL = GX_BL * Scale_, H = GX_H * Scale_;
    uint8_t fs[sizeof(gx_frame)];
    size_t nfs = 0;
    auto put = [&](const void* p, size_t n) { memcpy(fs + nfs, p, n); nfs += n; };
    put(&Frame.d3, 4); put(&Frame.aref, 4); put(&Frame.clear_color, 4); put(&Frame.clear_attr, 4);
    put(&Frame.rear_x, 1); put(&Frame.rear_y, 1); put(Frame.fog_table, sizeof Frame.fog_table);
    put(&Frame.fog_color, 4); put(&Frame.fog_offset, 4); put(Frame.edge, sizeof Frame.edge); put(Frame.toon, sizeof Frame.toon);
    const bool can = Reuse && !(Frame.d3 & (1u << 14)) && LastOut;
    for (unsigned b = 0; b < GX_NBINS; b++) {
        const unsigned hy0 = b * BL, top = hy0 ? hy0 - 1 : 0, bot = hy0 + BL + 1 < H ? hy0 + BL + 1 : H;
        std::vector<uint8_t>& k = NewKey[b];
        k.assign(fs, fs + nfs);
        for (unsigned i = 0; i < Frame.npolys; i++) {
            const gx_poly& p = Polys[i];
            if (p.ybot <= top || p.ytop >= bot) continue;
            uint8_t rec[32];
            memcpy(rec, p.rec, 32);
            const gx_texture* t;
            memcpy(&t, rec + 0x10, sizeof t);
            uint64_t serial = 0;
            if (t) memcpy(&serial, t->pad0, sizeof serial);
            memcpy(rec + 0x10, &serial, sizeof serial);
            k.insert(k.end(), rec, rec + 32);
            k.insert(k.end(), &p.vtx[0][0], &p.vtx[0][0] + 16 * p.count);
            k.insert(k.end(), (const uint8_t*)&p.ytop, (const uint8_t*)&p.ytop + 6);   // ytop ybot count flags
        }
        BinReuse[b] = can && k == BinKey[b];
        if (Stats && can && !BinReuse[b] && !BinKey[b].empty()) {
            // why not (the stats' "why"): the first differing byte's field. 0 frame state, 1 polygon set (sizes
            // differ), 2 a polygon record (attr, texture...), 3 its vertices' x y, 4 their w z, 5 colour, 6 s t,
            // 7 ytop/ybot/count/flags
            const std::vector<uint8_t>& o = BinKey[b];
            size_t i = 0, n = std::min(o.size(), k.size());
            while (i < n && o[i] == k[i]) i++;
            int why = 1;
            if (i < nfs) why = 0;
            else if (i < n) {
                size_t at = nfs;
                while (at < k.size()) {
                    const unsigned cnt = k[at + 8] & 0xF;   // (rec + 8: the count, a8's low byte)
                    const size_t len = 32 + 16 * cnt + 6;
                    if (i < at + len) {
                        const size_t r = i - at;
                        if (r < 32) why = r >= 16 && r < 24 ? 10 : r >= 8 && r < 12 ? 9 : r >= 4 && r < 8 ? 2 : 8;
                        else if (r < 32 + 16 * cnt) { const size_t v = (r - 32) % 16; why = v < 4 ? 4 : v < 8 ? 3 : v < 10 ? 4 : v < 12 ? 5 : 6; }
                        else why = 7;
                        break;
                    }
                    at += len;
                }
            }
            StWhy[why]++;
        }
        BinKey[b].swap(k);
    }
}

// ---- the bins and the threads ----

int GengisRenderer3D::Help = 1;
int GengisRenderer3D::Nice = 0;
std::vector<int> GengisRenderer3D::AbModes;
std::vector<int> GengisRenderer3D::OptAb;
double GengisRenderer3D::AbSecs = 5;
int GengisRenderer3D::WakeN = 1;
int GengisRenderer3D::Stats = 0;
int GengisRenderer3D::HrNeon = -1;
int GengisRenderer3D::Verify = 0;
int GengisRenderer3D::Lazy = 0;
int GengisRenderer3D::Units = 1;
int GengisRenderer3D::KeySkip = 1;
int GengisRenderer3D::VbWait = -1;
int GengisRenderer3D::Crc = 0;
int GengisRenderer3D::TexInval = 1;
int GengisRenderer3D::Zva = 1;
int GengisRenderer3D::Trim = 1;
int GengisRenderer3D::Reuse = 1;


void GengisRenderer3D::RenderBin(unsigned b, uint32_t* dst) {   // (b: a work unit, a frame bin unless Sub > 1)
    if (Sub == 4 && !gx_ref) gx4q_render_unit(&Frame, b, dst);
    else if (Sub == 4) {   // (render.gengis_verify's reference for a 16-line unit: its 32-line bin's half)
        static thread_local std::vector<uint32_t> h;
        const size_t n = (size_t)GX_BL * GX_W * 16 / 4;
        h.resize(2 * n);
        gx4_render_unit(&Frame, b / 2, h.data());
        memcpy(dst, h.data() + (b & 1) * n, n * sizeof(uint32_t));
    }
    else if (Sub > 1) gx4_render_unit(&Frame, b, dst);
    else if (Scale_ == 2) gx2_render_bin(&Frame, b, dst);
    else if (Scale_ == 3) gx3_render_bin(&Frame, b, dst);
    else if (Scale_ == 4) gx4_render_bin(&Frame, b, dst);
    else gx_render_bin(&Frame, b, dst);
}

bool GengisRenderer3D::RenderOne(bool emu) {
    const unsigned b = Next.fetch_add(1, std::memory_order_acq_rel);
    if (b >= NU) return false;
    // the frame's polygons and textures, converted by the first thread to claim a bin of it (melonDS's state stays
    // until VBlank, where Finish3DRendering waits for the frame first)
    int expect = 0;
    if (Converted.compare_exchange_strong(expect, 1, std::memory_order_acq_rel)) {
        if (emu) EmuConverts++;
        const uint64_t c0 = Stats ? ThreadNs() : 0;
        Convert();
        if (Stats) StConvNs += ThreadNs() - c0;
        gx_begin_frame(&Frame);
        Converted.store(2, std::memory_order_release);
        { std::lock_guard<std::mutex> l(M); }  // (a waiter between its test and its wait sees the notify)
        ConvDone.notify_all();
    } else {
        // another thread converts: wait on the condition variable instead of spinning (the four A55s are shared by
        // five busy threads; a spinner here took the core the converter or the emulator's thread needed)
        std::unique_lock<std::mutex> l(M);
        ConvDone.wait(l, [&] { return Converted.load(std::memory_order_acquire) == 2; });
    }
    const uint64_t b0 = Stats ? ThreadNs() : 0;
    const size_t un = (size_t)GX_BL * GX_W * Scale_ * Scale_ / Sub;   // a unit's words
    if (BinReuse[b / Sub] && Reuse != 2) {  // (render.gengis_reuse: the same inputs as the last rendered frame's bin, BinKeys)
        const size_t n = un;
        memcpy(Out + b * n, LastOut + b * n, n * sizeof(uint32_t));
        if (Stats) StReused.fetch_add(1, std::memory_order_relaxed);
    }
    else if (BinReuse[b / Sub]) {  // render.gengis_reuse=2: render it, and count the bins that differ from the copy
        const size_t n = un;
        RenderBin(b, Out + b * n);
        StReused.fetch_add(1, std::memory_order_relaxed);
        if (memcmp(Out + b * n, LastOut + b * n, n * sizeof(uint32_t))) {
            StReuseBad.fetch_add(1, std::memory_order_relaxed);
            Platform::Log(Platform::LogLevel::Warn, "[gengis] reuse check: bin %u of frame %u differs\n", b, Frame.gen);
        }
    }
    else if (!Verify) RenderBin(b, Out + b * un);
    else {
        // render.gengis_verify: the bin again on the reference paths (gx_ref: the code before render.gengis_hrneon
        // and the later 4x changes) into a scratch buffer, compared with what this render wrote; the two renders'
        // CPU times summed (in turn first: the second finds the bin's data in the caches)
        const size_t n = un;
        static thread_local std::vector<uint32_t> ref;
        ref.resize(n);
        const bool reffirst = Frame.gen & 1;
        uint64_t t0 = ThreadNs(), t1 = 0, t2 = 0;
        if (reffirst) { gx_ref = 1; RenderBin(b, ref.data()); gx_ref = 0; t1 = ThreadNs(); }
        RenderBin(b, Out + b * n);
        t2 = ThreadNs();
        if (!reffirst) { gx_ref = 1; RenderBin(b, ref.data()); gx_ref = 0; t1 = ThreadNs(); VerRefNs += t1 - t2; VerNewNs += t2 - t0; }
        else { VerRefNs += t1 - t0; VerNewNs += t2 - t1; }
        VerBins.fetch_add(1, std::memory_order_relaxed);
        if (memcmp(Out + b * n, ref.data(), n * sizeof(uint32_t))) {
            size_t i = 0;
            while (Out[b * n + i] == ref[i]) i++;
            if (VerBad.fetch_add(1, std::memory_order_relaxed) < 20)
                Platform::Log(Platform::LogLevel::Warn, "[gengis] verify: bin %u of frame %u differs (word %zu: %08x, reference %08x)\n",
                              b, Frame.gen, i, Out[b * n + i], ref[i]);
        }
    }
    if (Stats) {
        StPxOpa.fetch_add(gx_px_opa, std::memory_order_relaxed); StPxTrl.fetch_add(gx_px_trl, std::memory_order_relaxed);
        StPolyLines.fetch_add(gx_lines, std::memory_order_relaxed);
        gx_px_opa = gx_px_trl = gx_lines = 0;
        const uint64_t t = ThreadNs() - b0;
        StBinNs.fetch_add(t, std::memory_order_relaxed);
        uint64_t m = StBinMax.load(std::memory_order_relaxed);
        while (t > m && !StBinMax.compare_exchange_weak(m, t, std::memory_order_relaxed)) {}
        if (AllBins.load(std::memory_order_relaxed) % NU == NU - 1) StLastNs.store(MonoNs(), std::memory_order_relaxed);
    }
    if (emu) EmuBins++;
    AllBins.fetch_add(1, std::memory_order_relaxed);
    Done[b].store(Frame.gen, std::memory_order_release);
    { std::lock_guard<std::mutex> l(M); }  // (a waiter between its test and its wait sees the notify)
    BinDone.notify_all();
    return true;
}

void GengisRenderer3D::Worker(int idx) {
    pthread_setname_np(pthread_self(), "bds-gengis");  // (else "bananads", summed with the presenter by top -H)
    bananads::sched_off_emu_core();                     // (sched.emu_excl: never on the emulator's core)
    if (Nice) setpriority(PRIO_PROCESS, 0, Nice);
    unsigned seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> l(M);
            // the first Active threads wait on Wake (a frame's start wakes them alone), the others on WakeLate (and
            // wait again on the other one when render.gengis_ab moved Active across them)
            const bool first = idx < Active;
            (first ? Wake : WakeLate).wait(l, [&] {
                return Quit || (idx < Active) != first ||
                       (Gen != seen && Next.load(std::memory_order_acquire) < NU &&
                        (first || (LateWake && Late.load(std::memory_order_relaxed))));
            });
            if (Quit) return;
            if ((idx < Active) != first && !(Gen != seen && Next.load(std::memory_order_acquire) < NU)) continue;
            seen = Gen;
        }
        while (RenderOne()) {}
    }
}

// a waiter found a bin not drawn: the render threads not woken at the frame's start join it (render.gengis_wake)
void GengisRenderer3D::MarkLate() {
    if (!LateWake || Active >= (int)Threads.size()) return;
    LateHold.store(60, std::memory_order_relaxed);  // (the next 60 frames wake them all at the start: a heavy scene)
    if (Late.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard<std::mutex> l(M);
        if (Late.load(std::memory_order_relaxed)) return;
        Late.store(true, std::memory_order_relaxed);
    }
    LateFrames.fetch_add(1, std::memory_order_relaxed);
    WakeLate.notify_all();
}

void GengisRenderer3D::WaitBin(unsigned bin) {
    if (Done[bin].load(std::memory_order_acquire) != Frame.gen) MarkLate();
    while (Done[bin].load(std::memory_order_acquire) != Frame.gen) {
        // the emulator's thread renders a bin itself rather than wait (render.gengis_help)
        const bool help = Threads.empty() || Help == 2 || (Help == 1 && Converted.load(std::memory_order_acquire) == 2);
        if (help && RenderOne(true)) continue;
        std::unique_lock<std::mutex> l(M);
        BinDone.wait_for(l, std::chrono::milliseconds(1),
                         [&] { return Done[bin].load(std::memory_order_acquire) == Frame.gen; });
    }
}

void GengisRenderer3D::WaitRows(int line, unsigned gen) {
    if (line >= GX_H) line = GX_H - 1;
    const unsigned bin = (unsigned)line / (GX_BL / Sub);
    auto drawn = [&] { return (int)(Done[bin].load(std::memory_order_acquire) - gen) >= 0; };
    if (!drawn()) MarkLate();
    while (!drawn()) {
        if (Converted.load(std::memory_order_acquire) == 2 && RenderOne()) continue;  // (the 2D thread may help)
        std::unique_lock<std::mutex> l(M);
        BinDone.wait_for(l, std::chrono::milliseconds(1), drawn);
    }
}

void GengisRenderer3D::WaitAll() {
    for (unsigned b = 0; b < NU; b++) WaitBin(b);
}

void GengisRenderer3D::RenderFrame() {
    if (Stats) StFrames++;
    const uint64_t w0 = Stats ? MonoNs() : 0;
    WaitAll();  // the last frame's bins read VRAM and the polygons: done before either changes
    if (Stats) StWaitNs += MonoNs() - w0;
    if (Crc && Gen && Out) {
        // render.gengis_crc: the finished frame's 3D layer (all its rows) as a CRC-32C, folded per 60 rendered frames
        // into a log line (a deterministic run, e.g. mkbench's scripted input, gives the same lines at the same
        // frames: compare two builds' runs line by line)
        uint32_t c = ~0u;
        const uint64_t* w = (const uint64_t*)Out;
        for (size_t i = 0, n = OutWords() / 2; i < n; i++) c = __builtin_aarch64_crc32cx(c, w[i]);
        CrcFold = __builtin_aarch64_crc32cw(CrcFold, ~c);
        if (++CrcN == 60) {
            Platform::Log(Platform::LogLevel::Info, "[gengis] crc frames %u-%u: %08x\n", Gen - 59, Gen, CrcFold);
            CrcN = 0; CrcFold = 0;
        }
    }
    auto textureDirty = GPU.VRAMDirty_Texture.DeriveState(GPU.VRAMMap_Texture, GPU);
    auto texPalDirty = GPU.VRAMDirty_TexPal.DeriveState(GPU.VRAMMap_TexPal, GPU);
    const bool textureChanged = GPU.MakeVRAMFlat_TextureCoherent(textureDirty);
    const bool texPalChanged = GPU.MakeVRAMFlat_TexPalCoherent(texPalDirty);
    if (!AbModes.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= AbNext) {
            const int m = AbModes[AbIdx++ % AbModes.size()];
            AbNext = now + std::chrono::milliseconds((long)(AbSecs * 1000));
            {
                std::lock_guard<std::mutex> l(M);
                NoRaster = m == 0;
                Active = m == 0 ? 1 << 30 : m > 10 ? m - 10 : m;
                LateWake = m > 10;
            }
            Wake.notify_all();  // (the threads Active moved across wait again on the other condition variable)
            WakeLate.notify_all();
            Platform::Log(Platform::LogLevel::Info, "[gengis] ab mode %d\n", m);
        }
        if (NoRaster) return;  // (debug: the lines stay the last frame's)
    }
    if (!OptAb.empty()) {  // render.gengis_optab (no bin is in flight here: WaitAll)
        const auto now = std::chrono::steady_clock::now();
        if (now >= OptNext) {
            const int m = OptAb[OptIdx++ % OptAb.size()];
            OptNext = now + std::chrono::milliseconds((long)(AbSecs * 1000));
            gx_hrneon = m & 1;
            gx_defer = (m >> 1) & 1;
            gx_lazy = (m >> 2) & 1;
            if (Stats) StatsLine();
            Platform::Log(Platform::LogLevel::Info, "[gengis] optab mode %d\n", m);
        }
    }
    if (!(textureChanged || texPalChanged) && GPU3D.RenderFrameIdentical) return;  // the lines are the last frame's
    if (textureChanged || texPalChanged) {
        if (TexInval && Textures.size() < 4096) InvalidateTextures(textureDirty.Data, texPalDirty.Data, textureChanged, texPalChanged);
        else Textures.clear();  // (render.gengis_texinval keeps unused decodes while their VRAM stays: bounded here)
        if (Stats) StClears++;
    }
    if (Stats && StRendered && StRendered % Stats == 0) StatsLine();
    if (Stats) { StRendered++; StStartNs = MonoNs(); }
    LastOut = Out == OutBuf[0] || Out == OutBuf[1] ? (Gen ? Out : nullptr) : nullptr;  // (render.gengis_reuse)
    Out = Out == OutBuf[0] ? OutBuf[1] : OutBuf[0];  // (the workers see it through Next's store under M)
    Frame.gen = ++Gen;
    if (Gen % 3600 == 0) {  // (render.gengis_help's effect, in the game's log, a line a minute or so)
        Platform::Log(Platform::LogLevel::Info, "[gengis] %lu frames: the emulator's thread drew %lu of %lu bins and converted %lu frames; %lu late frames\n",
                      (unsigned long)Gen, EmuBins, AllBins.load(), EmuConverts, LateFrames.load());
    }
    Converted.store(0, std::memory_order_relaxed);
    // render.gengis_wake: the other render threads too while a frame in the last 60 was late
    const bool hold = LateWake && Active < (int)Threads.size() && LateHold.load(std::memory_order_relaxed) > 0;
    if (hold) LateHold.fetch_sub(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(M);
        Late.store(hold, std::memory_order_relaxed);
        Next.store(0, std::memory_order_release);
    }
    Wake.notify_all();
    if (hold) WakeLate.notify_all();
    if (Threads.empty()) WaitAll();
}

// VBlank (GPU.cpp: before GPU3D::VBlank swaps the polygon lists and latches the next frame's registers). The bins
// read only the frame's own copies once it is converted (Polys, Frame, the decoded textures, gx_begin_frame's
// blocks); the texture VRAM copies and the texture cache change only in RenderFrame, after its WaitAll. So
// render.gengis_vbwait=0 (default at 3x and 4x) waits here for the conversion alone (doing it when no render thread
// has started it), and the bins may run on until the next frame's start at line 215 (or the 2D thread's row waits)
void GengisRenderer3D::FinishRendering() {
    if (!VbConvOnly) { WaitAll(); return; }
    while (Converted.load(std::memory_order_acquire) != 2) {
        if (Converted.load(std::memory_order_acquire) == 0 && RenderOne(true)) continue;   // (converts, then draws one)
        std::unique_lock<std::mutex> l(M);
        ConvDone.wait_for(l, std::chrono::milliseconds(1), [&] { return Converted.load(std::memory_order_acquire) == 2; });
    }
}

// render.gengis_stats=N: a line every N rendered frames, per rendered frame: the bins' CPU time (all threads), the
// slowest bin, the conversion (polygons + textures) and its texture decoding, the polygons, the textures decoded,
// and how often texture VRAM changed (the cache cleared); the emulator's thread's wait at the next frame's start
void GengisRenderer3D::StatsLine() {
    const double n = (double)Stats;
    Platform::Log(Platform::LogLevel::Info,
                  "[gengis-stats] %u rendered of %lu frames: bins %.3f ms/frame (max bin %.3f), convert %.3f ms (decode %.3f ms, "
                  "%.1f tex %.0f texels), polys %.1f opa %.1f trl, clears %lu, emu wait %.3f ms/frame, d3 %04x, px %.0f opa %.0f trl, %.0f poly-bins, reused %.1f bins (checked bad %lu), why %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu, same-content decodes %lu\n",
                  Stats, StFrames, StBinNs.load() / n / 1e6, StBinMax.load() / 1e6, StConvNs / n / 1e6, StDecNs / n / 1e6,
                  StDecoded / n, StDecTexels / n, StOpa / n, StTrl / n, StClears, StWaitNs / (double)StFrames / 1e6,
                  (unsigned)Frame.d3, StPxOpa.load() / n, StPxTrl.load() / n, StPolyLines.load() / n, StReused.load() / n, (unsigned long)StReuseBad.load(), StWhy[0], StWhy[1], StWhy[2], StWhy[3], StWhy[4], StWhy[5],
                  StWhy[6], StWhy[7], StWhy[8], StWhy[9], StWhy[10], StSameTex);
    StSameTex = 0;
    for (auto& w : StWhy) w = 0;
    StReused = 0;
    StPxOpa = 0; StPxTrl = 0; StPolyLines = 0;
    StBinNs = 0; StBinMax = 0; StConvNs = StDecNs = StDecoded = StDecTexels = StOpa = StTrl = 0;
    StClears = 0; StWaitNs = 0; StFrames = 0;
}

u32* GengisRenderer3D::GetLine(int line) { return const_cast<u32*>(GetQuarter(line, 0)); }

const u32* GengisRenderer3D::GetQuarter(int line, int q) {
    u32* sc = Scrolled[q];
    if (GPU3D.AbortFrame) {
        memset(sc, 0, sizeof Scrolled[0]);
        return sc;
    }
    if (line < GX_H) WaitBin((unsigned)line / (GX_BL / Sub));  // (the same bin at 2x: 2 * GX_BL rows a bin)
    else line = GX_H - 1;
    const u32* raw = QuarterRow(line, q);  // (line < GX_H here)
    const u16 xpos = GPU3D.RenderXPos;
    if (!xpos) return raw;
    // the X scroll, as melonDS's software renderer applies it (at 2x 2 * xpos 2x pixels: xpos in each quarter)
    if (xpos & 0x100) {
        int i = 0, j = xpos;
        for (; j < 512; i++, j++) sc[i] = 0;
        for (j = 0; i < 256; i++, j++) sc[i] = raw[j];
    } else {
        int i = 0, j = xpos;
        for (; j < 256; i++, j++) sc[i] = raw[j];
        for (; i < 256; i++) sc[i] = 0;
    }
    return sc;
}

}  // namespace bananads
