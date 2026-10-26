// render.gpu3d=compute: melonDS's compute-shader renderer on GLES 3.1 (gpu3d_compute.h says how it is wired).
// Ported from melonDS's GPU3D_Compute.cpp / GPU3D_Compute_shaders.h / GPU3D_Texcache.h (GPL-3.0, the melonDS team).
#include "gpu3d_compute.h"

#include <assert.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <algorithm>
#include <string>
#include <unordered_map>

#include "GPU.h"
#include "GPU3D_Texcache.h"
#include "NDS.h"
#include "Platform.h"
#include "xxhash/xxhash.h"

#include "gpu3d_compute_shaders.h"
#include "gpu3d_compute_top.inc"
#include "gpu3d_spans.inc"

namespace bananads {

static uint64_t NowNs();
int ComputeGpuRenderer3D::WorkTiles = 8;
int ComputeGpuRenderer3D::Hires = 0;
int ComputeGpuRenderer3D::Stats = 0;
int ComputeGpuRenderer3D::Prof = 0;
static uint64_t PhaseNs[8];
static int PhaseN;
static uint64_t PhaseT;
static void Phase(int i) { if (!ComputeGpuRenderer3D::Prof) return; glFinish(); uint64_t t = NowNs(); if (i >= 0) PhaseNs[i] += t - PhaseT; PhaseT = t; }

static uint64_t NowNs() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

#define GLOG(...) Platform::Log(Platform::LogLevel::Info, __VA_ARGS__)

ComputeGpuRenderer3D::ComputeGpuRenderer3D(melonDS::GPU3D& gpu3D, int scale)
    : Renderer3D(gpu3D), Scale_(scale), Job(new Gpu3DJob), G(new Gpu3DGL) {
    const size_t words = (size_t)256 * 192 * scale * scale;
    for (auto& b : OutBuf) {
        b = static_cast<u32*>(aligned_alloc(64, words * 4));
        memset(b, 0, words * 4);
    }
    Out = OutBuf[0];
    T = std::thread([this] { Thread(); });
    std::unique_lock<std::mutex> l(M);
    CvDone.wait(l, [&] { return Ready.load() != 0; });
}

ComputeGpuRenderer3D::~ComputeGpuRenderer3D() {
    {
        std::lock_guard<std::mutex> l(M);
        Quit = true;
    }
    CvJob.notify_all();
    if (T.joinable()) T.join();
    for (auto b : OutBuf) free(b);
}

void ComputeGpuRenderer3D::Reset() {
    FinishRendering();
    ResetTex = true;
}

void ComputeGpuRenderer3D::WaitCpu() {
    if (CpuDone.load(std::memory_order_acquire) == Gen) return;
    std::unique_lock<std::mutex> l(M);
    CvDone.wait(l, [&] { return CpuDone.load(std::memory_order_acquire) == Gen; });
}

void ComputeGpuRenderer3D::WaitDone(unsigned gen) {
    if ((int)(Done.load(std::memory_order_acquire) - gen) >= 0) return;
    std::unique_lock<std::mutex> l(M);
    CvDone.wait(l, [&] { return (int)(Done.load(std::memory_order_acquire) - gen) >= 0; });
}

void ComputeGpuRenderer3D::FinishRendering() { WaitDone(Gen); }

void ComputeGpuRenderer3D::WaitRows(int, unsigned gen) { WaitDone(gen); }

void ComputeGpuRenderer3D::RenderFrame() {
    if (Ready != 1) return;
    WaitCpu();  // the last frame's span setup and texture decoding read the flat VRAM and the snapshot
    auto textureDirty = GPU.VRAMDirty_Texture.DeriveState(GPU.VRAMMap_Texture, GPU);
    auto texPalDirty = GPU.VRAMDirty_TexPal.DeriveState(GPU.VRAMMap_TexPal, GPU);
    const bool textureChanged = GPU.MakeVRAMFlat_TextureCoherent(textureDirty);
    const bool texPalChanged = GPU.MakeVRAMFlat_TexPalCoherent(texPalDirty);
    if (!(textureChanged || texPalChanged) && GPU3D.RenderFrameIdentical) return;  // the rows stay the last frame's
    WaitDone(Gen);  // (the GL thread works on one frame at a time; its Out buffer is about to be swapped)

    Gpu3DJob& j = *Job;
    j.texChanged = textureChanged;
    j.palChanged = texPalChanged;
    static_assert(sizeof(j.texDirty) == sizeof(textureDirty.Data), "texture dirty set");
    static_assert(sizeof(j.palDirty) == sizeof(texPalDirty.Data), "palette dirty set");
    if (textureChanged) memcpy(j.texDirty, textureDirty.Data, sizeof j.texDirty);
    if (texPalChanged) memcpy(j.palDirty, texPalDirty.Data, sizeof j.palDirty);

    // the polygons and their vertices (GPU3D's render bank is rewritten at the next VBlank's swap)
    const unsigned gen = Gen + 1;
    j.gen = gen;
    j.npolys = (int)GPU3D.RenderNumPolygons;
    Vertex* const vbase = GPU3D.VertexRAM;
    for (int i = 0; i < j.npolys; i++) {
        const Polygon* p = GPU3D.RenderPolygonRAM[i];
        Polygon& d = j.polys[i];
        d = *p;
        for (u32 v = 0; v < p->NumVertices; v++) {
            const size_t idx = (size_t)(p->Vertices[v] - vbase);
            if (idx < j.verts.size()) {
                if (j.vstamp[idx] != gen) {
                    j.vstamp[idx] = gen;
                    j.verts[idx] = *p->Vertices[v];
                }
                d.Vertices[v] = &j.verts[idx];
            }
        }
    }
    j.dispcnt = GPU3D.RenderDispCnt;

    Gpu3DJob::Meta& meta = j.meta;
    meta.DispCnt = GPU3D.RenderDispCnt;
    meta.NumPolygons = GPU3D.RenderNumPolygons;
    meta.AlphaRef = GPU3D.RenderAlphaRef;
    {
        u32 r = (GPU3D.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (GPU3D.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (GPU3D.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (GPU3D.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((GPU3D.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = GPU3D.RenderClearAttr1 & 0x3F008000;
        u8 xoff = (GPU3D.RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (GPU3D.RenderClearAttr2 >> 24) & 0xFF;
        meta.ClearBitmapOffset[0] = (float)xoff / 256.0f;
        meta.ClearBitmapOffset[1] = (float)yoff / 256.0f;
    }
    for (u32 i = 0; i < 32; i++) {
        u32 color = GPU3D.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E, g = (color >> 4) & 0x3E, b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;
        meta.ToonTable[i * 4 + 0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++) meta.ToonTable[i * 4 + 1] = GPU3D.RenderFogDensityTable[i];
    for (u32 i = 0; i < 8; i++) {
        u32 color = GPU3D.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E, g = (color >> 4) & 0x3E, b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;
        meta.ToonTable[i * 4 + 2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = GPU3D.RenderFogOffset;
    meta.FogShift = GPU3D.RenderFogShift;
    {
        u32 fogR = (GPU3D.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (GPU3D.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (GPU3D.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (GPU3D.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }

    Out = Out == OutBuf[0] ? OutBuf[1] : OutBuf[0];
    {
        std::lock_guard<std::mutex> l(M);
        Gen = gen;
        Posted = gen;
    }
    CvJob.notify_one();
}

u32* ComputeGpuRenderer3D::GetLine(int line) { return const_cast<u32*>(GetQuarter(line, 0)); }

const u32* ComputeGpuRenderer3D::GetQuarter(int line, int q) {
    u32* sc = Scrolled[q];
    if (GPU3D.AbortFrame) {
        memset(sc, 0, sizeof Scrolled[0]);
        return sc;
    }
    WaitDone(Gen);
    if (line >= 192) line = 191;
    const u32* raw = QuarterRow(line, q);
    const u16 xpos = GPU3D.RenderXPos;
    if (!xpos) return raw;
    if (xpos & 0x100) {
        int i = 0, k = xpos;
        for (; k < 512; i++, k++) sc[i] = 0;
        for (k = 0; i < 256; i++, k++) sc[i] = raw[k];
    } else {
        int i = 0, k = xpos;
        for (; k < 256; i++, k++) sc[i] = raw[k];
        for (; i < 256; i++) sc[i] = 0;
    }
    return sc;
}

// ---- the GL thread ----

static bool CompileProgram(GLuint& prog, const std::string& src, const char* name) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        GLOG("[gpu3d] shader %s: %s\n", name, log);
        glDeleteShader(sh);
        return false;
    }
    prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = "";
        glGetProgramInfoLog(prog, sizeof log, nullptr, log);
        GLOG("[gpu3d] program %s: %s\n", name, log);
        return false;
    }
    return true;
}

bool ComputeGpuRenderer3D::GLInit() {
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void* gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!egl || !gles) { GLOG("[gpu3d] no libEGL/libGLESv2\n"); return false; }
    *(void**)&eglGetProcAddress_ = dlsym(egl, "eglGetProcAddress");
    if (!eglGetProcAddress_) return false;
#define EGLF(v, n) if (!(*(void**)&v = (void*)eglGetProcAddress_(n))) { GLOG("[gpu3d] missing %s\n", n); return false; }
    EGLF(eglGetPlatformDisplayEXT_, "eglGetPlatformDisplayEXT") EGLF(eglInitialize_, "eglInitialize")
    EGLF(eglBindAPI_, "eglBindAPI") EGLF(eglCreateContext_, "eglCreateContext") EGLF(eglMakeCurrent_, "eglMakeCurrent")
    EGLF(eglQueryString_, "eglQueryString") EGLF(eglChooseConfig_, "eglChooseConfig") EGLF(eglGetError_, "eglGetError")
    EGLF(eglDestroyContext_, "eglDestroyContext") EGLF(eglTerminate_, "eglTerminate")
#define GLF(t, n) if (!(*(void**)&n = (void*)eglGetProcAddress_(#n)) && !(*(void**)&n = dlsym(gles, #n))) { GLOG("[gpu3d] missing " #n "\n"); return false; }
    GPU3D_GL_FUNCS(GLF)

    EGLDisplay dpy = eglGetPlatformDisplayEXT_(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint maj = 0, min = 0;
    if (!dpy || !eglInitialize_(dpy, &maj, &min)) { GLOG("[gpu3d] EGL (surfaceless) init failed: 0x%x\n", eglGetError_()); return false; }
    eglBindAPI_(EGL_OPENGL_ES_API);
    const char* ext = eglQueryString_(dpy, EGL_EXTENSIONS);
    EGLConfig cfg = nullptr;
    if (!ext || !strstr(ext, "EGL_KHR_no_config_context")) {
        EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE}, n = 0;
        if (!eglChooseConfig_(dpy, want, &cfg, 1, &n) || n < 1) { GLOG("[gpu3d] no EGL config\n"); return false; }
    }
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    EGLContext ctx = eglCreateContext_(dpy, cfg, EGL_NO_CONTEXT, ca);
    if (!ctx || !eglMakeCurrent_(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        GLOG("[gpu3d] no GLES 3.1 context: 0x%x\n", eglGetError_());
        return false;
    }
    GLint ssbo = 0, ssbosize = 0, inv = 0;
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &ssbo);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &ssbosize);
    glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &inv);
    GLOG("[gpu3d] %s / %s / %s; compute SSBOs %d (max %d MB), invocations %d\n", (const char*)glGetString(GL_RENDERER),
         (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION), ssbo,
         ssbosize >> 20, inv);

    Gpu3DGL& g = *G;
    g.ScaleFactor = Scale_;
    g.ScreenWidth = 256 * Scale_;
    g.ScreenHeight = 192 * Scale_;
    g.HiresCoordinates = Hires != 0;
    // (melonDS: TileSize 8 below 4.5x)
    g.TileSize = 8;
    g.CoarseTileCountY = 4;
    g.ClearCoarseBinMaskLocalSize = 64;
    g.CoarseTileArea = Gpu3DGL::CoarseTileCountX * g.CoarseTileCountY;
    g.CoarseTileW = Gpu3DGL::CoarseTileCountX * g.TileSize;
    g.CoarseTileH = g.CoarseTileCountY * g.TileSize;
    g.TilesPerLine = g.ScreenWidth / g.TileSize;
    g.TileLines = g.ScreenHeight / g.TileSize;
    g.MaxWorkTiles = g.TilesPerLine * g.TileLines * std::max(2, WorkTiles);

    // shaders
    const uint64_t c0 = NowNs();
    auto hdr = [&](std::initializer_list<const char*> defs) {
        std::string s = "#version 310 es\nprecision highp float;\nprecision highp int;\nprecision highp usampler2D;\n"
                        "precision highp usampler2DArray;\nprecision highp sampler2DArray;\n";
        for (const char* d : defs) s += std::string("#define ") + d + "\n";
        s += "#define ScreenWidth " + std::to_string(g.ScreenWidth) + "\n#define ScreenHeight " + std::to_string(g.ScreenHeight) +
             "\n#define MaxWorkTiles " + std::to_string(g.MaxWorkTiles) + "\n#define TileSize " + std::to_string(g.TileSize) +
             "\nconst int CoarseTileCountY = " + std::to_string(g.CoarseTileCountY) + ";\n#define CoarseTileArea " +
             std::to_string(g.CoarseTileArea) + "\n#define ClearCoarseBinMaskLocalSize " + std::to_string(g.ClearCoarseBinMaskLocalSize) + "\n";
        return s + ComputeRendererShaders::Common;
    };
    using namespace ComputeRendererShaders;
    struct P { GLuint* p; const std::string* src; std::initializer_list<const char*> d; };
    const P progs[] = {
        {&g.ShaderInterpXSpans[0], &InterpSpans, {"InterpSpans", "ZBuffer"}},
        {&g.ShaderInterpXSpans[1], &InterpSpans, {"InterpSpans", "WBuffer"}},
        {&g.ShaderBinCombined, &BinCombined, {"BinCombined"}},
        {&g.ShaderDepthBlend[0], &DepthBlend, {"DepthBlend", "ZBuffer"}},
        {&g.ShaderDepthBlend[1], &DepthBlend, {"DepthBlend", "WBuffer"}},
        {&g.ShaderRasteriseNoTexture[0], &Rasterise, {"Rasterise", "ZBuffer", "NoTexture"}},
        {&g.ShaderRasteriseNoTexture[1], &Rasterise, {"Rasterise", "WBuffer", "NoTexture"}},
        {&g.ShaderRasteriseNoTextureToon[0], &Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Toon"}},
        {&g.ShaderRasteriseNoTextureToon[1], &Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Toon"}},
        {&g.ShaderRasteriseNoTextureHighlight[0], &Rasterise, {"Rasterise", "ZBuffer", "NoTexture", "Highlight"}},
        {&g.ShaderRasteriseNoTextureHighlight[1], &Rasterise, {"Rasterise", "WBuffer", "NoTexture", "Highlight"}},
        {&g.ShaderRasteriseUseTextureDecal[0], &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Decal"}},
        {&g.ShaderRasteriseUseTextureDecal[1], &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Decal"}},
        {&g.ShaderRasteriseUseTextureModulate[0], &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Modulate"}},
        {&g.ShaderRasteriseUseTextureModulate[1], &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Modulate"}},
        {&g.ShaderRasteriseUseTextureToon[0], &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Toon"}},
        {&g.ShaderRasteriseUseTextureToon[1], &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Toon"}},
        {&g.ShaderRasteriseUseTextureHighlight[0], &Rasterise, {"Rasterise", "ZBuffer", "UseTexture", "Highlight"}},
        {&g.ShaderRasteriseUseTextureHighlight[1], &Rasterise, {"Rasterise", "WBuffer", "UseTexture", "Highlight"}},
        {&g.ShaderRasteriseShadowMask[0], &Rasterise, {"Rasterise", "ZBuffer", "ShadowMask"}},
        {&g.ShaderRasteriseShadowMask[1], &Rasterise, {"Rasterise", "WBuffer", "ShadowMask"}},
        {&g.ShaderClearCoarseBinMask, &ClearCoarseBinMask, {"ClearCoarseBinMask"}},
        {&g.ShaderClearIndirectWorkCount, &ClearIndirectWorkCount, {"ClearIndirectWorkCount"}},
        {&g.ShaderCalculateWorkListOffset, &CalcOffsets, {"CalculateWorkOffsets"}},
        {&g.ShaderSortWork, &SortWork, {"SortWork"}},
        {&g.ShaderFinalPass[0], &FinalPass, {"FinalPass"}},
        {&g.ShaderFinalPass[1], &FinalPass, {"FinalPass", "EdgeMarking"}},
        {&g.ShaderFinalPass[2], &FinalPass, {"FinalPass", "Fog"}},
        {&g.ShaderFinalPass[3], &FinalPass, {"FinalPass", "EdgeMarking", "Fog"}},
        {&g.ShaderFinalPass[4], &FinalPass, {"FinalPass", "AntiAliasing"}},
        {&g.ShaderFinalPass[5], &FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking"}},
        {&g.ShaderFinalPass[6], &FinalPass, {"FinalPass", "AntiAliasing", "Fog"}},
        {&g.ShaderFinalPass[7], &FinalPass, {"FinalPass", "AntiAliasing", "EdgeMarking", "Fog"}},
    };
    for (const P& p : progs) {
        std::string name;
        for (const char* d : p.d) name += std::string(d) + ",";
        if (!CompileProgram(*p.p, hdr(p.d) + *p.src, name.c_str())) return false;
    }
    GLOG("[gpu3d] 33 compute programs ready in %.0f ms\n", (NowNs() - c0) / 1e6);

    // buffers (melonDS's Init + SetRenderSettings)
    glGenBuffers(1, &g.YSpanSetupMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.YSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(Gpu3DGL::SpanSetupY) * Gpu3DGL::MaxYSpanSetups, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &g.RenderPolygonMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.RenderPolygonMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(Gpu3DGL::RenderPolygon) * 2048, nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &g.XSpanSetupMemory);
    glGenBuffers(1, &g.BinResultMemory);
    glGenBuffers(1, &g.FinalTileMemory);
    glGenBuffers(1, &g.YSpanIndicesMemory);
    glGenBuffers(3, g.TileMemory);
    glGenBuffers(1, &g.WorkDescMemory);
    glGenBuffers(1, &g.OutMemory);
    glGenBuffers(1, &g.MetaUniformMemory);
    glBindBuffer(GL_UNIFORM_BUFFER, g.MetaUniformMemory);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(Gpu3DJob::Meta), nullptr, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &g.DivTableMemory);
    {
        u32 t[256 * 1];
        for (int i = 0; i < 256; i++) t[i] = ComputeRendererShaders::DivStartTable[i];
        glBindBuffer(GL_UNIFORM_BUFFER, g.DivTableMemory);
        glBufferData(GL_UNIFORM_BUFFER, sizeof t, t, GL_STATIC_DRAW);
    }
    glGenSamplers(9, g.Samplers);
    for (u32 jj = 0; jj < 3; jj++)
        for (u32 i = 0; i < 3; i++) {
            const GLenum wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
            glSamplerParameteri(g.Samplers[i + jj * 3], GL_TEXTURE_WRAP_S, wrap[i]);
            glSamplerParameteri(g.Samplers[i + jj * 3], GL_TEXTURE_WRAP_T, wrap[jj]);
            glSamplerParameteri(g.Samplers[i + jj * 3], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(g.Samplers[i + jj * 3], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    glGenTextures(2, g.ClearBitmapTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g.ClearBitmapTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 256, 256);
        g.ClearBitmap[i].resize(256 * 256);
    }
    glGenTextures(1, &g.DummyCapture);  // (display captures are decoded from VRAM like any texture: never sampled)
    glBindTexture(GL_TEXTURE_2D_ARRAY, g.DummyCapture);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 1, 1, 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    for (int i = 0; i < 3; i++) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.TileMemory[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)4 * g.TileSize * g.TileSize * g.MaxWorkTiles, nullptr, GL_DYNAMIC_DRAW);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.FinalTileMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)4 * 3 * 2 * g.ScreenWidth * g.ScreenHeight, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.OutMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)4 * g.ScreenWidth * g.ScreenHeight, nullptr, GL_DYNAMIC_READ);
    const int binResultSize = sizeof(Gpu3DGL::BinResultHeader) + g.TilesPerLine * g.TileLines * Gpu3DGL::CoarseBinStride * 4 +
                              g.TilesPerLine * g.TileLines * Gpu3DGL::BinStride * 4 * 2;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.BinResultMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, binResultSize, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.WorkDescMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)g.MaxWorkTiles * 2 * 4 * 2, nullptr, GL_DYNAMIC_DRAW);
    const int maxYSpanIndices = 64 * 2048 * g.ScaleFactor;
    g.YSpanIndices.resize(maxYSpanIndices);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.YSpanIndicesMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)maxYSpanIndices * 2 * 4, nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.XSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)Gpu3DGL::SpanSetupXSize * maxYSpanIndices, nullptr, GL_DYNAMIC_DRAW);
    const GLenum err = glGetError();
    if (err) { GLOG("[gpu3d] GL error 0x%x after setup\n", err); return false; }
    GLOG("[gpu3d] %dx%d, tiles %d, %d work tiles (%.0f MB of tile memory)\n", g.ScreenWidth, g.ScreenHeight, g.TileSize,
         g.MaxWorkTiles, 3.0 * 4 * g.TileSize * g.TileSize * g.MaxWorkTiles / 1048576.0);
    return true;
}

void ComputeGpuRenderer3D::Thread() {
    prctl(PR_SET_NAME, "bds-gpu3d");
    const bool ok = GLInit();
    {
        std::lock_guard<std::mutex> l(M);
        Ready = ok ? 1 : -1;
    }
    CvDone.notify_all();
    if (!ok) return;
    uint64_t stCpu = 0, stGpu = 0, stRead = 0, stMaxAll = 0, stWork = 0;
    unsigned stN = 0;
    for (;;) {
        unsigned gen;
        {
            std::unique_lock<std::mutex> l(M);
            CvJob.wait(l, [&] { return Quit || Posted != Done.load(std::memory_order_relaxed); });
            if (Quit) break;
            gen = Posted;
        }
        Gpu3DJob& j = *Job;
        u32* out = Out;  // (set before Posted, under M)
        const uint64_t t0 = NowNs();
        GLFrame(j);  // (stores CpuDone once it no longer reads the snapshot)
        const uint64_t t1 = NowNs();
        // the frame read back into its rows
        Gpu3DGL& g = *G;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.OutMemory);
        const size_t bytes = (size_t)4 * g.ScreenWidth * g.ScreenHeight;
        const void* p = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, bytes, GL_MAP_READ_BIT);
        const uint64_t t2 = NowNs();
        if (p) memcpy(out, p, bytes);
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        if (Stats) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.BinResultMemory);
            const u32* h = (const u32*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, 16, GL_MAP_READ_BIT);
            if (h) stWork += h[3];  // (VariantWorkCount[0].w: the frame's work tiles)
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
        }
        const uint64_t t3 = NowNs();
        {
            std::lock_guard<std::mutex> l(M);
            Done.store(gen, std::memory_order_release);
        }
        CvDone.notify_all();
        if (Stats) {
            stCpu += t1 - t0;
            stGpu += t2 - t1;
            stRead += t3 - t2;
            stMaxAll = std::max(stMaxAll, t3 - t0);
            if (++stN == (unsigned)Stats) {
                GLOG("[gpu3d-stats] %u frames: setup+submit %.2f ms, GPU wait %.2f ms, readback %.2f ms, slowest %.2f ms; "
                     "%.0f work tiles (of %d), %lu textures decoded, %d polys\n",
                     stN, stCpu / 1e6 / stN, stGpu / 1e6 / stN, stRead / 1e6 / stN, stMaxAll / 1e6, (double)stWork / stN,
                     g.MaxWorkTiles, g.Decoded, j.npolys);
                if (Prof && PhaseN) {
                    std::string f;
                    for (int k = 0; k < 6; k++) { char b[32]; snprintf(b, sizeof b, " %.2f", PhaseNs[k] / 1e6 / PhaseN); f += b; PhaseNs[k] = 0; }
                    char fq[32] = "";
                    if (FILE* q = fopen("/sys/class/devfreq/fde60000.gpu/cur_freq", "r")) { if (!fgets(fq, sizeof fq, q)) fq[0] = 0; fclose(q); }
                    GLOG("[gpu3d-prof] ms/frame: spans%s (bin, sort, raster, depthblend, final in order after spans); variants %u; GPU Hz %s", f.c_str(), LastVariants, fq);
                    PhaseN = 0;
                }
                stCpu = stGpu = stRead = stMaxAll = stWork = 0;
                stN = 0;
                g.Decoded = 0;
            }
        }
    }
}

// melonDS's Texcache helpers
static u64 MaskedHash(u8* vram, u32 vramSize, u32 addr, u32 size) {
    u64 hash = 0;
    while (size > 0) {
        u32 pieceSize = addr + size > vramSize ? vramSize - addr : size;
        hash = XXH64(&vram[addr], pieceSize, hash);
        addr += pieceSize;
        addr &= (vramSize - 1);
        size -= pieceSize;
    }
    return hash;
}

static bool CheckInvalid(u32 start, u32 size, u64 oldHash, const u64* dirty, u8* vram, u32 vramSize) {
    u32 startBit = start / VRAMDirtyGranularity;
    u32 bitsCount = ((start + size + VRAMDirtyGranularity - 1) / VRAMDirtyGranularity) - startBit;
    u32 startEntry = startBit >> 6;
    u64 entriesCount = ((startBit + bitsCount + 0x3F) >> 6) - startEntry;
    for (u32 j = startEntry; j < startEntry + entriesCount; j++)
        if (GetRangedBitMask(j, startBit, bitsCount) & dirty[j & ((vramSize / VRAMDirtyGranularity) - 1)])
            if (MaskedHash(vram, vramSize, start, size) != oldHash) return true;
    return false;
}

void ComputeGpuRenderer3D::GLFrame(Gpu3DJob& jb) {
    Gpu3DGL& g = *G;
    melonDS::GPU& gpu = GPU;
    if (ResetTex.exchange(false)) {
        for (u32 i = 0; i < 8; i++)
            for (u32 k = 0; k < 8; k++) {
                for (GLuint t : g.TexArrays[i][k]) glDeleteTextures(1, &t);
                g.TexArrays[i][k].clear();
                g.FreeTextures[i][k].clear();
            }
        g.Cache.clear();
        g.ClearBitmapDirty = 3;
    }
    // the texture cache's invalidation (melonDS's Texcache::Update, its dirty sets taken on the emulator's thread)
    u8 clrBitmapDirty = 0;
    if (jb.texChanged || jb.palChanged) {
        for (u32 k = (0x40000 / (VRAMDirtyGranularity * 64)); k < (0x60000 / (VRAMDirtyGranularity * 64)); k++)
            if (jb.texChanged && jb.texDirty[k]) { clrBitmapDirty |= 1; break; }
        for (u32 k = (0x60000 / (VRAMDirtyGranularity * 64)); k < (0x80000 / (VRAMDirtyGranularity * 64)); k++)
            if (jb.texChanged && jb.texDirty[k]) { clrBitmapDirty |= 2; break; }
        for (auto it = g.Cache.begin(); it != g.Cache.end();) {
            Gpu3DGL::TexCacheEntry& e = it->second;
            bool bad = false;
            if (jb.texChanged)
                for (u32 i = 0; i < 2 && !bad; i++)
                    bad = CheckInvalid(e.TextureRAMStart[i], e.TextureRAMSize[i], e.TextureHash[i], jb.texDirty,
                                       gpu.VRAMFlat_Texture, sizeof(gpu.VRAMFlat_Texture));
            if (!bad && jb.palChanged && e.TexPalSize > 0)
                bad = CheckInvalid(e.TexPalStart, e.TexPalSize, e.TexPalHash, jb.palDirty, gpu.VRAMFlat_TexPal,
                                   sizeof(gpu.VRAMFlat_TexPal));
            if (bad) {
                g.FreeTextures[e.WidthLog2][e.HeightLog2].push_back(e.Texture);
                it = g.Cache.erase(it);
            } else
                ++it;
        }
    }
    auto getTexture = [&](u32 texParam, u32 palBase, GLuint& handle, u32& layer, u32*& helper) {
        texParam &= ~0xC00F0000;
        u32 fmt = (texParam >> 26) & 0x7;
        u64 key = texParam;
        if (fmt != 7) {
            key |= (u64)palBase << 32;
            if (fmt == 5) key &= ~((u64)1 << 29);
        }
        auto it = g.Cache.find(key);
        if (it != g.Cache.end()) {
            handle = it->second.Texture.TextureID;
            layer = it->second.Texture.Layer;
            helper = &it->second.LastVariant;
            return;
        }
        u32 widthLog2 = (texParam >> 20) & 0x7, heightLog2 = (texParam >> 23) & 0x7;
        u32 width = 8 << widthLog2, height = 8 << heightLog2;
        u32 addr = (texParam & 0xFFFF) * 8;
        Gpu3DGL::TexCacheEntry entry = {};
        entry.TextureRAMStart[0] = addr;
        entry.WidthLog2 = widthLog2;
        entry.HeightLog2 = heightLog2;
        u32* buf = g.DecodingBuffer.data();
        if (fmt == 7) {
            entry.TextureRAMSize[0] = width * height * 2;
            ConvertBitmapTexture<outputFmt_RGB6A5>(width, height, buf, addr, gpu);
        } else if (fmt == 5) {
            u32 slot1addr = 0x20000 + ((addr & 0x1FFFC) >> 1);
            if (addr >= 0x40000) slot1addr += 0x10000;
            entry.TextureRAMSize[0] = width * height / 16 * 4;
            entry.TextureRAMStart[1] = slot1addr;
            entry.TextureRAMSize[1] = width * height / 16 * 2;
            entry.TexPalStart = palBase * 16;
            entry.TexPalSize = 0x10000;
            ConvertCompressedTexture<outputFmt_RGB6A5>(width, height, buf, addr, slot1addr, entry.TexPalStart, gpu);
        } else {
            u32 texSize = 0, palAddr = palBase * 16, numPalEntries = 0;
            switch (fmt) {
            case 1: texSize = width * height; numPalEntries = 32; break;
            case 6: texSize = width * height; numPalEntries = 8; break;
            case 2: texSize = width * height / 4; numPalEntries = 4; palAddr >>= 1; break;
            case 3: texSize = width * height / 2; numPalEntries = 16; break;
            case 4: texSize = width * height; numPalEntries = 256; break;
            }
            palAddr &= 0x1FFFF;
            entry.TextureRAMSize[0] = texSize;
            entry.TexPalStart = palAddr;
            entry.TexPalSize = numPalEntries * 2;
            bool color0Transparent = texParam & (1 << 29);
            switch (fmt) {
            case 1: ConvertAXIYTexture<outputFmt_RGB6A5, 3, 5>(width, height, buf, addr, palAddr, gpu); break;
            case 6: ConvertAXIYTexture<outputFmt_RGB6A5, 5, 3>(width, height, buf, addr, palAddr, gpu); break;
            case 2: ConvertNColorsTexture<outputFmt_RGB6A5, 2>(width, height, buf, addr, palAddr, color0Transparent, gpu); break;
            case 3: ConvertNColorsTexture<outputFmt_RGB6A5, 4>(width, height, buf, addr, palAddr, color0Transparent, gpu); break;
            case 4: ConvertNColorsTexture<outputFmt_RGB6A5, 8>(width, height, buf, addr, palAddr, color0Transparent, gpu); break;
            }
        }
        for (int i = 0; i < 2; i++)
            if (entry.TextureRAMSize[i])
                entry.TextureHash[i] = MaskedHash(gpu.VRAMFlat_Texture, sizeof(gpu.VRAMFlat_Texture), entry.TextureRAMStart[i], entry.TextureRAMSize[i]);
        if (entry.TexPalSize)
            entry.TexPalHash = MaskedHash(gpu.VRAMFlat_TexPal, sizeof(gpu.VRAMFlat_TexPal), entry.TexPalStart, entry.TexPalSize);
        auto& texArrays = g.TexArrays[widthLog2][heightLog2];
        auto& freeTextures = g.FreeTextures[widthLog2][heightLog2];
        if (freeTextures.empty()) {
            GLuint array;
            u32 layers = std::min<u32>((8 * 1024 * 1024) / (width * height * 4), 64);
            glGenTextures(1, &array);
            glBindTexture(GL_TEXTURE_2D_ARRAY, array);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, width, height, layers);
            texArrays.push_back(array);
            for (u32 i = 0; i < layers; i++) freeTextures.push_back(Gpu3DGL::TexArrayEntry{array, i});
        }
        Gpu3DGL::TexArrayEntry place = freeTextures.back();
        freeTextures.pop_back();
        entry.Texture = place;
        glBindTexture(GL_TEXTURE_2D_ARRAY, place.TextureID);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, place.Layer, width, height, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, buf);
        g.Decoded++;
        handle = place.TextureID;
        layer = place.Layer;
        helper = &g.Cache.emplace(std::make_pair(key, entry)).first->second.LastVariant;
    };

    // the clear bitmap
    g.ClearBitmapDirty |= clrBitmapDirty;
    if (jb.dispcnt & (1 << 14)) {
        if (g.ClearBitmapDirty & 1) {
            u16* vram = (u16*)&gpu.VRAMFlat_Texture[0x40000];
            for (int i = 0; i < 256 * 256; i++) {
                u16 color = vram[i];
                u32 r = (color << 1) & 0x3E; if (r) r++;
                u32 gg = (color >> 4) & 0x3E; if (gg) gg++;
                u32 b = (color >> 9) & 0x3E; if (b) b++;
                u32 a = (color & 0x8000) ? 31 : 0;
                g.ClearBitmap[0][i] = r | (gg << 8) | (b << 16) | (a << 24);
            }
            glBindTexture(GL_TEXTURE_2D, g.ClearBitmapTex[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, g.ClearBitmap[0].data());
        }
        if (g.ClearBitmapDirty & 2) {
            u16* vram = (u16*)&gpu.VRAMFlat_Texture[0x60000];
            for (int i = 0; i < 256 * 256; i++) {
                u16 val = vram[i];
                g.ClearBitmap[1][i] = (((val & 0x7FFF) * 0x200) + 0x1FF) | ((u32)(val & 0x8000) << 9);
            }
            glBindTexture(GL_TEXTURE_2D, g.ClearBitmapTex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, g.ClearBitmap[1].data());
        }
        g.ClearBitmapDirty = 0;
    }

    // melonDS's ComputeRenderer3D::RenderFrame from here (variants, y spans), on the snapshot
    struct Variant {
        GLuint Texture, Sampler;
        u16 Width, Height;
        u8 BlendMode;
        int CaptureYOffset;
        bool operator==(const Variant& o) const {
            return Texture == o.Texture && Sampler == o.Sampler && BlendMode == o.BlendMode && CaptureYOffset == o.CaptureYOffset;
        }
    };
    int numYSpans = 0, numSetupIndices = 0;
    u32 numVariants = 0, prevVariant = 0, prevTexLayer = 0;
    static Variant variants[Gpu3DGL::MaxVariants];
    const bool enableTextureMaps = jb.dispcnt & (1 << 0);
    const int npolys = jb.npolys;
    for (int i = 0; i < npolys; i++) {
        Polygon* polygon = &jb.polys[i];
        u32 nverts = polygon->NumVertices;
        u32 vtop = polygon->VTop, vbot = polygon->VBottom;
        u32 curVL = vtop, curVR = vtop;
        u32 nextVL, nextVR;
        g.RenderPolygons[i].FirstXSpan = numSetupIndices;
        g.RenderPolygons[i].Attr = polygon->Attr;
        bool foundVariant = false;
        if (i > 0) {
            Polygon* prevPolygon = &jb.polys[i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam && prevPolygon->TexPalette == polygon->TexPalette &&
                           (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30) && prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }
        if (!foundVariant) {
            Variant variant;
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            variant.CaptureYOffset = -1;
            u32* textureLastVariant = nullptr;
            u32 textype = (polygon->TexParam >> 26) & 0x7;
            if (enableTextureMaps && textype) {
                getTexture(polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer, textureLastVariant);
                bool wrapS = (polygon->TexParam >> 16) & 1, wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1, mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = g.Samplers[(wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3];
                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant) {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }
            if (!foundVariant) {
                for (int k = (int)numVariants - 1; k >= 0; k--)
                    if (variants[k] == variant) {
                        foundVariant = true;
                        prevVariant = k;
                        break;
                    }
                if (!foundVariant) {
                    if (numVariants >= (u32)Gpu3DGL::MaxVariants) numVariants = Gpu3DGL::MaxVariants - 1;  // (overflow: reuse the last)
                    prevVariant = numVariants;
                    variants[numVariants] = variant;
                    variants[numVariants].Width = TextureWidth(polygon->TexParam);
                    variants[numVariants].Height = TextureHeight(polygon->TexParam);
                    numVariants++;
                }
                if (textureLastVariant) *textureLastVariant = prevVariant;
            }
        }
        g.RenderPolygons[i].Variant = prevVariant;
        g.RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        if (polygon->FacingView) {
            nextVL = curVL + 1;
            if (nextVL >= nverts) nextVL = 0;
            nextVR = curVR - 1;
            if ((s32)nextVR < 0) nextVR = nverts - 1;
        } else {
            nextVL = curVL - 1;
            if ((s32)nextVL < 0) nextVL = nverts - 1;
            nextVR = curVR + 1;
            if (nextVR >= nverts) nextVR = 0;
        }
        s32 scaledPositions[10][2];
        s32 ytop = g.ScreenHeight, ybot = 0;
        for (u32 k = 0; k < nverts; k++) {
            if (g.HiresCoordinates) {
                scaledPositions[k][0] = (polygon->Vertices[k]->HiresPosition[0] * g.ScaleFactor) >> 4;
                scaledPositions[k][1] = (polygon->Vertices[k]->HiresPosition[1] * g.ScaleFactor) >> 4;
            } else {
                scaledPositions[k][0] = polygon->Vertices[k]->FinalPosition[0] * g.ScaleFactor;
                scaledPositions[k][1] = polygon->Vertices[k]->FinalPosition[1] * g.ScaleFactor;
            }
            ytop = std::min(scaledPositions[k][1], ytop);
            ybot = std::max(scaledPositions[k][1], ybot);
        }
        g.RenderPolygons[i].YTop = ytop;
        g.RenderPolygons[i].YBot = ybot;
        g.RenderPolygons[i].XMin = g.ScreenWidth;
        g.RenderPolygons[i].XMax = 0;
        // (bounds: the span tables are sized like melonDS's; a frame past them is cut short rather than overrun)
        if (numYSpans + 2 + 2 * (int)nverts >= Gpu3DGL::MaxYSpanSetups ||
            numSetupIndices + (ybot - ytop) + 1 >= (int)g.YSpanIndices.size()) {
            g.RenderPolygons[i].YBot = g.RenderPolygons[i].YTop;
            continue;
        }
        if (ybot == ytop) {
            vtop = 0;
            vbot = 0;
            g.RenderPolygons[i].YBot++;
            int k = 1;
            if (scaledPositions[k][0] < scaledPositions[vtop][0]) vtop = k;
            if (scaledPositions[k][0] > scaledPositions[vbot][0]) vbot = k;
            k = nverts - 1;
            if (scaledPositions[k][0] < scaledPositions[vtop][0]) vtop = k;
            if (scaledPositions[k][0] > scaledPositions[vbot][0]) vbot = k;
            u32 curSpanL = numYSpans;
            g.SetupYSpanDummy(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, vtop, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            g.SetupYSpanDummy(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, vbot, 1, scaledPositions);
            g.YSpanIndices[numSetupIndices].PolyIdx = i;
            g.YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
            g.YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
            g.YSpanIndices[numSetupIndices].Y = ytop;
            numSetupIndices++;
        } else {
            u32 curSpanL = numYSpans;
            g.SetupYSpan(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
            u32 curSpanR = numYSpans;
            g.SetupYSpan(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);
            for (s32 y = ytop; y < ybot; y++) {
                if (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom) {
                    while (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom) {
                        curVL = nextVL;
                        if (polygon->FacingView) {
                            nextVL = curVL + 1;
                            if (nextVL >= nverts) nextVL = 0;
                        } else {
                            nextVL = curVL - 1;
                            if ((s32)nextVL < 0) nextVL = nverts - 1;
                        }
                    }
                    curSpanL = numYSpans;
                    g.SetupYSpan(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, curVL, nextVL, 0, scaledPositions);
                }
                if (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom) {
                    while (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom) {
                        curVR = nextVR;
                        if (polygon->FacingView) {
                            nextVR = curVR - 1;
                            if ((s32)nextVR < 0) nextVR = nverts - 1;
                        } else {
                            nextVR = curVR + 1;
                            if (nextVR >= nverts) nextVR = 0;
                        }
                    }
                    curSpanR = numYSpans;
                    g.SetupYSpan(&g.RenderPolygons[i], &g.YSpanSetups[numYSpans++], polygon, curVR, nextVR, 1, scaledPositions);
                }
                g.YSpanIndices[numSetupIndices].PolyIdx = i;
                g.YSpanIndices[numSetupIndices].SpanIdxL = curSpanL;
                g.YSpanIndices[numSetupIndices].SpanIdxR = curSpanR;
                g.YSpanIndices[numSetupIndices].Y = y;
                numSetupIndices++;
            }
        }
    }
    const bool wbuffer = npolys > 0 && jb.polys[0].WBuffer;
    const u32 dispcnt = jb.dispcnt;
    Gpu3DJob::Meta meta = jb.meta;
    meta.NumVariants = numVariants;
    LastVariants = numVariants;
    // the snapshot and the flat VRAM are no longer read: the emulator's thread may go on to the next frame
    {
        std::lock_guard<std::mutex> l(M);
        CpuDone.store(jb.gen, std::memory_order_release);
    }
    CvDone.notify_all();

    if (numYSpans > 0) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.YSpanSetupMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(Gpu3DGL::SpanSetupY) * numYSpans, g.YSpanSetups.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.YSpanIndicesMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, numSetupIndices * 4 * 2, g.YSpanIndices.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g.RenderPolygonMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, npolys * sizeof(Gpu3DGL::RenderPolygon), g.RenderPolygons);
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, g.RenderPolygonMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, g.XSpanSetupMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, g.YSpanSetupMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, g.YSpanIndicesMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, g.FinalTileMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, g.BinResultMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, g.WorkDescMemory);
    glBindBuffer(GL_UNIFORM_BUFFER, g.MetaUniformMemory);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof meta, &meta);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, g.MetaUniformMemory);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, g.DivTableMemory);

    Phase(-1);
    glUseProgram(g.ShaderClearCoarseBinMask);
    glDispatchCompute(g.TilesPerLine * g.TileLines / g.ClearCoarseBinMaskLocalSize, 1, 1);
    if (numYSpans > 0) {
        glUseProgram(g.ShaderClearIndirectWorkCount);
        glDispatchCompute((numVariants + 31) / 32, 1, 1);
        glUseProgram(g.ShaderInterpXSpans[wbuffer]);
        glDispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        Phase(0);
        glUseProgram(g.ShaderBinCombined);
        glDispatchCompute((npolys + 31) / 32, g.ScreenWidth / g.CoarseTileW, g.ScreenHeight / g.CoarseTileH);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        Phase(1);
        glUseProgram(g.ShaderCalculateWorkListOffset);
        glDispatchCompute((numVariants + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        glUseProgram(g.ShaderSortWork);
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, g.BinResultMemory);
        glDispatchComputeIndirect(offsetof(Gpu3DGL::BinResultHeader, SortWorkWorkCount));
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);
        Phase(2);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g.DummyCapture);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g.DummyCapture);
        glActiveTexture(GL_TEXTURE0);
        for (int i = 0; i < 3; i++) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2 + i, g.TileMemory[i]);
        const bool highLightMode = dispcnt & (1 << 1);
        GLuint shadersNoTexture[] = {g.ShaderRasteriseNoTexture[wbuffer], g.ShaderRasteriseNoTexture[wbuffer],
                                     highLightMode ? g.ShaderRasteriseNoTextureHighlight[wbuffer] : g.ShaderRasteriseNoTextureToon[wbuffer],
                                     g.ShaderRasteriseNoTexture[wbuffer], g.ShaderRasteriseShadowMask[wbuffer]};
        GLuint shadersUseTexture[] = {g.ShaderRasteriseUseTextureModulate[wbuffer], g.ShaderRasteriseUseTextureDecal[wbuffer],
                                      highLightMode ? g.ShaderRasteriseUseTextureHighlight[wbuffer] : g.ShaderRasteriseUseTextureToon[wbuffer],
                                      g.ShaderRasteriseUseTextureDecal[wbuffer], g.ShaderRasteriseShadowMask[wbuffer]};
        GLuint prevShader = 0, prevTexture = 0, prevSampler = 0;
        for (u32 i = 0; i < numVariants; i++) {
            GLuint shader;
            if (variants[i].Texture == 0)
                shader = shadersNoTexture[variants[i].BlendMode];
            else {
                shader = shadersUseTexture[variants[i].BlendMode];
                if (variants[i].Texture != prevTexture) {
                    glBindTexture(GL_TEXTURE_2D_ARRAY, variants[i].Texture);
                    prevTexture = variants[i].Texture;
                }
                if (variants[i].Sampler != prevSampler) {
                    glBindSampler(0, variants[i].Sampler);
                    prevSampler = variants[i].Sampler;
                }
            }
            if (shader != prevShader) {
                glUseProgram(shader);
                prevShader = shader;
            }
            glUniform1ui(0, i);
            glUniform2f(1, 1.f / variants[i].Width, 1.f / variants[i].Height);
            glUniform1i(2, 0);
            glDispatchComputeIndirect(offsetof(Gpu3DGL::BinResultHeader, VariantWorkCount) + i * 4 * 4);
        }
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    Phase(3);
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.ClearBitmapTex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g.ClearBitmapTex[1]);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < 3; i++) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2 + i, g.TileMemory[i]);
    glUseProgram(g.ShaderDepthBlend[wbuffer]);
    glDispatchCompute(g.ScreenWidth / g.TileSize, g.ScreenHeight / g.TileSize, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    Phase(4);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, g.OutMemory);
    u32 fp = 0;
    if (dispcnt & (1 << 4)) fp |= 4;
    if (dispcnt & (1 << 7)) fp |= 2;
    if (dispcnt & (1 << 5)) fp |= 1;
    glUseProgram(g.ShaderFinalPass[fp]);
    glDispatchCompute(g.ScreenWidth / 32, g.ScreenHeight, 1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    Phase(5);
    PhaseN++;
    glFlush();
}

#ifdef BANANADS_2D
ComputeBananaRenderer::ComputeBananaRenderer(melonDS::NDS& nds, int scale) : melonDS::BananaRenderer(nds) {
    auto r = std::make_unique<ComputeGpuRenderer3D>(GPU.GPU3D, scale);
    R = r.get();
    SetHires(&ComputeGpuRenderer3D::QuarterFn, R, &ComputeGpuRenderer3D::QuarterRowsFn, scale);
    SetHiresRows(&ComputeGpuRenderer3D::QuarterRowFn, &ComputeGpuRenderer3D::RowsGenFn, &ComputeGpuRenderer3D::WaitRowsFn);
    Rend3D = std::move(r);
}
#endif

}  // namespace bananads
