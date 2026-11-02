// The companion overlay's presenter (src/compoverlay.cpp) against a painter made here with bovl.c: where the icon
// goes, which touches it takes from the game, the icon's resampling and blend in both frame formats, and the copy of
// the painter's pictures by their changed parts.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "compoverlay.h"

using namespace bananads;

static int fails = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static std::string path;

static std::vector<uint32_t> icon(int w, int h, uint32_t px) { return std::vector<uint32_t>((size_t)w * h, px); }

static bool next_touch(bovl_shm* s, uint32_t* tail, uint32_t type, int x, int y) {
    bovl_touch t;
    if (!bovl_touch_next(s, tail, &t, nullptr)) return false;
    return t.type == type && t.x == x && t.y == y;
}

static void test_placement_and_touch() {
    bovl_shm* s = bovl_create(path.c_str(), 640, 480, 2);
    CHECK(s);
    if (!s) return;
    CompOverlay o;
    CHECK(!o.attach("/nonexistent/overlay.shm", 640, 480) && !o.on());
    CHECK(o.attach(path.c_str(), 640, 480) && o.on());
    o.ready(true);
    CHECK(s->presenter_state == BOVL_PRESENTER_READY);
    const OvlRect panel{0, 0, 640, 480};
    // nothing from the painter yet: no icon, every touch the game's
    o.service(panel);
    CHECK(!o.icon_shown());
    CHECK(!o.touch(true, 620, 460, 1) && !o.touch(false, 620, 460, 2));
    // the painter: a 44x44 icon in the bottom-right corner, 4 px in, 6 px of slop around it
    bovl_painter_start(s);
    auto ic = icon(44, 44, 0x80402010u);
    bovl_set_icon(s, ic.data(), 44, 44, 1, 2, BOVL_CORNER_BR, 4, 6);
    CHECK(o.service(panel));
    CHECK(o.icon_shown());
    const OvlRect r = o.icon_panel();
    CHECK(r.x == 640 - 4 - 44 && r.y == 480 - 4 - 44 && r.w == 44 && r.h == 44);
    CHECK(s->icon_rect.x == 592 && s->icon_rect.y == 432 && s->icon_rect.w == 44 && s->view_rect.w == 640);
    CHECK(!o.service(panel));   // (nothing new)
    // a press elsewhere is the game's, all of it
    CHECK(!o.touch(true, 100, 100, 10) && !o.touch(true, 110, 100, 11) && !o.touch(false, 110, 100, 12));
    // a tap on the icon (in its slop) opens; the game sees none of that press
    CHECK(o.touch(true, 588, 430, 20));
    CHECK(o.is_open() && bovl_is_open(s) && bovl_open_seq(s) == 1);
    CHECK(o.touch(true, 300, 300, 21) && o.touch(false, 300, 300, 22));   // (the rest of the press: swallowed)
    uint32_t tail = 0;
    bovl_touch t;
    CHECK(!bovl_touch_next(s, &tail, &t, nullptr));
    // open: every press goes to the painter (down, moves when it moved, up), none to the game
    CHECK(o.touch(true, 100, 200, 30) && o.touch(true, 100, 200, 31) && o.touch(true, 120, 210, 32) && o.touch(false, 0, 0, 33));
    CHECK(next_touch(s, &tail, BOVL_TOUCH_DOWN, 100, 200));
    CHECK(next_touch(s, &tail, BOVL_TOUCH_MOVE, 120, 210));
    CHECK(next_touch(s, &tail, BOVL_TOUCH_UP, 120, 210));   // (where it last was)
    CHECK(!bovl_touch_next(s, &tail, &t, nullptr));
    // Back to game sits where the icon was: a tap there closes
    CHECK(o.touch(true, 600, 440, 40) && !o.is_open() && !bovl_is_open(s));
    CHECK(o.touch(false, 600, 440, 41));
    CHECK(!bovl_touch_next(s, &tail, &t, nullptr));
    // the painter asks to open; the game's press held then is let go and the rest of it swallowed
    CHECK(!o.touch(true, 50, 50, 50));
    bovl_request_open(s, 1);
    CHECK(o.service(panel) && o.is_open() && o.release_game);
    o.release_game = false;
    CHECK(o.touch(true, 60, 50, 51) && o.touch(false, 60, 50, 52));
    CHECK(!bovl_touch_next(s, &tail, &t, nullptr));
    // closed by the painter in the middle of a press on the overlay: the painter gets its end
    CHECK(o.touch(true, 10, 10, 60));
    bovl_request_open(s, 0);
    CHECK(o.service(panel) && !o.is_open());
    CHECK(next_touch(s, &tail, BOVL_TOUCH_DOWN, 10, 10) && next_touch(s, &tail, BOVL_TOUCH_UP, 10, 10));
    CHECK(o.touch(true, 20, 20, 61) && o.touch(false, 20, 20, 62));   // (that press stays away from the game)
    // a smaller game picture (a shader drawing the DS screen 512x384 in the middle): the icon in its corner
    CHECK(o.service(OvlRect{64, 48, 512, 384}));
    CHECK(o.icon_panel().x == 64 + 512 - 4 - 44 && o.icon_panel().y == 48 + 384 - 4 - 44);
    CHECK(s->view_rect.x == 64 && s->view_rect.w == 512);
    // the icon turned off: not drawn, taps there are the game's
    bovl_set_icon(s, nullptr, 44, 44, 0, 0, BOVL_CORNER_BR, 4, 6);
    CHECK(o.service(panel) && !o.icon_shown());
    CHECK(!o.touch(true, 620, 460, 70) && !o.touch(false, 620, 460, 71));
    CHECK(s->icon_rect.w == 0);
    // the painter gone while open: closed
    bovl_set_icon(s, nullptr, 44, 44, 1, 0, BOVL_CORNER_BR, 4, 6);
    o.service(panel);
    o.set_open(true);
    bovl_painter_stop(s);
    o.service(panel);
    CHECK(!o.is_open() && !o.icon_shown());
    // a picture of another size than the panel (the plane scales it): touches in picture coordinates
    o.detach();
    CHECK(s->presenter_state == BOVL_PRESENTER_NONE);
    bovl_unmap(s);
    s = bovl_create(path.c_str(), 320, 240, 2);
    CHECK(o.attach(path.c_str(), 640, 480));
    bovl_painter_start(s);
    ic = icon(22, 22, 0xFF000000u);
    bovl_set_icon(s, ic.data(), 22, 22, 1, 0, BOVL_CORNER_TL, 2, 0);
    o.service(panel);
    CHECK(o.icon_panel().x == 4 && o.icon_panel().y == 4 && o.icon_panel().w == 44);
    CHECK(s->icon_rect.x == 2 && s->icon_rect.w == 22);
    CHECK(o.touch(true, 10, 10, 1) && o.is_open() && o.touch(false, 10, 10, 2));
    CHECK(o.touch(true, 400, 300, 3) && o.touch(false, 400, 300, 4));
    tail = 0;
    CHECK(next_touch(s, &tail, BOVL_TOUCH_DOWN, 200, 150));
    o.detach();
    bovl_unmap(s);
    unlink(path.c_str());
}

static void test_resample_blend() {
    // an opaque 4x4 picture to 2x2: the average of each 2x2 block; to 8x8: each pixel twice
    std::vector<uint32_t> src(16);
    for (int i = 0; i < 16; i++) src[i] = 0xFF000000u | (uint32_t)(i * 16) << 16;
    OvlPic h = ovl_resample(src.data(), 4, 4, 2, 2);
    CHECK(h.w == 2 && h.h == 2 && h.px[0] == (0xFF000000u | (uint32_t)((0 + 16 + 64 + 80) / 4) << 16));
    OvlPic d = ovl_resample(src.data(), 4, 4, 8, 8);
    CHECK(d.px[0] == src[0] && d.px[1] == src[0] && d.px[2] == src[1] && d.px[8 * 7 + 7] == src[15]);
    // 0.8 times (the RG DS's 2x frames): the edges are mixes, alpha never below a colour (premultiplied)
    std::vector<uint32_t> half(10 * 10, 0x80808080u);
    OvlPic e = ovl_resample(half.data(), 10, 10, 8, 8);
    bool premul = true;
    for (uint32_t v : e.px) premul = premul && ((v >> 16) & 255) <= (v >> 24) && (v & 255) <= (v >> 24);
    CHECK(e.w == 8 && premul && e.px[0] == 0x80808080u);

    // blends: opaque replaces, half mixes, clear leaves the frame's pixel alone (and isn't even read)
    OvlPic p;
    p.w = 3;
    p.h = 1;
    p.px = {0xFFFF0000u, 0x80000080u, 0};
    std::vector<uint32_t> f32(4 * 2, 0x00204060u);
    ovl_blend32((uint8_t*)f32.data(), 16, 4, 2, p, 1, 1);
    CHECK(f32[5] == 0xFF0000u);
    CHECK(f32[6] == (uint32_t)(((0x20 * 127 + 127) / 255) << 16 | ((0x40 * 127 + 127) / 255) << 8 | (0x80 + (0x60 * 127 + 127) / 255)));
    CHECK(f32[7] == 0x00204060u && f32[0] == 0x00204060u && f32[4] == 0x00204060u);
    // clipped at the buffer's edges (no write outside it)
    std::vector<uint32_t> g(4 * 2 + 4, 0x11111111u);
    ovl_blend32((uint8_t*)g.data(), 16, 4, 2, p, 2, -1);
    ovl_blend32((uint8_t*)g.data(), 16, 4, 2, p, 3, 1);
    CHECK(g[7] == 0xFF0000u && g[8] == 0x11111111u && g[11] == 0x11111111u);
    // RGB565
    std::vector<uint16_t> f16(4, 0x07E0);   // green
    ovl_blend565((uint8_t*)f16.data(), 8, 4, 1, p, 0, 0);
    CHECK(f16[0] == 0xF800 && f16[2] == 0x07E0);
    CHECK((f16[1] >> 5 & 63) > 25 && (f16[1] >> 5 & 63) < 38 && (f16[1] & 31) > 12);   // half green, half blue

    int r0, r1;
    ovl_ds_rows(432, 44, 480, &r0, &r1);   // the RG DS's panel-sized output
    CHECK(r0 == 172 && r1 == 191);
    ovl_ds_rows(345, 36, 384, &r0, &r1);   // a 2x frame
    CHECK(r0 == 172 && r1 == 191);
    ovl_ds_rows(700, 100, 768, &r0, &r1);
    CHECK(r0 == 175 && r1 == 192);
}

static void test_icon_for() {
    bovl_shm* s = bovl_create(path.c_str(), 640, 480, 2);
    CompOverlay o;
    CHECK(o.attach(path.c_str(), 640, 480));
    int x = -1, y = -1;
    CHECK(o.icon_for(512, 384, &x, &y) == nullptr);   // no icon yet
    bovl_painter_start(s);
    auto ic = icon(40, 40, 0xFFFFFFFFu);
    bovl_set_icon(s, ic.data(), 40, 40, 1, 0, BOVL_CORNER_BR, 0, 0);
    o.service(OvlRect{0, 0, 640, 480});
    const OvlPic* a = o.icon_for(512, 384, &x, &y);   // a 2x DS frame the plane scales to 640x480
    CHECK(a && a->w == 32 && a->h == 32 && x == 480 && y == 352);
    const OvlPic* b = o.icon_for(640, 480, &x, &y);   // a panel-sized buffer: 1:1
    CHECK(b && b->w == 40 && x == 600 && y == 440);
    CHECK(o.icon_for(512, 384, &x, &y) == a);         // (both kept)
    o.set_open(true);
    CHECK(o.icon_for(640, 480, &x, &y) == b);         // (drawn while the picture for the open isn't there yet)
    o.detach();
    bovl_unmap(s);
    unlink(path.c_str());
}

static void test_copy() {
    bovl_shm* s = bovl_create(path.c_str(), 64, 48, 2);
    CompOverlay o;
    CHECK(o.attach(path.c_str(), 640, 480));
    bovl_painter_start(s);
    std::vector<uint32_t> canvas(64 * 48, 0xFF0000u);
    bovl_slot h;
    CHECK(!o.claim(0, &h));
    CHECK(bovl_paint_xrgb(s, canvas.data(), 64, bovl_rect{0, 0, 64, 48}, 1) == 1);
    // the presenter's two buffers: one RGB565 (DRM pitch wider than the picture), one XRGB8888
    std::vector<uint8_t> b565(160 * 48, 0), b32(256 * 48, 0);
    CHECK(o.claim(0, &h) && h.frame == 1 && h.open_seq == 1);
    o.copy_into(b565.data(), 160, true, 0, h);
    o.done();
    CHECK(((uint16_t*)b565.data())[0] == 0xF800 && ((uint16_t*)b565.data())[80 * 47 + 63] == 0xF800);
    CHECK(((uint16_t*)b565.data())[64] == 0);   // (the pitch's padding untouched)
    CHECK(!o.claim(1, &h));
    canvas[10 * 64 + 5] = 0x0000FFu;
    CHECK(bovl_paint_xrgb(s, canvas.data(), 64, bovl_rect{5, 10, 1, 1}, 1) == 2);
    CHECK(o.claim(1, &h) && h.frame == 2);
    o.copy_into(b32.data(), 256, false, 0, h);   // (holds nothing: all of it)
    o.done();
    CHECK(((uint32_t*)b32.data())[10 * 64 + 5] == 0x0000FFu && ((uint32_t*)b32.data())[0] == 0xFF0000u);
    CHECK(o.copy_bytes == 64 * 48 * 2 + 64 * 48 * 4);
    canvas[0] = 0x00FF00u;
    CHECK(bovl_paint_xrgb(s, canvas.data(), 64, bovl_rect{0, 0, 1, 1}, 1) == 3);
    CHECK(o.claim(2, &h) && h.frame == 3);
    const uint64_t before = o.copy_bytes;
    o.copy_into(b565.data(), 160, true, 1, h);   // holds frame 1: frames 2 and 3's changes, a 6x11 rectangle
    o.done();
    CHECK(o.copy_bytes - before == 6 * 11 * 2);
    CHECK(((uint16_t*)b565.data())[0] == 0x07E0 && ((uint16_t*)b565.data())[10 * 80 + 5] == 0x001F);
    CHECK(s->st_copies == 3 && s->reading == BOVL_NONE);
    o.detach();
    bovl_unmap(s);
    unlink(path.c_str());
}

int main() {
    const char* tmp = getenv("TMPDIR");
    path = std::string(tmp && *tmp ? tmp : "/tmp") + "/compoverlay-test-" + std::to_string(getpid()) + ".shm";
    test_placement_and_touch();
    test_resample_blend();
    test_icon_for();
    test_copy();
    if (fails) {
        fprintf(stderr, "%d FAILED\n", fails);
        return 1;
    }
    fprintf(stderr, "compoverlay: all passed\n");
    return 0;
}
