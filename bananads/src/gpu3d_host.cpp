// render.gpu3d: the GPU 3D renderer's host (gpu3d_host.h): its thread and EGL context, the frame hand-off, the
// pack pass and the read-back into the 2D engine's hi-res rows.
#define GPU3D_GL_DEFINE
#include "gpu3d_host.h"
#include "gpu3d_gl.h"
#include "gl3d_shaders.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "Platform.h"

using namespace melonDS;

namespace bananads {

int GpuRenderer3D::Stats = 0;

__attribute__((weak)) std::unique_ptr<Gpu3DBackend> MakeCompute3DBackend(melonDS::GPU3D&) { return nullptr; }

namespace {
uint64_t NowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

struct Egl {
    void* lib = nullptr;
    PFNEGLGETPROCADDRESSPROC GetProc = nullptr;
    PFNEGLGETDISPLAYPROC GetDisplay = nullptr;
    PFNEGLINITIALIZEPROC Initialize = nullptr;
    PFNEGLBINDAPIPROC BindAPI = nullptr;
    PFNEGLCHOOSECONFIGPROC ChooseConfig = nullptr;
    PFNEGLCREATECONTEXTPROC CreateContext = nullptr;
    PFNEGLMAKECURRENTPROC MakeCurrent = nullptr;
    PFNEGLDESTROYCONTEXTPROC DestroyContext = nullptr;
    PFNEGLQUERYSTRINGPROC QueryString = nullptr;
    PFNEGLGETERRORPROC GetError = nullptr;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
};
Egl E;
}  // namespace

// the GL thread: a surfaceless GLES 3.1 context of its own (the presenter's shader context, if any, is separate)
bool GpuRenderer3D::GlSetup() {
    E.lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!E.lib) { Platform::Log(Platform::LogLevel::Error, "[gpu3d] no libEGL.so.1\n"); return false; }
    E.GetProc = (PFNEGLGETPROCADDRESSPROC)dlsym(E.lib, "eglGetProcAddress");
#define EL(f, n) E.f = (decltype(E.f))dlsym(E.lib, n); if (!E.f) { Platform::Log(Platform::LogLevel::Error, "[gpu3d] missing %s\n", n); return false; }
    EL(GetDisplay, "eglGetDisplay") EL(Initialize, "eglInitialize") EL(BindAPI, "eglBindAPI") EL(ChooseConfig, "eglChooseConfig")
    EL(CreateContext, "eglCreateContext") EL(MakeCurrent, "eglMakeCurrent") EL(DestroyContext, "eglDestroyContext")
    EL(QueryString, "eglQueryString") EL(GetError, "eglGetError")
#undef EL
    auto getPlat = (PFNEGLGETPLATFORMDISPLAYEXTPROC)E.GetProc("eglGetPlatformDisplayEXT");
    EGLint maj = 0, min = 0;
    E.dpy = getPlat ? getPlat(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
    if (E.dpy == EGL_NO_DISPLAY || !E.Initialize(E.dpy, &maj, &min)) {
        E.dpy = E.GetDisplay(EGL_DEFAULT_DISPLAY);
        if (E.dpy == EGL_NO_DISPLAY || !E.Initialize(E.dpy, &maj, &min)) {
            Platform::Log(Platform::LogLevel::Error, "[gpu3d] EGL init failed (0x%x)\n", E.GetError());
            return false;
        }
    }
    E.BindAPI(EGL_OPENGL_ES_API);
    EGLConfig cfg = nullptr;
    const char* ext = E.QueryString(E.dpy, EGL_EXTENSIONS);
    if (!ext || !strstr(ext, "EGL_KHR_no_config_context")) {
        const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
        EGLint n = 0;
        if (!E.ChooseConfig(E.dpy, want, &cfg, 1, &n) || n < 1) { Platform::Log(Platform::LogLevel::Error, "[gpu3d] no EGL config\n"); return false; }
    }
    const EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE};
    E.ctx = E.CreateContext(E.dpy, cfg, EGL_NO_CONTEXT, ca);
    if (E.ctx == EGL_NO_CONTEXT || !E.MakeCurrent(E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, E.ctx)) {
        Platform::Log(Platform::LogLevel::Error, "[gpu3d] no surfaceless GLES 3.1 context (0x%x)\n", E.GetError());
        return false;
    }
    if (!gpu3d_gl_load([](const char* n) { return (void*)E.GetProc(n); })) return false;
    Platform::Log(Platform::LogLevel::Info, "[gpu3d] %s on %s (%s), EGL %d.%d\n", Be->Name(),
                  (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION), maj, min);

    if (!Be->GlInit(Scale_)) { Platform::Log(Platform::LogLevel::Error, "[gpu3d] backend %s: init failed\n", Be->Name()); return false; }

    // the pack pass
    auto sh = [](GLenum t, const char* src) {
        GLuint o = glCreateShader(t);
        glShaderSource(o, 1, &src, nullptr);
        glCompileShader(o);
        GLint ok = 0;
        glGetShaderiv(o, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = "";
            glGetShaderInfoLog(o, sizeof log, nullptr, log);
            Platform::Log(Platform::LogLevel::Error, "[gpu3d] pack shader: %s\n", log);
        }
        return o;
    };
    PackProg = glCreateProgram();
    glAttachShader(PackProg, sh(GL_VERTEX_SHADER, gl3dsh::kPackVS));
    glAttachShader(PackProg, sh(GL_FRAGMENT_SHADER, gl3dsh::kPackFS));
    glLinkProgram(PackProg);
    GLint ok = 0;
    glGetProgramiv(PackProg, GL_LINK_STATUS, &ok);
    if (!ok) { Platform::Log(Platform::LogLevel::Error, "[gpu3d] pack program failed\n"); return false; }
    glUseProgram(PackProg);
    glUniform1i(glGetUniformLocation(PackProg, "uTex"), 0);
    glUniform1i(glGetUniformLocation(PackProg, "uScale"), Scale_);
    glGenVertexArrays(1, &Vao);
    glGenTextures(1, &PackTex);
    glBindTexture(GL_TEXTURE_2D, PackTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256 * Scale_, 192 * Scale_);
    glGenFramebuffers(1, &PackFb);
    glBindFramebuffer(GL_FRAMEBUFFER, PackFb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, PackTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        Platform::Log(Platform::LogLevel::Error, "[gpu3d] pack framebuffer incomplete\n");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR || true;
}

GpuRenderer3D::GpuRenderer3D(melonDS::GPU3D& gpu3D, std::unique_ptr<Gpu3DBackend> be, int scale)
    : Renderer3D(gpu3D), Be(std::move(be)), Scale_(scale) {
    for (auto& d : Done) d.store(0);
    const size_t words = (size_t)256 * 192 * scale * scale;
    for (auto& b : OutBuf) {
        b = (u32*)aligned_alloc(64, words * 4);
        memset(b, 0, words * 4);
    }
    Out = Target = OutBuf[0];
    if (!Be) return;
    T = std::thread([this] { Thread(); });
    std::unique_lock<std::mutex> l(M);
    DoneCv.wait(l, [this] { return Inited; });
}

GpuRenderer3D::~GpuRenderer3D() {
    if (T.joinable()) {
        {
            std::lock_guard<std::mutex> l(M);
            Quit = true;
        }
        Cv.notify_all();
        T.join();
    }
    for (auto& b : OutBuf) free(b);
}

void GpuRenderer3D::Thread() {
    pthread_setname_np(pthread_self(), "bds-gpu3d");
    const bool ok = GlSetup();
    {
        std::lock_guard<std::mutex> l(M);
        Ok_ = ok;
        Inited = true;
    }
    DoneCv.notify_all();
    if (!ok) return;
    const int W = 256 * Scale_, H = 192 * Scale_, CH = H / NCHUNK;
    unsigned seen = 0;
    for (;;) {
        unsigned gen;
        {
            std::unique_lock<std::mutex> l(M);
            Cv.wait(l, [&] { return Quit || Job != seen; });
            if (Quit) break;
            gen = seen = Job;
        }
        const uint64_t t0 = NowNs();
        const GLuint tex = Be->GlRender();
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, PackFb);
        glViewport(0, 0, W, H);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glUseProgram(PackProg);
        glBindVertexArray(Vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        const uint64_t t1 = NowNs();
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(fence);
        const uint64_t t2 = NowNs();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, PackFb);
        u32* dst = Target;
        for (int c = 0; c < NCHUNK; c++) {
            glReadPixels(0, c * CH, W, CH, GL_RGBA, GL_UNSIGNED_BYTE, dst + (size_t)c * CH * W);
            Done[c].store(gen, std::memory_order_release);
            {
                std::lock_guard<std::mutex> l(M);
            }
            DoneCv.notify_all();
        }
        const uint64_t t3 = NowNs();
        StGlNs += t1 - t0;
        StWaitNs += t2 - t1;
        StReadNs += t3 - t2;
        {
            std::lock_guard<std::mutex> l(M);
            Finished.store(gen, std::memory_order_release);
        }
        DoneCv.notify_all();
    }
    Be->GlShutdown();
    E.MakeCurrent(E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    E.DestroyContext(E.dpy, E.ctx);
}

void GpuRenderer3D::WaitIdle() {
    if (Finished.load(std::memory_order_acquire) == Gen) return;
    std::unique_lock<std::mutex> l(M);
    DoneCv.wait(l, [this] { return Finished.load(std::memory_order_acquire) == Gen; });
}

void GpuRenderer3D::Reset() {
    WaitIdle();
    Be->Reset();
}

void GpuRenderer3D::FinishRendering() { WaitIdle(); }

void GpuRenderer3D::RenderFrame() {
    const uint64_t t0 = NowNs();
    WaitIdle();  // one frame in flight: its inputs and read-back are done before the next Prepare
    const uint64_t t1 = NowNs();
    if (!Be->Prepare(GPU3D)) return;  // (identical frame: the rows stay the last render's)
    const uint64_t t2 = NowNs();
    Out = Out == OutBuf[0] ? OutBuf[1] : OutBuf[0];
    Target = Out;
    {
        std::lock_guard<std::mutex> l(M);
        Job = ++Gen;
    }
    Cv.notify_one();
    StEmuWaitNs += t1 - t0;
    StPrepNs += t2 - t1;
    if (Stats && ++StN % Stats == 0) {
        const double n = Stats * 1e6;
        Platform::Log(Platform::LogLevel::Info,
                      "[gpu3d] %u frames: emu wait %.3f ms, prepare %.3f ms (emu thread); gl submit %.3f ms, gpu wait %.3f ms, "
                      "read-back %.3f ms (gpu3d thread) per frame\n",
                      Stats, StEmuWaitNs / n, StPrepNs / n, StGlNs / n, StWaitNs / n, StReadNs / n);
        StEmuWaitNs = StPrepNs = StGlNs = StWaitNs = StReadNs = 0;
    }
}

void GpuRenderer3D::WaitChunk(int c, unsigned gen) {
    auto drawn = [&] { return (int)(Done[c].load(std::memory_order_acquire) - gen) >= 0; };
    if (drawn()) return;
    std::unique_lock<std::mutex> l(M);
    while (!drawn()) DoneCv.wait_for(l, std::chrono::milliseconds(1));
}

void GpuRenderer3D::WaitRows(int line, unsigned gen) {
    if (line >= 192) line = 191;
    WaitChunk(line * NCHUNK / 192, gen);
}

u32* GpuRenderer3D::GetLine(int line) { return const_cast<u32*>(GetQuarter(line, 0)); }

const u32* GpuRenderer3D::GetQuarter(int line, int q) {
    u32* sc = Scrolled[q];
    if (GPU3D.AbortFrame) {
        memset(sc, 0, sizeof Scrolled[0]);
        return sc;
    }
    if (line >= 192) line = 191;
    WaitChunk(line * NCHUNK / 192, Gen);
    const u32* raw = QuarterRow(line, q);
    const u16 xpos = GPU3D.RenderXPos;
    if (!xpos) return raw;
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
