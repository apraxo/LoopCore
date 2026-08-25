// perfmon.h -- a small always-on-top readout that clips to a screen edge.
//
// Uses uniform-alpha layering (SetLayeredWindowAttributes) rather than
// per-pixel alpha. Per-pixel alpha needs a premultiplied DIB, and GDI text
// drawn into one comes out opaque black -- which is why the detection
// overlay encodes its labels as shapes. Uniform alpha keeps text working.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

#include "common.h"

#include "common.h"

namespace lc {

struct PerfSnapshot {
    double loopP50   = 0.0;
    double loopP99   = 0.0;
    double cadenceHz = 0.0;
    double dropPct   = 0.0;
    int    detections = 0;
    float  topScore  = 0.0f;
    bool   engaged   = false;

    // Machine load, so the readout can answer "is it me or the machine"
    // without switching windows. Sampled at the same slow rate as the rest;
    // -1 means the figure was not available.
    float  cpuPct    = -1.0f;
    float  gpuPct    = -1.0f;
    float  vramPct   = -1.0f;
    int    gpuMHz    = 0;
    int    gpuMaxMHz = 0;
    int    gpuTempC  = 0;
    bool   throttled = false;
    // What the application in front is managing, and how thinned inference
    // is. Zero fps means it has not been measured yet.
    float  fgFps     = 0.0f;
    int    stride    = 1;
    // What distance scaling is doing to sensitivity, as a fraction. One means
    // no reduction; anything less is shown. Negative means the feature is off
    // and nothing should be drawn for it.
    float  sensScale = -1.0f;
    // Whether frames are being written to disk right now.
    bool   capturing = false;
};

class PerfMonitor {
public:
    ~PerfMonitor();

    bool create(HINSTANCE inst, Log& log);
    void destroy();
    void setVisible(bool v);

    // Whether recorders may see it. Hiding a window from capture makes some
    // recorders refuse the whole desktop, so this has to be releasable.
    void setCaptureVisible(bool v);
    bool visible() const { return visible_; }

    // Redrawn on a timer, not every frame; nobody reads a HUD at 240 Hz.
    void update(const PerfSnapshot& s);
    // The trace to draw. Owned elsewhere; the monitor only reads it.
    void setTrace(const TimeSeries* t) { trace_ = t; }

    // Edge 0 top, 1 right, 2 bottom, 3 left; `t` is the position along it,
    // 0 to 1. Anywhere on any edge, not a handful of fixed spots.
    void snapToEdge(int edge, float t, bool animate = true);
    int   edge() const { return edge_; }
    float edgeT() const { return edgeT_; }

    // Advances the glide. Called from the UI loop.
    void animate();

    // Seconds of history the graph covers.
    void setGraphSpan(float seconds) { spanSec_ = seconds; }

    // The window grows when the meters are on, so turning them off gives the
    // space back instead of leaving a gap.
    void setMeters(bool on);
    // A size multiplier on top of the display scale, so the readout can be
    // made legible on a large screen or tucked away on a small one.
    void setUserScale(float s);

    // Which display to sit on. Placement is worked out from that monitor's
    // own rectangle, so it lands correctly whatever its resolution.
    void setDisplay(int index);
    static int DisplayCount();
    // Human-readable, for the picker: "1920x1080 (primary)".
    static std::string DisplayName(int index);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void paint(HDC dc);

    HWND   hwnd_ = nullptr;
    HFONT  font_ = nullptr;
    HFONT  fontSmall_ = nullptr;
    bool   visible_ = false;
    bool   captureVisible_ = false;
    int    edge_  = 0;
    float  edgeT_ = 1.0f;
    // Animation state. Snapping instantly is jarring when the window was
    // just dragged; a short glide reads as the window settling.
    float  curX_ = 0, curY_ = 0;
    float  dstX_ = 0, dstY_ = 0;
    bool   animating_ = false;
    int64_t lastAnim_ = 0;
    int64_t lastPaint_ = 0;
    double  avg_ = 0.0;        // slow average, for the trend arrow
    PerfSnapshot snap_;

    // History sampled here, on a wall clock, rather than taken from the
    // loop's frame ring. A frame-count window means the span in seconds
    // drifts with the frame rate, and the ring runs out entirely at a long
    // span, which is what made a wide graph inconsistent.
    const TimeSeries* trace_ = nullptr;
    std::vector<TimeSeries::Column> cols_;
    static constexpr int kSampleHz = 10;
    float   spanSec_   = 4.0f;
    bool    meters_    = true;
    int     display_   = 0;
    float   dpiScale_  = 1.0f;
    float   userScale_ = 1.0f;
    // Restores topmost and visibility when something else has taken them.
    void    reassert();
    // Remembered from create(), so the window can be rebuilt if it vanishes
    // without anything here having asked for it.
    HINSTANCE inst_ = nullptr;
    Log*      log_  = nullptr;
    void    refreshScale();
    void    paintVertical(HDC dc);
    void    paintHorizontal(HDC dc);
};

} // namespace lc
