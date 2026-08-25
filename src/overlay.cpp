// overlay.cpp
#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace lc {

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

Overlay::~Overlay() { destroy(); }

bool Overlay::create(HINSTANCE inst, int x, int y, int w, int h, Log& log) {
    destroy();
    // w/h here are the screen bounds: the largest the overlay could ever
    // need to be. The back buffer is allocated once at that size, but the
    // window itself only ever covers the current region.
    maxW_ = w; maxH_ = h;
    x_ = x; y_ = y; w_ = w; h_ = h;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc   = OverlayProc;
        wc.hInstance     = inst;
        wc.lpszClassName = L"LoopcoreOverlay";
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) {
            log.error("Overlay window class registration failed.");
            return false;
        }
        registered = true;
    }

    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"LoopcoreOverlay", L"", WS_POPUP,
        x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) {
        log.error("Overlay window creation failed.");
        return false;
    }

    // Keep the overlay out of every screen-capture path, including our own.
    // Without this the model sees the boxes it just drew: coloured lines
    // land on the object edges it is trying to score, confidence drops, the
    // box moves, and the whole thing oscillates. A feedback loop, entirely
    // self-inflicted.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    if (!SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE)) {
        log.warn("This build of Windows cannot exclude the overlay from "
                 "capture (needs 10 version 2004 or newer). The model will "
                 "see the boxes it draws, which costs confidence. Turn the "
                 "overlay off while tuning.");
    }

    ensureBackBuffer();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
    return true;
}

void Overlay::ensureBackBuffer() {
    if (memDC_) return;
    HDC screen = GetDC(nullptr);
    memDC_ = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = maxW_;
    bi.bmiHeader.biHeight      = -maxH_;     // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    oldBmp_ = (HBITMAP)SelectObject(memDC_, bmp_);
    ReleaseDC(nullptr, screen);
}

void Overlay::destroy() {
    if (memDC_) {
        SelectObject(memDC_, oldBmp_);
        DeleteDC(memDC_);
        memDC_ = nullptr;
    }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; bits_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    visible_ = false;
}

void Overlay::setCaptureVisible(bool v) {
    // Applied unconditionally rather than only on a change.
    //
    // The cached flag survives a window being destroyed and recreated, but
    // the new window starts with default affinity -- so the two silently
    // disagree and the setting appears to work sometimes and not others.
    // The call is cheap; caching it was a false economy.
    if (!hwnd_) return;
    captureVisible_ = v;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    SetWindowDisplayAffinity(hwnd_, v ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
}

void Overlay::setVisible(bool v) {
    if (!hwnd_ || v == visible_) return;
    ShowWindow(hwnd_, v ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = v;
    lastSig_ = 0;
}

void Overlay::resizeTo(int x, int y, int w, int h) {
    w = std::clamp(w, 16, maxW_);
    h = std::clamp(h, 16, maxH_);
    if (x == x_ && y == y_ && w == w_ && h == h_) return;
    x_ = x; y_ = y; w_ = w; h_ = h;
    // Moved without touching the z-order.
    //
    // Passing HWND_TOPMOST here asked the compositor to re-evaluate the
    // window stack on every move, when all that was wanted was to relocate a
    // window already at the top. SWP_NOZORDER leaves the band alone, which
    // is both correct and far cheaper -- and this runs whenever the region
    // follows the cursor.
    SetWindowPos(hwnd_, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
    lastSig_ = 0;
}

// UpdateLayeredWindow wants premultiplied BGRA.
static inline void putPixel(uint8_t* p, int stride, int x, int y,
                            int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* q = p + y * stride + x * 4;
    q[0] = (uint8_t)(b * a / 255);
    q[1] = (uint8_t)(g * a / 255);
    q[2] = (uint8_t)(r * a / 255);
    q[3] = a;
}

static void drawRect(uint8_t* p, int stride, int w, int h,
                     int x1, int y1, int x2, int y2, int thick,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int t = 0; t < thick; ++t) {
        for (int x = x1 - t; x <= x2 + t; ++x) {
            putPixel(p, stride, x, y1 - t, w, h, r, g, b, a);
            putPixel(p, stride, x, y2 + t, w, h, r, g, b, a);
        }
        for (int y = y1 - t; y <= y2 + t; ++y) {
            putPixel(p, stride, x1 - t, y, w, h, r, g, b, a);
            putPixel(p, stride, x2 + t, y, w, h, r, g, b, a);
        }
    }
}

static void fillRect(uint8_t* p, int stride, int w, int h,
                     int x1, int y1, int x2, int y2,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int y = y1; y <= y2; ++y)
        for (int x = x1; x <= x2; ++x)
            putPixel(p, stride, x, y, w, h, r, g, b, a);
}

// A 3x5 pixel font, digits and a decimal point. GDI text cannot be used
// here: UpdateLayeredWindow needs a premultiplied DIB, and DrawText into one
// comes out opaque black. Plotting the glyphs directly is a few lines and
// works at any alpha.
//
// Each glyph is 15 bits, top row first, 3 bits per row, MSB leftmost.
static const uint16_t kGlyph[12] = {
    0b111101101101111, // 0
    0b010110010010111, // 1
    0b111001111100111, // 2
    0b111001111001111, // 3
    0b101101111001001, // 4
    0b111100111001111, // 5
    0b111100111101111, // 6
    0b111001001001001, // 7
    0b111101111101111, // 8
    0b111101111001111, // 9
    0b000000000000010, // .
    0b000000000000000, // space
};

static int glyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == '.') return 10;
    return 11;
}

static int textWidth(const char* s, int scale) {
    int n = 0;
    for (const char* p = s; *p; ++p) ++n;
    return n > 0 ? n * 4 * scale - scale : 0;   // 3 wide + 1 gap, no trailing gap
}

static void drawText(uint8_t* px, int stride, int w, int h,
                     int x, int y, int scale, const char* text,
                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (const char* p = text; *p; ++p) {
        const uint16_t bits = kGlyph[glyphIndex(*p)];
        for (int row = 0; row < 5; ++row) {
            for (int colIdx = 0; colIdx < 3; ++colIdx) {
                const int bit = 14 - (row * 3 + colIdx);
                if (!((bits >> bit) & 1)) continue;
                fillRect(px, stride, w, h,
                         x + colIdx * scale, y + row * scale,
                         x + colIdx * scale + scale - 1,
                         y + row * scale + scale - 1,
                         r, g, b, a);
            }
        }
        x += 4 * scale;
    }
}

void Overlay::render(const Config& cfg,
                     const std::vector<Det>& dets,
                     int roiL, int roiT, int roiR, int roiB)
{
    if (!hwnd_ || !bits_ || !visible_) return;

    // Put back on top if something took the topmost band away.
    //
    // A game entering exclusive fullscreen tears it down for every other
    // window, and the boxes stop appearing though nothing here changed. It
    // cannot be prevented, so it is checked for and corrected.
    {
        // Checked often, changed only when it has actually been lost.
        //
        // This used to call SetWindowPos unconditionally twice a second. A
        // z-order change on a topmost layered window sitting over a
        // fullscreen game forces the compositor to reconsider the whole
        // stack, and that is a stall the game feels -- not every time, but
        // whenever it coincides with something the driver was already doing.
        // Since this path only runs while the overlay is drawing, it only
        // happened once a target had been seen and the key was held, which
        // is exactly the report.
        //
        // Reading the style is a cheap local query and answers the same
        // question, so the expensive call now happens only in the case it
        // was written for.
        static int64_t lastAssert = 0;
        const int64_t now = now_ns();
        if (now - lastAssert > 1'000'000'000LL) {
            lastAssert = now;
            const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
            const bool topmost = (ex & WS_EX_TOPMOST) != 0;
            const bool shown = IsWindowVisible(hwnd_) != FALSE;
            if (!shown) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

            // Re-raised whenever the foreground window changes.
            //
            // Same underlying fault the performance readout had:
            // WS_EX_TOPMOST is a membership rather than a position, so a game
            // raising itself leaves this window behind it with the flag
            // intact and every check reporting it healthy.
            //
            // The readout probes with WindowFromPoint, which cannot be used
            // here: this window is WS_EX_TRANSPARENT, and hit testing skips
            // click-through windows entirely -- the probe would never find
            // itself and would re-raise every second forever.
            //
            // A foreground change is the event that actually reorders the
            // topmost band, so watching for it re-raises exactly when it is
            // needed and costs a single comparison otherwise.
            static HWND lastFg = nullptr;
            const HWND fg = GetForegroundWindow();
            const bool fgChanged = (fg != lastFg);
            lastFg = fg;

            if (!topmost || fgChanged)
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                             SWP_NOOWNERZORDER);
        }
    }

    // A margin so thick box lines near the edge are not clipped.
    const int pad = std::max(cfg.boxThickness, cfg.fovThickness) + 6;
    resizeTo(roiL - pad, roiT - pad,
             (roiR - roiL) + pad * 2, (roiB - roiT) + pad * 2);

    // Repainting an unchanged overlay is pure cost: UpdateLayeredWindow goes
    // through DWM every time. Hash what would be drawn and bail if it matches.
    size_t sig = 1469598103934665603ull;
    auto mix = [&sig](size_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix((size_t)dets.size());
    mix((size_t)cfg.boxThickness);
    mix((size_t)(cfg.overlayAlpha * 255));
    mix((size_t)(cfg.overlayMinConf * 255));
    mix((size_t)(cfg.drawConfText ? 16 : 0));
    // Part of the repaint hash, or a moving marker would be skipped by the
    // unchanged-frame check.
    mix((size_t)(cfg.drawLeadDot ? 32 : 0));
    if (cfg.drawLeadDot) {
        mix((size_t)(int)(leadX_ * 4.0f));
        mix((size_t)(int)(leadY_ * 4.0f));
    }
    mix((size_t)cfg.fovThickness);
    mix((size_t)(cfg.actionFovOn ? cfg.actionFov : 0));
    mix((size_t)((cfg.drawRoiRect    ? 1 : 0) |
                 (cfg.drawLabels     ? 2 : 0) |
                 (cfg.drawConfidence ? 4 : 0) |
                 (cfg.drawCenterDot  ? 8 : 0)));
    mix((size_t)(cfg.boxColor[0] * 255) ^ ((size_t)(cfg.boxColor[1] * 255) << 8) ^
        ((size_t)(cfg.boxColor[2] * 255) << 16));
    for (const Det& d : dets) {
        mix((size_t)(int)d.x1); mix((size_t)(int)d.y1);
        mix((size_t)(int)d.x2); mix((size_t)(int)d.y2);
        mix((size_t)(int)(d.score * 100)); mix((size_t)d.cls);
    }
    if (sig == lastSig_) return;
    lastSig_ = sig;

    const int stride = maxW_ * 4;
    uint8_t* px = (uint8_t*)bits_;
    for (int y = 0; y < h_; ++y)
        std::memset(px + (size_t)y * stride, 0, (size_t)w_ * 4);

    const uint8_t alpha = (uint8_t)std::clamp(int(cfg.overlayAlpha * 255.0f), 0, 255);
    const uint8_t br = (uint8_t)(cfg.boxColor[0] * 255);
    const uint8_t bg = (uint8_t)(cfg.boxColor[1] * 255);
    const uint8_t bb = (uint8_t)(cfg.boxColor[2] * 255);
    const uint8_t rr = (uint8_t)(cfg.roiColor[0] * 255);
    const uint8_t rg = (uint8_t)(cfg.roiColor[1] * 255);
    const uint8_t rb = (uint8_t)(cfg.roiColor[2] * 255);

    if (cfg.drawRoiRect) {
        drawRect(px, stride, w_, h_, roiL - x_, roiT - y_,
                 roiR - x_ - 1, roiB - y_ - 1,
                 std::max(1, cfg.fovThickness), rr, rg, rb,
                 (uint8_t)(alpha * 0.7f));

        // The action region, when it is smaller than the detection region.
        if (cfg.actionFovOn && cfg.actionFov < cfg.fov) {
            const int ccx = (roiL + roiR) / 2 - x_;
            const int ccy = (roiT + roiB) / 2 - y_;
            const int ah = cfg.actionFov / 2;
            drawRect(px, stride, w_, h_, ccx - ah, ccy - ah, ccx + ah, ccy + ah,
                     std::max(1, cfg.fovThickness), br, bg, bb,
                     (uint8_t)(alpha * 0.85f));
        }
    }

    for (const Det& d : dets) {
        if (d.score < cfg.overlayMinConf) continue;
        const int x1 = (int)d.x1 - x_, y1 = (int)d.y1 - y_;
        const int x2 = (int)d.x2 - x_, y2 = (int)d.y2 - y_;
        drawRect(px, stride, w_, h_, x1, y1, x2, y2,
                 cfg.boxThickness, br, bg, bb, alpha);

        if (cfg.drawCenterDot) {
            const int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
            fillRect(px, stride, w_, h_, cx - 2, cy - 2, cx + 2, cy + 2,
                     br, bg, bb, alpha);
        }
        if (cfg.drawConfText) {
            // "0.87" -- two decimals is as much as a score is worth, and any
            // more would not fit above a small box.
            char buf[8];
            const int pct = (int)(std::clamp(d.score, 0.0f, 0.999f) * 100.0f + 0.5f);
            snprintf(buf, sizeof(buf), "0.%02d", pct);

            const int scale = std::max(1, cfg.boxThickness);
            const int tw = textWidth(buf, scale);
            const int th = 5 * scale;
            int tx = x1;
            int ty = y1 - th - 3 * scale;
            // Flip below the box when there is no room above it.
            if (ty < 0) ty = y2 + 3 * scale;
            if (tx + tw > w_) tx = w_ - tw;

            drawText(px, stride, w_, h_, tx, ty, scale, buf, br, bg, bb, alpha);
        }

        if (cfg.drawLabels || cfg.drawConfidence) {
            // A filled tab above the box. GDI text on a premultiplied DIB
            // renders opaque black, so class is encoded as tab width and
            // score as tab fill instead of drawn as glyphs.
            const int tabW = 18 + (cfg.drawLabels ? d.cls * 10 : 0);
            const int tabH = 5;
            const int fillW = cfg.drawConfidence
                            ? std::max(2, (int)(tabW * std::clamp(d.score, 0.0f, 1.0f)))
                            : tabW;
            const int tabY = cfg.drawConfText
                           ? y1 - tabH - 2 - (5 * std::max(1, cfg.boxThickness) + 3)
                           : y1 - tabH - 2;

            // Outline the full width, fill only the score's share. Without
            // the outline a partial bar just looks like a tab of arbitrary
            // length; with it, the empty remainder shows it is a measurement.
            if (cfg.drawConfidence)
                drawRect(px, stride, w_, h_, x1, tabY, x1 + tabW, tabY + tabH,
                         1, br, bg, bb, (uint8_t)(alpha * 0.45f));
            fillRect(px, stride, w_, h_, x1, tabY, x1 + fillW, tabY + tabH,
                     br, bg, bb, alpha);
        }
    }

    // Where prediction is aiming.
    //
    // A cross rather than a filled dot: it has to be readable against a
    // detection box drawn in the same colour, and a hollow shape stays
    // legible on top of one.
    // The aim's recent path, coloured by how fast it was moving.
    //
    // Written into the same pixel buffer as everything else here. My first
    // attempt used GDI pens on a device context, which this function does
    // not have -- the overlay composes a bitmap itself and blits it once.
    //
    // Drawn before the lead marker and the boxes so it sits behind them: it
    // is context for what the aim did, not something to read over a target.
    if (cfg.drawPath && pathN_ > 1) {
        // The trace is relative to where the movement began, so it is
        // anchored to the middle of the region to land somewhere useful.
        const float ax = (float)(roiL + roiR) * 0.5f - (float)x_;
        const float ay = (float)(roiT + roiB) * 0.5f - (float)y_;

        // Speed is scaled against the fastest point in this trace rather
        // than an absolute figure: what matters is where this movement was
        // quick relative to itself, and an absolute scale would wash every
        // gentle path into one colour.
        float fastest = 1.0f;
        for (int i = 0; i < pathN_; ++i)
            if (path_[i].speed > fastest) fastest = path_[i].speed;

        for (int i = 1; i < pathN_; ++i) {
            const float f = std::min(1.0f, path_[i].speed / fastest);
            const uint8_t pr = 0x20;
            const uint8_t pg = (uint8_t)(230.0f * (1.0f - f) + 70.0f * f);
            const uint8_t pb = (uint8_t)(90.0f * (1.0f - f) + 255.0f * f);
            // Older points fade, so the live end of the line is obvious.
            const uint8_t pa =
                (uint8_t)(50.0f + 185.0f * ((float)i / (float)pathN_));

            // A straight run between consecutive samples. They arrive every
            // control tick, so the gaps are a pixel or two and the result
            // reads as a curve without needing one drawn.
            const int x0 = (int)(ax + path_[i - 1].x);
            const int y0 = (int)(ay + path_[i - 1].y);
            const int x1 = (int)(ax + path_[i].x);
            const int y1 = (int)(ay + path_[i].y);
            const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
            if (steps == 0) {
                fillRect(px, stride, w_, h_, x0, y0, x0, y0, pr, pg, pb, pa);
                continue;
            }
            for (int sIdx = 0; sIdx <= steps; ++sIdx) {
                const float u = (float)sIdx / (float)steps;
                const int xx = (int)(x0 + (x1 - x0) * u);
                const int yy = (int)(y0 + (y1 - y0) * u);
                fillRect(px, stride, w_, h_, xx, yy, xx, yy, pr, pg, pb, pa);
            }
        }
    }

    if (cfg.drawLeadDot && leadX_ >= 0.0f && leadY_ >= 0.0f) {
        const int lx = (int)leadX_ - x_;
        const int ly = (int)leadY_ - y_;
        if (lx > 6 && ly > 6 && lx < w_ - 6 && ly < h_ - 6) {
            const uint8_t lr = 0x8C, lg = 0xE8, lb = 0xD4;
            const uint8_t a = 235;
            fillRect(px, stride, w_, h_, lx - 7, ly - 1, lx - 2, ly + 1,
                     lr, lg, lb, a);
            fillRect(px, stride, w_, h_, lx + 3, ly - 1, lx + 8, ly + 1,
                     lr, lg, lb, a);
            fillRect(px, stride, w_, h_, lx - 1, ly - 7, lx + 1, ly - 2,
                     lr, lg, lb, a);
            fillRect(px, stride, w_, h_, lx - 1, ly + 3, lx + 1, ly + 8,
                     lr, lg, lb, a);
            drawRect(px, stride, w_, h_, lx - 3, ly - 3, lx + 3, ly + 3,
                     1, lr, lg, lb, a);
        }
    }

    POINT src{0, 0};
    POINT dst{x_, y_};
    SIZE  sz{w_, h_};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(hwnd_, screen, &dst, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

} // namespace lc
