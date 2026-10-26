// Gengis Engine as melonDS's 3D renderer (render.renderer=gengis): SuperDrastic's rasterizer at 1x or 2x (gengis/gx.c,
// render.scale),
// fed from melonDS's GPU3D state, on a pool of render threads. melonDS's 2D renderer composites its lines as it does
// the software 3D renderer's. docs/GENGIS.md says what it does differently from melonDS's renderer.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "GPU3D.h"
#include "GPU_Soft.h"
#ifdef BANANADS_2D
#include "GPU_Banana.h"
#endif

extern "C" {
#include "gx.h"
}

namespace bananads {

class GengisRenderer3D : public melonDS::Renderer3D {
public:
    GengisRenderer3D(melonDS::GPU3D& gpu3D, int threads, bool defer, int scale = 1);
    ~GengisRenderer3D() override;
    void Reset() override;
    void RenderFrame() override;
    void FinishRendering() override;
    void RestartFrame() override {}
    melonDS::u32* GetLine(int line) override;  // at 2x: quarter 0 of the line
    int Scale() const { return Scale_; }
    // the game's DISP3DCNT bits the picture may use (render.edge_marking, render.fog): the others are cleared
    // before the rasterizer sees them
    static void SetEffects(bool edge_marking, bool fog) {
        DispMask = ~0u & ~(edge_marking ? 0u : 1u << 5) & ~(fog ? 0u : 1u << 7);
    }
    static melonDS::u32 DispMask;
    // render.gengis_help: when the emulator's thread needs a bin that isn't drawn yet, it draws one itself: 2 always,
    // 1 (the default) only once a render thread has converted the frame (its polygons and textures, the long part,
    // kept off the emulator's thread), 0 never (it waits); with no render threads it always does
    static int Help;
    static int Reuse;  // render.gengis_reuse: a bin with the last rendered frame's inputs is copied (BinKeys); 2: rendered
                       // and compared with the copy (a check: [gengis] lines and the stats' "checked bad")
    static int Trim;  // render.gengis_trim: opaque polygons skip the bins' context lines when edge marking is off
    static int Zva;  // render.gengis_zva: dc zva before the clear's and the 2x output's stores (same bytes)
    static int TexInval;  // render.gengis_texinval: 1 (blocks) drops only the textures whose VRAM changed, 0 all
    static int Stats;  // render.gengis_stats: a [gengis-stats] line every N rendered frames (0: off)
    static int Nice;  // sched.gengis_nice: the workers' nice value (0: the default)
    unsigned long EmuBins = 0, EmuConverts = 0;  // drawn and converted on the emulator's thread (logged at the end)
    std::atomic<unsigned long> AllBins{0};
    // at 2x, DS line `line`'s four 256-pixel lines: quarter q is 2x row 2 * line + (q >> 1), its even pixels (q & 1
    // clear) or odd ones, with the X scroll applied
    const melonDS::u32* GetQuarter(int line, int q);
    static const melonDS::u32* QuarterFn(void* self, melonDS::u32 line, int q) {
        return static_cast<GengisRenderer3D*>(self)->GetQuarter((int)line, q);
    }
    // GetQuarter's lines are the frame's own rows (no X scroll, no aborted frame: else a scratch line, rewritten by
    // the next call): final once it has waited for their bin, and unchanged through the next frame's rendering (two
    // buffers), i.e. until the frame after next starts at line 215, long after this frame's end
    bool QuarterRows() const { return !GPU3D.AbortFrame && !GPU3D.RenderXPos; }
    static bool QuarterRowsFn(void* self) { return static_cast<GengisRenderer3D*>(self)->QuarterRows(); }
    // render.3d_wait2d: GetQuarter's row without waiting for its bin (when QuarterRows holds), the render that will
    // draw it, and a wait for that render's bin of a line, for the 2D thread (the bin drawn by then, or a later
    // render's: those go into the other buffer). The emulator's thread then waits for the frame only at VBlank.
    const melonDS::u32* QuarterRow(int line, int q) const {
        if (line >= GX_H) line = GX_H - 1;
        return Scale_ == 2 ? Out + (2 * line + (q >> 1)) * 2 * GX_W + (q & 1) * GX_W : Out + line * GX_W;
    }
    void WaitRows(int line, unsigned gen);
    static const melonDS::u32* QuarterRowFn(void* self, melonDS::u32 line, int q) {
        return static_cast<GengisRenderer3D*>(self)->QuarterRow((int)line, q);
    }
    static unsigned RowsGenFn(void* self) { return static_cast<GengisRenderer3D*>(self)->Frame.gen; }
    static void WaitRowsFn(void* self, melonDS::u32 line, unsigned gen) { static_cast<GengisRenderer3D*>(self)->WaitRows((int)line, gen); }

private:
    struct Texture {
        gx_texture t{};
        // the 512-byte blocks of texture VRAM (1024) and texture palette VRAM (256) the decode read: the entry stays
        // valid while none of them is dirty (render.gengis_texinval=blocks)
        uint64_t texblk[1024 / 64] = {}, palblk[256 / 64] = {};
        std::vector<uint32_t> texels;
        std::vector<uint8_t> idx;
        std::vector<uint32_t> pal;
    };
    const gx_texture* GetTexture(uint32_t texparam, uint32_t texpal);
    void Decode(Texture& tx, uint32_t texparam, uint32_t texpal);
    void Convert();
    bool RenderOne(bool emu = false);  // claim and render a bin of the frame; false when none is left
    void WaitBin(unsigned bin);
    void WaitAll();
    void Worker();
    void StatsLine();
    void BinKeys();
    std::vector<uint8_t> BinKey[GX_NBINS], NewKey[GX_NBINS];
    bool BinReuse[GX_NBINS] = {};
    uint32_t* LastOut = nullptr;  // the last rendered frame's rows (the other buffer)
    uint64_t TexSerial = 0;
    std::atomic<uint64_t> StReused{0}, StReuseBad{0};
    void InvalidateTextures(const uint64_t* texdirty, const uint64_t* paldirty, bool tex, bool pal);
    std::atomic<uint64_t> StBinNs{0}, StBinMax{0}, StLastNs{0}, StPxOpa{0}, StPxTrl{0}, StPolyLines{0};
    uint64_t StConvNs = 0, StDecNs = 0, StDecoded = 0, StDecTexels = 0, StOpa = 0, StTrl = 0, StWaitNs = 0, StStartNs = 0;
    unsigned long StClears = 0, StFrames = 0, StRendered = 0;

    std::unordered_map<uint64_t, std::unique_ptr<Texture>> Textures;
    std::vector<gx_poly> Polys;
    std::vector<uint32_t> YRange;  // Frame.yr
    gx_frame Frame{};
    int Scale_;
    // two frames' rows, alternated by each frame rendered: a frame's stay as they are through the next frame's
    // rendering, so the 2D thread can read them (QuarterRows) until the frame's end without a wait at the next start
    alignas(64) uint32_t OutBuf[2][GX_H * GX_W * 4];  // 1x: GX_H lines of GX_W; 2x: gx2_render_bin's rows
    uint32_t* Out = OutBuf[0];                         // the frame being rendered or shown
    alignas(16) uint32_t Scrolled[4][GX_W];

    // the frame's bins: claimed by the workers (and by the emulator's thread while it waits for one)
    std::atomic<unsigned> Next{GX_NBINS};
    std::atomic<unsigned> Done[GX_NBINS];
    std::atomic<int> Converted{2};  // the frame's conversion: 0 to do, 1 being done, 2 done
    unsigned Gen = 0;
    std::mutex M;
    std::condition_variable Wake, BinDone, ConvDone;  // ConvDone: the frame's conversion finished (a waiter blocks, no spin)
    bool Quit = false;
    std::vector<std::thread> Threads;
};

// before a savestate: BananaRenderer's 2D thread (render.2d_thread) idle
// and after one: its copies of VRAM made again (GPU::DoSavestate clears the VRAM flats without dirty bits)
inline void Drain2D(melonDS::SoftRenderer*) {}
inline void Resync2D(melonDS::SoftRenderer*) {}
// render.scale=2: the 2D side composites the four quarters (only the Banana 2D engine; the caller makes sure)
inline void SetHires(melonDS::SoftRenderer*, GengisRenderer3D*, int) {}
#ifdef BANANADS_2D
inline void Drain2D(melonDS::BananaRenderer* r) { r->Drain2D(); }
inline void Resync2D(melonDS::BananaRenderer* r) { r->Resync2D(); }
inline void SetHires(melonDS::BananaRenderer* r, GengisRenderer3D* g, int scale) {
    if (scale == 2) {
        r->SetHires(&GengisRenderer3D::QuarterFn, g, &GengisRenderer3D::QuarterRowsFn);
        r->SetHiresRows(&GengisRenderer3D::QuarterRowFn, &GengisRenderer3D::RowsGenFn, &GengisRenderer3D::WaitRowsFn);
    }
}
#endif

// a software renderer (melonDS's, or BananaRenderer with the Banana 2D engine) with Gengis as the 3D renderer
template <class Base>
class GengisRendererOn : public Base {
public:
    GengisRendererOn(melonDS::NDS& nds, int threads, bool defer, int scale = 1) : Base(nds) {
        auto r = std::make_unique<GengisRenderer3D>(this->GPU.GPU3D, threads, defer, scale);
        SetHires(this, r.get(), scale);
        this->Rend3D = std::move(r);
    }
    void PreSavestate() override {
        Drain2D(this);
        this->Rend3D->FinishRendering();
    }
    void PostSavestate() override { Resync2D(this); }
    void SetRenderSettings(melonDS::RendererSettings&) override {}
};
using GengisSoftRenderer = GengisRendererOn<melonDS::SoftRenderer>;
#ifdef BANANADS_2D
using GengisBananaRenderer = GengisRendererOn<melonDS::BananaRenderer>;
#endif

}  // namespace bananads
