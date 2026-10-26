// render.gpu3d=gl: melonDS's classic OpenGL 3D renderer (src/GPU3D_OpenGL.cpp, GPL-3.0-or-later, the melonDS team)
// ported to GLES 3.1 as a Gpu3DBackend (gpu3d_host.h). The split: Prepare (the emulator's thread) does everything that
// reads the DS's state (the texture cache's VRAM checks and decodes, the clear bitmap, the polygon/vertex/index
// buffers, the shader constants), recording the texture cache's GL work as operations on virtual handles; GlRender
// (the GPU thread) replays those and draws exactly what melonDS's RenderFrame/RenderSceneChunk draw. Left out:
// display-capture textures at hi-res (captures stay in VRAM at 1x, as with Gengis), "better polygons" (off as in
// melonDS's default). The edge/fog passes draw through a second framebuffer (colour only), as GLES forbids sampling
// the depth/attribute textures attached to the framebuffer being drawn.
#include "gpu3d_host.h"
#include "gpu3d_gl.h"
#include "gl3d_shaders.h"

#include <string.h>
#include <string>
#include <vector>

#include "GPU.h"
#include "GPU3D_Texcache.h"
#include "Platform.h"

using namespace melonDS;

namespace bananads {

namespace {

GLuint Compile(const char* vs, const char* fs, const char* name, const char* defs,
               std::initializer_list<std::pair<const char*, int>> attrs) {
    auto sh = [&](GLenum type, const char* src) -> GLuint {
        std::string s = src;
        if (defs && *defs) {
            const size_t pos = s.find('\n') + 1;
            s = s.substr(0, pos) + defs + s.substr(pos);
        }
        const char* p = s.c_str();
        GLuint o = glCreateShader(type);
        glShaderSource(o, 1, &p, nullptr);
        glCompileShader(o);
        GLint ok = 0;
        glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = "";
            glGetShaderInfoLog(o, sizeof log, nullptr, log);
            Platform::Log(Platform::LogLevel::Error, "[gpu3d] %s %s shader: %s\n", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
            glDeleteShader(o);
            return 0;
        }
        return o;
    };
    GLuint v = sh(GL_VERTEX_SHADER, vs), f = sh(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (auto& a : attrs) glBindAttribLocation(p, a.second, a.first);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        Platform::Log(Platform::LogLevel::Error, "[gpu3d] %s link: %s\n", name, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void DefaultTexParams(GLuint tex) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

}  // namespace

class Gl3DBackend;

// the texture cache's GL work, recorded on the emulator's thread (virtual handles) and replayed on the GPU thread
struct RecLoader {
    Gl3DBackend* B;
    u32 GenerateTexture(u32 width, u32 height, u32 layers);
    void UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data);
    void DeleteTexture(u32 handle);
};

class Gl3DBackend : public Gpu3DBackend {
public:
    explicit Gl3DBackend(melonDS::GPU3D& g) : G3(g), GPU(g.GPU), TexCache(g.GPU, RecLoader{this}) {
        ClearBitmap[0].resize(256 * 256);
        ClearBitmap[1].resize(256 * 256);
    }
    const char* Name() const override { return "gl"; }
    bool GlInit(int scale) override;
    void GlShutdown() override;
    bool Prepare(melonDS::GPU3D& g3d) override;
    unsigned GlRender() override;
    void Reset() override {
        TexCache.Reset();
        ClearBitmapDirty = 3;
    }

    struct Op {
        int kind;  // 0 generate, 1 upload, 2 delete
        u32 id, w, h, l;
        size_t off;
    };
    std::vector<Op> Ops;
    std::vector<u32> Arena;
    u32 NextId = 1;
    std::vector<GLuint> Real;  // virtual handle -> GL name

private:
    struct RendererPolygon {
        u32 Attr;
        bool IsShadowMask, Translucent, IsShadow;
        u32 NumIndices, IndicesOffset;
        GLuint PrimType;
        u32 NumEdgeIndices, EdgeIndicesOffset;
        u32 RenderKey;
        u32 TexID;  // virtual
        u32 TexRepeat;
    };
    enum { RenderMode_Opaque = 0, RenderMode_Translucent, RenderMode_ShadowMask };

    void SetupPolygon(RendererPolygon* rp, Polygon* polygon) const;
    u32* SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32 texlayer, u32* vptr) const;
    void BuildPolygons(int npolys);
    void SetupPolygonTexture(const RendererPolygon* poly) const;
    int RenderSinglePolygon(int i) const;
    int RenderPolygonBatch(int i) const;
    void UseRenderShader(bool wbuffer);
    void RenderSceneChunk();

    melonDS::GPU3D& G3;
    melonDS::GPU& GPU;
    melonDS::Texcache<RecLoader, u32> TexCache;
    std::vector<Polygon*> SrcPolys;  // Prepare only

    // the frame, as Prepare left it
    RendererPolygon PolygonList[2048]{};
    int NumFinalPolys = 0, NumOpaqueFinalPolys = -1;
    bool TexEnable = false, WBufferFirst = false, HavePolys = false;
    u32 DispCnt = 0, ClearAttr1 = 0, ClearAttr2 = 0, FogColor = 0;
    std::vector<u32> ClearBitmap[2];
    u8 ClearBitmapDirty = 3, ClearBitmapUpload = 0;

    struct {
        float uScreenSize[2];
        u32 uDispCnt;
        u32 __pad0;
        float uToonColors[32][4];
        float uEdgeColors[8][4];
        float uFogColor[4];
        float uFogDensity[34][4];
        u32 uFogOffset;
        u32 uFogShift;
        u32 _pad1[2];
    } ShaderConfig{};

    u32 VertexBuffer[10240 * 7]{};
    u32 NumVertices = 0;
    u16 IndexBuffer[2048 * 40]{};
    u32 NumIndices = 0, NumEdgeIndices = 0;
    static constexpr u32 EdgeIndicesOffset = 2048 * 30;

    // GPU thread
    int ScaleFactor = 0, ScreenW = 0, ScreenH = 0;
    GLuint ClearShaderPlain = 0, ClearShaderBitmap = 0, RenderShader[2] = {}, FinalPassEdgeShader = 0, FinalPassFogShader = 0;
    GLint ClearUniformLoc[4]{}, ClearBitmapULoc[2]{}, RenderModeULoc = -1;
    int CurShaderID = -1;
    GLuint ShaderConfigUBO = 0, ClearVertexBufferID = 0, ClearVertexArrayID = 0, ClearBitmapTex[2]{};
    GLuint VertexBufferID = 0, VertexArrayID = 0, IndexBufferID = 0;
    GLuint ColorBufferTex = 0, DepthBufferTex = 0, AttrBufferTex = 0, MainFramebuffer = 0, FinalFramebuffer = 0;
};

u32 RecLoader::GenerateTexture(u32 width, u32 height, u32 layers) {
    const u32 id = B->NextId++;
    B->Ops.push_back({0, id, width, height, layers, 0});
    return id;
}
void RecLoader::UploadTexture(u32 handle, u32 width, u32 height, u32 layer, void* data) {
    const size_t off = B->Arena.size();
    B->Arena.resize(off + (size_t)width * height);
    memcpy(&B->Arena[off], data, (size_t)width * height * 4);
    B->Ops.push_back({1, handle, width, height, layer, off});
}
void RecLoader::DeleteTexture(u32 handle) { B->Ops.push_back({2, handle, 0, 0, 0, 0}); }

bool Gl3DBackend::GlInit(int scale) {
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glDepthRangef(0, 1);
    glClearDepthf(1.0);

    using namespace gl3dsh;
    if (!(ClearShaderPlain = Compile(kClearVS, kClearFS, "ClearShaderPlain", "", {{"vPosition", 0}}))) return false;
    ClearUniformLoc[0] = glGetUniformLocation(ClearShaderPlain, "uColor");
    ClearUniformLoc[1] = glGetUniformLocation(ClearShaderPlain, "uDepth");
    ClearUniformLoc[2] = glGetUniformLocation(ClearShaderPlain, "uOpaquePolyID");
    ClearUniformLoc[3] = glGetUniformLocation(ClearShaderPlain, "uFogFlag");
    if (!(ClearShaderBitmap = Compile(kClearBitmapVS, kClearBitmapFS, "ClearShaderBitmap", "", {{"vPosition", 0}}))) return false;
    ClearBitmapULoc[0] = glGetUniformLocation(ClearShaderBitmap, "uClearBitmapOffset");
    ClearBitmapULoc[1] = glGetUniformLocation(ClearShaderBitmap, "uOpaquePolyID");
    glUseProgram(ClearShaderBitmap);
    glUniform1i(glGetUniformLocation(ClearShaderBitmap, "ClearBitmapColor"), 0);
    glUniform1i(glGetUniformLocation(ClearShaderBitmap, "ClearBitmapDepth"), 1);

    for (int w = 0; w < 2; w++) {
        GLuint p = Compile(kRenderVS, kRenderFS, w ? "RenderShaderW" : "RenderShaderZ", w ? "#define WBuffer\n" : "",
                           {{"vPosition", 0}, {"vColor", 1}, {"vTexcoord", 2}, {"vPolygonAttr", 3}});
        if (!p) return false;
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "uConfig"), 0);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "CurTexture"), 0);
        RenderShader[w] = p;
    }
    if (!(FinalPassEdgeShader = Compile(kFinalPassVS, kFinalPassEdgeFS, "FinalPassEdgeShader", "", {{"vPosition", 0}}))) return false;
    if (!(FinalPassFogShader = Compile(kFinalPassVS, kFinalPassFogFS, "FinalPassFogShader", "", {{"vPosition", 0}}))) return false;
    for (GLuint p : {FinalPassEdgeShader, FinalPassFogShader}) {
        glUniformBlockBinding(p, glGetUniformBlockIndex(p, "uConfig"), 0);
        glUseProgram(p);
        glUniform1i(glGetUniformLocation(p, "DepthBuffer"), 0);
        glUniform1i(glGetUniformLocation(p, "AttrBuffer"), 1);
    }

    glGenBuffers(1, &ShaderConfigUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    static_assert((sizeof(ShaderConfig) & 15) == 0);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ShaderConfig), &ShaderConfig, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ShaderConfigUBO);

    const float clearvtx[6 * 2] = {-1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, -1.0, 1.0, -1.0, 1.0, 1.0};
    glGenBuffers(1, &ClearVertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(clearvtx), clearvtx, GL_STATIC_DRAW);
    glGenVertexArrays(1, &ClearVertexArrayID);
    glBindVertexArray(ClearVertexArrayID);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    glGenTextures(2, ClearBitmapTex);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (i == 0) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 256, 256, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, nullptr);
        else glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 256, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);
    }

    glGenBuffers(1, &VertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VertexBuffer), nullptr, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &VertexArrayID);
    glBindVertexArray(VertexArrayID);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_SHORT, 7 * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, 7 * 4, (void*)(2 * 4));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 2, GL_SHORT, 7 * 4, (void*)(3 * 4));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 3, GL_UNSIGNED_INT, 7 * 4, (void*)(4 * 4));
    glGenBuffers(1, &IndexBufferID);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBufferID);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(IndexBuffer), nullptr, GL_DYNAMIC_DRAW);

    // render targets at the scale
    ScaleFactor = scale;
    ScreenW = 256 * scale;
    ScreenH = 192 * scale;
    glGenFramebuffers(1, &MainFramebuffer);
    glGenFramebuffers(1, &FinalFramebuffer);
    glGenTextures(1, &ColorBufferTex);
    DefaultTexParams(ColorBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenTextures(1, &DepthBufferTex);
    DefaultTexParams(DepthBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, ScreenW, ScreenH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glGenTextures(1, &AttrBufferTex);
    DefaultTexParams(AttrBufferTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const GLenum fbassign[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glBindFramebuffer(GL_FRAMEBUFFER, MainFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ColorBufferTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, DepthBufferTex, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, AttrBufferTex, 0);
    glDrawBuffers(2, fbassign);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Platform::Log(Platform::LogLevel::Error, "[gpu3d] main framebuffer incomplete\n");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, FinalFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ColorBufferTex, 0);
    glDrawBuffers(1, fbassign);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    return true;
}

void Gl3DBackend::GlShutdown() {
    for (GLuint t : Real)
        if (t) glDeleteTextures(1, &t);
    Real.clear();
    glDeleteFramebuffers(1, &MainFramebuffer);
    glDeleteFramebuffers(1, &FinalFramebuffer);
    glDeleteTextures(1, &ColorBufferTex);
    glDeleteTextures(1, &DepthBufferTex);
    glDeleteTextures(1, &AttrBufferTex);
    glDeleteVertexArrays(1, &VertexArrayID);
    glDeleteBuffers(1, &VertexBufferID);
    glDeleteBuffers(1, &IndexBufferID);
    glDeleteVertexArrays(1, &ClearVertexArrayID);
    glDeleteBuffers(1, &ClearVertexBufferID);
    glDeleteTextures(2, ClearBitmapTex);
    glDeleteBuffers(1, &ShaderConfigUBO);
    for (GLuint p : {ClearShaderPlain, ClearShaderBitmap, RenderShader[0], RenderShader[1], FinalPassEdgeShader, FinalPassFogShader})
        if (p) glDeleteProgram(p);
}

void Gl3DBackend::UseRenderShader(bool wbuffer) {
    const int flags = (int)wbuffer;
    if (CurShaderID == flags) return;
    glUseProgram(RenderShader[flags]);
    CurShaderID = flags;
    RenderModeULoc = glGetUniformLocation(RenderShader[flags], "uRenderMode");
}

void Gl3DBackend::SetupPolygon(RendererPolygon* rp, Polygon* polygon) const {
    rp->Attr = polygon->Attr;
    rp->IsShadowMask = polygon->IsShadowMask;
    rp->Translucent = polygon->Translucent;
    rp->IsShadow = polygon->IsShadow;
    rp->RenderKey = (polygon->Attr >> 14) & 0x1;
    if (!polygon->IsShadowMask) {
        if (polygon->Translucent) {
            if (polygon->IsShadow) rp->RenderKey |= 0x20000;
            else rp->RenderKey |= 0x10000;
            rp->RenderKey |= (polygon->Attr >> 10) & 0x2;
            rp->RenderKey |= (polygon->Attr >> 13) & 0x4;
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16;
            if ((polygon->Attr & 0x001F0000) == 0x001F0000) rp->RenderKey |= 0x4000;
        } else {
            if ((polygon->Attr & 0x001F0000) == 0) rp->RenderKey |= 0x2;
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16;
        }
    } else {
        rp->RenderKey |= 0x30000;
    }
    const u32 textype = (polygon->TexParam >> 26) & 0x7;
    const u32 texattr = (polygon->TexParam >> 16) & 0x3FF;
    if (TexEnable && textype != 0) rp->RenderKey |= (0x80000 | (texattr << 20));
}

u32* Gl3DBackend::SetupVertex(const Polygon* poly, int vid, const Vertex* vtx, u32 vtxattr, u32 texlayer, u32* vptr) const {
    u32 z = poly->FinalZ[vid];
    const u32 w = poly->FinalW[vid];
    const u32 alpha = (poly->Attr >> 16) & 0x1F;
    u32 zshift = 0;
    while (z > 0xFFFF) { z >>= 1; zshift++; }
    const u32 x = (vtx->HiresPosition[0] * ScaleFactor) >> 4;
    const u32 y = (vtx->HiresPosition[1] * ScaleFactor) >> 4;
    *vptr++ = x | (y << 16);
    *vptr++ = z | (w << 16);
    *vptr++ = (vtx->FinalColor[0] >> 1) | ((vtx->FinalColor[1] >> 1) << 8) | ((vtx->FinalColor[2] >> 1) << 16) | (alpha << 24);
    *vptr++ = (u16)vtx->TexCoords[0] | ((u16)vtx->TexCoords[1] << 16);
    *vptr++ = vtxattr | (zshift << 16);
    *vptr++ = texlayer;
    *vptr++ = TextureWidth(poly->TexParam) | (TextureHeight(poly->TexParam) << 16);
    return vptr;
}

void Gl3DBackend::BuildPolygons(int npolys) {
    u32* vptr = &VertexBuffer[0];
    u32 vidx = 0, iidx = 0, eidx = EdgeIndicesOffset;
    u32 curtexparam = 0, curtexpal = 0, curtexid = 0, curtexlayer = (u32)-1;
    for (int i = 0; i < npolys; i++) {
        RendererPolygon* rp = &PolygonList[i];
        Polygon* poly = SrcPolys[i];
        rp->IndicesOffset = iidx;
        rp->NumIndices = 0;
        const u32 vidx_first = vidx;
        const u32 polyattr = poly->Attr;
        const u32 texparam = poly->TexParam & ~0xC00F0000;
        const u32 texpal = poly->TexPalette;
        u32 vtxattr = polyattr & 0x1F00C8F0;
        if (poly->FacingView) vtxattr |= (1 << 8);
        if (poly->WBuffer) vtxattr |= (1 << 9);
        if (texparam != curtexparam || texpal != curtexpal) {
            const u32 textype = (texparam >> 26) & 0x7;
            if (TexEnable && textype != 0) {
                u32* halp;
                TexCache.GetTexture(texparam, texpal, curtexid, curtexlayer, halp);
                curtexlayer |= 0xFFFF0000;
            } else {
                curtexid = 0;
                curtexlayer = (u32)-1;
            }
            curtexparam = texparam;
            curtexpal = texpal;
        }
        rp->TexID = curtexid;
        rp->TexRepeat = (poly->TexParam >> 16) & 0xF;
        if (poly->Type == 1) {  // line
            rp->PrimType = GL_LINES;
            u32 lastx = 0, lasty = 0;
            int nout = 0;
            for (u32 j = 0; j < poly->NumVertices; j++) {
                Vertex* vtx = poly->Vertices[j];
                if (j > 0 && lastx == vtx->FinalPosition[0] && lasty == vtx->FinalPosition[1]) continue;
                lastx = vtx->FinalPosition[0];
                lasty = vtx->FinalPosition[1];
                vptr = SetupVertex(poly, j, vtx, vtxattr, curtexlayer, vptr);
                IndexBuffer[iidx++] = vidx;
                rp->NumIndices++;
                vidx++;
                if (++nout >= 2) break;
            }
        } else {
            rp->PrimType = GL_TRIANGLES;
            for (u32 j = 0; j < poly->NumVertices; j++) {
                Vertex* vtx = poly->Vertices[j];
                vptr = SetupVertex(poly, j, vtx, vtxattr, curtexlayer, vptr);
                if (j >= 2) {
                    IndexBuffer[iidx++] = vidx_first;
                    IndexBuffer[iidx++] = vidx - 1;
                    IndexBuffer[iidx++] = vidx;
                    rp->NumIndices += 3;
                }
                vidx++;
            }
        }
        rp->EdgeIndicesOffset = eidx;
        rp->NumEdgeIndices = 0;
        u32 vidx_cur = vidx_first;
        for (u32 j = 1; j < poly->NumVertices; j++) {
            IndexBuffer[eidx++] = vidx_cur;
            IndexBuffer[eidx++] = vidx_cur + 1;
            vidx_cur++;
            rp->NumEdgeIndices += 2;
        }
        IndexBuffer[eidx++] = vidx_cur;
        IndexBuffer[eidx++] = vidx_first;
        rp->NumEdgeIndices += 2;
    }
    NumVertices = vidx;
    NumIndices = iidx;
    NumEdgeIndices = eidx - EdgeIndicesOffset;
}

bool Gl3DBackend::Prepare(melonDS::GPU3D& g3) {
    u8 clrBitmapDirty;
    if (!TexCache.Update(clrBitmapDirty) && g3.RenderFrameIdentical) return false;
    ClearBitmapDirty |= clrBitmapDirty;
    ClearBitmapUpload = 0;
    DispCnt = g3.RenderDispCnt;
    if (DispCnt & (1 << 14)) {
        if (ClearBitmapDirty & 1) {
            const u16* vram = (const u16*)&GPU.VRAMFlat_Texture[0x40000];
            for (int i = 0; i < 256 * 256; i++) {
                const u16 color = vram[i];
                u32 r = (color << 1) & 0x3E; if (r) r++;
                u32 g = (color >> 4) & 0x3E; if (g) g++;
                u32 b = (color >> 9) & 0x3E; if (b) b++;
                const u32 a = (color & 0x8000) ? 31 : 0;
                ClearBitmap[0][i] = r | (g << 8) | (b << 16) | (a << 24);
            }
        }
        if (ClearBitmapDirty & 2) {
            const u16* vram = (const u16*)&GPU.VRAMFlat_Texture[0x60000];
            for (int i = 0; i < 256 * 256; i++) {
                const u16 val = vram[i];
                ClearBitmap[1][i] = (((val & 0x7FFF) * 0x200) + 0x1FF) | ((val & 0x8000) << 9);
            }
        }
        ClearBitmapUpload = ClearBitmapDirty;
        ClearBitmapDirty = 0;
    }
    TexEnable = !!(DispCnt & 1);
    ClearAttr1 = g3.RenderClearAttr1;
    ClearAttr2 = g3.RenderClearAttr2;
    FogColor = g3.RenderFogColor;

    ShaderConfig.uScreenSize[0] = 256 * ScaleFactor;
    ShaderConfig.uScreenSize[1] = 192 * ScaleFactor;
    ShaderConfig.uDispCnt = DispCnt;
    for (int i = 0; i < 32; i++) {
        const u16 c = g3.RenderToonTable[i];
        ShaderConfig.uToonColors[i][0] = (float)(c & 0x1F) / 31.0;
        ShaderConfig.uToonColors[i][1] = (float)((c >> 5) & 0x1F) / 31.0;
        ShaderConfig.uToonColors[i][2] = (float)((c >> 10) & 0x1F) / 31.0;
    }
    for (int i = 0; i < 8; i++) {
        const u16 c = g3.RenderEdgeTable[i];
        ShaderConfig.uEdgeColors[i][0] = (float)(c & 0x1F) / 31.0;
        ShaderConfig.uEdgeColors[i][1] = (float)((c >> 5) & 0x1F) / 31.0;
        ShaderConfig.uEdgeColors[i][2] = (float)((c >> 10) & 0x1F) / 31.0;
    }
    {
        const u32 c = g3.RenderFogColor;
        ShaderConfig.uFogColor[0] = (float)(c & 0x1F) / 31.0;
        ShaderConfig.uFogColor[1] = (float)((c >> 5) & 0x1F) / 31.0;
        ShaderConfig.uFogColor[2] = (float)((c >> 10) & 0x1F) / 31.0;
        ShaderConfig.uFogColor[3] = (float)((c >> 16) & 0x1F) / 31.0;
    }
    for (int i = 0; i < 34; i++) ShaderConfig.uFogDensity[i][0] = (float)g3.RenderFogDensityTable[i] / 127.0;
    ShaderConfig.uFogOffset = g3.RenderFogOffset;
    ShaderConfig.uFogShift = g3.RenderFogShift;

    HavePolys = g3.RenderNumPolygons != 0;
    NumFinalPolys = 0;
    NumOpaqueFinalPolys = -1;
    if (HavePolys) {
        WBufferFirst = g3.RenderPolygonRAM[0]->WBuffer;
        SrcPolys.clear();
        int npolys = 0, firsttrans = -1;
        for (u32 i = 0; i < g3.RenderNumPolygons; i++) {
            Polygon* p = g3.RenderPolygonRAM[i];
            if (p->Degenerate) continue;
            SetupPolygon(&PolygonList[npolys], p);
            SrcPolys.push_back(p);
            if (firsttrans < 0 && p->Translucent) firsttrans = npolys;
            npolys++;
        }
        NumFinalPolys = npolys;
        NumOpaqueFinalPolys = firsttrans;
        BuildPolygons(npolys);
    }
    return true;
}

void Gl3DBackend::SetupPolygonTexture(const RendererPolygon* poly) const {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, poly->TexID < Real.size() ? Real[poly->TexID] : 0);
    GLint repeatS, repeatT;
    if (poly->TexRepeat & 1) repeatS = (poly->TexRepeat & 4) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    else repeatS = GL_CLAMP_TO_EDGE;
    if (poly->TexRepeat & 2) repeatT = (poly->TexRepeat & 8) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    else repeatT = GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, repeatS);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, repeatT);
}

int Gl3DBackend::RenderSinglePolygon(int i) const {
    const RendererPolygon* rp = &PolygonList[i];
    SetupPolygonTexture(rp);
    glDrawElements(rp->PrimType, rp->NumIndices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->IndicesOffset * 2));
    return 1;
}

int Gl3DBackend::RenderPolygonBatch(int i) const {
    const RendererPolygon* rp = &PolygonList[i];
    int numpolys = 0;
    u32 numindices = 0;
    for (int iend = i; iend < NumFinalPolys; iend++) {
        const RendererPolygon* c = &PolygonList[iend];
        if (c->PrimType != rp->PrimType || c->RenderKey != rp->RenderKey || c->TexID != rp->TexID || c->TexRepeat != rp->TexRepeat) break;
        numpolys++;
        numindices += c->NumIndices;
    }
    SetupPolygonTexture(rp);
    glDrawElements(rp->PrimType, numindices, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(rp->IndicesOffset * 2));
    return numpolys;
}

unsigned Gl3DBackend::GlRender() {
    // the texture cache's recorded work
    for (const Op& o : Ops) {
        if (o.id >= Real.size()) Real.resize(o.id + 64, 0);
        if (o.kind == 0) {
            GLuint t;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D_ARRAY, t);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8UI, o.w, o.h, o.l);
            Real[o.id] = t;
        } else if (o.kind == 1) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, Real[o.id]);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, o.l, o.w, o.h, 1, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, &Arena[o.off]);
        } else {
            if (Real[o.id]) glDeleteTextures(1, &Real[o.id]);
            Real[o.id] = 0;
        }
    }
    Ops.clear();
    Arena.clear();
    if (ClearBitmapUpload & 1) {
        glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, ClearBitmap[0].data());
    }
    if (ClearBitmapUpload & 2) {
        glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, ClearBitmap[1].data());
    }

    CurShaderID = -1;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, MainFramebuffer);
    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ShaderConfig), &ShaderConfig);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ShaderConfigUBO);

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glViewport(0, 0, ScreenW, ScreenH);
    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glDepthFunc(GL_ALWAYS);
    glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
    glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

    if (DispCnt & (1 << 14)) {
        glUseProgram(ClearShaderBitmap);
        const u32 polyid = (ClearAttr1 >> 24) & 0x3F;
        const u8 xoff = (ClearAttr2 >> 16) & 0xFF, yoff = (ClearAttr2 >> 24) & 0xFF;
        glUniform2f(ClearBitmapULoc[0], (float)xoff / 256.0f, (float)yoff / 256.0f);
        glUniform1ui(ClearBitmapULoc[1], polyid);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
    } else {
        glUseProgram(ClearShaderPlain);
        const u32 r = ClearAttr1 & 0x1F, g = (ClearAttr1 >> 5) & 0x1F, b = (ClearAttr1 >> 10) & 0x1F;
        const u32 fog = (ClearAttr1 >> 15) & 1, a = (ClearAttr1 >> 16) & 0x1F, polyid = (ClearAttr1 >> 24) & 0x3F;
        const u32 z = ((ClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        glUniform4ui(ClearUniformLoc[0], r, g, b, a);
        glUniform1ui(ClearUniformLoc[1], z);
        glUniform1ui(ClearUniformLoc[2], polyid);
        glUniform1ui(ClearUniformLoc[3], fog);
    }
    glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
    glBindVertexArray(ClearVertexArrayID);
    glDrawArrays(GL_TRIANGLES, 0, 2 * 3);

    if (HavePolys) {
        glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices * 7 * 4, VertexBuffer);
        glBindVertexArray(VertexArrayID);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, IndexBufferID);
        if (NumIndices) glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, NumIndices * 2, IndexBuffer);
        if (NumEdgeIndices)
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, EdgeIndicesOffset * 2, NumEdgeIndices * 2, IndexBuffer + EdgeIndicesOffset);
        RenderSceneChunk();
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    return ColorBufferTex;
}

void Gl3DBackend::RenderSceneChunk() {
    UseRenderShader(WBufferFirst);
    const GLboolean fogenable = (DispCnt & (1 << 7)) ? GL_TRUE : GL_FALSE;

    // pass 1: opaque pixels
    glUniform1i(RenderModeULoc, RenderMode_Opaque);
    glLineWidth(1.0);
    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glBindVertexArray(VertexArrayID);
    glActiveTexture(GL_TEXTURE0);
    for (int i = 0; i < NumFinalPolys;) {
        RendererPolygon* rp = &PolygonList[i];
        if (rp->IsShadowMask || rp->Translucent) { i++; continue; }
        glDepthFunc((rp->Attr & (1 << 14)) ? GL_LEQUAL : GL_LESS);
        const u32 polyid = (rp->Attr >> 24) & 0x3F;
        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glStencilMask(0xFF);
        i += RenderPolygonBatch(i);
    }

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    if (DispCnt & (1 << 3)) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    else glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);
    glLineWidth(1.0);

    if (NumOpaqueFinalPolys > -1) {
        // pass 2: translucent pixels against the background, blending off when its alpha is zero
        if ((ClearAttr1 & 0x001F0000) == 0) {
            glDisable(GL_BLEND);
            for (int i = 0; i < NumFinalPolys;) {
                RendererPolygon* rp = &PolygonList[i];
                if (rp->IsShadowMask) {
                    glUniform1i(RenderModeULoc, RenderMode_ShadowMask);
                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glDepthFunc(GL_LESS);
                    glStencilFunc(GL_EQUAL, 0xFF, 0xFF);
                    glStencilOp(GL_KEEP, GL_INVERT, GL_KEEP);
                    glStencilMask(0x01);
                    i += RenderPolygonBatch(i);
                } else if (rp->Translucent) {
                    const bool needopaque = (rp->Attr & 0x001F0000) == 0x001F0000;
                    const u32 polyattr = rp->Attr, polyid = (polyattr >> 24) & 0x3F;
                    glDepthFunc((polyattr & (1 << 14)) ? GL_LEQUAL : GL_LESS);
                    if (needopaque) {
                        glUniform1i(RenderModeULoc, RenderMode_Opaque);
                        glDisable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);
                        glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                        glStencilMask(0xFF);
                        glDepthMask(GL_TRUE);
                        RenderSinglePolygon(i);
                    }
                    glUniform1i(RenderModeULoc, RenderMode_Translucent);
                    const GLboolean transfog = !(polyattr & (1 << 15)) ? fogenable : GL_FALSE;
                    if (rp->IsShadow) {
                        const u32 clrpolyid = (ClearAttr1 >> 24) & 0x3F;
                        if (polyid != clrpolyid) { i++; continue; }
                        glEnable(GL_BLEND);
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);
                        glStencilFunc(GL_EQUAL, 0xFE, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40 | polyid));
                        glDepthMask((polyattr & (1 << 11)) ? GL_TRUE : GL_FALSE);
                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    } else {
                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);
                        glStencilFunc(GL_EQUAL, 0xFF, 0xFE);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                        glStencilMask(~(0x40 | polyid));
                        glDepthMask((polyattr & (1 << 11)) ? GL_TRUE : GL_FALSE);
                        i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                    }
                } else
                    i++;
            }
            glEnable(GL_BLEND);
            glStencilMask(0xFF);
        }

        // pass 3: translucent pixels
        for (int i = 0; i < NumFinalPolys;) {
            RendererPolygon* rp = &PolygonList[i];
            if (rp->IsShadowMask) {
                glStencilMask(0x80);
                glClear(GL_STENCIL_BUFFER_BIT);
                glUniform1i(RenderModeULoc, RenderMode_ShadowMask);
                glDisable(GL_BLEND);
                glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                glDepthMask(GL_FALSE);
                glDepthFunc(GL_LESS);
                glStencilFunc(GL_ALWAYS, 0x80, 0x80);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
                i += RenderPolygonBatch(i);
            } else if (rp->Translucent) {
                const bool needopaque = (rp->Attr & 0x001F0000) == 0x001F0000;
                const u32 polyattr = rp->Attr, polyid = (polyattr >> 24) & 0x3F;
                glDepthFunc((polyattr & (1 << 14)) ? GL_LEQUAL : GL_LESS);
                if (needopaque) {
                    glUniform1i(RenderModeULoc, RenderMode_Opaque);
                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_TRUE, GL_TRUE, fogenable, GL_FALSE);
                    glStencilFunc(GL_ALWAYS, polyid, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0xFF);
                    glDepthMask(GL_TRUE);
                    RenderSinglePolygon(i);
                }
                glUniform1i(RenderModeULoc, RenderMode_Translucent);
                const GLboolean transfog = !(polyattr & (1 << 15)) ? fogenable : GL_FALSE;
                if (rp->IsShadow) {
                    glDisable(GL_BLEND);
                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glStencilFunc(GL_EQUAL, polyid, 0x3F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
                    glStencilMask(0x80);
                    RenderSinglePolygon(i);
                    glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);
                    glStencilFunc(GL_EQUAL, 0xC0 | polyid, 0x80);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);
                    glDepthMask((polyattr & (1 << 11)) ? GL_TRUE : GL_FALSE);
                    i += RenderSinglePolygon(i);
                } else {
                    glEnable(GL_BLEND);
                    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, transfog, GL_FALSE);
                    glStencilFunc(GL_NOTEQUAL, 0x40 | polyid, 0x7F);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    glStencilMask(0x7F);
                    glDepthMask((polyattr & (1 << 11)) ? GL_TRUE : GL_FALSE);
                    i += needopaque ? RenderSinglePolygon(i) : RenderPolygonBatch(i);
                }
            } else
                i++;
        }
    }

    if (DispCnt & 0x00A0) {  // fog/edge marking: through the colour-only framebuffer
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FinalFramebuffer);
        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDepthMask(GL_FALSE);
        glStencilMask(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, DepthBufferTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, AttrBufferTex);
        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);
        if (DispCnt & (1 << 5)) {
            glUseProgram(FinalPassEdgeShader);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            glDrawArrays(GL_TRIANGLES, 0, 2 * 3);
        }
        if (DispCnt & (1 << 7)) {
            glUseProgram(FinalPassFogShader);
            if (DispCnt & (1 << 6)) glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            else glBlendFuncSeparate(GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_SRC_ALPHA);
            const u32 c = FogColor;
            glBlendColor((float)(c & 0x1F) / 31.0f, (float)((c >> 5) & 0x1F) / 31.0f, (float)((c >> 10) & 0x1F) / 31.0f,
                         (float)((c >> 16) & 0x1F) / 31.0f);
            glDrawArrays(GL_TRIANGLES, 0, 2 * 3);
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

std::unique_ptr<Gpu3DBackend> MakeGl3DBackend(melonDS::GPU3D& g) { return std::make_unique<Gl3DBackend>(g); }

}  // namespace bananads
