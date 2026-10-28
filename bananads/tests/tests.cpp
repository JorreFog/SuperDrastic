// Host tests for BananaDS's own parts (no ROM needed): settings, the Wi-Fi DNS bridge, the MAC, the mic gate.
// Built and run by CMake's `ctest` (or ./bananads-tests).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "config.h"
#include "mic.h"
#include "wifi.h"

using namespace bananads;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static void test_config() {
    Config c;
    CHECK(c.set_line("wifi.dns = altwfc"));
    CHECK(c.set_line("# a comment"));
    CHECK(c.set_line("mic.threshold=\"0.03\""));
    CHECK(!c.set_line("no equals sign"));
    CHECK(c.str("wifi.dns") == "altwfc");
    CHECK(std::fabs(c.real("mic.threshold", 1) - 0.03) < 1e-9);
    CHECK(c.num("missing", 7) == 7);
    CHECK(Config::env_name("mic.hold_ms") == "BANANADS_MIC_HOLD_MS");
    setenv("BANANADS_WIFI_DNS", "wiilink", 1);
    CHECK(c.str("wifi.dns") == "wiilink");  // the environment wins over the file
    unsetenv("BANANADS_WIFI_DNS");
    c.set("jit", "off");
    CHECK(!c.flag("jit", true));
    c.set("jit", "maybe");
    CHECK(c.flag("jit", true));  // not a flag: the default
}

static void test_wifi_names() {
    CHECK(wfc_dns_address("kaeru") == 0xB23E2BD4);    // 178.62.43.212
    CHECK(wfc_dns_address("altwfc") == 0xAC6858ED);   // 172.104.88.237
    CHECK(wfc_dns_address("wiilink") == 0xA7EBE524);  // 167.235.229.36
    CHECK(wfc_dns_address("1.2.3.4") == 0x01020304);
    CHECK(wfc_dns_address("system") == 0);
    CHECK(wfc_dns_address("nonsense") == 0xFFFFFFFF);

    auto a = wifi_mac_from_id("4c4c4544004e4b10805ab4c04f4e3432");
    auto b = wifi_mac_from_id("0123456789abcdef0123456789abcdef");
    CHECK(a[0] == 0x00 && a[1] == 0x09 && a[2] == 0xBF);  // Nintendo's prefix
    CHECK(a != b);                                           // two handhelds, two MACs
    CHECK(a == wifi_mac_from_id("4c4c4544004e4b10805ab4c04f4e3432"));  // and the same one every start
}

static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void test_dns_frames() {
    // A DNS query from the DS (10.64.0.16:1234) to slirp's DNS (10.64.0.2:53).
    uint8_t q[14 + 20 + 8 + 12] = {};
    const uint8_t dsmac[6] = {0x00, 0x09, 0xBF, 1, 2, 3};
    memcpy(q, WfcDns::kSlirpMAC, 6);
    memcpy(q + 6, dsmac, 6);
    q[12] = 0x08;
    uint8_t* ip = q + 14;
    ip[0] = 0x45;
    ip[9] = 17;
    ip[12] = 10, ip[13] = 64, ip[14] = 0, ip[15] = 16;
    ip[16] = 10, ip[17] = 64, ip[18] = 0, ip[19] = 2;
    uint8_t* udp = ip + 20;
    udp[0] = 0x04, udp[1] = 0xD2;
    udp[3] = 53;
    udp[5] = 8 + 12;
    CHECK(WfcDns::is_dns_query(q, sizeof q));
    udp[3] = 54;
    CHECK(!WfcDns::is_dns_query(q, sizeof q));  // not DNS
    udp[3] = 53;
    ip[19] = 3;
    CHECK(!WfcDns::is_dns_query(q, sizeof q));  // not to slirp's DNS
    CHECK(!WfcDns::is_dns_query(q, 30));         // too short

    uint8_t answer[40];
    for (int i = 0; i < 40; i++) answer[i] = (uint8_t)i;
    uint8_t f[200];
    int n = WfcDns::build_reply(f, sizeof f, dsmac, 0x0A400010, 1234, answer, 40);
    CHECK(n == 14 + 20 + 8 + 40);
    CHECK(!memcmp(f, dsmac, 6));
    CHECK(!memcmp(f + 6, WfcDns::kSlirpMAC, 6));
    CHECK(be16(f + 14 + 2) == 20 + 8 + 40);
    uint32_t sum = 0;  // a valid IPv4 header sums to 0xFFFF
    for (int i = 0; i < 20; i += 2) sum += be16(f + 14 + i);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    CHECK(sum == 0xFFFF);
    CHECK(be16(f + 34) == 53 && be16(f + 36) == 1234);
    CHECK(!memcmp(f + 42, answer, 40));
    CHECK(WfcDns::build_reply(f, 50, dsmac, 1, 1, answer, 40) == 0);  // doesn't fit
}

// Feeds `blocks` blocks of 1024 samples: noise at `level` (full scale 1) plus a speaker output at `out`.
static int feed(Mic& m, int blocks, double level, double out, uint32_t& seed) {
    std::vector<int16_t> in(1024), spk(512), got(4096);
    int nonzero = 0;
    for (int b = 0; b < blocks; b++) {
        for (auto& s : spk) s = (int16_t)(out * 32767 * ((b + (&s - spk.data())) % 2 ? 1 : -1));
        // A 1024-sample block at 44.1 kHz is ~23 ms: the sound thread hands over four 256-frame chunks meanwhile.
        for (int c = 0; c < 4; c++) m.note_output(spk.data(), (int)spk.size());
        for (auto& s : in) {
            seed = seed * 1664525u + 1013904223u;
            double v = level * 32767 * ((int32_t)seed / 2147483648.0) * 1.7320508;  // uniform: RMS = level
            s = (int16_t)std::max(-32768.0, std::min(32767.0, v));
        }
        m.push(in.data(), (int)in.size(), 44100);
        int n = m.read(got.data(), (int)got.size());
        for (int i = 0; i < n; i++) nonzero += got[i] != 0;
    }
    return nonzero;
}

static void test_mic() {
    uint32_t seed = 1;
    {
        Mic m;  // default gate, threshold 0.15
        CHECK(feed(m, 70, 0.005, 0, seed) == 0);  // learning the floor, then a quiet room: the game hears silence
        CHECK(feed(m, 5, 0.6, 0, seed) > 0);      // a blow: it hears it
        CHECK(m.meter().open);
        feed(m, 30, 0.005, 0, seed);              // quiet again: closes after the hold
        CHECK(!m.meter().open);
    }
    {
        Mic m;
        feed(m, 70, 0.005, 0, seed);
        // The game's own sound leaking into the mic, louder than the threshold alone (0.4 of the speaker's level):
        // learned in the first blocks, then kept out.
        feed(m, 60, 0.2, 0.5, seed);
        CHECK(feed(m, 40, 0.25, 0.6, seed) == 0);
        // Someone blowing while the game plays: still heard.
        int heard = feed(m, 5, 0.6, 0.2, seed);
        if (!heard) fprintf(stderr, "  meter: level %.3f floor %.3f bar %.3f\n", m.meter().level, m.meter().floor, m.meter().bar);
        CHECK(heard > 0);
    }
    {
        MicTuning t;
        t.threshold = 0;  // gate off: everything passes
        Mic m(t);
        CHECK(feed(m, 3, 0.01, 0, seed) > 0);
    }
    {
        Mic m;
        m.blow(100);  // the menu's Blow, without a mic
        std::vector<int16_t> got(8192);
        int n = m.read(got.data(), (int)got.size());
        CHECK(n == (int)(100 * Mic::kDSRate / 1000));
        double sum = 0;
        for (int i = 0; i < n; i++) sum += (double)got[i] * got[i];
        CHECK(std::sqrt(sum / n) > 2000);  // loud enough for a game's blow check
        CHECK(m.read(got.data(), 100) == 0);
    }
    {
        Mic m;  // resampling: 44.1 kHz in, ~47.7 kHz out
        MicTuning t;
        t.threshold = 0;
        m.set_tuning(t);
        std::vector<int16_t> in(1024, 1000), got(60000);
        int n = 0;
        for (int i = 0; i < 43; i++) {  // a second, read as it comes (as the emulator does)
            m.push(in.data(), (int)in.size(), 44100);
            n += m.read(got.data(), (int)got.size());
        }
        CHECK(std::abs(n - (int)(44032 * Mic::kDSRate / 44100)) < 64);
    }
    {
        Mic m;  // a second recorded while the game reads nothing: it then gets the newest kMaxLag samples, not
        MicTuning t;  // the whole second late
        t.threshold = 0;
        m.set_tuning(t);
        std::vector<int16_t> in(44100, 1000), got(60000);
        m.push(in.data(), (int)in.size(), 44100);
        CHECK(m.read(got.data(), (int)got.size()) == Mic::kMaxLag);
        CHECK(m.read(got.data(), (int)got.size()) == 0);
    }
}

int main() {
    test_config();
    test_wifi_names();
    test_dns_frames();
    test_mic();
    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("all BananaDS host tests passed\n");
    return 0;
}
