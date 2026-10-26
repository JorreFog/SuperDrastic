// render.gpu3d (experimental, default off, render.scale 3 and 4 only): the 3D layer drawn by the GPU (GLES 3.1 on a
// thread of its own, "bds-gpu3d") instead of Gengis Engine. The host (this class) owns the thread, its surfaceless EGL
// context, the frame hand-off and the 3D rows the Banana 2D engine reads; a backend draws the frame:
//   gl:      melonDS's classic OpenGL renderer (GPU3D_OpenGL.cpp) ported to GLES (gl3d_backend.cpp)
//   compute: melonDS's compute renderer (G1's port), the same interface
// The frame's 3D image is read back (one pass that packs it into Gengis's hi-res row layout and melonDS's 3D pixel
// format, then glReadPixels in four chunks): the 2D engine, display capture (1x quarter 0) and the presenter work as
// with Gengis. Emulation is unchanged: geometry stays on the emulator's thread, the renderer only reads GPU3D's
// render state and the 3D VRAM flats, all copied out (Prepare) on the emulator's thread at RenderFrame.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "GPU3D.h"
#include "gengis_renderer.h"  // (Drain2D, Resync2D)
#include "GPU_Soft.h"
#ifdef BANANADS_2D
#include "GPU_Banana.h"
#endif

namespace bananads {

// A GPU 3D renderer. Prepare runs on the emulator's thread, everything named Gl* on the GPU thread with the context
// current. Only one frame is in flight: Prepare for frame N+1 runs after GlRender (and the read-back) of frame N.
class Gpu3DBackend {
public:
    virtual ~Gpu3DBackend() = default;
    virtual const char* Name() const = 0;
    // GL thread: programs, buffers and the scale's render targets; false: the backend can't run (logged)
    virtual bool GlInit(int scale) = 0;
    virtual void GlShutdown() = 0;
    // emulator's thread, at RenderFrame: copy what the frame needs out of GPU3D and VRAM (the backend makes the
    // texture VRAM flats coherent itself, as melonDS's renderers do). false: nothing new to draw (the last image stays)
    virtual bool Prepare(melonDS::GPU3D& g3d) = 0;
    // GL thread: draw the prepared frame; returns the texture holding it (256*scale x 192*scale, row 0 = DS line 0,
    // normalized RGBA8: rgb = c6 / 63, a = a5 / 31, melonDS's GL convention)
    virtual unsigned GlRender() = 0;
    virtual void Reset() = 0;  // emulator's thread (melonDS's Renderer3D::Reset)
};

std::unique_ptr<Gpu3DBackend> MakeGl3DBackend(melonDS::GPU3D& g);       // gl3d_backend.cpp
std::unique_ptr<Gpu3DBackend> MakeCompute3DBackend(melonDS::GPU3D& g);  // G1 (weak: nullptr while not linked in)

class GpuRenderer3D : public melonDS::Renderer3D {
public:
    GpuRenderer3D(melonDS::GPU3D& gpu3D, std::unique_ptr<Gpu3DBackend> be, int scale);
    ~GpuRenderer3D() override;
    bool Ok() const { return Ok_; }
    void Reset() override;
    void RenderFrame() override;
    void FinishRendering() override;
    void RestartFrame() override {}
    melonDS::u32* GetLine(int line) override;
    static int Stats;  // render.gpu3d_stats: a [gpu3d] timing line every N frames
    static int ReadMode;  // render.gpu3d_read: 2 heap (default: drawn into cached dma-bufs), 1 pbo, 0 direct glReadPixels
    static int Clear;     // render.gpu3d_clear (on): a glClear before the clear plane's quad
    static int Finish;    // render.gpu3d_finish (measurement): glFinish after the backend's draw, its GPU time apart
    static std::string MesaDebug;  // render.gpu3d_mesa_debug: PAN_MESA_DEBUG for the context (e.g. perf)

    // the Banana 2D engine's hi-res interface (as GengisRenderer3D's: gx.h's 3x/4x row layout, melonDS's 3D format)
    const melonDS::u32* GetQuarter(int line, int q);
    bool QuarterRows() const { return !GPU3D.AbortFrame && !GPU3D.RenderXPos; }
    const melonDS::u32* QuarterRow(int line, int q) const {
        if (line >= 192) line = 191;
        return Out + (Scale_ * line + q / Scale_) * Scale_ * 256 + (q % Scale_) * 256;
    }
    void WaitRows(int line, unsigned gen);
    static const melonDS::u32* QuarterFn(void* s, melonDS::u32 l, int q) { return static_cast<GpuRenderer3D*>(s)->GetQuarter((int)l, q); }
    static bool QuarterRowsFn(void* s) { return static_cast<GpuRenderer3D*>(s)->QuarterRows(); }
    static const melonDS::u32* QuarterRowFn(void* s, melonDS::u32 l, int q) { return static_cast<GpuRenderer3D*>(s)->QuarterRow((int)l, q); }
    static unsigned RowsGenFn(void* s) { return static_cast<GpuRenderer3D*>(s)->Gen; }
    static void WaitRowsFn(void* s, melonDS::u32 l, unsigned gen) { static_cast<GpuRenderer3D*>(s)->WaitRows((int)l, gen); }

private:
    enum { NCHUNK = 4 };  // read-back chunks (48 DS lines each)
    void Thread();
    bool GlSetup();
    void WaitChunk(int c, unsigned gen);
    void WaitIdle();
    std::unique_ptr<Gpu3DBackend> Be;
    int Scale_;
    bool Ok_ = false;
    melonDS::u32* OutBuf[2] = {};
    melonDS::u32* Out = nullptr;
    melonDS::u32* Target = nullptr;  // the buffer the job reads back into
    unsigned Gen = 0;                 // renders kicked
    std::atomic<unsigned> Done[NCHUNK];
    std::atomic<unsigned> Finished{0};  // renders fully read back
    unsigned Job = 0;                   // the gen the thread is asked to render
    bool Quit = false, Inited = false;
    std::mutex M;
    std::condition_variable Cv, DoneCv;
    alignas(16) melonDS::u32 Scrolled[16][256];
    std::thread T;
    // GL objects of the pack pass (host side)
    unsigned Pbo = 0;
    bool HeapSetup();
    int HeapFd[2] = {-1, -1};
    melonDS::u32* HeapMap[2] = {};
    unsigned HeapTex[2] = {}, HeapFb[2] = {};
    bool HeapOwned = false;
    uint64_t StFinNs = 0;
    unsigned PackProg = 0, PackTex = 0, PackFb = 0, Vao = 0;
    int PackScaleLoc = -1;
    // stats
    uint64_t StPrepNs = 0, StGlNs = 0, StWaitNs = 0, StReadNs = 0, StEmuWaitNs = 0;
    unsigned StN = 0;
};

inline void SetHires(melonDS::SoftRenderer*, GpuRenderer3D*, int) {}
#ifdef BANANADS_2D
inline void SetHires(melonDS::BananaRenderer* r, GpuRenderer3D* g, int scale) {
    r->SetHires(&GpuRenderer3D::QuarterFn, g, &GpuRenderer3D::QuarterRowsFn, scale);
    r->SetHiresRows(&GpuRenderer3D::QuarterRowFn, &GpuRenderer3D::RowsGenFn, &GpuRenderer3D::WaitRowsFn);
}

// the Banana 2D engine with a GPU 3D renderer (as GengisRendererOn)
class GpuBananaRenderer : public melonDS::BananaRenderer {
public:
    GpuBananaRenderer(melonDS::NDS& nds, std::unique_ptr<GpuRenderer3D> r, int scale) : melonDS::BananaRenderer(nds) {
        bananads::SetHires(static_cast<melonDS::BananaRenderer*>(this), r.get(), scale);
        this->Rend3D = std::move(r);
    }
    void PreSavestate() override {
        melonDS::BananaRenderer::Drain2D();
        this->Rend3D->FinishRendering();
    }
    void PostSavestate() override { melonDS::BananaRenderer::Resync2D(); }
    void SetRenderSettings(melonDS::RendererSettings&) override {}
};
#endif

}  // namespace bananads
