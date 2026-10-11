// Gengis Engine as melonDS's 3D renderer (render.renderer=gengis): SuperDrastic's rasterizer at 1x (gengis/gx.c),
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
    GengisRenderer3D(melonDS::GPU3D& gpu3D, int threads, bool defer);
    ~GengisRenderer3D() override;
    void Reset() override;
    void RenderFrame() override;
    void FinishRendering() override;
    void RestartFrame() override {}
    melonDS::u32* GetLine(int line) override;

private:
    struct Texture {
        gx_texture t{};
        std::vector<uint32_t> texels;
        std::vector<uint8_t> idx;
        std::vector<uint32_t> pal;
    };
    const gx_texture* GetTexture(uint32_t texparam, uint32_t texpal);
    void Decode(Texture& tx, uint32_t texparam, uint32_t texpal);
    void Convert();
    bool RenderOne();     // claim and render a bin of the frame; false when none is left
    void WaitBin(unsigned bin);
    void WaitAll();
    void Worker();

    std::unordered_map<uint64_t, std::unique_ptr<Texture>> Textures;
    std::vector<gx_poly> Polys;
    gx_frame Frame{};
    alignas(64) uint32_t Out[GX_H * GX_W];
    uint32_t Scrolled[GX_W];

    // the frame's bins: claimed by the workers (and by the emulator's thread while it waits for one)
    std::atomic<unsigned> Next{GX_NBINS};
    std::atomic<unsigned> Done[GX_NBINS];
    std::atomic<int> Converted{2};  // the frame's conversion: 0 to do, 1 being done, 2 done
    unsigned Gen = 0;
    std::mutex M;
    std::condition_variable Wake, BinDone;
    bool Quit = false;
    std::vector<std::thread> Threads;
};

// a software renderer (melonDS's, or BananaRenderer with the Banana 2D engine) with Gengis as the 3D renderer
template <class Base>
class GengisRendererOn : public Base {
public:
    GengisRendererOn(melonDS::NDS& nds, int threads, bool defer) : Base(nds) {
        this->Rend3D = std::make_unique<GengisRenderer3D>(this->GPU.GPU3D, threads, defer);
    }
    void PreSavestate() override { this->Rend3D->FinishRendering(); }
    void PostSavestate() override {}
    void SetRenderSettings(melonDS::RendererSettings&) override {}
};
using GengisSoftRenderer = GengisRendererOn<melonDS::SoftRenderer>;
#ifdef BANANADS_2D
using GengisBananaRenderer = GengisRendererOn<melonDS::BananaRenderer>;
#endif

}  // namespace bananads
