// capture.h -- Windows.Graphics.Capture, event driven, zero host copies.
#pragma once

#include <d3d11.h>
#include <cuda_runtime.h>
#include <functional>
#include <string>
#include <vector>
#include <memory>

#include "common.h"

namespace lc {

// A ROI already cropped on the GPU and mapped into CUDA.
struct MappedRoi {
    cudaTextureObject_t tex = 0;
    int  width  = 0;
    int  height = 0;
    int  left   = 0;   // screen coords, for mapping boxes back
    int  top    = 0;

    // When this frame arrived, not when it finished being processed.
    //
    // Everything downstream that reasons about staleness needs the moment
    // the world actually looked like this. Timestamping later folds the
    // whole pipeline -- copy, preprocess, inference -- into zero, and the
    // prediction then leads by that much too little.
    int64_t stampNs = 0;

    // Host copy, only populated when something asked for it. Reading the
    // frame back costs a PCIe transfer, so it does not happen unless the
    // dataset capture is actually running.
    const uint8_t* host   = nullptr;
    int            stride = 0;
};

// Runs the whole pipeline inside the FrameArrived callback. That callback
// fires on a WinRT thread-pool thread the moment the compositor hands us a
// frame -- no polling, no phase jitter, and frame age is as low as the OS
// can make it. If the callback overruns the frame cadence the pool simply
// drops frames, which we count rather than queue: a queue would trade
// latency for throughput, and latency is the whole point here.
class Capture {
public:
    // Returns true when the frame was actually processed. An idle frame
    // costs almost nothing, and mixing those into the loop timing turns the
    // graph into a square wave between two unrelated populations.
    using FrameFn = std::function<bool(const MappedRoi&)>;

    Capture();
    ~Capture();

    bool start(int monitorIndex, Shared& shared, FrameFn onFrame);
    void stop();
    bool running() const { return running_; }

    // True once after the display changed size, so consumers that cached the
    // old dimensions can rebuild rather than drawing against them.
    bool takeSizeChanged();

    // Force the screen size rather than using the detected one. Passing 0
    // for either dimension returns to detection.
    void setSizeOverride(int w, int h);

    // Displays available to capture, in the same order start() indexes them,
    // each labelled with its real size.
    struct DisplayInfo { int index; int width, height; bool primary; std::string label; };
    static std::vector<DisplayInfo> Displays();
    int monitorIndex() const { return monitorIndex_; }
    // What was detected, regardless of any override, for the UI to show.
    int detectedWidth()  const { return detectedW_; }
    int detectedHeight() const { return detectedH_; }

    int screenLeft() const { return screenX_; }
    int screenTop()  const { return screenY_; }

    int screenWidth()  const { return screenW_; }
    int screenHeight() const { return screenH_; }

    // Ask for a host-side copy of each ROI. Off by default: it costs a
    // device-to-host transfer per frame, which is exactly what the rest of
    // this design goes out of its way to avoid.
    void setHostReadback(bool on) { wantHost_ = on; }

    // Called from the capture thread each frame to resize the ROI.
    // Recreates and re-registers the shared texture only when the size changes.
    bool ensureRoi(int w, int h, Log& log);

    ID3D11Device*        device()  { return device_.get(); }
    cudaStream_t         stream()  { return stream_; }

private:
    // Invoked by the WinRT frame pool. Takes void* so the header stays free
    // of C++/WinRT includes.
    void onFrameArrived(void* framePool);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    struct DeviceDeleter { void operator()(ID3D11Device* p) const { if (p) p->Release(); } };
    std::unique_ptr<ID3D11Device, DeviceDeleter> device_;

    cudaStream_t stream_ = nullptr;
    int  screenW_ = 0, screenH_ = 0;
    // Where the captured monitor sits on the virtual desktop.
    //
    // The region is worked out in coordinates local to that monitor, but the
    // overlay, the cursor and every consumer downstream deal in desktop
    // coordinates. On a single display at the origin the two are the same,
    // which is why this was missing and why it only showed up with a second
    // monitor attached.
    int  screenX_ = 0, screenY_ = 0;
    int  detectedW_ = 0, detectedH_ = 0;
    int  monitorIndex_ = 0;
    bool running_ = false;
    bool wantHost_ = false;
};

} // namespace lc
