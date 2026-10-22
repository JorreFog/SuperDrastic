// melonDS's Platform interface (Platform.h), for BananaDS: plain POSIX files and threads, the log, and the
// hooks that reach the emulator (saves, the mic, Wi-Fi).
#include "Platform.h"

#include <sys/stat.h>
#include <dlfcn.h>
#include <semaphore.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#include "Net.h"
#include "emu.h"

#ifdef BANANADS_LAN
#include "MPInterface.h"
#endif

namespace bananads {
FILE* g_log = nullptr;  // main.cpp opens it (log.file); stderr too
int g_log_level = 1;    // 0 debug .. 3 error
bool g_log_time = false;  // log.time: each line after the clock since boot (CLOCK_BOOTTIME, /proc/uptime's)
}

void bananads::start_mark(const char* what) {
    if (g_log_time) melonDS::Platform::Log(melonDS::Platform::LogLevel::Info, "[start] %s\n", what);
}

namespace melonDS::Platform {

using bananads::Emu;

void SignalStop(StopReason reason, void* userdata) {
    Log(LogLevel::Info, "[emu] the DS stopped (reason %d)\n", (int)reason);
    if (userdata) Emu::from(userdata)->stop_requested();
}

// --- files ---

static const char* mode_str(FileMode m) {
    bool r = m & FileMode::Read, w = m & FileMode::Write;
    if (m & FileMode::Append) return r ? "a+b" : "ab";
    if (r && w) return (m & FileMode::Preserve) ? "r+b" : "w+b";
    if (w) return (m & FileMode::Preserve) ? "r+b" : "wb";
    return "rb";
}

FileHandle* OpenFile(const std::string& path, FileMode mode) {
    if ((mode & FileMode::ReadWrite) == FileMode::None) return nullptr;
    bool exists = access(path.c_str(), F_OK) == 0;
    if ((mode & FileMode::NoCreate) && !exists) return nullptr;
    if ((mode & FileMode::Write) && (mode & FileMode::Preserve) && !exists) {
        FILE* c = fopen(path.c_str(), "wb");  // r+b needs the file to exist
        if (c) fclose(c);
    }
    return (FileHandle*)fopen(path.c_str(), mode_str(mode));
}

std::string GetLocalFilePath(const std::string& filename) {
    const char* dir = getenv("BANANADS_DATA");
    std::string d = dir ? dir : ".";
    return filename.empty() || filename[0] == '/' ? filename : d + "/" + filename;
}

FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(GetLocalFilePath(path), mode); }
bool FileExists(const std::string& name) { return access(name.c_str(), F_OK) == 0; }
bool LocalFileExists(const std::string& name) { return FileExists(GetLocalFilePath(name)); }

bool CheckFileWritable(const std::string& path) {
    if (FileExists(path)) return access(path.c_str(), W_OK) == 0;
    size_t s = path.find_last_of('/');
    return access(s == std::string::npos ? "." : path.substr(0, s).c_str(), W_OK) == 0;
}
bool CheckLocalFileWritable(const std::string& name) { return CheckFileWritable(GetLocalFilePath(name)); }

bool CloseFile(FileHandle* f) { return fclose((FILE*)f) == 0; }
bool IsEndOfFile(FileHandle* f) { return feof((FILE*)f) != 0; }
bool FileReadLine(char* str, int count, FileHandle* f) { return fgets(str, count, (FILE*)f) != nullptr; }
u64 FilePosition(FileHandle* f) { return (u64)ftello((FILE*)f); }

bool FileSeek(FileHandle* f, s64 off, FileSeekOrigin o) {
    int w = o == FileSeekOrigin::Start ? SEEK_SET : o == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;
    return fseeko((FILE*)f, off, w) == 0;
}
void FileRewind(FileHandle* f) { rewind((FILE*)f); }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* f) { return fread(data, size, count, (FILE*)f); }
bool FileFlush(FileHandle* f) { return fflush((FILE*)f) == 0; }
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* f) { return fwrite(data, size, count, (FILE*)f); }

u64 FileWriteFormatted(FileHandle* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf((FILE*)f, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : (u64)n;
}

u64 FileLength(FileHandle* f) {
    struct stat st;
    return fstat(fileno((FILE*)f), &st) == 0 ? (u64)st.st_size : 0;
}

// --- the log ---

bool ROMRelease(u8* rom) { return bananads::rom_unmap(rom); }

void Log(LogLevel level, const char* fmt, ...) {
    if ((int)level < bananads::g_log_level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (bananads::g_log_time) {
        static bool bol = true;  // (a line can come in pieces: the time at its start only)
        char t[2048 + 24];
        timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        if (bol) snprintf(t, sizeof t, "%lld.%03ld %s", (long long)ts.tv_sec, ts.tv_nsec / 1000000, buf);
        else snprintf(t, sizeof t, "%s", buf);
        size_t n = strlen(buf);
        bol = n && buf[n - 1] == '\n';
        memcpy(buf, t, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
    }
    fputs(buf, stderr);
    if (bananads::g_log) {
        fputs(buf, bananads::g_log);
        fflush(bananads::g_log);
    }
}

// --- threads ---

struct Thread { std::thread t; };
Thread* Thread_Create(std::function<void()> func) { return new Thread{std::thread(std::move(func))}; }
void Thread_Free(Thread* t) {
    if (t->t.joinable()) t->t.detach();
    delete t;
}
void Thread_Wait(Thread* t) {
    if (t->t.joinable()) t->t.join();
}

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) {
    std::lock_guard<std::mutex> l(s->m);
    s->count = 0;
}
void Semaphore_Wait(Semaphore* s) {
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [s] { return s->count > 0; });
    s->count--;
}
bool Semaphore_TryWait(Semaphore* s, int timeout_ms) {
    std::unique_lock<std::mutex> l(s->m);
    if (!s->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [s] { return s->count > 0; })) return false;
    s->count--;
    return true;
}
void Semaphore_Post(Semaphore* s, int count) {
    {
        std::lock_guard<std::mutex> l(s->m);
        s->count += count;
    }
    s->cv.notify_all();
}

struct Mutex { std::mutex m; };
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->m.lock(); }
void Mutex_Unlock(Mutex* m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->m.try_lock(); }

void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }

u64 GetMSCount() { return GetUSCount() / 1000; }
u64 GetUSCount() {
    using namespace std::chrono;
    return (u64)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// --- saves, firmware, clock ---

void WriteNDSSave(const u8* data, u32 len, u32 off, u32 n, void* userdata) {
    if (userdata) Emu::from(userdata)->write_save(data, len, off, n);
}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}       // no GBA slot on the handhelds
void WriteFirmware(const Firmware&, u32, u32, void*) {}      // the firmware is generated at every start
void WriteDateTime(int, int, int, int, int, int, void*) {}   // the DS's clock follows the handheld's

// --- local wireless (DS to DS) ---
// With BANANADS_LAN two handhelds on one network play together (melonDS's LAN, over ENet). Without it the game
// sees no other DS nearby.

#ifdef BANANADS_LAN
void MP_Begin(void*) { MPInterface::Get().Begin(0); }
void MP_End(void*) { MPInterface::Get().End(0); }
int MP_SendPacket(u8* d, int l, u64 ts, void*) { return MPInterface::Get().SendPacket(0, d, l, ts); }
int MP_RecvPacket(u8* d, u64* ts, void*) { return MPInterface::Get().RecvPacket(0, d, ts); }
int MP_SendCmd(u8* d, int l, u64 ts, void*) { return MPInterface::Get().SendCmd(0, d, l, ts); }
int MP_SendReply(u8* d, int l, u64 ts, u16 aid, void*) { return MPInterface::Get().SendReply(0, d, l, ts, aid); }
int MP_SendAck(u8* d, int l, u64 ts, void*) { return MPInterface::Get().SendAck(0, d, l, ts); }
int MP_RecvHostPacket(u8* d, u64* ts, void*) { return MPInterface::Get().RecvHostPacket(0, d, ts); }
u16 MP_RecvReplies(u8* d, u64 ts, u16 m, void*) { return MPInterface::Get().RecvReplies(0, d, ts, m); }
#else
void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int len, u64, void*) { return len; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int len, u64, void*) { return len; }
int MP_SendReply(u8*, int len, u64, u16, void*) { return len; }
int MP_SendAck(u8*, int len, u64, void*) { return len; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
#endif

// --- internet (Nintendo WFC revivals) ---

int Net_SendPacket(u8* data, int len, void* userdata) {
    Net* n = userdata ? Emu::from(userdata)->net() : nullptr;
    return n ? n->SendPacket(data, len, 0) : 0;
}
int Net_RecvPacket(u8* data, void* userdata) {
    Net* n = userdata ? Emu::from(userdata)->net() : nullptr;
    return n ? n->RecvPacket(data, 0) : 0;
}

// --- DSi camera: not on these handhelds ---

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*) {
    for (int i = 0; i < width * height; i++) frame[i] = 0x80808080;
}

// --- the microphone ---

// (the log says when the game listens and the loudest it was given, every 5 s: "the mic doesn't work" reports)
static int mic_peak, mic_reads;
static std::chrono::steady_clock::time_point mic_since;
void Mic_Start(void* userdata) {
    if (!userdata) return;
    Emu::from(userdata)->mic().set_active(true);
    Log(LogLevel::Info, "[mic] the game listens\n");
    mic_peak = mic_reads = 0;
    mic_since = std::chrono::steady_clock::now();
}
void Mic_Stop(void* userdata) {
    if (!userdata) return;
    Emu::from(userdata)->mic().set_active(false);
    Log(LogLevel::Info, "[mic] the game stopped listening\n");
}
int Mic_ReadInput(s16* data, int maxlength, void* userdata) {
    if (!userdata) return 0;
    const int n = Emu::from(userdata)->mic().read(data, maxlength);
    for (int i = 0; i < n; i++) mic_peak = std::max(mic_peak, data[i] < 0 ? -(int)data[i] : (int)data[i]);
    mic_reads += n;
    const auto now = std::chrono::steady_clock::now();
    if (now - mic_since >= std::chrono::seconds(5)) {
        Log(LogLevel::Info, "[mic] 5 s: %d samples to the game, loudest %.3f\n", mic_reads, mic_peak / 32768.0);
        mic_peak = mic_reads = 0;
        mic_since = now;
    }
    return n;
}

// --- DSi AAC (DSP HLE): not needed for DS games ---

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

// --- GBA-slot add-ons ---

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType type, void*) {
    return type == MotionAccelerationZ ? 9.80665f : 0.0f;  // lying flat, still
}

// --- dynamic libraries ---

DynamicLibrary* DynamicLibrary_Load(const char* lib) { return (DynamicLibrary*)dlopen(lib, RTLD_NOW | RTLD_LOCAL); }
void DynamicLibrary_Unload(DynamicLibrary* lib) { dlclose(lib); }
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name) { return dlsym(lib, name); }

}  // namespace melonDS::Platform
