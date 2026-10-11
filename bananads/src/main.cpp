// bananads [options] <game.nds>
//   --config <file>     settings file (default: $BANANADS_DATA/bananads.conf, BANANADS_DATA defaults to
//                       /storage/.config/bananads when that exists, else the current directory)
//   --set key=value     one setting, over the file and the environment's BANANADS_* (repeatable)
//   --frontend <name>   sdl (a desktop window), headless (benchmark/tests); kms is milestone D in docs/PLAN.md
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
    fprintf(stderr, "usage: bananads [--config file] [--set key=value]... [--frontend sdl|headless] game.nds\n");
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

    Emu emu(cfg);
    if (!emu.load_rom(rom)) return 1;
    if (cfg.flag("resume.on_start", true) && emu.load_state(0))
        Log(LogLevel::Info, "[resume] resumed where the last session quit\n");

    std::string fe = cfg.str("frontend",
#ifdef BANANADS_SDL
                             "sdl"
#else
                             "headless"
#endif
    );
    int rc;
    if (fe == "headless") {
        rc = run_headless(emu, cfg);
    }
#ifdef BANANADS_SDL
    else if (fe == "sdl") {
        rc = run_sdl(emu, cfg);
    }
#endif
    else {
        Log(LogLevel::Error, "frontend \"%s\" isn't in this build (see docs/PLAN.md)\n", fe.c_str());
        rc = 2;
    }
    if (g_log) fclose(g_log);
    return rc;
}
