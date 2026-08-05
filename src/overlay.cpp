// overlay.cpp
#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace lc {

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

Overlay::~Overlay() { destroy(); }

bool Overlay::create(HINSTANCE inst, int x, int y, int w, int h, Log& log) {
    destroy();
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
    bi.bmiHeader.biWidth       = w_;
    bi.bmiHeader.biHeight      = -h_;        // top-down
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

void Overlay::setVisible(bool v) {
    if (!hwnd_ || v == visible_) return;
    ShowWindow(hwnd_, v ? SW_SHOWNOACTIVATE : SW_HIDE);
    visible_ = v;
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

void Overlay::render(const Config& cfg,
                     const std::vector<Det>& dets,
                     int roiL, int roiT, int roiR, int roiB)
{
    if (!hwnd_ || !bits_ || !visible_) return;

    const int stride = w_ * 4;
    std::memset(bits_, 0, (size_t)stride * h_);
    uint8_t* px = (uint8_t*)bits_;

    const uint8_t alpha = (uint8_t)std::clamp(int(cfg.overlayAlpha * 255.0f), 0, 255);
    const uint8_t br = (uint8_t)(cfg.boxColor[0] * 255);
    const uint8_t bg = (uint8_t)(cfg.boxColor[1] * 255);
    const uint8_t bb = (uint8_t)(cfg.boxColor[2] * 255);
    const uint8_t rr = (uint8_t)(cfg.roiColor[0] * 255);
    const uint8_t rg = (uint8_t)(cfg.roiColor[1] * 255);
    const uint8_t rb = (uint8_t)(cfg.roiColor[2] * 255);

    if (cfg.drawRoiRect)
        drawRect(px, stride, w_, h_, roiL - x_, roiT - y_, roiR - x_ - 1, roiB - y_ - 1,
                 1, rr, rg, rb, (uint8_t)(alpha * 0.6f));

    for (const Det& d : dets) {
        if (d.score < cfg.overlayMinConf) continue;
        int x1 = (int)d.x1 - x_, y1 = (int)d.y1 - y_;
        int x2 = (int)d.x2 - x_, y2 = (int)d.y2 - y_;
        drawRect(px, stride, w_, h_, x1, y1, x2, y2,
                 cfg.boxThickness, br, bg, bb, alpha);

        if (cfg.drawCenterDot) {
            int cx = (x1 + x2) / 2, cy = (y1 + y2) / 2;
            fillRect(px, stride, w_, h_, cx - 2, cy - 2, cx + 2, cy + 2,
                     br, bg, bb, alpha);
        }
        if (cfg.drawLabels || cfg.drawConfidence) {
            // A filled tab above the box; GDI text on a premultiplied DIB
            // renders opaque black, so the class is encoded as tab width
            // and the score as tab fill instead.
            int tabW = 18 + (cfg.drawLabels ? d.cls * 10 : 0);
            int tabH = 5;
            int fillW = cfg.drawConfidence
                      ? std::max(2, (int)(tabW * std::clamp(d.score, 0.0f, 1.0f)))
                      : tabW;
            fillRect(px, stride, w_, h_, x1, y1 - tabH - 2, x1 + fillW, y1 - 2,
                     br, bg, bb, alpha);
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
