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
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-buf.h>
// <linux/dma-heap.h> (not in the toolchain's headers)
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "Platform.h"

using namespace melonDS;

namespace bananads {

int GpuRenderer3D::Stats = 0;
int GpuRenderer3D::ReadMode = 1;
int GpuRenderer3D::Finish = 0;
int GpuRenderer3D::Clear = 1;
std::string GpuRenderer3D::MesaDebug;

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
    if (!MesaDebug.empty()) setenv("PAN_MESA_DEBUG", MesaDebug.c_str(), 1);
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
    if (ReadMode == 2 && !HeapSetup()) {
        Platform::Log(Platform::LogLevel::Warn, "[gpu3d] no cached dma-heap target: read-back through a pixel buffer\n");
        ReadMode = 1;
    }
    glGenBuffers(1, &Pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, Pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (size_t)256 * 192 * Scale_ * Scale_ * 4, nullptr, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

// render.gpu3d_read=heap: the pack pass draws straight into two dma-bufs from /dev/dma_heap/system (CPU-cached pages,
// imported into EGL as linear ABGR8888 render targets); they ARE the 2D engine's 3D rows: no read-back copy, the CPU
// cache invalidated with DMA_BUF_IOCTL_SYNC once the GPU is done
bool GpuRenderer3D::HeapSetup() {
    auto createImage = (PFNEGLCREATEIMAGEKHRPROC)E.GetProc("eglCreateImageKHR");
    auto target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)E.GetProc("glEGLImageTargetTexture2DOES");
    if (!createImage || !target) return false;
    const int hfd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (hfd < 0) return false;
    const int W = 256 * Scale_, H = 192 * Scale_;
    const size_t len = (size_t)W * H * 4;
    for (int i = 0; i < 2; i++) {
        dma_heap_allocation_data a{};
        a.len = len;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { close(hfd); return false; }
        HeapFd[i] = (int)a.fd;
        void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, HeapFd[i], 0);
        if (p == MAP_FAILED) { close(hfd); return false; }
        HeapMap[i] = (u32*)p;
        const EGLint at[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_ABGR8888,
                             EGL_DMA_BUF_PLANE0_FD_EXT, HeapFd[i], EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                             EGL_DMA_BUF_PLANE0_PITCH_EXT, W * 4, EGL_NONE};
        EGLImageKHR img = createImage(E.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, at);
        if (img == EGL_NO_IMAGE_KHR) { Platform::Log(Platform::LogLevel::Warn, "[gpu3d] dma-buf import failed (0x%x)\n", E.GetError()); close(hfd); return false; }
        glGenTextures(1, &HeapTex[i]);
        glBindTexture(GL_TEXTURE_2D, HeapTex[i]);
        target(GL_TEXTURE_2D, img);
        glGenFramebuffers(1, &HeapFb[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, HeapFb[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, HeapTex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Platform::Log(Platform::LogLevel::Warn, "[gpu3d] dma-buf framebuffer incomplete\n");
            close(hfd);
            return false;
        }
    }
    close(hfd);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Platform::Log(Platform::LogLevel::Info, "[gpu3d] the 3D rows are 2 cached dma-heap buffers the GPU draws into (no read-back)\n");
    return true;
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
    if (ReadMode == 2) {  // the rows live in the dma-bufs
        for (int i = 0; i < 2; i++) {
            if (HeapMap[i]) memset(HeapMap[i], 0, words * 4);
            free(OutBuf[i]);
            OutBuf[i] = HeapMap[i];
        }
        HeapOwned = true;
        Out = Target = OutBuf[0];
    }
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
    if (!HeapOwned)
        for (auto& b : OutBuf) free(b);
    for (int i = 0; i < 2; i++) {
        if (HeapMap[i]) munmap(HeapMap[i], (size_t)256 * 192 * Scale_ * Scale_ * 4);
        if (HeapFd[i] >= 0) close(HeapFd[i]);
    }
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
        uint64_t tf = 0;
        if (Finish) {
            const uint64_t a = NowNs();
            glFinish();
            tf = NowNs() - a;
        }
        const int hb = Target == OutBuf[1] ? 1 : 0;
        if (ReadMode == 2) {  // the CPU's reads of this buffer (two frames ago) are over
            dma_buf_sync sy{DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
            ioctl(HeapFd[hb], DMA_BUF_IOCTL_SYNC, &sy);
        }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ReadMode == 2 ? HeapFb[hb] : PackFb);
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
        glBindFramebuffer(GL_READ_FRAMEBUFFER, PackFb);
        if (ReadMode == 1) {  // the GPU copies into the pixel buffer; the CPU reads that linear buffer
            glBindBuffer(GL_PIXEL_PACK_BUFFER, Pbo);
            for (int c = 0; c < NCHUNK; c++)
                glReadPixels(0, c * CH, W, CH, GL_RGBA, GL_UNSIGNED_BYTE, (void*)((size_t)c * CH * W * 4));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        const uint64_t t1 = NowNs();
        glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(fence);
        const uint64_t t2 = NowNs();
        u32* dst = Target;
        if (ReadMode == 2) {
            dma_buf_sync sy{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            ioctl(HeapFd[hb], DMA_BUF_IOCTL_SYNC, &sy);
        }
        if (ReadMode == 1) glBindBuffer(GL_PIXEL_PACK_BUFFER, Pbo);
        for (int c = 0; c < NCHUNK; c++) {
            if (ReadMode == 1) {
                const size_t len = (size_t)CH * W * 4;
                const void* p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, c * len, len, GL_MAP_READ_BIT);
                if (p) memcpy(dst + (size_t)c * CH * W, p, len);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            } else if (ReadMode == 0)
                glReadPixels(0, c * CH, W, CH, GL_RGBA, GL_UNSIGNED_BYTE, dst + (size_t)c * CH * W);
            Done[c].store(gen, std::memory_order_release);
            {
                std::lock_guard<std::mutex> l(M);
            }
            DoneCv.notify_all();
        }
        if (ReadMode == 1) glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        const uint64_t t3 = NowNs();
        StFinNs += tf;
        StGlNs += t1 - t0 - tf;
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
                      "read-back %.3f ms, finish %.3f ms (gpu3d thread) per frame\n",
                      Stats, StEmuWaitNs / n, StPrepNs / n, StGlNs / n, StWaitNs / n, StReadNs / n, StFinNs / n);
        StEmuWaitNs = StPrepNs = StGlNs = StWaitNs = StReadNs = StFinNs = 0;
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
