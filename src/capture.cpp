// capture.cpp
#include "capture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <inspectable.h>
#include <dxgi1_2.h>
#include <d3d11_4.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <cuda_d3d11_interop.h>

#include <algorithm>
#include <atomic>
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
        roiW = roiH = 0;
    }
};

Capture::Capture() : impl_(std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

// ---------------------------------------------------------------- start

bool Capture::start(int monitorIndex, Shared& shared, FrameFn onFrame) {
    stop();
    impl_->shared = &shared;
    impl_->cb     = std::move(onFrame);
    Log& log = shared.log;

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

    MONITORINFO mi{sizeof(MONITORINFO)};
    GetMonitorInfoW(ec.hmon, &mi);
    screenW_ = mi.rcMonitor.right  - mi.rcMonitor.left;
    screenH_ = mi.rcMonitor.bottom - mi.rcMonitor.top;

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
    if (cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking) != cudaSuccess) {
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
        auto winrtDevice = inspectable.as<WGDX::Direct3D11::IDirect3DDevice>();

        // 2 buffers: enough to avoid a stall, few enough that we never sit
        // on a stale frame.
        impl_->pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            WGDX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            { impl_->item.Size().Width, impl_->item.Size().Height });

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
                onFrameArrived(pool);
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

    impl_->roiW = w;
    impl_->roiH = h;
    return true;
}

// ------------------------------------------------------------ frame path

void Capture::onFrameArrived(void* poolPtr) {
    auto& pool = *(WGC::Direct3D11CaptureFramePool*)poolPtr;
    Shared& sh = *impl_->shared;

    int64_t t0 = now_ns();
    int64_t prev = impl_->lastArrive.exchange(t0);
    if (prev) sh.timings.stage[ST_CADENCE].add((t0 - prev) / 1e6);
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

    Config cfg = sh.snapshotConfig();
    if (!cfg.pipelineOn) return;

    sh.timings.stage[ST_ARRIVE].add((now_ns() - t0) / 1e6);

    if (!ensureRoi(cfg.roiW, cfg.roiH, sh.log)) return;
    const int rw = impl_->roiW, rh = impl_->roiH;

    // ---- where is the ROI this frame? ---------------------------------
    int cx = screenW_ / 2, cy = screenH_ / 2;
    if (cfg.roiMode == ROI_MOUSE) {
        POINT p{};
        if (GetCursorPos(&p)) { cx = p.x; cy = p.y; }
    }
    cx += cfg.roiOffX;
    cy += cfg.roiOffY;
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
        roi.width = rw; roi.height = rh; roi.left = left; roi.top = top;
    }

    if (impl_->cb) impl_->cb(roi);

    cudaGraphicsUnmapResources(1, &impl_->cudaRes, stream_);
    sh.timings.stage[ST_TOTAL].add((now_ns() - t0) / 1e6);
    sh.counters.frames++;
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
