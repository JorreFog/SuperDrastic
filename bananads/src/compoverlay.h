// The companion overlay's presenter (~/banana-emu/common/COMPANION-SPEC.md section 10; bovl.h from common, copied
// into third_party/bcomp): banana-ui paints a picture of the DS bottom panel's size into
// /run/banana/companion-overlay.shm (BANANA_COMPANION_OVERLAY) and an icon picture with its badge; BananaDS draws the
// icon over the game's bottom screen (a blend of a few thousand pixels into the frame on its way to the panel), and
// while the overlay is open shows banana-ui's picture on that panel instead of the game's bottom screen. The game
// keeps running and keeps every button; the touchscreen is the overlay's: a tap on the icon opens and closes it (the
// game never sees that press), touches while it is open go to banana-ui, everything else to the game as before.
//
// This part has no DRM in it (host-tested: tests/compoverlay_test.cpp): where the icon goes, which touches it takes,
// the icon resampled for the buffer it is blended into and the blend itself, and the copy of a new picture into a
// buffer of the frontend's. frontend_kms.cpp owns the buffers and the commits.
#pragma once
#include <cstdint>
#include <vector>

extern "C" {
#include "third_party/bcomp/bovl.h"
}

namespace bananads {

struct OvlRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    bool in(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// a picture of premultiplied 0xAARRGGBB pixels
struct OvlPic {
    int w = 0, h = 0;
    std::vector<uint32_t> px;
};
// src (premultiplied, sw x sh) to dw x dh: each output pixel the average of the source area it covers (shrinking
// and growing alike), so a 44-pixel icon blended into a DS frame of 0.8 times the panel's size keeps its edges soft
OvlPic ovl_resample(const uint32_t* src, int sw, int sh, int dw, int dh);
// p over a buffer's pixels at (x, y), clipped to bw x bh; XRGB8888 or RGB565 (the frame's own format). Each row's
// span is read once into cached memory and written back once: scanout buffers are uncached, slow to read piecemeal
void ovl_blend32(uint8_t* map, int pitch, int bw, int bh, const OvlPic& p, int x, int y);
void ovl_blend565(uint8_t* map, int pitch, int bw, int bh, const OvlPic& p, int x, int y);
// rows [y0, y0 + h) of a picture of src_h rows: the DS lines (0..191) they show
void ovl_ds_rows(int y0, int h, int src_h, int* row0, int* row1);

class CompOverlay {
public:
    ~CompOverlay() { detach(); }
    // attach to the painter's file (path null: BANANA_COMPANION_OVERLAY); W x H: the panels' mode. False: no overlay
    bool attach(const char* path, int W, int H);
    void detach();
    bool on() const { return shm_ != nullptr; }
    bovl_shm* shm() const { return shm_; }
    void ready(bool r);               // the presenter can show pictures (its buffers made and tested)

    // each presenter wake-up: the painter's icon and requests; view: where the DS bottom screen's picture is on its
    // panel (panel px), for the icon's corner. Returns true when the picture on the panel changes (icon, open)
    bool service(const OvlRect& view);
    bool icon_shown() const;          // the icon drawn over the game now (closed, the painter ready, icon on)
    bool is_open() const { return open_; }
    void set_open(bool open);
    // a touch on the panel the DS's bottom screen is on (panel px): true when the overlay takes it (the game must not
    // see it), false for the game. ms: CLOCK_MONOTONIC
    bool touch(bool down, int px, int py, uint32_t ms);
    // a press on the game that started before the overlay opened (or a tap that opened it from the painter's side):
    // the frontend lets go of the game's touch when this is set, then clears it
    bool release_game = false;

    // the icon for a buffer of bw x bh that shows the whole panel (the plane scales it): resampled once per size.
    // Null when no icon is to be drawn. *x, *y: where in the buffer
    const OvlPic* icon_for(int bw, int bh, int* x, int* y);

    // a new picture for a buffer that holds frame `has` (0: nothing): claim it (false: none newer than `newer_than`),
    // then copy_into + done. The copy brings `has` up to the claimed frame (only what changed since, when the painter's
    // history reaches back that far). fmt565: the buffer is RGB565 (else XRGB8888)
    bool claim(uint64_t newer_than, bovl_slot* hdr);
    void copy_into(uint8_t* map, int pitch, bool fmt565, uint64_t has, const bovl_slot& hdr);
    void done();
    uint32_t open_seq() const { return shm_ ? bovl_open_seq(shm_) : 0; }
    uint32_t icon_seq() const { return icon_seq_; }   // (the painter's icon last read: a new one changes it)

    // statistics for the log at the quit (also in the file's st_* counters)
    uint64_t icon_draws = 0, icon_ns = 0, copies = 0, copy_bytes = 0, copy_ns = 0, frames_shown = 0, toggles = 0;
    int pic_w() const { return shm_ ? (int)shm_->width : 0; }
    int pic_h() const { return shm_ ? (int)shm_->height : 0; }
    OvlRect icon_panel() const { return icon_; }   // where the icon is on the panel (panel px)

private:
    bovl_shm* shm_ = nullptr;
    int W_ = 0, H_ = 0;
    bool open_ = false, painting_ = false;
    int claimed_ = -1;                // the slot claimed (claim .. done)
    uint32_t icon_seq_ = 0;
    bovl_icon* ic_ = nullptr;         // the painter's icon as last read (heap: ~57 KB)
    bool have_icon_ = false;
    OvlRect view_, icon_, hit_;       // panel px
    // touch: a press that began on the icon (toggled, the rest of it swallowed), or on the open overlay (sent)
    bool down_ = false, swallow_ = false, capture_ = false;
    int last_x_ = -1, last_y_ = -1;
    struct Cached { int bw = 0, bh = 0; OvlRect r; OvlPic pic; uint32_t seq = 0; };
    Cached cache_[2];
    int cache_next_ = 0;
    void place();
    int to_pic_x(int px) const { return W_ ? px * (int)shm_->width / W_ : px; }
    int to_pic_y(int py) const { return H_ ? py * (int)shm_->height / H_ : py; }
};

}  // namespace bananads
