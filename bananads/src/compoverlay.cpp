// The companion overlay's presenter: see compoverlay.h.
#include "compoverlay.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace bananads {

static uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

OvlPic ovl_resample(const uint32_t* src, int sw, int sh, int dw, int dh) {
    OvlPic out;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return out;
    out.w = dw;
    out.h = dh;
    out.px.assign((size_t)dw * dh, 0);
    // across, then down: each output column/row the source span [i * s / d, (i + 1) * s / d) with partial edges
    std::vector<float> mid((size_t)dw * sh * 4);
    const double kx = (double)sw / dw, ky = (double)sh / dh;
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < dw; x++) {
            const double a = x * kx, b = (x + 1) * kx;
            float acc[4] = {0, 0, 0, 0};
            for (int sx = (int)a; sx < sw && sx < b; sx++) {
                const double wgt = std::min(b, (double)sx + 1) - std::max(a, (double)sx);
                if (wgt <= 0) continue;
                const uint32_t p = src[(size_t)y * sw + sx];
                for (int c = 0; c < 4; c++) acc[c] += (float)(wgt * ((p >> (24 - 8 * c)) & 255));
            }
            for (int c = 0; c < 4; c++) mid[((size_t)y * dw + x) * 4 + c] = (float)(acc[c] / kx);
        }
    for (int x = 0; x < dw; x++)
        for (int y = 0; y < dh; y++) {
            const double a = y * ky, b = (y + 1) * ky;
            float acc[4] = {0, 0, 0, 0};
            for (int sy = (int)a; sy < sh && sy < b; sy++) {
                const double wgt = std::min(b, (double)sy + 1) - std::max(a, (double)sy);
                if (wgt <= 0) continue;
                for (int c = 0; c < 4; c++) acc[c] += (float)(wgt * mid[((size_t)sy * dw + x) * 4 + c]);
            }
            uint32_t v[4];
            for (int c = 0; c < 4; c++) v[c] = (uint32_t)std::clamp((int)std::lround(acc[c] / ky), 0, 255);
            for (int c = 1; c < 4; c++) v[c] = std::min(v[c], v[0]);   // (premultiplied: no channel above alpha)
            out.px[(size_t)y * dw + x] = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
        }
    return out;
}

// the columns of a row that aren't fully see-through: only those are read and written
static bool row_span(const OvlPic& p, int y, int* x0, int* x1) {
    const uint32_t* r = &p.px[(size_t)y * p.w];
    int a = 0, b = p.w;
    while (a < b && !(r[a] >> 24)) a++;
    while (b > a && !(r[b - 1] >> 24)) b--;
    *x0 = a;
    *x1 = b;
    return a < b;
}

static inline uint32_t over(uint32_t s, uint32_t d) {   // premultiplied s over opaque d (0x00RRGGBB)
    const uint32_t a = s >> 24;
    if (a == 255) return s & 0xFFFFFF;
    const uint32_t k = 255 - a;
    const uint32_t r = ((s >> 16) & 255) + (((d >> 16) & 255) * k + 127) / 255;
    const uint32_t g = ((s >> 8) & 255) + (((d >> 8) & 255) * k + 127) / 255;
    const uint32_t b = (s & 255) + ((d & 255) * k + 127) / 255;
    return std::min(r, 255u) << 16 | std::min(g, 255u) << 8 | std::min(b, 255u);
}

template <typename Px, typename Load, typename Store>
static void blend_rows(uint8_t* map, int pitch, int bw, int bh, const OvlPic& p, int x, int y, Load ld, Store st) {
    Px row[512];
    for (int py = 0; py < p.h; py++) {
        const int by = y + py;
        if (by < 0 || by >= bh) continue;
        int a, b;
        if (!row_span(p, py, &a, &b)) continue;
        a = std::max(a, -x);
        b = std::min(b, bw - x);
        if (a >= b || b - a > 512) continue;
        Px* d = (Px*)(map + (size_t)by * pitch) + x + a;
        const int n = b - a;
        memcpy(row, d, (size_t)n * sizeof(Px));   // (one read of the uncached row)
        const uint32_t* s = &p.px[(size_t)py * p.w + a];
        for (int i = 0; i < n; i++)
            if (s[i] >> 24) row[i] = st(over(s[i], ld(row[i])));
        memcpy(d, row, (size_t)n * sizeof(Px));
    }
}

void ovl_blend32(uint8_t* map, int pitch, int bw, int bh, const OvlPic& p, int x, int y) {
    blend_rows<uint32_t>(map, pitch, bw, bh, p, x, y, [](uint32_t v) { return v & 0xFFFFFF; },
                         [](uint32_t v) { return v; });
}

void ovl_blend565(uint8_t* map, int pitch, int bw, int bh, const OvlPic& p, int x, int y) {
    blend_rows<uint16_t>(map, pitch, bw, bh, p, x, y,
        [](uint16_t c) {   // (as the display expands 5/6 bits to 8)
            const uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
            return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
        },
        [](uint32_t v) { return (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) | ((v >> 3) & 0x001F)); });
}

void ovl_ds_rows(int y0, int h, int src_h, int* row0, int* row1) {
    if (src_h <= 0 || h <= 0) { *row0 = *row1 = 0; return; }
    *row0 = std::clamp(y0 * 192 / src_h, 0, 192);
    *row1 = std::clamp((int)(((int64_t)(y0 + h) * 192 + src_h - 1) / src_h), 0, 192);
}

// ---- the presenter ----

bool CompOverlay::attach(const char* path, int W, int H) {
    detach();
    if (bovl_attach(path, &shm_) != 0 || W <= 0 || H <= 0) {
        if (shm_) { bovl_detach(shm_); shm_ = nullptr; }
        return false;
    }
    W_ = W;
    H_ = H;
    ic_ = new bovl_icon();
    icon_seq_ = 0;
    have_icon_ = false;
    open_ = false;
    painting_ = false;
    if (bovl_is_open(shm_)) bovl_set_open(shm_, 0);   // (a file left open by a presenter before: closed now)
    return true;
}

void CompOverlay::detach() {
    if (shm_) {
        shm_->st_frames = frames_shown;
        bovl_detach(shm_);
    }
    shm_ = nullptr;
    delete ic_;
    ic_ = nullptr;
    have_icon_ = false;
    open_ = false;
}

void CompOverlay::ready(bool r) {
    if (shm_) bovl_presenter_ready(shm_, r ? 1 : 0);
}

bool CompOverlay::icon_shown() const {
    return shm_ && have_icon_ && ic_->on && ic_->w && ic_->h && bovl_painting(shm_);
}

void CompOverlay::place() {
    if (!have_icon_ || !ic_->w || !ic_->h) {
        icon_ = hit_ = OvlRect();
        return;
    }
    const int pw = (int)shm_->width, ph = (int)shm_->height;
    const int iw = (int)ic_->w * W_ / pw, ih = (int)ic_->h * H_ / ph;
    const int mx = (int)ic_->margin * W_ / pw, my = (int)ic_->margin * H_ / ph;
    const bool right = ic_->corner == BOVL_CORNER_BR || ic_->corner == BOVL_CORNER_TR;
    const bool bottom = ic_->corner == BOVL_CORNER_BR || ic_->corner == BOVL_CORNER_BL;
    icon_.w = iw;
    icon_.h = ih;
    icon_.x = right ? view_.x + view_.w - mx - iw : view_.x + mx;
    icon_.y = bottom ? view_.y + view_.h - my - ih : view_.y + my;
    const int s = (int)ic_->slop * W_ / pw;
    hit_ = OvlRect{icon_.x - s, icon_.y - s, iw + 2 * s, ih + 2 * s};
    const auto pic = [&](const OvlRect& r) {
        return bovl_rect{to_pic_x(r.x), to_pic_y(r.y), to_pic_x(r.x + r.w) - to_pic_x(r.x), to_pic_y(r.y + r.h) - to_pic_y(r.y)};
    };
    bovl_set_rects(shm_, icon_shown() ? pic(icon_) : bovl_rect{0, 0, 0, 0}, pic(view_));
}

bool CompOverlay::service(const OvlRect& view) {
    if (!shm_) return false;
    bool changed = false;
    if (bovl_icon_read(shm_, &icon_seq_, ic_) == 1) {
        have_icon_ = true;
        changed = true;
    }
    uint32_t want;
    if (bovl_take_request(shm_, &want) && (want != 0) != open_) {
        set_open(want != 0);
        changed = true;
    }
    if (open_ && !bovl_painting(shm_)) {   // (the painter went away: nothing to show)
        set_open(false);
        changed = true;
    }
    if (bovl_painting(shm_) != painting_) {
        painting_ = !painting_;
        changed = true;
    }
    if (view.x != view_.x || view.y != view_.y || view.w != view_.w || view.h != view_.h) {
        view_ = view;
        changed = true;
    }
    if (changed) place();
    return changed;
}

void CompOverlay::set_open(bool o) {
    if (!shm_ || o == open_) return;
    open_ = o;
    toggles++;
    if (o && down_ && !swallow_ && !capture_) {   // the game's press under it: let go, the rest of it swallowed
        release_game = true;
        swallow_ = true;
    }
    if (!o && capture_) {   // closed during a press on the overlay: it ends there
        bovl_touch_push(shm_, BOVL_TOUCH_UP, last_x_, last_y_, (uint32_t)(now_ns() / 1000000));
        capture_ = false;
        swallow_ = down_;
    }
    bovl_set_open(shm_, o ? 1 : 0);
    place();
}

bool CompOverlay::touch(bool down, int px, int py, uint32_t ms) {
    if (!shm_) return false;
    const int x = to_pic_x(px), y = to_pic_y(py);
    if (down && !down_) {   // a press begins
        down_ = true;
        if ((icon_shown() || open_) && hit_.in(px, py)) {   // the icon (or Back to game, in its place): open/close
            swallow_ = true;
            set_open(!open_);
            return true;
        }
        if (open_) {
            capture_ = true;
            last_x_ = x;
            last_y_ = y;
            bovl_touch_push(shm_, BOVL_TOUCH_DOWN, x, y, ms);
            return true;
        }
        return false;
    }
    if (down) {   // it moves
        if (swallow_) return true;
        if (capture_) {
            if (x != last_x_ || y != last_y_) bovl_touch_push(shm_, BOVL_TOUCH_MOVE, x, y, ms);
            last_x_ = x;
            last_y_ = y;
            return true;
        }
        return false;
    }
    if (!down_) return false;
    down_ = false;   // it ends
    if (swallow_) {
        swallow_ = false;
        return true;
    }
    if (capture_) {
        capture_ = false;
        bovl_touch_push(shm_, BOVL_TOUCH_UP, last_x_, last_y_, ms);
        return true;
    }
    return false;
}

const OvlPic* CompOverlay::icon_for(int bw, int bh, int* x, int* y) {
    if (!icon_shown() || icon_.empty() || bw <= 0 || bh <= 0) return nullptr;
    // the icon's rectangle in the buffer (which shows the whole panel: the plane scales it, or it is the panel's size)
    const int x0 = (int)((int64_t)icon_.x * bw / W_), y0 = (int)((int64_t)icon_.y * bh / H_);
    const int x1 = (int)((int64_t)(icon_.x + icon_.w) * bw / W_), y1 = (int)((int64_t)(icon_.y + icon_.h) * bh / H_);
    const OvlRect r{x0, y0, x1 - x0, y1 - y0};
    if (r.empty()) return nullptr;
    for (auto& c : cache_)
        if (c.bw == bw && c.bh == bh && c.seq == icon_seq_ && c.r.x == r.x && c.r.y == r.y && c.r.w == r.w && c.r.h == r.h) {
            *x = r.x;
            *y = r.y;
            return &c.pic;
        }
    Cached& c = cache_[cache_next_++ & 1];   // (two: the shader's panel-sized output and the DS-sized frames, in turn)
    c.bw = bw;
    c.bh = bh;
    c.r = r;
    c.seq = icon_seq_;
    c.pic = ovl_resample(ic_->px, (int)ic_->w, (int)ic_->h, r.w, r.h);
    *x = r.x;
    *y = r.y;
    return &c.pic;
}

bool CompOverlay::claim(uint64_t newer_than, bovl_slot* hdr) {
    if (!shm_) return false;
    claimed_ = bovl_claim(shm_, newer_than, hdr);
    return claimed_ >= 0;
}

void CompOverlay::copy_into(uint8_t* map, int pitch, bool fmt565, uint64_t has, const bovl_slot& hdr) {
    if (!shm_ || claimed_ < 0) return;
    const uint64_t t0 = now_ns();
    const bovl_rect c = bovl_changed(shm_, has, hdr.frame);
    const uint16_t* src = bovl_pixels(shm_, (uint32_t)claimed_);
    const size_t ss = shm_->stride / 2;
    for (int y = c.y; y < c.y + c.h; y++) {
        const uint16_t* s = src + (size_t)y * ss + c.x;
        uint8_t* d = map + (size_t)y * pitch;
        if (fmt565) memcpy(d + (size_t)c.x * 2, s, (size_t)c.w * 2);
        else {
            uint32_t* d32 = (uint32_t*)d + c.x;
            for (int i = 0; i < c.w; i++) {
                const uint32_t v = s[i], r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                d32[i] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        }
    }
    const uint64_t dt = now_ns() - t0;
    copies++;
    copy_bytes += (uint64_t)c.w * c.h * (fmt565 ? 2 : 4);
    copy_ns += dt;
    shm_->st_copies = copies;
    shm_->st_bytes = copy_bytes;
    shm_->st_copy_ns = copy_ns;
}

void CompOverlay::done() {
    if (shm_ && claimed_ >= 0) bovl_release(shm_);
    claimed_ = -1;
}

}  // namespace bananads
