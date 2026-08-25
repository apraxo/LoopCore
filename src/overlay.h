// overlay.h -- click-through, always-on-top box overlay.
#pragma once

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

    // Whether screen capture may see the overlay. Off means invisible to
    // WGC, DXGI, and every recorder -- including our own capture path.
    void setCaptureVisible(bool v);
    bool visible() const { return visible_; }

    // dets are in screen coordinates. The window is moved and resized to
    // hug the region: a full-screen layered window recomposited at 60 Hz is
    // enough work to make the desktop itself feel broken.
    // `leadX/leadY` mark where prediction is aiming, in screen coordinates.
    // Pass -1 for both to draw nothing.
    void setLeadMarker(float x, float y) { leadX_ = x; leadY_ = y; }

    // The recent aim path in screen pixels, oldest first. Speed colours it,
    // so where a movement was quick and where it eased can be read at a
    // glance rather than inferred.
    struct PathPt { float x, y, speed; };
    void setPath(const PathPt* pts, int count) {
        pathN_ = 0;
        if (!pts) return;
        pathN_ = count < kPathCap ? count : kPathCap;
        for (int i = 0; i < pathN_; ++i) path_[i] = pts[i];
    }

    void render(const Config& cfg,
                const std::vector<Det>& dets,
                int roiL, int roiT, int roiR, int roiB);

private:
    void ensureBackBuffer();
    void resizeTo(int x, int y, int w, int h);

    HWND    hwnd_    = nullptr;
    HDC     memDC_   = nullptr;
    HBITMAP bmp_     = nullptr;
    HBITMAP oldBmp_  = nullptr;
    void*   bits_    = nullptr;
    int     x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    int     maxW_ = 0, maxH_ = 0;
    bool    visible_ = false;
    bool    captureVisible_ = false;
    float   leadX_ = -1.0f, leadY_ = -1.0f;
    static constexpr int kPathCap = 96;
    PathPt  path_[kPathCap] = {};
    int     pathN_ = 0;
    size_t  lastSig_ = 0;      // skip the repaint when nothing changed
};

} // namespace lc
