// capture.h -- Windows.Graphics.Capture, event driven, zero host copies.
#pragma once

#include <d3d11.h>
#include <cuda_runtime.h>
#include <functional>
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
};

// Runs the whole pipeline inside the FrameArrived callback. That callback
// fires on a WinRT thread-pool thread the moment the compositor hands us a
// frame -- no polling, no phase jitter, and frame age is as low as the OS
// can make it. If the callback overruns the frame cadence the pool simply
// drops frames, which we count rather than queue: a queue would trade
// latency for throughput, and latency is the whole point here.
class Capture {
public:
    using FrameFn = std::function<void(const MappedRoi&)>;

    Capture();
    ~Capture();

    bool start(int monitorIndex, Shared& shared, FrameFn onFrame);
    void stop();
    bool running() const { return running_; }

    int screenWidth()  const { return screenW_; }
    int screenHeight() const { return screenH_; }

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
    bool running_ = false;
};

} // namespace lc
