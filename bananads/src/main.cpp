// bananads [options] <game.nds>
//   --config <file>     settings file (default: $BANANADS_DATA/bananads.conf, BANANADS_DATA defaults to
//                       /storage/.config/bananads when that exists, else the current directory)
//   --set key=value     one setting, over the file and the environment's BANANADS_* (repeatable)
//   --frontend <name>   kms (the handheld's panels: the default without a display server), sdl (a desktop window),
//                       headless (benchmark/tests)
//   --version
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Platform.h"
#include "config.h"
#include "emu.h"
#include "frontend.h"

namespace bananads {
extern FILE* g_log;
extern int g_log_level;
extern bool g_log_time;
}

using namespace bananads;
using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

// quit.fast (off by default until the RG DS Plus is understood, below): the quit leaves the emulator's memory to a helper that shares it (CLONE_VM) and goes after BananaDS
// has: the melonDS console's teardown (its JIT's fast-memory area unmapped, ~140 ms on the RG DS Plus) and the exit's
// own unmapping of everything (~50 ms) then happen after the menu has been told the game ended, not before. The save
// and the console's stop are done first (Emu::shutdown); nothing is written after them.
#ifndef SYS_close_range
#define SYS_close_range 436  // (the toolchain's headers predate it; the handhelds' kernel has it)
#endif
static pid_t g_fast_parent;
static int mm_holder(void*) {
    // (raw system calls only: this shares BananaDS's memory and its thread pointer, and runs no C library code that
    // keeps state)
    syscall(SYS_close_range, 0U, ~0U, 0U);
    syscall(SYS_prctl, PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);  // killed as BananaDS's thread that made it ends
    if (syscall(SYS_getppid) != g_fast_parent) syscall(SYS_exit, 0);
    for (;;) syscall(SYS_ppoll, nullptr, 0, nullptr, nullptr, 0);
    return 0;
}
[[noreturn]] static void fast_exit(int code) {
    fflush(nullptr);
    g_fast_parent = getpid();
    alignas(16) static char stack[16384];
    clone(mm_holder, stack + sizeof stack, CLONE_VM | SIGCHLD, nullptr);  // (failed: the exit unmaps it all itself)
    _exit(code);
}

static void usage() {
    fprintf(stderr, "usage: bananads [--config file] [--set key=value]... [--frontend kms|sdl|headless] game.nds\n");
}

int main(int argc, char** argv) {
    Config cfg;
    std::string conf, rom;
    std::vector<std::string> sets;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version") {
            printf("BananaDS %s\n", BANANADS_VERSION);
            return 0;
        } else if (a == "--config" && i + 1 < argc) conf = argv[++i];
        else if (a == "--set" && i + 1 < argc) sets.push_back(argv[++i]);
        else if (a == "--frontend" && i + 1 < argc) sets.push_back(std::string("frontend=") + argv[++i]);
        else if (a[0] == '-') {
            usage();
            return 2;
        } else rom = a;
    }
    if (rom.empty()) {
        usage();
        return 2;
    }

    if (!getenv("BANANADS_DATA")) {
        struct stat st;
        setenv("BANANADS_DATA", stat("/storage/.config/bananads", &st) == 0 ? "/storage/.config/bananads" : ".", 0);
    }
    if (conf.empty()) conf = std::string(getenv("BANANADS_DATA")) + "/bananads.conf";
    cfg.load_file(conf);
    for (auto& s : sets)
        if (!cfg.set_line(s)) {
            fprintf(stderr, "bananads: --set wants key=value, not \"%s\"\n", s.c_str());
            return 2;
        }

    g_log_level = (int)cfg.num("log.level", 1);
    g_log_time = cfg.flag("log.time", false);
    std::string logfile = cfg.str("log.file", "");
    if (!logfile.empty()) g_log = fopen(logfile.c_str(), "w");
    Log(LogLevel::Info, "BananaDS %s, %s\n", BANANADS_VERSION, rom.c_str());

    // the emulator in a scope of its own: its destructor stops the console, which logs, so it has to be gone before the
    // log file is closed (it wrote into the closed FILE: heap corruption at quit, a crash in about one quit of three)
    const int rc = [&]() -> int {
        Emu emu(cfg);
        if (!emu.load_rom(rom)) return 1;
        if (cfg.flag("resume.on_start", true) && emu.load_state(0))
            Log(LogLevel::Info, "[resume] resumed where the last session quit\n");

        // the default: a window where there is a display server, the panels where there isn't (a handheld's console)
        const bool display = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
        std::string def = "headless";
#ifdef BANANADS_KMS
        def = "kms";
#endif
#ifdef BANANADS_SDL
        if (display || def == "headless") def = "sdl";
#endif
        std::string fe = cfg.str("frontend", def);
        int code;
        if (fe == "headless") {
            code = run_headless(emu, cfg);
        }
#ifdef BANANADS_SDL
        else if (fe == "sdl") {
            code = run_sdl(emu, cfg);
        }
#endif
#ifdef BANANADS_KMS
        else if (fe == "kms") {
            code = run_kms(emu, cfg);
            start_mark("quit: frontend returned");
            if (cfg.flag("quit.fast", false)) {
                emu.shutdown();
                start_mark("quit: fast exit");
                if (FILE* f = g_log) {
                    g_log = nullptr;
                    fclose(f);
                }
                fast_exit(code);
            }
        }
#endif
        else {
            Log(LogLevel::Error, "frontend \"%s\" isn't in this build\n", fe.c_str());
            code = 2;
        }
        return code;
    }();
    start_mark("quit: emulator gone");
    // (a log line after this, from anything still running at exit, goes nowhere rather than into the closed FILE)
    if (FILE* f = g_log) {
        g_log = nullptr;
        fclose(f);
    }
    return rc;
}
