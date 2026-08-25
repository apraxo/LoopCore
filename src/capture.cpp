// capture.cpp
#include "capture.h"

#include <windows.h>
#include <inspectable.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.Metadata.h>

#include <cuda_d3d11_interop.h>

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdio>
#include <string>

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGDX = winrt::Windows::Graphics::DirectX;

namespace lc {

// ---------------------------------------------------------------- helpers

static std::string hres(HRESULT hr) {
    char buf[32];
    sprintf_s(buf, "0x%08X", (unsigned)hr);
    return buf;
}

struct Capture::Impl {
    WGC::GraphicsCaptureItem       item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession    session{nullptr};
    winrt::event_token             token{};

    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Texture2D>     roiTex;     // persistent, CUDA-registered
    WGDX::Direct3D11::IDirect3DDevice   winrtDevice{nullptr};
    std::atomic<bool>                   sizeChanged{false};
    // A size override requested from another thread. Applied by the capture
    // thread at a point where nothing is mapped.
    std::atomic<int>                    wantW{0};
    std::atomic<int>                    wantH{0};
    std::atomic<bool>                   applySize{false};
    winrt::com_ptr<ID3D11Texture2D>     stageTex;   // CPU-readable, only when asked
    std::vector<uint8_t>                hostBuf;
    cudaGraphicsResource*               cudaRes = nullptr;
    cudaArray_t                         cudaArr = nullptr;
    cudaTextureObject_t                 texObj  = 0;

    int roiW = 0, roiH = 0;

    Shared*  shared = nullptr;
    FrameFn  cb;
    std::atomic<bool> busy{false};
    std::atomic<int64_t> lastArrive{0};

    void releaseRoi() {
        if (texObj) { cudaDestroyTextureObject(texObj); texObj = 0; }
        if (cudaRes) { cudaGraphicsUnregisterResource(cudaRes); cudaRes = nullptr; }
        cudaArr = nullptr;
        roiTex = nullptr;
        stageTex = nullptr;
        hostBuf.clear();
        roiW = roiH = 0;
    }
};

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

// ---------------------------------------------------------------- start

std::vector<Capture::DisplayInfo> Capture::Displays() {
    // Enumerated in exactly the order start() walks them, so an index shown
    // here means the same thing when passed back in.
    struct Ctx { std::vector<DisplayInfo>* v; } ctx;
    std::vector<DisplayInfo> out;
    ctx.v = &out;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* c = (Ctx*)p;
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(MONITORINFOEXW);
            if (!GetMonitorInfoW(h, &mi)) return TRUE;

            DisplayInfo d;
            d.index   = (int)c->v->size();
            d.width   = mi.rcMonitor.right - mi.rcMonitor.left;
            d.height  = mi.rcMonitor.bottom - mi.rcMonitor.top;
            d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            char buf[96];
            snprintf(buf, sizeof(buf), "%d: %d x %d%s", d.index, d.width,
                     d.height, d.primary ? "  primary" : "");
            d.label = buf;
            c->v->push_back(std::move(d));
            return TRUE;
        }, (LPARAM)&ctx);
    return out;
}

bool Capture::start(int monitorIndex, Shared& shared, FrameFn onFrame) {
    monitorIndex_ = monitorIndex;
    stop();
    impl_->shared = &shared;
    impl_->cb     = std::move(onFrame);
    Log& log = shared.log;

    // C++/WinRT needs an initialised apartment before any activation factory
    // call. Multi-threaded, to match the free-threaded frame pool. Throws if
    // the thread is already in an apartment, which is fine.
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const&) {
        // already initialised on this thread; nothing to do
    }

    if (!WGC::GraphicsCaptureSession::IsSupported()) {
        log.error("Windows.Graphics.Capture is not available. Windows 10 1903 "
                  "or newer is required.");
        return false;
    }

    // ---- pick the monitor ---------------------------------------------
    struct EnumCtx { int want; int seen; HMONITOR hmon; } ec{monitorIndex, 0, nullptr};
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* c = (EnumCtx*)p;
            if (c->seen == c->want) { c->hmon = h; return FALSE; }
            c->seen++;
            return TRUE;
        }, (LPARAM)&ec);
    if (!ec.hmon) ec.hmon = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);

    // Provisional only. GetMonitorInfo reports what Windows thinks this
    // process should see, which is the scaled size if the process is not
    // considered DPI aware -- 1440p at 133% comes back as exactly 1920x1080.
    // The capture item's own size is the real pixel count and replaces this
    // below, once the item exists.
    MONITORINFO mi{sizeof(MONITORINFO)};
    GetMonitorInfoW(ec.hmon, &mi);
    screenW_ = mi.rcMonitor.right  - mi.rcMonitor.left;
    screenH_ = mi.rcMonitor.bottom - mi.rcMonitor.top;
    screenX_ = mi.rcMonitor.left;
    screenY_ = mi.rcMonitor.top;
    const int reportedW = screenW_, reportedH = screenH_;

    // ---- D3D11 device ---------------------------------------------------
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    // flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        log.error("D3D11CreateDevice failed (" + hres(hr) + "). Update the GPU driver.");
        return false;
    }
    device_.reset(dev);
    impl_->ctx.attach(ctx);

    // The capture callback and the UI thread both touch this device.
    winrt::com_ptr<ID3D11Multithread> mt;
    if (SUCCEEDED(dev->QueryInterface(winrt::guid_of<ID3D11Multithread>(), mt.put_void())))
        mt->SetMultithreadProtected(TRUE);

    // ---- bind CUDA to the same adapter ----------------------------------
    winrt::com_ptr<IDXGIDevice> dxgiDev;
    dev->QueryInterface(winrt::guid_of<IDXGIDevice>(), dxgiDev.put_void());
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.put());
    int cudaDev = 0;
    if (cudaD3D11GetDevice(&cudaDev, adapter.get()) != cudaSuccess) {
        log.warn("This display adapter is not CUDA-capable. Falling back to CUDA device 0; "
                 "if the sim runs on a different GPU than the display, expect a copy.");
        cudaDev = 0;
    }
    cudaSetDevice(cudaDev);
    // Highest priority the device offers.
    //
    // This orders our work against other work in this process rather than
    // against another application, so it is not a cure for a busy GPU. It
    // costs nothing and removes one variable.
    int loPri = 0, hiPri = 0;
    cudaDeviceGetStreamPriorityRange(&loPri, &hiPri);
    if (cudaStreamCreateWithPriority(&stream_, cudaStreamNonBlocking, hiPri)
            != cudaSuccess &&
        cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) != cudaSuccess) {
        log.error("cudaStreamCreate failed.");
        return false;
    }
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, cudaDev);
    log.info(std::string("CUDA device: ") + prop.name +
             "  sm_" + std::to_string(prop.major) + std::to_string(prop.minor));

    // ---- WinRT capture item --------------------------------------------
    try {
        auto interop = winrt::get_activation_factory<
            WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        WGC::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForMonitor(
            ec.hmon, winrt::guid_of<WGC::GraphicsCaptureItem>(),
            winrt::put_abi(item)));
        impl_->item = item;

        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDev.get(), inspectable.put()));
        // Kept rather than dropped: recreating the pool after a resolution
        // change needs the same device back.
        impl_->winrtDevice = inspectable.as<WGDX::Direct3D11::IDirect3DDevice>();
        auto& winrtDevice = impl_->winrtDevice;

        // 2 buffers: enough to avoid a stall, few enough that we never sit
        // on a stale frame.
        impl_->pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            WGDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            { impl_->item.Size().Width, impl_->item.Size().Height });

        // The authority on how big the screen is.
        //
        // This comes from the graphics capture stack rather than from the
        // window manager, so it is the true pixel count whatever the process
        // is considered to be in terms of DPI awareness. Every coordinate
        // downstream is compared against captured pixels, so this is the
        // number they all have to agree with.
        {
            const auto sz = impl_->item.Size();
            if (sz.Width > 0 && sz.Height > 0) {
                if (sz.Width != reportedW || sz.Height != reportedH) {
                    log.warn("Windows reported this display as " +
                             std::to_string(reportedW) + "x" +
                             std::to_string(reportedH) + " but it is really " +
                             std::to_string(sz.Width) + "x" +
                             std::to_string(sz.Height) +
                             ". Using the real size. The difference means "
                             "display scaling is in play and this process is "
                             "not being told about it.");
                }
                screenW_ = sz.Width;
                screenH_ = sz.Height;
                detectedW_ = sz.Width;
                detectedH_ = sz.Height;
            }
        }

        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);

        // Hide the yellow capture border where the OS allows it.
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired"))
                impl_->session.IsBorderRequired(false);
        } catch (...) {}
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
                impl_->session.IsCursorCaptureEnabled(false);
        } catch (...) {}

        impl_->token = impl_->pool.FrameArrived(
            [this](WGC::Direct3D11CaptureFramePool const& pool,
                   winrt::Windows::Foundation::IInspectable const&) {
                onFrameArrived(const_cast<WGC::Direct3D11CaptureFramePool*>(&pool));
            });

        impl_->session.StartCapture();
    } catch (winrt::hresult_error const& e) {
        log.error("Capture setup failed: " + winrt::to_string(e.message()) +
                  " (" + hres(e.code()) + ")");
        return false;
    }

    running_ = true;
    log.info("Capture running on monitor " + std::to_string(monitorIndex) +
             " (" + std::to_string(screenW_) + "x" + std::to_string(screenH_) + ")");
    return true;
}

// ---------------------------------------------------------------- ROI

void Capture::setSizeOverride(int w, int h) {
    // Requested here, applied on the capture thread.
    //
    // The previous version freed the CUDA resources from whichever thread
    // called it, while the capture thread could be part-way through mapping
    // them. That is a use-after-free inside the driver, and it crashed
    // exactly there. Nothing outside the capture thread may touch them.
    if (!impl_) return;
    const int wantW = (w > 0 && h > 0) ? w : 0;
    const int wantH = (w > 0 && h > 0) ? h : 0;
    if (impl_->wantW.load() == wantW && impl_->wantH.load() == wantH) return;
    impl_->wantW = wantW;
    impl_->wantH = wantH;
    impl_->applySize = true;
}

bool Capture::takeSizeChanged() {
    bool expected = true;
    return impl_ && impl_->sizeChanged.compare_exchange_strong(expected, false);
}

bool Capture::ensureRoi(int w, int h, Log& log) {
    w = std::max(64, std::min(w, screenW_));
    h = std::max(64, std::min(h, screenH_));
    if (impl_->roiTex && impl_->roiW == w && impl_->roiH == h) return true;

    impl_->releaseRoi();

    D3D11_TEXTURE2D_DESC d{};
    d.Width  = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device_->CreateTexture2D(&d, nullptr, &tex);
    if (FAILED(hr)) {
        log.error("CreateTexture2D failed for the ROI (" + hres(hr) + ").");
        return false;
    }
    impl_->roiTex.attach(tex);

    cudaError_t ce = cudaGraphicsD3D11RegisterResource(
        &impl_->cudaRes, tex, cudaGraphicsRegisterFlagsNone);
    if (ce != cudaSuccess) {
        log.error(std::string("cudaGraphicsD3D11RegisterResource failed: ") +
                  cudaGetErrorString(ce));
        impl_->releaseRoi();
        return false;
    }
    cudaGraphicsResourceSetMapFlags(impl_->cudaRes, cudaGraphicsMapFlagsReadOnly);

    impl_->stageTex = nullptr;   // size changed, so the old one is wrong
    impl_->hostBuf.clear();
    impl_->roiW = w;
    impl_->roiH = h;
    return true;
}

// ------------------------------------------------------------ frame path

void Capture::onFrameArrived(void* poolPtr) {
    auto& pool = *(WGC::Direct3D11CaptureFramePool*)poolPtr;
    Shared& sh = *impl_->shared;

    // The frame pool hands this to an ordinary thread-pool thread. Every
    // millisecond it waits to be scheduled lands in the measured inference
    // time, because that measurement wraps a synchronise. Raising it once
    // per thread costs nothing and takes a chunk out of the variance.
    {
        static thread_local bool raised = false;
        if (!raised) {
            raised = true;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }

    int64_t t0 = now_ns();
    int64_t prev = impl_->lastArrive.exchange(t0);
    if (prev) {
        const double gap = (t0 - prev) / 1e6;
        sh.timings.stage[ST_CADENCE].add(gap);
        sh.traces.cadence.add(gap, t0);
    }
    sh.counters.arrivals++;

    // Newest-frame-wins: if the pipeline is still working, drop this one.
    bool expected = false;
    if (!impl_->busy.compare_exchange_strong(expected, true)) {
        auto frame = pool.TryGetNextFrame();   // still must drain the pool
        sh.counters.skipped++;
        return;
    }
    struct Clear { std::atomic<bool>* b; ~Clear() { b->store(false); } } clear{&impl_->busy};

    auto frame = pool.TryGetNextFrame();
    if (!frame) return;

    // The display may have changed size since capture started.
    //
    // The frame reports what the desktop is now; the pool was sized for what
    // it was. Left alone, every coordinate downstream is computed against a
    // stale resolution, which puts the region and every box in the wrong
    // place -- toward the top left when the screen has grown, because the
    // centre of a 1080p desktop is the upper-left quadrant of a 1440p one.
    //
    // Checking the frame rather than listening for a window message means
    // this is noticed however the change happened: a game switching mode, a
    // driver event, or the user changing it in settings.
    {
        const auto size = frame.ContentSize();
        if (size.Width != screenW_ || size.Height != screenH_) {
            sh.log.info("Display size changed from " +
                        std::to_string(screenW_) + "x" + std::to_string(screenH_) +
                        " to " + std::to_string(size.Width) + "x" +
                        std::to_string(size.Height) + ". Recreating capture.");

            screenW_ = size.Width;
            screenH_ = size.Height;
            detectedW_ = size.Width;
            detectedH_ = size.Height;

            // The pool has to be rebuilt at the new size, and the ROI
            // texture with it: the old one was registered with CUDA against
            // a surface that no longer describes the desktop.
            frame.Close();
            pool.Recreate(impl_->winrtDevice,
                          WGDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                          2, size);
            impl_->releaseRoi();
            impl_->sizeChanged = true;
            return;
        }
    }

    // Applied here: before anything is mapped, and on the only thread
    // allowed to release these resources.
    {
        bool expected = true;
        if (impl_->applySize.compare_exchange_strong(expected, false)) {
            const int ww = impl_->wantW.load();
            const int hh = impl_->wantH.load();
            if (ww > 0 && hh > 0) { screenW_ = ww; screenH_ = hh; }
            else if (detectedW_ > 0) { screenW_ = detectedW_; screenH_ = detectedH_; }
            impl_->releaseRoi();
            impl_->sizeChanged = true;
            return;      // rebuild on the next frame, with nothing in flight
        }
    }

    Config cfg = sh.snapshotConfig();
    if (!cfg.pipelineOn) return;

    sh.timings.stage[ST_ARRIVE].add((now_ns() - t0) / 1e6);

    if (!ensureRoi(cfg.fov, cfg.fov, sh.log)) return;
    const int rw = impl_->roiW, rh = impl_->roiH;

    // ---- where is the ROI this frame? ---------------------------------
    int cx = screenW_ / 2, cy = screenH_ / 2;
    if (cfg.roiMode == ROI_MOUSE) {
        POINT p{};
        // The cursor is reported on the virtual desktop; everything below
        // works in coordinates local to the captured monitor.
        if (GetCursorPos(&p)) { cx = p.x - screenX_; cy = p.y - screenY_; }
    }
    // Offsets are a percentage of the screen, so a profile saved on one
    // monitor lands in the same relative spot on another.
    cx += (int)(cfg.roiOffXPct * 0.01f * screenW_);
    cy += (int)(cfg.roiOffYPct * 0.01f * screenH_);
    int left = std::max(0, std::min(cx - rw / 2, screenW_ - rw));
    int top  = std::max(0, std::min(cy - rh / 2, screenH_ - rh));

    // ---- crop on the GPU ----------------------------------------------
    {
        ScopedTimer t(sh.timings.stage[ST_COPY]);
        auto access = frame.Surface().as<
            ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> src;
        if (FAILED(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), src.put_void())))
            return;

        D3D11_BOX box{};
        box.left   = (UINT)left;
        box.top    = (UINT)top;
        box.front  = 0;
        box.right  = (UINT)(left + rw);
        box.bottom = (UINT)(top + rh);
        box.back   = 1;
        impl_->ctx->CopySubresourceRegion(impl_->roiTex.get(), 0, 0, 0, 0,
                                          src.get(), 0, &box);
        impl_->ctx->Flush();
    }

    // ---- map into CUDA --------------------------------------------------
    MappedRoi roi;
    {
        ScopedTimer t(sh.timings.stage[ST_MAP]);
        if (cudaGraphicsMapResources(1, &impl_->cudaRes, stream_) != cudaSuccess) {
            sh.counters.errors++;
            sh.log.error("cudaGraphicsMapResources failed.");
            return;
        }
        cudaArray_t arr = nullptr;
        cudaGraphicsSubResourceGetMappedArray(&arr, impl_->cudaRes, 0, 0);
        if (arr != impl_->cudaArr) {
            if (impl_->texObj) cudaDestroyTextureObject(impl_->texObj);
            impl_->cudaArr = arr;
            cudaResourceDesc rd{};
            rd.resType = cudaResourceTypeArray;
            rd.res.array.array = arr;
            cudaTextureDesc td{};
            td.addressMode[0] = cudaAddressModeClamp;
            td.addressMode[1] = cudaAddressModeClamp;
            td.filterMode     = cudaFilterModeLinear;
            td.readMode       = cudaReadModeNormalizedFloat;
            td.normalizedCoords = 0;
            cudaCreateTextureObject(&impl_->texObj, &rd, &td, nullptr);
        }
        roi.tex = impl_->texObj;
        // Translated back to the desktop on the way out, because that is
        // the space the overlay and the controller both work in.
        roi.width = rw; roi.height = rh;
        roi.left = left + screenX_;
        roi.top  = top  + screenY_;
        roi.stampNs = t0;
    }

    // Optional readback for dataset capture. Deliberately after the CUDA
    // map so the inference path is never delayed by it.
    if (wantHost_) {
        if (!impl_->stageTex) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width  = rw;
            sd.Height = rh;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* st = nullptr;
            if (SUCCEEDED(device_->CreateTexture2D(&sd, nullptr, &st)))
                impl_->stageTex.attach(st);
        }
        if (impl_->stageTex) {
            impl_->ctx->CopyResource(impl_->stageTex.get(), impl_->roiTex.get());
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(impl_->ctx->Map(impl_->stageTex.get(), 0,
                                          D3D11_MAP_READ, 0, &ms))) {
                impl_->hostBuf.resize((size_t)ms.RowPitch * rh);
                memcpy(impl_->hostBuf.data(), ms.pData, impl_->hostBuf.size());
                impl_->ctx->Unmap(impl_->stageTex.get(), 0);
                roi.host   = impl_->hostBuf.data();
                roi.stride = (int)ms.RowPitch;
            }
        }
    }

    const bool worked = impl_->cb ? impl_->cb(roi) : false;

    cudaGraphicsUnmapResources(1, &impl_->cudaRes, stream_);

    // Only frames that did the work are timed. An idle skip is a different
    // thing entirely and averaging the two together describes neither.
    if (worked) {
        const double ms = (now_ns() - t0) / 1e6;
        sh.timings.stage[ST_TOTAL].add(ms);
        sh.traces.loop.add(ms, t0);
        sh.counters.frames++;
    }
}

void Capture::stop() {
    if (!running_) { impl_->releaseRoi(); return; }
    running_ = false;
    try {
        if (impl_->pool) impl_->pool.FrameArrived(impl_->token);
        if (impl_->session) impl_->session.Close();
        if (impl_->pool) impl_->pool.Close();
    } catch (...) {}
    impl_->session = nullptr;
    impl_->pool = nullptr;
    impl_->item = nullptr;
    impl_->releaseRoi();
    if (stream_) { cudaStreamDestroy(stream_); stream_ = nullptr; }
    impl_->ctx = nullptr;
    device_.reset();
}

} // namespace lc
