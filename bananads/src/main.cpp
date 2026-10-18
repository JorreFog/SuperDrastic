// bananads [options] <game.nds>
//   --config <file>     settings file (default: $BANANADS_DATA/bananads.conf, BANANADS_DATA defaults to
//                       /storage/.config/bananads when that exists, else the current directory)
//   --set key=value     one setting, over the file and the environment's BANANADS_* (repeatable)
//   --frontend <name>   kms (the handheld's panels: the default without a display server), sdl (a desktop window),
//                       headless (benchmark/tests)
//   --version
#include <sys/stat.h>

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
}

using namespace bananads;
using melonDS::Platform::Log;
using melonDS::Platform::LogLevel;

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
        }
#endif
        else {
            Log(LogLevel::Error, "frontend \"%s\" isn't in this build\n", fe.c_str());
            code = 2;
        }
        return code;
    }();
    if (g_log) fclose(g_log);
    return rc;
}
