// render.gpu3d=compute (experimental, 3x/4x only): melonDS's compute-shader 3D renderer (GPU3D_Compute, desktop GL 4.3)
// ported to GLES 3.1 for the RG DS Plus's Mali-G52 (panfrost). The emulator's thread keeps melonDS's geometry engine
// (GX); at the frame's render point it snapshots the frame's polygons and render registers and hands them to a GL
// thread of its own (bds-gpu3d, its own surfaceless EGL context), which sets up the spans, decodes the textures,
// runs the compute passes and reads the finished frame back into two CPU row buffers in Gengis's layout. The Banana
// 2D engine composites those rows exactly as it does Gengis's (QuarterRow/WaitRows), so 2D, display capture and
// presentation are unchanged. Pictures are melonDS's GPU renderer's, not Gengis's (not bit-identical to the software
// rasterizers); emulation is not affected (the renderer only reads the GPU3D state).
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "GPU3D.h"
#include "GPU_Soft.h"
#ifdef BANANADS_2D
#include "GPU_Banana.h"
#endif

namespace bananads {

struct Gpu3DJob;
struct Gpu3DGL;

class ComputeGpuRenderer3D : public melonDS::Renderer3D {
public:
    ComputeGpuRenderer3D(melonDS::GPU3D& gpu3D, int scale);
    ~ComputeGpuRenderer3D() override;
    bool Ok() const { return Ready == 1; }
    void Reset() override;
    void RenderFrame() override;
    void FinishRendering() override;
    void RestartFrame() override {}
    melonDS::u32* GetLine(int line) override;

    static int WorkTiles;  // render.gpu3d_worktiles: work tiles per screen tile the GPU buffers hold (8)
    static int Hires;      // render.gpu3d_hires: melonDS's high-resolution vertex coordinates (0)
    static int Prof;       // render.gpu3d_prof: glFinish after each pass, times in the stats line (slow: measurement only)
    unsigned LastVariants = 0;
    static int Stats;      // render.gpu3d_stats: a [gpu3d] line every N rendered frames (0: off)

    // the Banana 2D engine's interface (as GengisRenderer3D's)
    const melonDS::u32* GetQuarter(int line, int q);
    static const melonDS::u32* QuarterFn(void* self, melonDS::u32 line, int q) {
        return static_cast<ComputeGpuRenderer3D*>(self)->GetQuarter((int)line, q);
    }
    bool QuarterRows() const { return !GPU3D.AbortFrame && !GPU3D.RenderXPos; }
    static bool QuarterRowsFn(void* self) { return static_cast<ComputeGpuRenderer3D*>(self)->QuarterRows(); }
    const melonDS::u32* QuarterRow(int line, int q) const {
        if (line >= 192) line = 191;
        return Out + (Scale_ * line + q / Scale_) * Scale_ * 256 + (q % Scale_) * 256;
    }
    static const melonDS::u32* QuarterRowFn(void* self, melonDS::u32 line, int q) {
        return static_cast<ComputeGpuRenderer3D*>(self)->QuarterRow((int)line, q);
    }
    void WaitRows(int line, unsigned gen);
    static unsigned RowsGenFn(void* self) { return static_cast<ComputeGpuRenderer3D*>(self)->Gen; }
    static void WaitRowsFn(void* self, melonDS::u32 line, unsigned gen) {
        static_cast<ComputeGpuRenderer3D*>(self)->WaitRows((int)line, gen);
    }

private:
    void Thread();
    bool GLInit();
    void GLFrame(Gpu3DJob& j);
    void WaitDone(unsigned gen);
    void WaitCpu();

    int Scale_;
    melonDS::u32* OutBuf[2] = {};
    melonDS::u32* Out = nullptr;
    melonDS::u32 Scrolled[16][256];
    std::unique_ptr<Gpu3DJob> Job;
    std::unique_ptr<Gpu3DGL> G;

    unsigned Gen = 0;                       // the last frame handed to the GL thread (emulator's thread)
    std::atomic<unsigned> CpuDone{0};       // the GL thread no longer reads the job's snapshot / the flat VRAM
    std::atomic<unsigned> Done{0};          // the frame's rows are in its Out buffer
    std::atomic<int> Ready{0};              // 1: GL up and shaders compiled; -1: failed
    std::atomic<bool> ResetTex{false};
    std::mutex M;
    std::condition_variable CvJob, CvDone;
    bool Quit = false;
    unsigned Posted = 0;
    std::thread T;
};

#ifdef BANANADS_2D
// BananaRenderer (the Banana 2D engine) with the compute renderer as its 3D renderer
class ComputeBananaRenderer : public melonDS::BananaRenderer {
public:
    ComputeBananaRenderer(melonDS::NDS& nds, int scale);
    bool Ok() const { return R && R->Ok(); }
    void PreSavestate() override {
        Drain2D();
        this->Rend3D->FinishRendering();
    }
    void PostSavestate() override { Resync2D(); }
    void SetRenderSettings(melonDS::RendererSettings&) override {}
    ComputeGpuRenderer3D* R = nullptr;
};
#endif

}  // namespace bananads
