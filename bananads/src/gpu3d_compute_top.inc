// (gpu3d_compute.cpp's first part: GL loading, the GL-side state; melonDS's span setup follows from gpu3d_spans.inc)
#define GL_GLES_PROTOTYPES 0
#include <GLES3/gl31.h>
#define EGL_EGL_PROTOTYPES 0
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>

namespace bananads {
using namespace melonDS;

#define GPU3D_GL_FUNCS(X) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC, glBufferSubData) X(PFNGLBINDBUFFERBASEPROC, glBindBufferBase) \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D) \
    X(PFNGLTEXSTORAGE2DPROC, glTexStorage2D) X(PFNGLTEXSTORAGE3DPROC, glTexStorage3D) X(PFNGLTEXSUBIMAGE3DPROC, glTexSubImage3D) \
    X(PFNGLDELETETEXTURESPROC, glDeleteTextures) X(PFNGLGENSAMPLERSPROC, glGenSamplers) \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) X(PFNGLBINDSAMPLERPROC, glBindSampler) \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute) X(PFNGLDISPATCHCOMPUTEINDIRECTPROC, glDispatchComputeIndirect) \
    X(PFNGLMEMORYBARRIERPROC, glMemoryBarrier) X(PFNGLUNIFORM1UIPROC, glUniform1ui) X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) X(PFNGLDELETESHADERPROC, glDeleteShader) \
    X(PFNGLDELETEPROGRAMPROC, glDeleteProgram) X(PFNGLMAPBUFFERRANGEPROC, glMapBufferRange) \
    X(PFNGLUNMAPBUFFERPROC, glUnmapBuffer) X(PFNGLFENCESYNCPROC, glFenceSync) X(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync) \
    X(PFNGLDELETESYNCPROC, glDeleteSync) X(PFNGLGETSTRINGPROC, glGetString) X(PFNGLGETINTEGERVPROC, glGetIntegerv) \
    X(PFNGLGETERRORPROC, glGetError) X(PFNGLFLUSHPROC, glFlush) X(PFNGLFINISHPROC, glFinish) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLDELETESAMPLERSPROC, glDeleteSamplers)

#define GPU3D_GL_DECL(t, n) static t n;
GPU3D_GL_FUNCS(GPU3D_GL_DECL)

static PFNEGLGETPROCADDRESSPROC eglGetProcAddress_;
static PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT_;
static PFNEGLINITIALIZEPROC eglInitialize_;
static PFNEGLBINDAPIPROC eglBindAPI_;
static PFNEGLCREATECONTEXTPROC eglCreateContext_;
static PFNEGLMAKECURRENTPROC eglMakeCurrent_;
static PFNEGLQUERYSTRINGPROC eglQueryString_;
static PFNEGLCHOOSECONFIGPROC eglChooseConfig_;
static PFNEGLGETERRORPROC eglGetError_;
static PFNEGLDESTROYCONTEXTPROC eglDestroyContext_;
static PFNEGLTERMINATEPROC eglTerminate_;

// the frame handed from the emulator's thread to the GL thread
struct Gpu3DJob {
    unsigned gen = 0;
    int npolys = 0;
    std::vector<Polygon> polys = std::vector<Polygon>(2048);
    std::vector<Vertex> verts = std::vector<Vertex>(6144 * 2);
    std::vector<unsigned> vstamp = std::vector<unsigned>(6144 * 2);
    u32 dispcnt = 0;
    bool texChanged = false, palChanged = false;
    u64 texDirty[512 * 1024 / VRAMDirtyGranularity / 64] = {};
    u64 palDirty[128 * 1024 / VRAMDirtyGranularity / 64] = {};
    struct Meta {
        u32 NumPolygons, NumVariants, AlphaRef, DispCnt;
        u32 ToonTable[4 * 34];
        u32 ClearColor, ClearDepth, ClearAttr;
        u32 FogOffset, FogShift, FogColor;
        float ClearBitmapOffset[2];
    } meta{};
    Vertex* vbase = nullptr;  // GPU3D.VertexRAM (the emulator's), for the vertex pointers' indices
};

struct Gpu3DGL {
    struct SpanSetupY {
        s32 Z0, Z1, W0, W1;
        s32 ColorR0, ColorG0, ColorB0;
        s32 ColorR1, ColorG1, ColorB1;
        s32 TexcoordU0, TexcoordV0;
        s32 TexcoordU1, TexcoordV1;
        s32 I0, I1;
        s32 Linear;
        s32 IRecip;
        s32 W0n, W0d, W1d;
        s32 Increment;
        s32 X0, X1, Y0, Y1;
        s32 XMin, XMax;
        s32 DxInitial;
        s32 XCovIncr;
        u32 IsDummy;
    };
    struct SetupIndices { u16 PolyIdx, SpanIdxL, SpanIdxR, Y; };
    struct RenderPolygon {
        u32 FirstXSpan;
        s32 YTop, YBot;
        s32 XMin, XMax;
        s32 XMinY, XMaxY;
        u32 Variant;
        u32 Attr;
        float TextureLayer;
    };
    static constexpr int SpanSetupXSize = 4 * 24;  // (XSpanSetup: 24 words)
    static constexpr int CoarseTileCountX = 8;
    static constexpr int BinStride = 2048 / 32;
    static constexpr int CoarseBinStride = BinStride / 32;
    static constexpr int MaxVariants = 256;
    struct BinResultHeader {
        u32 VariantWorkCount[MaxVariants * 4];
        u32 SortedWorkOffset[MaxVariants];
        u32 SortWorkWorkCount[4];
    };
    static const int MaxYSpanSetups = 6144 * 2;

    int TileSize = 8, CoarseTileCountY = 4, CoarseTileArea = 32, CoarseTileW = 64, CoarseTileH = 32;
    int ClearCoarseBinMaskLocalSize = 64;
    int ScaleFactor = 4, ScreenWidth = 1024, ScreenHeight = 768, TilesPerLine = 128, TileLines = 96, MaxWorkTiles = 0;
    bool HiresCoordinates = false;

    std::vector<SetupIndices> YSpanIndices;
    std::vector<SpanSetupY> YSpanSetups = std::vector<SpanSetupY>(MaxYSpanSetups);
    RenderPolygon RenderPolygons[2048];

    GLuint ShaderInterpXSpans[2], ShaderBinCombined, ShaderDepthBlend[2];
    GLuint ShaderRasteriseNoTexture[2], ShaderRasteriseNoTextureToon[2], ShaderRasteriseNoTextureHighlight[2];
    GLuint ShaderRasteriseUseTextureDecal[2], ShaderRasteriseUseTextureModulate[2], ShaderRasteriseUseTextureToon[2];
    GLuint ShaderRasteriseUseTextureHighlight[2], ShaderRasteriseShadowMask[2];
    GLuint ShaderClearCoarseBinMask, ShaderClearIndirectWorkCount, ShaderCalculateWorkListOffset, ShaderSortWork;
    GLuint ShaderFinalPass[8];
    GLuint YSpanIndicesMemory, YSpanSetupMemory, XSpanSetupMemory, BinResultMemory, RenderPolygonMemory, WorkDescMemory;
    GLuint TileMemory[3], FinalTileMemory, OutMemory, MetaUniformMemory, DivTableMemory;
    GLuint Samplers[9];
    GLuint ClearBitmapTex[2];
    GLuint DummyCapture;
    std::vector<u32> ClearBitmap[2];
    u8 ClearBitmapDirty = 3;

    // the texture cache (melonDS's Texcache, its VRAM-dirty step split off onto the emulator's thread)
    struct TexArrayEntry { GLuint TextureID; u32 Layer; };
    struct TexCacheEntry {
        u32 LastVariant;
        u32 TextureRAMStart[2], TextureRAMSize[2];
        u32 TexPalStart, TexPalSize;
        u8 WidthLog2, HeightLog2;
        TexArrayEntry Texture;
        u64 TextureHash[2];
        u64 TexPalHash;
    };
    std::unordered_map<u64, TexCacheEntry> Cache;
    std::vector<TexArrayEntry> FreeTextures[8][8];
    std::vector<GLuint> TexArrays[8][8];
    std::vector<u32> DecodingBuffer = std::vector<u32>(1024 * 1024);
    unsigned long Decoded = 0;

    void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to);
    void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2]);
    void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2]);
};
}  // namespace bananads
