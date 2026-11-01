// BananaDS's side of the universal companion (~/banana-emu/common/COMPANION-SPEC.md, spec v1, gba's): the game's
// identity in /run/banana/game-id, and DS memory exported through banana-ui's /run/banana/companion.shm (bcomp.c,
// common 3423fe7, copied into third_party/bcomp). Without BANANA_COMPANION in the environment the export is off and
// costs nothing (bcomp_attach fails once at the start).
#include "companion.h"
#include "emu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
extern "C" {
#include "third_party/bcomp/bcomp.h"
}

static bcomp_producer prod;
static const char* const GAME_ID = "/run/banana/game-id";

static void write_game_id(const bananads::Emu& emu, const char* saves, const char* states) {
    const std::string rom = emu.rom_path();
    unsigned char h[0x200] = {};
    long size = 0;
    if (FILE* f = fopen(rom.c_str(), "rb")) {
        if (fread(h, 1, sizeof h, f) != sizeof h) memset(h, 0, sizeof h);
        fseek(f, 0, SEEK_END); size = ftell(f); fclose(f);
    }
    auto clean = [](const unsigned char* p, int n) {   // a header field as text: printable ASCII, trailing spaces/NULs cut
        std::string s;
        for (int i = 0; i < n && p[i]; i++) s += (p[i] >= 32 && p[i] < 127 && p[i] != '\n') ? (char)p[i] : '?';
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    };
    std::string base = rom.substr(rom.find_last_of('/') + 1);
    if (base.find('.') != std::string::npos) base = base.substr(0, base.find_last_of('.'));
    const std::string tmp = std::string(GAME_ID) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f, "version=1\nsystem=nds\ncore=BananaDS 0.1.0 (melonDS)\nfrontend=BananaDS\npid=%d\nrom=%s\nrom_member=\n"
               "rom_base=%s\nrom_size=%ld\nserial=%s\nheader_title=%s\nsave_dir=%s\nstate_dir=%s\nstarted=%ld\n",
            (int)getpid(), rom.c_str(), base.c_str(), size, clean(h + 12, 4).c_str(), clean(h, 12).c_str(),
            saves ? saves : "", states ? states : "", (long)time(nullptr));
    fclose(f);
    rename(tmp.c_str(), GAME_ID);
}

void companion_start(const bananads::Emu& emu, const char* saves, const char* states, const char* path) {
    struct stat st;
    if (stat("/run/banana", &st) == 0) write_game_id(emu, saves, states);
    // banana-ui's BANANA_COMPANION, else companion.path (tests: bananads.env passes only BANANADS_* keys)
    if (bcomp_attach(&prod, getenv("BANANA_COMPANION") || !path || !*path ? nullptr : path) != 0) return;
    bcomp_set_info(&prod, "nds", "BananaDS 0.1.0 (melonDS)", "BananaDS");
    // systems/nds.json: main RAM (RA 0x0000000), the ARM9's DTCM (RA 0x1000000; the export's start is RA's, the real
    // base is the game's CP15 choice)
    bcomp_add_region(&prod, "main", 0x02000000u, emu.main_ram_size(), BCOMP_R_RAM, 0x0000000u, emu.main_ram());
    bcomp_add_region(&prod, "dtcm", 0x0E000000u, 0x4000u, BCOMP_R_RAM, 0x1000000u, emu.dtcm());
    bcomp_publish(&prod);
    bcomp_set_state(&prod, 2, 0);
}

void companion_frame(uint64_t frame) {
    if (prod.shm) bcomp_service(&prod, frame);
}

void companion_stop() {
    if (prod.shm) { bcomp_set_state(&prod, 4, 0); bcomp_detach(&prod); }
    unlink(GAME_ID);
}
