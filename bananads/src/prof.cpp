// prof.hz: a sampling profiler of the emulator's thread (prof.h). Settings (docs/SETTINGS.md):
//   prof.hz       samples per second of the thread's CPU time (0: off, the default). The kernel checks thread CPU
//                 timers at its tick, so the rate it reaches is at most CONFIG_HZ (the report says what it got)
//   prof.from     the first frame sampled (e.g. headless.measure_from's); prof.frames: how many (0: to the end)
//   prof.out      the report's file (default: the log); prof.raw: every sampled address with its count and the
//                 modules' ranges, for resolving offline (addr2line -f -C -e <unstripped binary>)
//   prof.syms     the binary to read function symbols from (default /proc/self/exe): the unstripped build of this
//                 same binary, since Buildroot strips what goes onto the device
//   prof.top      the symbols listed (40); prof.samples: the sample array (262144 PCs, 2 MB)
#include "prof.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Platform.h"
#include "config.h"

#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
#define BANANADS_PROF 1
#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
#endif

using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

namespace bananads {

#ifndef BANANADS_PROF
std::unique_ptr<Profiler> Profiler::create(const Config& cfg) {
    if (cfg.num("prof.hz", 0) > 0) Log(LogLevel::Warn, "[prof] prof.hz: only in Linux aarch64/x86-64 builds\n");
    return nullptr;
}
#else

namespace {

// The samples: written by the handler on the emulator's thread only, read once the timer is gone.
uintptr_t* g_pcs = nullptr;
uint32_t g_cap = 0;
std::atomic<uint32_t> g_n{0};
std::atomic<uint64_t> g_dropped{0}, g_overruns{0};
std::atomic<bool> g_taken{false};  // one profiler a process (difftest has two Emus)

void on_sigprof(int, siginfo_t* si, void* ucv) {
    const int saved = errno;
    const ucontext_t* uc = static_cast<const ucontext_t*>(ucv);
#if defined(__aarch64__)
    const uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    const uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
    if (si->si_code == SI_TIMER && si->si_overrun > 0)
        g_overruns.fetch_add((uint64_t)si->si_overrun, std::memory_order_relaxed);
    const uint32_t i = g_n.load(std::memory_order_relaxed);
    if (i < g_cap) {
        g_pcs[i] = pc;
        g_n.store(i + 1, std::memory_order_release);
    } else {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    errno = saved;
}

double now_s(clockid_t c) {
    timespec t{};
    clock_gettime(c, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

struct Sym {
    uint64_t addr, size;
    const char* name;  // into the mapped file's string table
};

// Function symbols of an ELF64 file: .symtab, else .dynsym. Also its entry point and .text, to check that a
// prof.syms file is the running binary's unstripped twin.
struct ElfSyms {
    std::vector<Sym> syms;
    const char* table = "none";
    uint64_t entry = 0, text_addr = 0, text_size = 0;
    void* map = nullptr;
    size_t map_len = 0;
    ~ElfSyms() {
        if (map) munmap(map, map_len);
    }
    bool load(const char* path) {
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
            close(fd);
            return false;
        }
        map_len = (size_t)st.st_size;
        map = mmap(nullptr, map_len, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED) {
            map = nullptr;
            return false;
        }
        const uint8_t* b = static_cast<const uint8_t*>(map);
        const Elf64_Ehdr* eh = reinterpret_cast<const Elf64_Ehdr*>(b);
        if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
            eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > map_len)
            return false;
        entry = eh->e_entry;
        const Elf64_Shdr* sh = reinterpret_cast<const Elf64_Shdr*>(b + eh->e_shoff);
        const int n = eh->e_shnum;
        if (eh->e_shstrndx < n && sh[eh->e_shstrndx].sh_offset < map_len) {
            const char* names = reinterpret_cast<const char*>(b + sh[eh->e_shstrndx].sh_offset);
            for (int i = 0; i < n; i++)
                if (sh[i].sh_name < sh[eh->e_shstrndx].sh_size && strcmp(names + sh[i].sh_name, ".text") == 0) {
                    text_addr = sh[i].sh_addr;
                    text_size = sh[i].sh_size;
                }
        }
        for (uint32_t want : {(uint32_t)SHT_SYMTAB, (uint32_t)SHT_DYNSYM}) {
            for (int i = 0; i < n; i++) {
                if (sh[i].sh_type != want || sh[i].sh_link >= (uint32_t)n) continue;
                const Elf64_Shdr& strs = sh[sh[i].sh_link];
                if (sh[i].sh_offset + sh[i].sh_size > map_len || strs.sh_offset + strs.sh_size > map_len) continue;
                const Elf64_Sym* s = reinterpret_cast<const Elf64_Sym*>(b + sh[i].sh_offset);
                const size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
                const char* str = reinterpret_cast<const char*>(b + strs.sh_offset);
                for (size_t k = 0; k < count; k++) {
                    const int type = ELF64_ST_TYPE(s[k].st_info);
                    const uint16_t shn = s[k].st_shndx;
                    if (s[k].st_value == 0 || shn == SHN_UNDEF || shn >= n || s[k].st_name >= strs.sh_size) continue;
                    if (!(sh[shn].sh_flags & SHF_EXECINSTR)) continue;
                    // functions, and the assembly's labels (ARM_Dispatch); not the mapping symbols ($x, $d)
                    if (type != STT_FUNC && type != STT_NOTYPE) continue;
                    const char* name = str + s[k].st_name;
                    if (!name[0] || name[0] == '$' || (name[0] == '.' && name[1] == 'L')) continue;
                    syms.push_back({s[k].st_value, s[k].st_size, name});
                }
            }
            if (!syms.empty()) {
                table = want == SHT_SYMTAB ? ".symtab" : ".dynsym";
                break;
            }
        }
        // by address; at one address the sized symbol first, then keep one
        std::sort(syms.begin(), syms.end(), [](const Sym& a, const Sym& c) {
            return a.addr != c.addr ? a.addr < c.addr : a.size > c.size;
        });
        syms.erase(std::unique(syms.begin(), syms.end(), [](const Sym& a, const Sym& c) { return a.addr == c.addr; }),
                   syms.end());
        return true;
    }
    // the symbol holding addr (its size, or up to the next symbol for an unsized one), else nullptr
    const Sym* find(uint64_t addr) const {
        auto it = std::upper_bound(syms.begin(), syms.end(), addr, [](uint64_t a, const Sym& s) { return a < s.addr; });
        if (it == syms.begin()) return nullptr;
        const Sym& s = *(it - 1);
        if (s.size > 0 && addr >= s.addr + s.size) return nullptr;
        return &s;
    }
};

struct Module {
    uintptr_t lo, hi, bias;
    std::string name;
    bool main;
};

int collect_modules(dl_phdr_info* info, size_t, void* p) {
    auto& mods = *static_cast<std::vector<Module>*>(p);
    const bool main = mods.empty();  // the program comes first
    std::string name = info->dlpi_name && info->dlpi_name[0] ? info->dlpi_name : "";
    if (const size_t sl = name.rfind('/'); sl != std::string::npos) name = name.substr(sl + 1);
    if (main && name.empty()) name = "bananads";
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
            mods.push_back({info->dlpi_addr + ph.p_vaddr, info->dlpi_addr + ph.p_vaddr + ph.p_memsz, info->dlpi_addr,
                            name.empty() ? "?" : name, main});
    }
    return 0;
}

std::string demangle(const char* name) {
    int status = 0;
    char* d = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (!d) return name;
    std::string s = d;
    free(d);
    return s;
}

// "void melonDS::DMA::Run9<true>()" -> "melonDS::DMA": the function's scope; a C function is its own
std::string scope_of(const std::string& fn) {
    int depth = 0;
    size_t end = fn.size(), start = 0;
    for (size_t i = 0; i < fn.size(); i++) {
        const char c = fn[i];
        if (c == '<') depth++;
        else if (c == '>') depth--;
        else if (c == '(' && depth == 0) { end = i; break; }
        else if (c == ' ' && depth == 0) start = i + 1;  // past a return type
    }
    std::string s = fn.substr(start, end - start);
    // the last "::" outside template arguments
    depth = 0;
    size_t cut = std::string::npos;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] == '<') depth++;
        else if (s[i] == '>') depth--;
        else if (depth == 0 && s[i] == ':' && s[i + 1] == ':') cut = i;
    }
    return cut == std::string::npos ? s : s.substr(0, cut);
}

class ProfilerImpl final : public Profiler {
public:
    explicit ProfilerImpl(const Config& cfg)
        : hz_(cfg.num("prof.hz", 0)), from_(cfg.num("prof.from", 0)), frames_(cfg.num("prof.frames", 0)),
          top_(cfg.num("prof.top", 40)), cap_(cfg.num("prof.samples", 262144)), out_(cfg.str("prof.out", "")),
          raw_(cfg.str("prof.raw", "")), syms_(cfg.str("prof.syms", "")) {}
    ~ProfilerImpl() override { finish(); }

    void frame(long n) override {
        if (state_ == Idle && n >= from_) start(n);
        if (state_ != Running) return;
        last_frame_ = n;
        cpu_last_ = now_s(CLOCK_THREAD_CPUTIME_ID);
        wall_last_ = now_s(CLOCK_MONOTONIC);
        if (frames_ > 0 && n >= first_frame_ + frames_) {
            stop();
            report();
        }
    }
    void finish() override {
        if (state_ == Running) {
            stop();
            report();
        }
    }

private:
    enum State { Idle, Running, Done };

    void start(long n) {
        state_ = Done;  // unless all goes well
        if (g_taken.exchange(true)) {
            Log(LogLevel::Warn, "[prof] another profiler runs in this process: this one is off\n");
            return;
        }
        g_cap = (uint32_t)std::clamp(cap_, 1024L, 1L << 26);
        g_pcs = static_cast<uintptr_t*>(calloc(g_cap, sizeof(uintptr_t)));  // touched now, not in the handler
        if (!g_pcs) return;
        memset(g_pcs, 0, (size_t)g_cap * sizeof(uintptr_t));
        g_n = 0;
        g_dropped = 0;
        g_overruns = 0;
        struct sigaction sa{};
        sa.sa_sigaction = on_sigprof;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;  // a system call it lands in goes on as if nothing happened
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        tid_ = (long)syscall(SYS_gettid);
        sigevent sev{};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev.sigev_notify_thread_id = (pid_t)tid_;
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer_) != 0) {
            Log(LogLevel::Error, "[prof] timer_create: %s\n", strerror(errno));
            return;
        }
        const long ns = 1000000000L / std::clamp(hz_, 1L, 100000L);
        itimerspec its{};
        its.it_interval.tv_sec = ns / 1000000000L;
        its.it_interval.tv_nsec = ns % 1000000000L;
        its.it_value = its.it_interval;
        cpu0_ = cpu_last_ = now_s(CLOCK_THREAD_CPUTIME_ID);
        wall0_ = wall_last_ = now_s(CLOCK_MONOTONIC);
        first_frame_ = last_frame_ = n;
        if (timer_settime(timer_, 0, &its, nullptr) != 0) {
            Log(LogLevel::Error, "[prof] timer_settime: %s\n", strerror(errno));
            timer_delete(timer_);
            return;
        }
        state_ = Running;
        Log(LogLevel::Info, "[prof] sampling the emulator's thread (tid %ld) at %ld Hz of its CPU time from frame %ld\n",
            tid_, hz_, n);
    }

    void stop() {
        timer_delete(timer_);
        signal(SIGPROF, SIG_IGN);  // one still pending goes nowhere (its default would end the process)
        state_ = Done;
    }

    void report() {
        const uint32_t n = std::min(g_n.load(std::memory_order_acquire), g_cap);
        std::vector<Module> mods;
        dl_iterate_phdr(collect_modules, &mods);
        // the binary's symbols: prof.syms (the unstripped twin), else the binary itself
        ElfSyms elf;
        const std::string self = "/proc/self/exe";
        const std::string spath = syms_.empty() ? self : syms_;
        std::string symnote;
        if (!elf.load(spath.c_str())) {
            symnote = "can't read " + spath;
        } else if (!syms_.empty()) {
            ElfSyms me;
            if (me.load(self.c_str()) && (me.entry != elf.entry || me.text_addr != elf.text_addr || me.text_size != elf.text_size))
                symnote = "WARNING: " + syms_ + " is not this binary's build (entry or .text differ): names are wrong";
        }
        if (symnote.empty() && strcmp(elf.table, ".symtab") != 0)
            symnote = std::string("the binary has no .symtab (stripped): ") +
                      "copy the unstripped build next to it and pass --set prof.syms=<file>, or use prof.raw";
        // JIT blocks: MELONDS_JIT_PERFMAP's map (start size name), the last name at a start wins
        std::map<uintptr_t, std::pair<uintptr_t, std::string>> jitmap;
        if (getenv("MELONDS_JIT_PERFMAP")) {
            const char* dir = getenv("MELONDS_JIT_PERFDIR") ? getenv("MELONDS_JIT_PERFDIR") : "/tmp";
            char path[512];
            snprintf(path, sizeof(path), "%s/perf-%d.map", dir, (int)getpid());
            if (FILE* f = fopen(path, "r")) {
                char line[256], name[128];
                unsigned long a, s;
                while (fgets(line, sizeof(line), f))
                    if (sscanf(line, "%lx %lx %127s", &a, &s, name) == 3) jitmap[a] = {s, name};
                fclose(f);
            }
        }
        struct Count { uint64_t n = 0; std::string mod; };
        std::unordered_map<std::string, Count> by_sym;
        std::map<std::string, uint64_t> by_mod, by_scope, by_jit;
        std::unordered_map<uintptr_t, uint64_t> by_pc;
        uint64_t jit = 0;
        std::vector<std::pair<uintptr_t, uint32_t>> sorted;
        sorted.reserve(n);
        for (uint32_t i = 0; i < n; i++) by_pc[g_pcs[i]]++;
        for (const auto& [pc, cnt] : by_pc) {
            const Module* m = nullptr;
            for (const Module& mm : mods)
                if (pc >= mm.lo && pc < mm.hi) { m = &mm; break; }
            std::string name, mod;
            if (!m) {
                mod = "[JIT code]";
                jit += cnt;
                auto it = jitmap.upper_bound(pc);
                if (it != jitmap.begin() && pc < std::prev(it)->first + std::prev(it)->second.first)
                    name = std::prev(it)->second.second;
                else
                    name = jitmap.empty() ? "[JIT code]" : "[JIT code, not in the map]";
                by_jit[name] += cnt;
                // stub_eXXX (the shared exits and dispatch), mj9/mj7 blocks
                name = jitmap.empty() ? "[JIT code]" : name.compare(0, 5, "stub_") == 0 ? "[JIT " + name + "]" : "[JIT blocks]";
            } else if (m->main) {
                mod = m->name;
                const Sym* s = elf.find(pc - m->bias);
                if (s) name = demangle(s->name);
                else {
                    char b[64];
                    snprintf(b, sizeof(b), "%s+0x%" PRIxPTR, m->name.c_str(), pc - m->bias);
                    name = b;
                }
            } else {
                mod = m->name;
                Dl_info di{};
                if (dladdr((void*)pc, &di) && di.dli_sname) name = demangle(di.dli_sname);
                else {
                    char b[160];
                    snprintf(b, sizeof(b), "%s+0x%" PRIxPTR, m->name.c_str(), pc - m->bias);
                    name = b;
                }
            }
            Count& c = by_sym[name];
            c.n += cnt;
            c.mod = mod;
            by_mod[mod] += cnt;
            by_scope[m ? scope_of(name) : "[JIT code]"] += cnt;
        }
        const double cpu = cpu_last_ - cpu0_, wall = wall_last_ - wall0_;
        const long nframes = last_frame_ - first_frame_;
        std::string r;
        char line[512];
        auto add = [&](const char* fmt, auto... args) {
            snprintf(line, sizeof(line), fmt, args...);
            r += line;
        };
        auto pct = [&](uint64_t c) { return n ? 100.0 * (double)c / (double)n : 0.0; };
        add("# BananaDS prof: the emulator's thread (tid %ld), prof.hz=%ld of its CPU time, frames %ld-%ld\n", tid_,
            hz_, first_frame_, last_frame_);
        add("# %u samples (%.0f a second of its CPU time; %" PRIu64 " dropped, %" PRIu64
            " timer overruns: the kernel's tick caps the rate); %.2f s CPU in %.2f s (%.0f%% busy), %.1f fps\n",
            n, cpu > 0 ? n / cpu : 0.0, g_dropped.load(), g_overruns.load(), cpu, wall,
            wall > 0 ? 100.0 * cpu / wall : 0.0, wall > 0 ? nframes / wall : 0.0);
        add("# symbols: %s %s (%zu functions)%s%s\n", spath.c_str(), elf.table, elf.syms.size(),
            symnote.empty() ? "" : "; ", symnote.c_str());
        add("# JIT code: %.1f%% of the samples%s\n", pct(jit),
            jitmap.empty() ? " (MELONDS_JIT_PERFMAP=1 names its blocks and stubs)" : "");
        auto ranked = [](const auto& m) {
            std::vector<std::pair<std::string, uint64_t>> v;
            for (const auto& [k, c] : m) v.push_back({k, c});
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            return v;
        };
        r += "\n## by module\n";
        for (const auto& [k, c] : ranked(by_mod)) add("%6.2f%% %7" PRIu64 "  %s\n", pct(c), c, k.c_str());
        add("\n## top %ld symbols\n", top_);
        {
            std::vector<std::pair<std::string, uint64_t>> v;
            for (const auto& [k, c] : by_sym) v.push_back({k, c.n});
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            double cum = 0;
            for (size_t i = 0; i < v.size() && (long)i < top_; i++) {
                cum += pct(v[i].second);
                add("%6.2f%% %6.2f%% %7" PRIu64 "  %-12.12s %.300s\n", pct(v[i].second), cum, v[i].second,
                    by_sym[v[i].first].mod.c_str(), v[i].first.c_str());
            }
        }
        r += "\n## by scope (C++ class or namespace; a C function is its own)\n";
        {
            const auto v = ranked(by_scope);
            for (size_t i = 0; i < v.size() && i < 30; i++)
                add("%6.2f%% %7" PRIu64 "  %.300s\n", pct(v[i].second), v[i].second, v[i].first.c_str());
        }
        if (!jitmap.empty()) {
            r += "\n## JIT code by block (mj9_/mj7_<guest address>) and stub (MELONDS_JIT_PERFMAP)\n";
            const auto v = ranked(by_jit);
            for (size_t i = 0; i < v.size() && i < 30; i++)
                add("%6.2f%% %7" PRIu64 "  %s\n", pct(v[i].second), v[i].second, v[i].first.c_str());
        }
        if (!out_.empty()) {
            if (FILE* f = fopen(out_.c_str(), "w")) {
                fputs(r.c_str(), f);
                fclose(f);
                Log(LogLevel::Info, "[prof] %u samples, JIT code %.1f%%: the report is in %s\n", n, pct(jit), out_.c_str());
            } else {
                Log(LogLevel::Error, "[prof] prof.out: can't write %s\n", out_.c_str());
                out_.clear();
            }
        }
        if (out_.empty()) {
            size_t i = 0;
            while (i < r.size()) {
                const size_t e = r.find('\n', i);
                Log(LogLevel::Info, "[prof] %s\n", r.substr(i, e - i).c_str());
                i = e == std::string::npos ? r.size() : e + 1;
            }
        }
        if (!raw_.empty()) {
            if (FILE* f = fopen(raw_.c_str(), "w")) {
                fprintf(f, "# prof.raw: <address> <samples> <module>; then the modules: # module <lo> <hi> <bias> <name>\n");
                for (const Module& m : mods)
                    fprintf(f, "# module 0x%" PRIxPTR " 0x%" PRIxPTR " 0x%" PRIxPTR " %s\n", m.lo, m.hi, m.bias, m.name.c_str());
                for (const auto& [pc, cnt] : by_pc) {
                    const char* mod = "[JIT]";
                    for (const Module& mm : mods)
                        if (pc >= mm.lo && pc < mm.hi) { mod = mm.name.c_str(); break; }
                    fprintf(f, "0x%" PRIxPTR " %" PRIu64 " %s\n", pc, cnt, mod);
                }
                fclose(f);
            } else {
                Log(LogLevel::Error, "[prof] prof.raw: can't write %s\n", raw_.c_str());
            }
        }
        free(g_pcs);
        g_pcs = nullptr;
        g_cap = 0;
        g_taken = false;
    }

    long hz_, from_, frames_, top_, cap_;
    std::string out_, raw_, syms_;
    State state_ = Idle;
    timer_t timer_{};
    long tid_ = 0;
    long first_frame_ = 0, last_frame_ = 0;
    double cpu0_ = 0, cpu_last_ = 0, wall0_ = 0, wall_last_ = 0;
};

}  // namespace

std::unique_ptr<Profiler> Profiler::create(const Config& cfg) {
    if (cfg.num("prof.hz", 0) <= 0) return nullptr;
    return std::make_unique<ProfilerImpl>(cfg);
}
#endif

}  // namespace bananads
