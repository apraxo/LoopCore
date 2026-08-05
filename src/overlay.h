// overlay.h -- click-through, always-on-top box overlay.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

#include "common.h"

namespace lc {

// A layered per-pixel-alpha window covering the captured monitor. It is
// WS_EX_TRANSPARENT so clicks pass straight through to the sim, and
// WS_EX_NOREDIRECTIONBITMAP-free so UpdateLayeredWindow works.
//
// Redrawn at ~60 Hz from the UI thread. It is deliberately not on the
// capture path: drawing for a human to look at has no business adding
// microseconds to the control loop.
class Overlay {
public:
    ~Overlay();

    bool create(HINSTANCE inst, int x, int y, int w, int h, Log& log);
    void destroy();
    void setVisible(bool v);
    bool visible() const { return visible_; }

    // dets are in screen coordinates.
    void render(const Config& cfg,
                const std::vector<Det>& dets,
                int roiL, int roiT, int roiR, int roiB);

private:
    void ensureBackBuffer();

    HWND    hwnd_    = nullptr;
    HDC     memDC_   = nullptr;
    HBITMAP bmp_     = nullptr;
    HBITMAP oldBmp_  = nullptr;
    void*   bits_    = nullptr;
    int     x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool    visible_ = false;
};

} // namespace lc
