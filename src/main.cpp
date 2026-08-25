// main.cpp -- loopcore control panel.
//
// Threads that matter:
//   * WinRT thread pool : FrameArrived -> crop -> CUDA -> TensorRT -> publish
//   * builder thread    : ONNX parse and engine build, when converting
//   * this thread       : Win32 pump, panel, overlay redraw at ~60 Hz
//
// One rule keeps them from tangling: the UI never holds g_shared.cfgMutex
// while it draws. It takes a copy, draws against that, writes it back, and
// queues anything with side effects for after the lock is gone. Holding the
// lock across the draw meant any UI action that re-read the config
// deadlocked against itself, which froze the app while a topmost overlay was
// on screen -- and that looks exactly like Windows itself has broken.

#include <windows.h>
#include <d3d11.h>
#include <shellscalingapi.h>
#include <timeapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <psapi.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <bitset>
#include <functional>
#include <map>

#include "common.h"
#include "capture.h"
#include "capture_writer.h"
#include "chrome.h"
#include "control.h"
#include "crashlog.h"
#include "engine.h"
#include "flasher.h"
#include "gpumon.h"
#include "hwinfo.h"
#include "overlay.h"
#include "perfmon.h"
#include "preprocess.cuh"
#include "rawinput.h"
#include "serialports.h"
#include "paths.h"
#include "sketch.h"
#include "sessiontrack.h"
#include "store.h"
#include "theme.h"
#include "tuner.h"
#include "builder.h"
#include "resource.h"

#include <NvInferVersion.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#if IMGUI_VERSION_NUM >= 19200
    #define LC_CHILD_BORDER ImGuiChildFlags_Borders
#else
    #define LC_CHILD_BORDER ImGuiChildFlags_Border
#endif

using namespace lc;
namespace ui = lc::ui;

// --------------------------------------------------------------- globals

static HWND                    g_hwnd = nullptr;
static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;

static Shared        g_shared;
static Capture       g_capture;
static Engine        g_engine;
static Overlay       g_overlay;
static ModelBuilder  g_builder;
static Controller    g_control;
static PerfMonitor   g_perf;
static Flasher       g_flasher;
static CaptureWriter g_capwriter;
static Tuner         g_tuner;
static SessionTracker g_session;
static MouseSketch   g_sketch;
static std::string   g_sketchResult;
// The model being renamed in place, and the text being typed for it.
static std::string   g_renaming;
// An engine to load once the window and the pipeline are up.
static std::string   g_pendingStartupLoad;
// Set when Present reports the swap chain cannot be seen.
static bool          g_occluded = false;
static Engine::BenchResult g_benchResult;
static char          g_renameBuf[128] = "";
static char          g_sketchName[64] = "";

// The capture thread infers while the UI thread can load. Loading frees and
// reallocates every device buffer, so doing it under an inference is a use
// after free -- which is what crashed on picking a model from the library.
static std::mutex    g_engineMutex;

// Turns a stored display choice into a real index. -1, or anything stale
// after a monitor was unplugged, resolves to the primary one -- which is
// what someone who never opened the setting expects.
// Anything that touches the disk, answered once and reused.
//
// These were being called from inside card bodies, which run every frame:
// opening a file per library row, four path searches for the Firmware card,
// a display enumeration. The overlay repaints from this same thread, so a
// slow disk stalls it -- which is boxes freezing in place for a fraction of
// a second and then catching up, exactly as reported.
struct DiskCache {
    std::string arduinoCli, sketchDir, hexShield, hexNoShield;
    std::map<std::string, store::EngineMeta> meta;
    std::map<std::string, std::string> source;
    std::vector<Capture::DisplayInfo> displays;
    std::vector<rawin::DeviceInfo> devices;
    int64_t stampNs = 0;
};
static DiskCache g_disk;


static int ResolveDisplay(int stored) {
    // Enumerating displays is a system call per monitor, and this is asked
    // from card bodies. The cached list is refreshed on a timer.
    const auto& cached = g_disk.displays;
    const auto all = cached.empty() ? Capture::Displays() : cached;
    if (all.empty()) return 0;
    if (stored >= 0 && stored < (int)all.size()) return stored;
    for (const auto& d : all) if (d.primary) return d.index;
    return 0;
}


// Set from the window procedure, applied on the UI thread. Rebuilding the
// font atlas cannot happen inside a message handler, because a frame may
// already be in progress.
static std::atomic<float> g_pendingDpiScale{0.0f};

static char g_modelPath[512] = "";
static int  g_tab = 0;
static bool g_showHelp = false;
static std::vector<RawDet> g_rawScratch;

static hw::GpuInfo g_gpu;
static std::vector<SerialPortInfo> g_ports;
static void RefreshPorts() { g_ports = EnumerateSerialPorts(); }


static std::vector<std::string> g_classNames;
static std::string ClassLabel(int cls) {
    if (cls >= 0 && cls < (int)g_classNames.size() && !g_classNames[cls].empty())
        return g_classNames[cls];
    return "Class " + std::to_string(cls);
}
static hw::Advice  g_advice;

static std::vector<store::ModelEntry> g_library;

// Rebuilt on a timer and whenever the library changes, never per frame.
static void RefreshDiskCache(bool force = false) {
    const int64_t now = now_ns();
    if (!force && g_disk.stampNs != 0 &&
        now - g_disk.stampNs < 2'000'000'000LL) return;
    g_disk.stampNs = now;

    g_disk.arduinoCli  = FindArduinoCli();
    g_disk.sketchDir   = FindSketchDir();
    g_disk.hexShield   = FindBundledHex(true);
    g_disk.hexNoShield = FindBundledHex(false);
    g_disk.displays    = Capture::Displays();
    g_disk.devices     = rawin::Devices();

    g_disk.meta.clear();
    g_disk.source.clear();
    for (const auto& m : g_library) {
        g_disk.meta[m.path] = store::loadEngineMeta(m.path);
        g_disk.source[m.path] = store::findSourceFor(m.path);
    }
    if (g_modelPath[0] && !g_disk.meta.count(g_modelPath))
        g_disk.meta[g_modelPath] = store::loadEngineMeta(g_modelPath);
}

static std::vector<store::Profile>    g_profiles;
static char g_profileName[64] = "";

static void RefreshLibrary()  { g_library  = store::listModels(); }
static void RefreshProfiles() { g_profiles = store::listProfiles(); }

// Anything with a side effect is recorded here during the draw and executed
// after the config lock has been released.
struct Pending {
    std::string loadModel;      // load or convert this path
    std::string deleteModel;
    // The model to rename, and what to call it.
    std::string renameModel, renameTo;
    bool        benchmark = false;
    std::string loadProfile;
    std::string deleteProfile;
    std::string saveProfile;
    std::string openFolder;
    bool        browse       = false;
    bool        browseCapture = false;
    bool        verifyEngine  = false;
    bool        copyReport    = false;
    // A one-off line to send to the board, and a profile to write out.
    std::string sketchCmd;
    std::string saveSketch;
    int         switchMonitor = -1;
    bool        applyTopmost = false;
    bool        applyPriority= false;
    bool        refreshAll   = false;
};
static Pending g_pending;

// ------------------------------------------------------------ d3d for ui

static void CreateRTV() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
static void ReleaseRTV() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            want, 2, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx) != S_OK)
        return false;
    CreateRTV();
    return true;
}

static void CleanupDeviceD3D() {
    ReleaseRTV();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx  = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev  = nullptr; }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT r = 0;
    if (chrome::HandleMessage(hWnd, msg, wp, lp, &r)) return r;
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp)) return true;

    switch (msg) {
    case WM_DPICHANGED: {
        // Moved to a display with different scaling. Windows supplies the
        // rectangle the window should take to stay the same physical size;
        // taking it is the difference between the window appearing to jump
        // and appearing to stay put.
        const RECT* r = (const RECT*)lp;
        if (r) {
            SetWindowPos(hWnd, nullptr, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_pendingDpiScale = (float)HIWORD(wp) / 96.0f;
        return 0;
    }
    case WM_INPUT:
        rawin::Handle(lp);
        return 0;
    case WM_SIZE:
        if (g_dev && wp != SIZE_MINIMIZED) {
            ReleaseRTV();
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            CreateRTV();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// ------------------------------------------------------------- pipeline

// True when the model's output is going to be used for something. With the
// overlay on, detections are always shown, so it always runs. With the
// overlay off, nothing consumes the result unless control is engaged -- so
// running the model then is pure waste.
static bool ShouldInfer(const Config& cfg) {
    if (!cfg.pipelineOn) return false;
    // Enough work to hold the clocks, and no more.
    //
    // Two failures either side of this. Inferring on every arrival held the
    // clocks perfectly and competed with the game for the card, which is
    // what made the delay climb whenever the game was in front. A fixed
    // fifty a second stopped competing and let the clocks sag to a third of
    // boost instead -- and inference time is inversely proportional to
    // clock, so that is the same delay arriving by the opposite route. It
    // took a minute or two because that is how long the card takes to wind
    // down.
    //
    // Neither fixed rate is right, because the rate that holds the clocks
    // depends on the card, its power settings and what else is running. So
    // the clock is measured and the rate follows it: the loop asks for as
    // little work as keeps the reading up.
    if (cfg.keepGpuBoosted && cfg.aiEnabled) {
        static int64_t lastKeepAlive = 0;
        static int64_t periodNs = 20'000'000LL;   // start at 50 Hz
        static int64_t lastCheck = 0;

        const int64_t nowK = now_ns();
        if (nowK - lastCheck > 400'000'000LL) {
            lastCheck = nowK;
            const auto g = gpumon::Poll();
            if (g.valid && g.coreMaxMHz > 0) {
                const float frac = (float)g.coreMHz / (float)g.coreMaxMHz;
                // Below four fifths of boost, ask for more often; comfortably
                // above it, back off. The gap between the two thresholds
                // stops it oscillating around a single point.
                if (frac < 0.80f)
                    periodNs = std::max((int64_t)4'000'000, periodNs / 2);
                else if (frac > 0.90f)
                    periodNs = std::min((int64_t)40'000'000, periodNs * 3 / 2);
            }
        }

        if (nowK - lastKeepAlive >= periodNs) {
            lastKeepAlive = nowK;
            return true;
        }
    }
    // A tune needs detections whatever the overlays are set to; without this
    // the whole measurement runs against a model that is not being asked to
    // do anything.
    if (Tuner::WantsInference()) return true;
    if (!cfg.aiEnabled) return false;
    if (cfg.overlayOn) return true;
    if (!cfg.controlEnabled) return false;
    if (cfg.activationMode == ActAlways) return true;
    // Either activation key, not just the first.
    //
    // This is what decides whether a frame is inferred at all, and it was
    // only ever asked about activationKey. Holding the second key therefore
    // engaged the controller -- which reads both -- while the model never
    // ran, so there were no detections, no target, and nothing to move
    // toward. The key registered, the aim did not, and the two halves
    // disagreed about whether anything was happening.
    //
    // Deliberately its own check rather than a call into the controller: this
    // runs on the capture thread for every arrival, and it must not take the
    // lock the control loop is using.
    //
    // It is also deliberately more permissive than the controller, which can
    // additionally require the press to come from a chosen device. Inferring
    // on a press this loop would go on to ignore costs one frame of work;
    // refusing to infer on a press it would have acted on costs the shot. Of
    // the two ways to be wrong, extra work is the cheap one.
    auto down = [](int vk) {
        return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    return down(cfg.activationKey) || down(cfg.activationKey2);
}

// Whether the previous arrival actually ran the model, as opposed to merely
// reaching the point where it might have.
static bool g_lastFrameWorked = false;

static bool OnFrame(const MappedRoi& roi) {
    Config cfg = g_shared.snapshotConfig();

    // What this frame cost the application in front, and whether to skip it.
    //
    // The gap since the previous arrival is that application's frame
    // interval, so comparing gaps on frames we worked on against gaps on
    // frames we skipped measures the cost directly. Skipping is the only
    // lever that gives performance back: a priority change moves work
    // around, it does not remove any.
    static int64_t lastArrival = 0;
    static int strideCount = 0;
    bool skipThisFrame = false;
    {
        const int64_t nowA = roi.stampNs ? roi.stampNs : now_ns();
        const double gap = lastArrival ? (nowA - lastArrival) / 1e6 : 0.0;
        lastArrival = nowA;

        const int stride = g_shared.load.stride.load();
        if (stride > 1) {
            if (++strideCount < stride) skipThisFrame = true;
            else strideCount = 0;
        } else {
            strideCount = 0;
        }

        // Attributed to whichever case actually applied to the previous
        // interval, which is the one just measured.
        //
        // The flag is only set once this frame is known to have inferred,
        // which is further down -- passing the stride test is not the same
        // as doing the work. Several checks after this point can still bow
        // out: no engine, a load in progress, or the activation key not
        // being held. Recording "worked" here counted all of those as busy
        // frames, so the busy bucket filled with intervals where nothing ran
        // and the idle bucket stayed nearly empty. Every figure derived from
        // the pair -- the frame-rate cost, and the adaptive stride that
        // reads it -- was measuring the wrong thing.
        if (gap > 0.1 && gap < 500.0) g_shared.load.note(gap, g_lastFrameWorked);
        g_lastFrameWorked = false;
    }
    if (skipThisFrame) {
        g_shared.counters.idle++;
        return false;
    }

    // Held for the whole inference. A load waits for the current frame
    // rather than pulling the buffers out from under it.
    std::unique_lock<std::mutex> engineLock(g_engineMutex, std::try_to_lock);
    if (!engineLock.owns_lock()) return false;   // a load is in progress
    if (!g_engine.ready()) return false;
    if (!ShouldInfer(cfg)) {
        g_shared.counters.idle++;
        return false;
    }

    // Marked before anything this frame queues, so the device time ahead of
    // the model can be told from the model's own.
    g_engine.markFrameStart(g_capture.stream());

    {
        // No wall-clock timer here.
        //
        // The launch is asynchronous, so timing the call measured how long it
        // took to put work on a queue -- a few microseconds that told nobody
        // anything, while the row was labelled as though it were the cost of
        // preprocessing. The device time recorded after the model is the real
        // figure and it goes into this same row.
        cudaError_t ce = launch_preprocess(
            roi.tex, g_engine.inputBuffer(),
            g_engine.inputW(), g_engine.inputH(),
            roi.width, roi.height,
            g_engine.inputIsHalf(), 0.5f,
            g_capture.stream());
        if (ce != cudaSuccess) {
            g_shared.counters.errors++;
            g_shared.log.error(std::string("Preprocess kernel failed: ") +
                               cudaGetErrorString(ce));
            return false;
        }
    }

    {
        ScopedTimer t(g_shared.timings.stage[ST_INFER]);
        // Decoded generously and trimmed later. "Max boxes" is about what
        // gets drawn, but applying it here threw away the box being tracked
        // whenever a higher-scoring one appeared -- so tracking could not
        // possibly hold on to anything.
        constexpr int kDecodeMax = 64;
        if (!g_engine.infer(g_capture.stream(), cfg.confThresh, kDecodeMax,
                            g_rawScratch, g_shared.log)) {
            g_shared.counters.errors++;
            return false;
        }
    }

    // Device time, recorded separately. A large gap between this and the
    // inference row above is the thread waiting, not the model working.
    // Past every early-out, so the model definitely ran on this frame.
    g_lastFrameWorked = true;

    const double gpuMs = g_engine.diag().gpuMs;
    g_shared.timings.stage[ST_GPU].add(gpuMs);

    // The device time spent before the model ran. Recorded into the same
    // stage the launch used, replacing a figure that only ever measured how
    // long the launch call took to return -- microseconds, and useless.
    {
        const double preMs = g_engine.diag().preMs;
        if (preMs > 0.0) g_shared.timings.stage[ST_PRE].add(preMs);
    }
    g_shared.traces.gpu.add(gpuMs, now_ns());

    // And again, split by who had the foreground.
    //
    // A GPU is shared between processes and the one the user is looking at
    // is served first, so the same work can take twice as long purely
    // because another window came forward. Splitting it is the only way to
    // tell that apart from the model itself getting slower, and the
    // foreground check is a single cheap call.
    {
        static HWND fg = nullptr;
        static int64_t lastFgCheck = 0;
        const int64_t nowFg = now_ns();
        if (nowFg - lastFgCheck > 200'000'000LL) {
            lastFgCheck = nowFg;
            fg = GetForegroundWindow();
        }
        const bool oursInFront = (fg == g_hwnd);
        g_shared.timings.stage[oursInFront ? ST_GPU_FG : ST_GPU_BG].add(gpuMs);
    }

    // Reused between frames so the per-frame path does no allocation once it
    // has settled. Only ever touched on the capture thread.
    static std::vector<Det> dets;

    {
        ScopedTimer t(g_shared.timings.stage[ST_POST]);
        LetterboxMeta m = letterbox_meta(g_engine.inputW(), g_engine.inputH(),
                                          roi.width, roi.height);
        dets.clear();
        dets.reserve(g_rawScratch.size());
        for (const RawDet& r : g_rawScratch) {
            Det d;
            d.x1 = (r.x1 - m.padX) / m.scale + roi.left;
            d.y1 = (r.y1 - m.padY) / m.scale + roi.top;
            d.x2 = (r.x2 - m.padX) / m.scale + roi.left;
            d.y2 = (r.y2 - m.padY) / m.scale + roi.top;
            d.score = r.score;
            d.cls   = r.cls;
            dets.push_back(d);
        }
    }

    {
        // Published as a copy, and the local kept.
        //
        // Swapping handed our data over and gave back the previous frame's,
        // so both the capture writer and the controller had to read it back
        // out again -- two extra vector copies and two extra acquisitions of
        // a lock the UI thread also wants, every frame, for no gain.
        {
            ScopedTimer tp(g_shared.timings.stage[ST_PUBLISH]);
            std::lock_guard<std::mutex> lk(g_shared.results.m);
            g_shared.results.dets = dets;
            g_shared.results.roiL = roi.left;
            g_shared.results.roiT = roi.top;
            g_shared.results.roiR = roi.left + roi.width;
            g_shared.results.roiB = roi.top + roi.height;
            g_shared.results.stamp = now_ns();
            g_shared.counters.detections += dets.size();
            if (dets.empty()) g_shared.counters.empty++;
        }

        // Control first, and before anything optional. It is the only
        // consumer whose latency is felt, so nothing else goes ahead of it.
        {
            ScopedTimer tc(g_shared.timings.stage[ST_CONTROL]);
            g_control.submit(dets,
                             roi.left + roi.width * 0.5f,
                             roi.top  + roi.height * 0.5f,
                             cfg, roi.stampNs);
        }

        if (g_capwriter.running() && roi.host) {
            float top = 0.0f;
            for (const auto& d : dets) top = std::max(top, d.score);
            g_capwriter.offer(roi.host, roi.width, roi.height, roi.stride,
                              dets, top, roi.left, roi.top, cfg);
        }
    }
    return true;
}

// --------------------------------------------------------- actions

// Runs from the main loop, never during the draw.
static void DoLoadOrConvert(const std::string& path) {
    if (g_builder.busy()) {
        g_shared.log.warn("A conversion is already running.");
        return;
    }

    const std::string ext = ModelBuilder::extensionOf(path);

    if (ext == "engine") {
        std::string local;
        if (!store::importInto(path, store::modelsDir(), local, g_shared.log)) return;
        strncpy_s(g_modelPath, sizeof(g_modelPath), local.c_str(), _TRUNCATE);
        std::lock_guard<std::mutex> el(g_engineMutex);
        if (g_engine.load(local, g_shared.log)) {
            g_classNames = store::loadClassNames(local);
            if (!g_classNames.empty())
                g_shared.log.info("Loaded " + std::to_string(g_classNames.size()) +
                                  " class names.");
        }
        RefreshLibrary();
        RefreshDiskCache(true);
        return;
    }

    // Sources go to bin/converted; only the built engine joins the library.
    std::string src;
    if (!store::importInto(path, store::convertedDir(), src, g_shared.log)) return;

    // A matching engine that is newer than the source is the same build.
    // Rebuilding it would cost minutes for an identical file.
    {
        const size_t slash = src.find_last_of("\\/");
        const size_t dot   = src.find_last_of('.');
        const std::string stem = src.substr(slash + 1,
                                            (dot > slash) ? dot - slash - 1
                                                          : std::string::npos);
        const std::string existing = store::modelsDir() + "\\" + stem + ".engine";

        WIN32_FILE_ATTRIBUTE_DATA se{}, ee{};
        if (GetFileAttributesExA(existing.c_str(), GetFileExInfoStandard, &ee) &&
            GetFileAttributesExA(src.c_str(), GetFileExInfoStandard, &se)) {
            ULARGE_INTEGER et{}, st2{};
            et.LowPart = ee.ftLastWriteTime.dwLowDateTime;
            et.HighPart = ee.ftLastWriteTime.dwHighDateTime;
            st2.LowPart = se.ftLastWriteTime.dwLowDateTime;
            st2.HighPart = se.ftLastWriteTime.dwHighDateTime;
            if (et.QuadPart >= st2.QuadPart) {
                g_shared.log.info("An engine built from this source already "
                                  "exists and is newer; loading it instead of "
                                  "rebuilding.");
                strncpy_s(g_modelPath, sizeof(g_modelPath), existing.c_str(), _TRUNCATE);
                std::lock_guard<std::mutex> el(g_engineMutex);
                if (g_engine.load(existing, g_shared.log))
                    g_classNames = store::loadClassNames(existing);
                RefreshLibrary();
                return;
            }
        }
    }

    Config cfg = g_shared.snapshotConfig();
    BuildOptions o;
    o.width       = cfg.engineW;
    o.height      = cfg.engineH;
    o.fp16        = cfg.buildFp16;
    o.embedNms    = cfg.buildEmbedNms;
    o.portable    = cfg.buildPortable;
    o.workspaceMB = cfg.workspaceMB;
    o.optLevel    = cfg.buildOptLevel;
    o.outputDir   = store::modelsDir();

    g_shared.log.info("Converting " + src + " to a TensorRT engine.");
    g_builder.start(src, o, g_shared.log);
}

static void DoBrowse() {
    char file[512] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = "Models (*.engine;*.onnx;*.pt)\0*.engine;*.onnx;*.pt\0"
                      "TensorRT engine (*.engine)\0*.engine\0"
                      "ONNX (*.onnx)\0*.onnx\0"
                      "PyTorch (*.pt)\0*.pt\0"
                      "All files\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        strncpy_s(g_modelPath, sizeof(g_modelPath), file, _TRUNCATE);
        // Acted on immediately rather than waiting for a second click.
        //
        // Picking a file is the decision; the button that followed only
        // asked the user to confirm what they had already chosen, and its
        // label changed between Load and Convert for reasons that are the
        // program's business rather than theirs. A .pt or .onnx is converted
        // and an .engine is loaded -- both are "use this model", which is
        // what was meant by choosing it.
        g_pending.loadModel = g_modelPath;
    }
}

static void ApplyPriority(int mode) {
    // HIGH_PRIORITY_CLASS can starve the shell badly enough that the taskbar
    // stops responding, so it is opt-in and warned about rather than default.
    DWORD cls = NORMAL_PRIORITY_CLASS;
    if (mode == 1) cls = ABOVE_NORMAL_PRIORITY_CLASS;
    else if (mode == 2) cls = HIGH_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), cls);
}

// A folder picker, not a file one. SHBrowseForFolder is the version that
// exists on every Windows without pulling in the newer COM dialog.
static bool DoBrowseFolder(char* out, size_t cap) {
    BROWSEINFOA bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = "Where should captures be written?";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListA(pidl, path) == TRUE;
    CoTaskMemFree(pidl);
    if (ok) strncpy_s(out, cap, path, _TRUNCATE);
    return ok;
}

static void RunPending(Config& cfg) {
    // The engine remembered from last session, loaded on the first frame.
    //
    // Routed through the same pending action a click would use, so it takes
    // exactly the path a manual load takes -- including refreshing the
    // library and the disk cache -- rather than being a second, subtly
    // different way of loading a model.
    if (!g_pendingStartupLoad.empty()) {
        g_pending.loadModel = g_pendingStartupLoad;
        g_pendingStartupLoad.clear();
    }

    if (g_pending.browse) DoBrowse();
    if (g_pending.browseCapture)
        DoBrowseFolder(cfg.capturePath, sizeof(cfg.capturePath));

    // Restarting capture has to happen here, on the UI thread, and only
    // between frames: stop() closes the pool and waits for callbacks to
    // finish, which cannot be done from inside one.
    if (g_pending.switchMonitor >= 0) {
        const int want = g_pending.switchMonitor;
        g_pending.switchMonitor = -1;
        g_shared.log.info("Switching capture to display " +
                          std::to_string(want) + ".");
        g_capture.stop();
        if (!g_capture.start(want, g_shared, OnFrame)) {
            g_shared.log.error("Could not start capture on that display. "
                               "Falling back to the first one.");
            g_capture.start(ResolveDisplay(g_shared.snapshotConfig().captureMonitor),
                    g_shared, OnFrame);
        }
        g_overlay.destroy();
        g_overlay.create(GetModuleHandleW(nullptr), 0, 0,
                         g_capture.screenWidth(), g_capture.screenHeight(),
                         g_shared.log);
        ui::Toast("Capture display changed.");
    }

    if (!g_pending.sketchCmd.empty()) {
        g_control.sendBoardLine(g_pending.sketchCmd.c_str());
        g_pending.sketchCmd.clear();
    }

    if (!g_pending.saveSketch.empty()) {
        const std::string nm = g_pending.saveSketch;
        g_pending.saveSketch.clear();
        // Written beside the config, as plain text: the value of this file is
        // that someone can read it, compare it against another mouse, or send
        // it to somebody with the same model.
        std::string clean;
        for (char c : nm)
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ')
                clean.push_back(c == ' ' ? '-' : c);
        if (clean.empty()) clean = "mouse";
        const std::string path = store::configDir() + "\\" + clean + ".mouse";
        std::ofstream f(path);
        if (f) {
            f << "# loopcore mouse profile\n# " << clean << "\n\n"
              << g_sketchResult;
            f.close();
            ui::Toast(("Saved " + clean + ".mouse").c_str());
            g_shared.log.info("Mouse profile written to " + path);
        } else {
            ui::Toast("Could not write the profile.");
        }
    }

    if (g_pending.benchmark) {
        g_pending.benchmark = false;
        // On the capture stream, because that is the one the model actually
        // runs on -- benchmarking a different stream would measure a
        // different thing.
        g_benchResult = g_engine.benchmark(g_capture.stream(), 60,
                                           g_shared.log);
    }

    if (g_pending.copyReport) {
        g_pending.copyReport = false;

        // Yielded to for the duration.
        //
        // Building this means sorting thirteen rings of four thousand
        // samples and assembling a large string, which is long enough to
        // starve the control thread on a busy machine -- and a starved
        // control loop is the one thing that must not happen, because the
        // tick that follows a stall has real movement to dispense. Lowering
        // this thread while the work runs costs a few milliseconds of report
        // and protects the aim.
        struct PriorityDip {
            int prev;
            PriorityDip() : prev(GetThreadPriority(GetCurrentThread())) {
                SetThreadPriority(GetCurrentThread(),
                                  THREAD_PRIORITY_BELOW_NORMAL);
            }
            ~PriorityDip() { SetThreadPriority(GetCurrentThread(), prev); }
        } dip;
        std::string r;
        r.reserve(16384);
        char b[400];

        const auto cs = g_control.state();
        snprintf(b, sizeof(b), "loopcore timing report\nengaged: %s\n",
                 cs.engaged ? "yes" : "no");
        r += b;

        {
            std::lock_guard<std::mutex> lk(g_shared.results.m);
            float top = 0.0f;
            for (const auto& d : g_shared.results.dets)
                top = std::max(top, d.score);
            snprintf(b, sizeof(b), "targets: %d   best confidence: %.3f\n",
                     (int)g_shared.results.dets.size(), top);
            r += b;
            for (const auto& d : g_shared.results.dets) {
                snprintf(b, sizeof(b),
                         "  class %d  %.2f  box %.0f,%.0f  %.0fx%.0f\n",
                         d.cls, d.score, d.x1, d.y1, d.x2 - d.x1, d.y2 - d.y1);
                r += b;
            }
        }

        if (g_engine.ready()) {
            r += "engine: " + g_engine.describe() + "\n";
            const auto dg = g_engine.diag();
            snprintf(b, sizeof(b),
                     "decode: %d candidates, %d kept, best %.3f\n",
                     dg.rawCandidates, dg.kept, dg.maxScore);
            r += b;
        }
        snprintf(b, sizeof(b),
                 "response scale: %.3f px per unit%s   lead trust %.2f\n",
                 g_control.responseScale(),
                 g_control.responseLearned() ? "" : "  (not yet learned)",
                 g_control.leadTrust());
        r += b;

        const auto gsr = gpumon::Poll();
        if (gsr.valid) {
            snprintf(b, sizeof(b),
                     "gpu: %d/%d MHz, %d C, %d%% load, %d/%d W, throttle: %s\n",
                     gsr.coreMHz, gsr.coreMaxMHz, gsr.tempC, gsr.utilGpuPct,
                     gsr.powerW, gsr.powerLimitW, gsr.throttle.c_str());
            r += b;
        }

        static std::vector<double> sc2;
        r += "\nstage            mean     p50     p90     p99     max       n\n";
        for (int st2 = 0; st2 < ST_COUNT; ++st2) {
            const auto sm = g_shared.timings.stage[st2].summary(sc2);
            snprintf(b, sizeof(b), "%-14s %7.3f %7.3f %7.3f %7.3f %7.3f %7d\n",
                     stage_name(st2), sm.mean, sm.p50, sm.p90, sm.p99,
                     sm.max, sm.n);
            r += b;
        }

        const uint64_t arr = g_shared.counters.arrivals.load();
        const uint64_t frm = g_shared.counters.frames.load();
        const uint64_t skp = g_shared.counters.skipped.load();
        snprintf(b, sizeof(b),
                 "\narrived %llu  processed %llu  dropped %llu (%.1f%%)  "
                 "idle %llu  errors %llu\n",
                 (unsigned long long)arr, (unsigned long long)frm,
                 (unsigned long long)skp, arr ? 100.0 * skp / arr : 0.0,
                 (unsigned long long)g_shared.counters.idle.load(),
                 (unsigned long long)g_shared.counters.errors.load());
        r += b;

        // The trace, as a column per line: time, low, mean, high. Far more
        // useful than a wall of raw samples, because the spread is what
        // distinguishes a steady loop from an alternating one.
        {
            std::vector<TimeSeries::Column> cols;
            g_shared.traces.loop.sample(cfg.perfGraphSecs, 60, cols);
            r += "\nloop trace, oldest first: low / mean / high, ms\n";
            for (const auto& c : cols) {
                if (!c.has) { r += "  -\n"; continue; }
                snprintf(b, sizeof(b), "  %6.2f %6.2f %6.2f\n",
                         c.mn, c.mean, c.mx);
                r += b;
            }
        }

        ImGui::SetClipboardText(r.c_str());
        ui::Toast("Timing report copied.");
    }

    if (g_pending.verifyEngine) {
        g_pending.verifyEngine = false;
        if (g_engine.ready()) {
            std::lock_guard<std::mutex> el(g_engineMutex);
            const auto r = g_engine.smokeTest(g_capture.stream(), g_shared.log);
            ui::Toast(r.ran && r.finite && r.nonZero
                      ? "Engine verified." : "Engine check failed, see the log.");
        } else {
            ui::Toast("No engine loaded.");
        }
    }

    if (!g_pending.loadModel.empty())   DoLoadOrConvert(g_pending.loadModel);
    if (!g_pending.deleteModel.empty()) {
        store::removeModel(g_pending.deleteModel, g_shared.log);
        RefreshLibrary();
    }
    if (!g_pending.renameModel.empty()) {
        const std::string was = g_pending.renameModel;
        if (store::renameModel(was, g_pending.renameTo, g_shared.log)) {
            // The loaded path is followed across the rename, or the library
            // would stop recognising the running model as its own entry.
            if (was == g_modelPath) {
                const size_t sl = was.find_last_of("\\/");
                const size_t dt = was.find_last_of('.');
                const std::string dir = (sl == std::string::npos)
                                      ? std::string() : was.substr(0, sl + 1);
                const std::string ext = (dt != std::string::npos && dt > sl)
                                      ? was.substr(dt) : std::string();
                const std::string now =
                    dir + store::sanitiseName(g_pending.renameTo) + ext;
                strncpy_s(g_modelPath, sizeof(g_modelPath), now.c_str(),
                          _TRUNCATE);
            }
            RefreshLibrary();
            RefreshDiskCache(true);
        }
        g_pending.renameModel.clear();
        g_pending.renameTo.clear();
    }
    if (!g_pending.saveProfile.empty()) {
        const std::string name = store::sanitiseName(g_pending.saveProfile);
        if (store::saveConfig(store::profilePath(name), cfg, g_modelPath, g_shared.log))
            RefreshProfiles();
    }
    if (!g_pending.loadProfile.empty()) {
        std::string model;
        if (store::loadConfig(g_pending.loadProfile, cfg, model, g_shared.log)) {
            ApplyPriority(cfg.priorityMode);
            SetWindowPos(g_hwnd, cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            // A profile names a model, so loading one is an explicit request
            // to use it. This is the only path that auto-loads.
            if (!model.empty()) {
                strncpy_s(g_modelPath, sizeof(g_modelPath), model.c_str(), _TRUNCATE);
                if (GetFileAttributesA(model.c_str()) != INVALID_FILE_ATTRIBUTES)
                    { std::lock_guard<std::mutex> el(g_engineMutex);
                      g_engine.load(model, g_shared.log); }
                else
                    g_shared.log.warn("This profile refers to " + model +
                                      ", which is no longer there.");
            }
        }
    }
    if (!g_pending.deleteProfile.empty()) {
        store::removeProfile(g_pending.deleteProfile, g_shared.log);
        RefreshProfiles();
    }
    if (!g_pending.openFolder.empty())
        // ShellExecute opens a URL as happily as a folder.
        ShellExecuteA(nullptr, "open", g_pending.openFolder.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    if (g_pending.applyTopmost)
        SetWindowPos(g_hwnd, cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    if (g_pending.applyPriority) ApplyPriority(cfg.priorityMode);
    if (g_pending.refreshAll) { RefreshLibrary(); RefreshProfiles(); }

    g_pending = Pending{};
}

// ------------------------------------------------------------------- ui

static void TitleBar(float winW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    // Scaled with the rest of the panel. Left fixed it would be a third of
    // the intended height at 150% while everything inside it grew.
    // Scaled with the rest of the panel. Left fixed it would be a third of
    // the intended height at 150% while everything inside it grew.
    const float h = (float)chrome::kTitleBarHeight * ui::UiScale();
    // Hit testing lives in the window procedure and has to agree with what
    // was actually drawn, so it is told rather than left to guess.
    chrome::SetTitleBarHeight((int)h);

    dl->AddRectFilled(wp, ImVec2(wp.x + winW, wp.y + h), ui::col::base);
    // Easter egg: the mark comes alive under the pointer. Eased in and out
    // so it does not snap the moment the cursor crosses it.
    {
        static float logoHover = 0.0f;
        const ImVec2 lp(wp.x + 10, wp.y + h * 0.5f - 11);
        const float ls = 22.0f;
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const bool over = mp.x >= lp.x && mp.x <= lp.x + ls &&
                          mp.y >= lp.y && mp.y <= lp.y + ls;
        const float dt = ImGui::GetIO().DeltaTime;
        const float target = over ? 1.0f : 0.0f;
        logoHover += (target - logoHover) *
                     (1.0f - expf(-9.0f * (dt > 0.05f ? 0.05f : dt)));
        ui::DrawLogo(dl, lp, ls, logoHover);
    }

    // Everything competes for one row, so measure first and shed the
    // optional pieces before anything is drawn. Overlapping widgets are
    // worse than absent ones, and the help and caption glyphs are the last
    // things that may be covered.
    ImGui::PushFont(ui::fontTitle);
    const float nameW = ImGui::CalcTextSize("loopcore").x;
    ImGui::PopFont();

    // Debug is hidden outside advanced mode.
    //
    // The count is what hides it, rather than removing the entry: g_tab and
    // the saved card orders are indexed by position, so shortening the list
    // from the end keeps every other tab meaning what it did. Removing an
    // entry from the middle would renumber the rest and shuffle four tabs'
    // worth of saved layout.
    static const char* tabs[] = {"General", "Visual", "Model", "Settings",
                                 "Capture", "Debug"};
    const Config tcfg = g_shared.snapshotConfig();
    const int tabCount = tcfg.advancedMode ? 6 : 5;

    // Sitting on Debug when it disappears would leave the panel showing a
    // tab that is no longer selectable. The index is only moved when that
    // actually happens, so returning to advanced mode does not reset it.
    if (g_tab >= tabCount) g_tab = 0;

    const bool live = g_capture.running() && g_engine.ready();
    const char* status = g_builder.busy() ? "BUILDING" : live ? "RUNNING" : "IDLE";
    // Running is green regardless of the theme, not the theme's accent.
    //
    // The accent is orange in Amber, which is also the building colour, so
    // two states meaning opposite things looked the same. A fixed green
    // picked to suit the background is the one that can be recognised
    // without reading it.
    // The pill's fill follows the theme; only the ring is fixed green.
    //
    // A whole pill in a colour the theme never uses looks bolted on. The ring
    // is what needs to be recognisable without reading, and it can be that on
    // its own.
    const ImU32 statusCol = g_builder.busy() ? ui::col::warn
                          : live             ? ui::col::accent
                                             : ui::col::textFaint;
    ImGui::PushFont(ui::fontSmall);
    const float pillW = 8 + 6 + 7 + ImGui::CalcTextSize(status).x + 8;
    ImGui::PopFont();

    const float logoEnd  = 10 + 22 + 8;
    // A gap before the buttons, so there is always somewhere to grab the
    // window even when every tab is showing.
    const float dragGap  = 26.0f;
    const float rightEnd = winW - chrome::kCaptionButtonsWidth;

    float textW = 0.0f;
    for (int i = 0; i < 6; ++i) textW += ImGui::CalcTextSize(tabs[i]).x;

    // Padding per tab is what gives. Wide when there is room, tight when
    // there is not, and the wordmark and pill go before it reaches the floor.
    auto tabsWidthFor = [&](float pad) { return textW + pad * 6.0f; };

    bool showName = true, showPill = true;
    float pad = 18.0f;
    auto fits = [&]() {
        const float used = logoEnd + (showName ? nameW + 14 : 0) +
                           tabsWidthFor(pad) +
                           (showPill ? pillW + 12 : 0) +
                           // Room for the capture mark, so the tabs are not
                           // measured as though it were never there.
                           (tcfg.captureEnabled ? 20 : 0) + dragGap;
        return used < rightEnd;
    };
    if (!fits()) { pad = 14.0f; }
    if (!fits()) { showPill = false; }
    if (!fits()) { showName = false; }
    if (!fits()) { pad = 10.0f; }

    if (showName) {
        ImGui::PushFont(ui::fontTitle);
        dl->AddText(ImVec2(wp.x + logoEnd,
                           wp.y + h * 0.5f - ImGui::CalcTextSize("loopcore").y * 0.5f),
                    ui::col::text, "loopcore");
        ImGui::PopFont();
    }

    const float tabsX = wp.x + logoEnd + (showName ? nameW + 14 : 0);
    ImGui::SetCursorScreenPos(ImVec2(tabsX, wp.y));
    float tabsW = 0.0f;
    ui::TabRow(tabs, tabCount, &g_tab, h, &tabsW, pad);
    chrome::SetNoDragSpan((int)(tabsX - wp.x), (int)(tabsX - wp.x + tabsW));

    // A camera beside the pill while frames are being written.
    //
    // Capture is easy to leave running by accident -- it is keyed to the
    // same button as aiming, and it fills a disk quietly. A mark that is
    // present or absent says so without taking a row anywhere.
    // Shown whenever capture is armed, not only while frames are landing.
    //
    // Tying it to the writer meant the mark appeared and vanished with the
    // activation key -- which is the moment it is least useful, since the
    // user is busy aiming. The thing worth knowing is that capture is
    // switched on at all, because that is what fills a disk over an evening.
    if (tcfg.captureEnabled) {
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        ui::CameraGlyph(tdl,
                        ImVec2(wp.x + rightEnd - pillW - dragGap - 16.0f,
                               wp.y + h * 0.5f),
                        13.0f, ui::col::warn);
    }

    if (showPill) {
        ImGui::SetCursorScreenPos(
            ImVec2(wp.x + rightEnd - pillW - dragGap, wp.y + h * 0.5f - 10));
        // The ring marks running, so it goes while a build is in progress.
        //
        // `live` stays true during a build -- the previous engine is still
        // loaded and still working -- so the pill said BUILDING in amber
        // while wearing the green running ring. Both halves were true and
        // together they said nothing.
        ui::StatusPill(status, statusCol, g_builder.busy(),
                       (live && !g_builder.busy()) ? ui::OkGreen() : 0);
    }

    ImGui::SetCursorScreenPos(ImVec2(wp.x + rightEnd, wp.y));
    switch (ui::CaptionButtons(chrome::Maximized(g_hwnd))) {
    case 1: g_showHelp = true; break;
    case 2: ShowWindow(g_hwnd, SW_MINIMIZE); break;
    case 3: ShowWindow(g_hwnd, chrome::Maximized(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
    case 4: PostMessage(g_hwnd, WM_CLOSE, 0, 0); break;
    default: break;
    }

    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + h));
}

// A tab's cards, declared rather than written out in sequence, so the order
// can be stored and rearranged.
// Marks the card just added as advanced-only.
//
// Applied after the fact rather than passed in, so the card list reads the
// same whichever mode is in force and nothing about the card's contents
// depends on it. Hidden cards keep their place in the saved order, because
// `visible` is a filter over a list that never changes length.
static void AdvancedOnly(std::vector<struct CardDef>& v, bool advanced);

struct CardDef {
    const char* title;
    std::function<void()> body;
    // A card that is not applicable right now is skipped rather than removed,
    // because the saved order refers to cards by index and a list that
    // changes length would shuffle everything after it.
    bool visible = true;
    // Drawn full width above the columns, and not draggable.
    //
    // Some cards are a status line rather than a setting -- they are read at
    // a glance and never rearranged, and squeezing them into half the width
    // wastes the half that matters. Pinning them also keeps them out of the
    // column balancing, which is only meaningful for cards that can move.
    bool pinned = false;
};

// Per-card animation state, keyed by title. Positions are animated rather
// than assigned, so a reorder reads as the cards moving aside rather than
// teleporting.
struct CardAnim {
    float curX   = -1.0f;   // animated too: switching columns instantly is
    float curY   = -1.0f;   // the one motion that looked like a teleport
    float velX   = 0.0f;    // spring state, not just a target
    float velY   = 0.0f;
    float height = 90.0f;   // measured last frame
    int   column = 0;
};

// Moves a value toward a target as a critically damped spring.
//
// An exponential approach starts at full speed and eases only into the
// finish, which is why a card getting out of the way looked like it snapped
// and then drifted. A spring accelerates from rest and settles without
// overshooting, which is the motion the eye reads as something being moved
// rather than teleported and corrected.
//
// Critically damped is the specific case worth using here: any less and it
// wobbles past the target, any more and it crawls.
static void Spring(float& x, float& v, float target, float dt, float stiffness) {
    if (dt <= 0.0f) return;
    if (dt > 0.05f) dt = 0.05f;          // a stalled frame must not launch it
    const float w = stiffness;
    const float a = -2.0f * w * v - w * w * (x - target);
    v += a * dt;
    x += v * dt;
    // Settled: parked exactly, so a card is never left drifting by a
    // fraction of a pixel for ever.
    if (std::fabs(x - target) < 0.15f && std::fabs(v) < 2.0f) {
        x = target;
        v = 0.0f;
    }
}
// Keyed by tab and title together.
//
// Title alone was not unique: "Field of view" exists on both General and
// Visual, so the two tabs shared one animation entry and each switch dragged
// the card from wherever the other tab had left it. Every card needs its own
// state, and a title is only unique within its own tab.
static std::map<std::string, CardAnim> g_cardAnim;

static std::string CardKey(int tabIndex, const char* title) {
    return std::to_string(tabIndex) + "/" + title;
}

// Which card is being dragged, if any, and where it was grabbed.
// The card being dragged, as tab and title.
//
// A title alone is not unique -- "Field of view" exists on both General and
// Visual -- so a drag started on one tab matched the identically named card
// on the other. The animation state was already keyed this way; this had
// been left behind.
static std::string g_dragTitle;
static int         g_dragTab = -1;
static float       g_dragGrabDy = 0.0f;
static float       g_dragGrabDx = 0.0f;
static float       g_dragMouseY = 0.0f;
static int         g_dragColumn = 0;

static void AdvancedOnly(std::vector<CardDef>& v, bool advanced) {
    if (!v.empty() && !advanced) v.back().visible = false;
}

// Marks the card just added as pinned to the top, full width.
static void PinnedFullWidth(std::vector<CardDef>& v) {
    if (!v.empty()) v.back().pinned = true;
}

static void RenderCards(int tabIndex, Config& cfg, std::vector<CardDef>& cards) {
    const int n = (int)cards.size();
    int* order = cfg.cardOrder[tabIndex];
    int& count = cfg.cardOrderCount[tabIndex];

    // Saved order first, dropping anything stale, then any card the saved
    // order never knew about. A config from an older build must not be able
    // to hide a card that has since been added.
    std::vector<int> seq;
    std::vector<bool> used((size_t)n, false);
    for (int i = 0; i < count && i < Config::kOrderMax; ++i) {
        const int idx = order[i];
        if (idx >= 0 && idx < n && !used[idx]) { seq.push_back(idx); used[idx] = true; }
    }
    for (int i = 0; i < n; ++i) if (!used[i]) seq.push_back(i);

    // Only visible cards take part in the layout; hidden ones keep their
    // place in the order.
    // Pinned cards are drawn first, across the full width, and take no part
    // in the ordering or the column balancing below.
    for (size_t i = 0; i < cards.size(); ++i) {
        if (!cards[i].visible || !cards[i].pinned) continue;
        // Width zero means "take what is available", which for a card
        // outside the columns is the whole panel.
        if (ui::BeginCard(cards[i].title, true, 0.0f)) cards[i].body();
        ui::EndCard();
        ImGui::Dummy(ImVec2(0, 6));
    }

    std::vector<int> vis;
    for (int idx : seq)
        if (cards[idx].visible && !cards[idx].pinned) vis.push_back(idx);
    if (vis.empty()) return;

    const float gap    = 10.0f;
    const float colGap = 12.0f;
    const float avail  = ImGui::GetContentRegionAvail().x;
    const bool  single = avail < 600.0f;
    const float colW   = single ? avail : (avail - colGap) * 0.5f;

    // The split is stored, not recomputed. Deriving it from the count meant
    // the layout rebalanced itself the moment a card was moved, shoving
    // cards back across the divide against the user's intent.
    // -1 means "decide for me", and it stays that way until a card is
    // dragged. Committing to a number on the first frame would freeze in a
    // split computed before any card had been measured.
    int& savedSplit = cfg.cardSplit[tabIndex];

    auto balancedSplit = [&]() {
        // Balanced by height, not by count.
        //
        // Cards differ enormously -- a log viewer is many times a toggle --
        // so an even split by number regularly left one column twice as tall
        // as the other, and the user scrolling past nothing to reach the far
        // half. Heights come from the last frame; on the very first frame
        // nothing has been measured and the count is the only guess there is.
        float total = 0.0f;
        bool measured = false;
        for (int idx : vis) {
            const auto it = g_cardAnim.find(CardKey(tabIndex, cards[idx].title));
            if (it != g_cardAnim.end() && it->second.curY >= 0.0f) {
                total += it->second.height;
                measured = true;
            }
        }
        if (!measured || total <= 1.0f)
            return std::max(1, (int)(vis.size() + 1) / 2);

        float run = 0.0f;
        int cut = (int)vis.size();
        for (size_t i = 0; i < vis.size(); ++i) {
            const auto it = g_cardAnim.find(CardKey(tabIndex, cards[vis[i]].title));
            const float h = (it != g_cardAnim.end()) ? it->second.height : 90.0f;
            // The card that takes the running total past halfway starts the
            // second column, so neither side can run far over.
            if (run + h * 0.5f > total * 0.5f) { cut = (int)i; break; }
            run += h;
        }
        return std::clamp(cut, 1, std::max(1, (int)vis.size() - 1));
    };

    // Decided once per arrangement, then held.
    //
    // This used to run every frame, and that is a feedback loop: the split
    // is computed from measured card heights, moving a card changes which
    // column its height counts toward, which changes the split, which moves
    // the card back. It oscillates, and because each pass also nudges the
    // animation targets the oscillation grows until cards are flung off the
    // panel entirely.
    //
    // It also meant anything that changed a height -- a warning appearing, a
    // toggle revealing a line of text, switching to advanced mode -- silently
    // re-laid-out the whole tab. Recomputing only when the set of visible
    // cards actually changes keeps the useful behaviour and removes all of
    // that: heights wobble constantly, but which cards exist does not.
    static std::map<int, std::pair<uint64_t, int>> autoCache;
    const bool autoSplit = (savedSplit < 0 || savedSplit > (int)vis.size());
    int effSplit;
    if (!autoSplit) {
        effSplit = savedSplit;
    } else {
        // A cheap signature of the arrangement: which cards are showing, and
        // in what order. Heights are deliberately not part of it.
        uint64_t sig = 1469598103934665603ull;
        for (int idx : vis) {
            sig ^= (uint64_t)(idx + 1);
            sig *= 1099511628211ull;
        }
        // Nothing is cached until at least one card has been measured, or
        // the very first frame -- where every height is still unknown --
        // would freeze a split derived from card count alone.
        // Every card measured, not merely one.
        //
        // This asked whether *any* card had been laid out, which is true on
        // the second frame -- when most heights are still the placeholder.
        // The split was then computed from mostly-fictional numbers and
        // cached, so a tab could settle into a visibly lopsided layout and
        // stay there. Waiting for the whole set costs a frame or two of the
        // count-based split, which is what was shown anyway.
        bool measuredAll = !vis.empty();
        for (int idx : vis) {
            const auto ai = g_cardAnim.find(CardKey(tabIndex, cards[idx].title));
            if (ai == g_cardAnim.end() || ai->second.curY < 0.0f) {
                measuredAll = false;
                break;
            }
        }
        const bool measuredAny = measuredAll;

        auto it = autoCache.find(tabIndex);
        if (!measuredAny) {
            effSplit = balancedSplit();
        } else if (it == autoCache.end() || it->second.first != sig) {
            const int fresh = balancedSplit();
            autoCache[tabIndex] = {sig, fresh};
            effSplit = fresh;
        } else {
            effSplit = it->second.second;
        }
    }

    int half = single ? (int)vis.size() : effSplit;

    // Fold state comes from the config on the first pass, then follows the UI.
    // Everything below keys card state by tab, so two tabs may share a title
    // without sharing a fold.
    ui::SetCardKeyPrefix((std::to_string(tabIndex) + "/").c_str());

    static bool foldApplied[Config::kOrderTabs] = {};
    if (!foldApplied[tabIndex]) {
        foldApplied[tabIndex] = true;
        for (int i = 0; i < n && i < 32; ++i)
            ui::SetCardOpen(cards[i].title,
                            (cfg.cardCollapsed[tabIndex] & (1u << i)) == 0);
    }

    const ImVec2 origin = ImGui::GetCursorPos();
    const float  originScreenY = ImGui::GetCursorScreenPos().y;
    const bool   dragging = !g_dragTitle.empty() && g_dragTab == tabIndex;

    // --- where each card wants to be -------------------------------------
    // While dragging, the held card is lifted out and a slot opened at the
    // position the pointer is over, so the rest visibly part around it.
    std::vector<int> layout = vis;
    int dragSlot = -1;
    int pendingSplit = effSplit;
    if (dragging) {
        const auto it = std::find_if(layout.begin(), layout.end(),
            [&](int i) { return cards[i].title == g_dragTitle; });
        if (it != layout.end()) layout.erase(it);

        // Which column the pointer is over, not which one the card started
        // in. Deriving it from the card meant a held card could never cross
        // between columns, however far sideways it was dragged.
        if (!single) {
            const float midX = ImGui::GetWindowPos().x - ImGui::GetScrollX() +
                               origin.x + colW + colGap * 0.5f;
            g_dragColumn = (ImGui::GetIO().MousePos.x > midX) ? 1 : 0;
        } else {
            g_dragColumn = 0;
        }

        // With the dragged card lifted out, the divide sits one earlier if
        // it came from the left.
        int liftedFrom = 0;
        for (size_t i = 0; i < vis.size(); ++i)
            if (cards[vis[i]].title == g_dragTitle) liftedFrom = (int)i;
        half = effSplit - ((liftedFrom < effSplit) ? 1 : 0);
        half = std::clamp(half, 0, (int)layout.size());

        // Insertion point within whichever column the pointer is over.
        // Walking that column's own cards and comparing against their
        // midpoints puts the card wherever it was dropped, rather than only
        // ever at one end.
        const int startIdx = std::clamp(g_dragColumn == 0 ? 0 : half,
                                        0, (int)layout.size());
        const int endIdx   = std::clamp(g_dragColumn == 0 ? half : (int)layout.size(),
                                        startIdx, (int)layout.size());

        int slot = endIdx - startIdx;      // past the last card by default
        float y = 0.0f;
        for (int i = startIdx; i < endIdx; ++i) {
            const auto& a2 = g_cardAnim[CardKey(tabIndex, cards[layout[i]].title)];
            if (g_dragMouseY < originScreenY + y + a2.height * 0.5f) {
                slot = i - startIdx;
                break;
            }
            y += a2.height + gap;
        }
        dragSlot = startIdx + slot;
        // Dropping into the left column grows it; into the right shrinks it.
        // That is what lets a column hold as many cards as the user wants.
        pendingSplit = half + ((g_dragColumn == 0) ? 1 : 0);

        int dragIdx = -1;
        for (int i = 0; i < n; ++i) if (cards[i].title == g_dragTitle) dragIdx = i;
        if (dragIdx >= 0) layout.insert(layout.begin() + dragSlot, dragIdx);
    }

    // --- animate toward those positions ----------------------------------
    const float dt = ImGui::GetIO().DeltaTime;

    float colY[2] = {0.0f, 0.0f};
    float maxY = 0.0f;

    for (size_t i = 0; i < layout.size(); ++i) {
        const int idx = layout[i];
        const CardDef& c = cards[idx];
        CardAnim& an = g_cardAnim[CardKey(tabIndex, c.title)];

        const int col = single ? 0 : ((int)i < half ? 0 : 1);
        const float targetY = colY[col];
        const float x = origin.x + (col == 1 ? colW + colGap : 0.0f);

        const bool isDragged = dragging && c.title == g_dragTitle;

        if (an.curY < 0.0f) {                      // first sight
            an.curY = targetY; an.curX = x;
            an.velX = an.velY = 0.0f;
        }
        if (an.curX < 0.0f) an.curX = x;

        if (isDragged) {
            // Follows the pointer directly on both axes, so it feels picked
            // up rather than animated after the fact.
            an.curY = g_dragMouseY - originScreenY - g_dragGrabDy;
            an.velX = an.velY = 0.0f;   // released from where it is, at rest
            an.curX = ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x +
                      ImGui::GetScrollX() - g_dragGrabDx;
            an.curX = std::clamp(an.curX, origin.x, origin.x + avail - colW);
        } else {
            // Vertical motion is the one seen most, so it is given the
            // gentler spring; sideways moves are rarer and want to look
            // deliberate rather than leisurely.
            Spring(an.curY, an.velY, targetY, dt, 13.0f);
            Spring(an.curX, an.velX, x, dt, 17.0f);

            // A backstop, not a substitute for the fix above.
            //
            // A spring is only stable while its target is stable, and no
            // amount of tuning saves one whose target is being recomputed
            // against its own output. That loop is closed now, but a card
            // leaving the panel is unrecoverable from the user's side --
            // there is nothing left on screen to drag back -- so it is worth
            // making structurally impossible rather than merely unlikely.
            const float slack = 600.0f;
            an.curX = std::clamp(an.curX, x - slack, x + slack);
            an.curY = std::clamp(an.curY, targetY - slack, targetY + slack);
            if (!std::isfinite(an.curX) || !std::isfinite(an.velX)) {
                an.curX = x; an.velX = 0.0f;
            }
            if (!std::isfinite(an.curY) || !std::isfinite(an.velY)) {
                an.curY = targetY; an.velY = 0.0f;
            }
        }
        an.column = col;

        ImGui::SetCursorPos(ImVec2(an.curX, origin.y + an.curY));
        ImGui::PushID(c.title);
        if (isDragged) {
            // Lift: a brighter border, so it reads as above the others
            // rather than merely misplaced.
            ImGui::PushStyleColor(ImGuiCol_Border,
                ImGui::ColorConvertU32ToFloat4(ui::col::accent));
        }
        // The column width has to reach the card itself. Left to fill the
        // available space, every card spans the whole panel and the second
        // column lands on top of the first.
        if (ui::BeginCard(c.title, true, colW)) c.body();
        const bool held    = ui::LastHeaderHeld();
        const float hdrTop = ui::LastHeaderTop();
        ui::EndCard();
        if (isDragged) ImGui::PopStyleColor();
        ImGui::PopID();

        // A height that flutters by a pixel is treated as unchanged.
        //
        // Layout below a card follows its height, so a value that alternates
        // between two nearby numbers moves everything under it every frame
        // and feeds the springs continuously. Text that reflows at a
        // particular width, or a value whose formatted length changes, is
        // enough to cause that. Ignoring sub-pixel churn keeps genuine growth
        // responsive while refusing to chase noise.
        const float h = std::max(24.0f, ui::LastCardHeight());
        if (std::fabs(h - an.height) > 1.5f) an.height = h;
        // The damped value drives the layout, not the raw one -- otherwise
        // the churn is filtered out of the split calculation and let straight
        // back into the positions, which is the half that actually moves.
        colY[col] += an.height + gap;
        maxY = std::max(maxY, colY[col]);

        // Picking a card up: the header is held and nothing else is.
        if (held && g_dragTitle.empty()) {
            g_dragTitle  = c.title;
            g_dragTab    = tabIndex;
            g_dragGrabDy = ImGui::GetIO().MousePos.y - hdrTop;
            g_dragGrabDx = ImGui::GetIO().MousePos.x -
                           (ImGui::GetWindowPos().x - ImGui::GetScrollX() + x);
            g_dragColumn = col;
        }

    }

    // The cards are positioned absolutely, so the parent needs telling how
    // much room they took or it will not scroll.
    // Absolutely positioned cards do not grow the parent by themselves, so
    // reserve the space they occupied. A one-pixel dummy is not enough:
    // ImGui wants an item that actually covers the extent.
    ImGui::SetCursorPos(origin);
    ImGui::Dummy(ImVec2(avail, maxY > 0.0f ? maxY : 1.0f));

    if (dragging) {
        g_dragMouseY = ImGui::GetIO().MousePos.y;

        // Dragging to the top of a scrolled view means they want it at the
        // top, so take them there rather than making them let go first.
        const float winTop = ImGui::GetWindowPos().y;
        const float winBot = winTop + ImGui::GetWindowSize().y;
        const float edge   = 48.0f;
        if (g_dragMouseY < winTop + edge)
            ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - 900.0f * dt));
        else if (g_dragMouseY > winBot - edge)
            ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(),
                                       ImGui::GetScrollY() + 900.0f * dt));

        // Released: commit whatever slot it was hovering.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            std::vector<int> newSeq;
            for (int idx : layout) newSeq.push_back(idx);
            for (int idx : seq)
                if (std::find(newSeq.begin(), newSeq.end(), idx) == newSeq.end())
                    newSeq.push_back(idx);   // hidden cards keep their place
            seq = newSeq;
            savedSplit = std::clamp(pendingSplit, 0, (int)vis.size());
            g_dragTitle.clear();
            g_dragTab = -1;
        }
    }

    unsigned int folded = 0;
    for (int i = 0; i < n && i < 32; ++i)
        if (!ui::GetCardOpen(cards[i].title, true)) folded |= (1u << i);
    cfg.cardCollapsed[tabIndex] = folded;

    count = (int)std::min<size_t>(seq.size(), Config::kOrderMax);
    for (int i = 0; i < count; ++i) order[i] = seq[i];
}

// Every reason the output would not move, gathered in one place.
//
// These conditions are spread across capture, the engine, the controller,
// the raw input filter and the serial sink, and finding out which one is
// biting means checking five tabs. Severity 2 means nothing can possibly
// happen; 1 means it will work but probably not as expected.
static int DiagnoseOutput(const Config& cfg, std::vector<std::string>& out) {
    int worst = 0;
    // Blockers are collected apart from advice, so the things that must be
    // done can be listed as steps rather than mixed into a paragraph of
    // caveats that are merely worth knowing.
    std::vector<std::string> blockers, notes;
    auto add = [&](int sev, std::string text) {
        worst = std::max(worst, sev);
        if (sev >= 2) blockers.push_back(std::move(text));
        else          notes.push_back(std::move(text));
    };

    // A card sitting well below its own boost clock, while the model waits
    // on it.
    //
    // Inference time is close to inversely proportional to core clock, so a
    // card idling at three quarters speed makes every frame a third slower
    // than the same model on the same hardware warmed up. Nothing the loop
    // reports distinguishes that from a slow engine -- every figure just
    // reads "slower" -- so it is worth ruling out before concluding the
    // model or the build is at fault.
    {
        const auto gclk = gpumon::Poll();
        static std::vector<double> gq;
        const auto gsum = g_shared.timings.stage[ST_GPU].summary(gq);
        if (gclk.valid && gclk.coreMaxMHz > 0 && gsum.n > 200) {
            const float frac = (float)gclk.coreMHz / (float)gclk.coreMaxMHz;
            if (frac < 0.80f)
                add(1, "The card is at " +
                       std::to_string((int)(frac * 100.0f)) +
                       "% of its boost clock, so inference is proportionally "
                       "slower than this hardware can manage. Set Power "
                       "management mode to Prefer maximum performance in the "
                       "NVIDIA control panel, or switch on Keep model "
                       "running.");
        }
    }

    // --- the pipeline has to be running at all --------------------------
    if (!g_capture.running())
        add(2, "Screen capture is not running. Check the log for why it "
               "failed to start.");
    if (!cfg.pipelineOn)
        add(2, "The pipeline is paused.");
    // No separate model switch to mention any more: it follows Enable, so
    // the one line below covers both.
    // A build in progress is reported whether or not something is already
    // loaded.
    //
    // This used to sit inside "no engine ready", so converting a second model
    // while one was running left the box saying everything was in place --
    // true of the loaded model, and not what someone watching a build wants
    // to be told. The build is worth mentioning either way; only the wording
    // differs.
    if (g_engine.ready() && g_builder.busy()) {
        const auto pr = g_builder.progress();
        char b[200];
        if (pr.etaSeconds > 0.0)
            snprintf(b, sizeof(b),
                     "Still building a model: %s, about %d s left. The one "
                     "already loaded keeps working until it finishes.",
                     pr.phase.c_str(), (int)(pr.etaSeconds + 0.5));
        else
            snprintf(b, sizeof(b),
                     "Still building a model: %s. The one already loaded "
                     "keeps working until it finishes.", pr.phase.c_str());
        add(1, b);
    }

    if (!g_engine.ready()) {
        if (g_builder.busy()) {
            const auto pr = g_builder.progress();
            char b[200];
            if (pr.etaSeconds > 0.0)
                snprintf(b, sizeof(b),
                         "A model is still being built: %s, about %d s left. "
                         "It will start working on its own when that "
                         "finishes.", pr.phase.c_str(), (int)(pr.etaSeconds + 0.5));
            else
                snprintf(b, sizeof(b),
                         "A model is still being built: %s. It will start "
                         "working on its own when that finishes.",
                         pr.phase.c_str());
            add(2, b);
        } else {
            add(2, "No model is loaded. Load or convert one on the Model tab.");
        }
    }
    if (!cfg.controlEnabled)
        add(2, "Enable is off, so the model is not running and nothing is "
               "sent.");

    // --- something has to ask for movement ------------------------------
    if (cfg.activationKey <= 0 && cfg.activationKey2 <= 0 &&
        cfg.activationMode != ActAlways)
        add(2, "No activation key is set.");
    if (cfg.activationMode == ActToggle)
        add(0, "Toggle mode: the key switches it on and off rather than "
               "holding it.");

    // --- the output path ------------------------------------------------
    const auto st = g_control.state();
    if (cfg.inputMethod == InputArduino) {
        if (cfg.serialPort <= 0)
            add(2, "Arduino output is selected but no device is chosen.");
        else if (!st.sinkReady)
            add(2, "COM" + std::to_string(cfg.serialPort) + " is not open. "
                   "Another program may have it, or the board is unplugged.");

        if (cfg.serialProtocol == ProtoBinary)
            add(1, "Protocol is set to binary. The prebuilt hex files speak "
                   "ASCII and ignore binary packets entirely, so nothing "
                   "moves even though the port is open.");

        // Clicking with the wrong mouse is invisible otherwise: the port is
        // open, the model is running, and nothing happens.
        // The specific failure this arrangement produces, named.
        // The "chosen device has not sent anything for a while" notice is
        // gone. The filter now falls back to accepting any device when the
        // chosen one stops answering, so the condition it warned about no
        // longer blocks anything -- and it was being reported twice.

    }

    // --- prediction that cannot lead ------------------------------------
    if (!cfg.predictionOn)
        add(0, "Prediction is off, so the aim never leads a moving target.");
    else if (cfg.predictionMs < 5.0f || cfg.predictionGain < 0.05f)
        add(1, "Prediction is on but the lead is effectively zero. Lead time "
               "wants to be near the real end-to-end latency, which the Debug "
               "tab reports.");
    else if (g_control.responseLearned() &&
             (g_control.responseScale() < 0.12f ||
              g_control.responseScale() > 4.0f))
        add(1, "The measured response scale is " +
               std::to_string(g_control.responseScale()).substr(0, 5) +
               " px per unit, which is outside the range any real setup "
               "produces. Every move is divided by it, so a figure this far "
               "out makes the aim either barely move or fly across the "
               "screen. Run Auto-tune, or switch the response off and set it "
               "by hand.");
    // The "response scale not measured yet" notice is deliberately absent.
    // It measures itself within seconds of the aim being used, so reporting
    // the gap told the user about a state that resolves on its own.

    // Capture is on, and it is not free.
    //
    // Writing frames means pulling the region back into system memory every
    // time one is kept, which lands in the loop time rather than in the gpu
    // figure -- so it reads as "the whole thing got slower" with nothing in
    // the per-stage numbers to point at. Keyed to the aim button by default,
    // which is exactly when the delay is felt.
    if (cfg.captureEnabled && g_capwriter.running()) {
        static std::vector<double> capq, infq;
        const auto lo = g_shared.timings.stage[ST_TOTAL].summary(capq);
        const auto in = g_shared.timings.stage[ST_INFER].summary(infq);
        if (lo.n > 300 && in.n > 300 && lo.p50 - in.p50 > 1.8) {
            char cb[220];
            snprintf(cb, sizeof(cb),
                     "Capture is running and is adding about %.1f ms to each "
                     "frame beyond the model. Switch it off on the Capture "
                     "tab when you are not collecting training images.",
                     lo.p50 - in.p50);
            add(1, cb);
        }
    }

    // Movement that leaves loopcore and never arrives.
    //
    // SendInput returns the number of events accepted and sets a last error
    // when it accepts none. The usual cause is UIPI: the window being aimed
    // at runs elevated and loopcore does not, so Windows discards every
    // event before it reaches the target -- silently, with the key
    // registering and the model running and nothing moving.
    //
    // This existed once and was lost to an edit in this block, the same way
    // the Action FOV notice below was. Both were caught by checking which
    // state fields nothing reads.
    if (cfg.inputMethod == InputMouse && cfg.controlEnabled) {
        const auto os2 = g_control.state();
        if (os2.outputBlocked)
            add(2, "Windows is refusing the injected movement. That happens "
                   "when the window being aimed at runs with administrator "
                   "rights and loopcore does not -- the events are discarded "
                   "before they reach it, with no error anywhere. Run "
                   "loopcore as administrator too, or use a board.");
        else if (os2.rejected > 50)
            add(1, "Windows has discarded " + std::to_string(os2.rejected) +
                   " movement events. Synthetic input is being rejected "
                   "somewhere; a board avoids the question entirely.");
    }

    // Targets found, every one of them outside the action region.
    //
    // Worth naming precisely: the boxes are drawn, the model is clearly
    // working, and nothing moves. Without this the only conclusion available
    // is that the aim is broken, when a setting is doing exactly what it says
    // and simply was not wanted.
    //
    // This existed once and was lost to a later edit in the same block --
    // which is how outsideActionFov ended up published with nothing reading
    // it, and how the whole-program check found it.
    if (cfg.actionFovOn && cfg.controlEnabled) {
        const auto fovState = g_control.state();
        if (!fovState.hasTarget && fovState.outsideActionFov > 0)
            add(1, "Targets are being detected but every one of them is "
                   "outside the Action FOV, so none can be aimed at. Turn it "
                   "off on the Field of view card, or make the region "
                   "larger.");
    }

    // --- is the model actually seeing anything --------------------------
    if (g_engine.ready()) {
        const auto dg = g_engine.diag();
        const uint64_t frames = g_shared.counters.frames.load();
        if (frames > 300 && dg.rawCandidates == 0 && g_engine.seenClasses() == 0)
            add(1, "The model has run but has never produced a detection. "
                   "It may not be the right model for what is on screen, or "
                   "the region may not be over anything it recognises.");
    }

    // --- inference has to be running when the key is held ---------------
    if (!cfg.keepGpuBoosted && !cfg.overlayOn && !cfg.drawRoiRect &&
        cfg.activationMode != ActAlways)

    if (!blockers.empty()) {
        out.push_back(blockers.size() == 1
                      ? "One thing is stopping this from working:"
                      : "These have to be sorted before anything will move:");
        for (const auto& b : blockers) out.push_back("-  " + b);
        if (!notes.empty()) out.push_back("");
    }
    for (const auto& n : notes) out.push_back(blockers.empty() ? n : "-  " + n);

    return worst;
}

static void TabGeneral(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Control", [&] {
        // The one switch.
        //
        // The model used to have its own, which was never a real choice:
        // running it with this off spends the card on a result nobody reads,
        // and this without it has nothing to act on. It follows this now.
        ui::Toggle("Enable", &cfg.controlEnabled,
            "Runs the model and sends movement. This is the only switch: "
            "there was never a useful state where one was on and the other "
            "was off.");

        if (cfg.controlEnabled && !g_engine.ready())
            ui::Notice(2, "No model is loaded, so there is nothing to detect "
                          "with. Load or convert one on the Model tab.");

        const char* modes[] = {"Hold key", "Key toggles", "Always on"};
        ui::ComboRow("Activation", &cfg.activationMode, modes, 3,
            "Hold: output only while the key is down. Toggles: the key "
            "switches it on and off. Always on: acts whenever a target is "
            "visible.");

        if (cfg.activationMode != ActAlways) {
            ui::KeybindRow("Key", &cfg.activationKey,
                "Click, then press any key or mouse button. Escape cancels.");

            if (cfg.activationKey2 > 0) {
                ui::KeybindRow("Second key", &cfg.activationKey2,
                    "Either key engages. They are equals, not a primary and a "
                    "fallback -- there is no case where one should work and "
                    "the other should not.");
                if (ui::GhostButton("Remove second key", ImVec2(170, 0)))
                    cfg.activationKey2 = 0;
            } else if (ui::GhostButton("Add a second key", ImVec2(170, 0))) {
                cfg.activationKey2 = VK_XBUTTON1;
            }

            if (cfg.activationKey2 > 0 &&
                cfg.activationKey2 == cfg.activationKey)
                ui::Notice(1, "Both keys are the same, which is the same as "
                              "having one.");

            // Whether the controller is actually seeing each key.
            //
            // This separates two failures that look identical from outside:
            // a key that never registers, and a key that registers while the
            // movement it produces is going nowhere. They need opposite
            // fixes, and nothing on screen used to tell them apart.
            {
                const auto ks = g_control.state();
                ImGui::PushFont(ui::fontSmall);
                if (cfg.activationKey2 > 0)
                    ImGui::TextDisabled("seen now:  key %s   second key %s",
                                        ks.key1Down ? "down" : "up",
                                        ks.key2Down ? "down" : "up");
                else
                    ImGui::TextDisabled("seen now:  key %s",
                                        ks.key1Down ? "down" : "up");
                ImGui::PopFont();
                ui::Hint("If a key reads down here and nothing moves, the "
                         "problem is the output rather than the bind.");
            }
        }

        // Right beneath the key, because "I press it and nothing happens" is
        // the question this answers. The conditions behind it live in five
        // different subsystems, and checking them by hand means five tabs.
        {
            // Re-evaluated a few times a second, not every frame.
            //
            // Several of these conditions flicker on and off between frames
            // as detections come and go, and a box that changes height with
            // them shoves every card below it up and down. The information
            // does not change fast enough to be worth that.
            // The empty case is throttled like every other.
            //
            // This re-ran whenever the list was empty, which is most of the
            // time -- so a condition that comes and goes was evaluated every
            // frame and the box appeared and vanished at the frame rate,
            // resizing the card with it. Nothing here needs to be noticed
            // faster than a few times a second.
            static std::vector<std::string> why;
            static int sev = 0;
            static int64_t lastCheck = 0;
            static bool firstRun = true;
            const int64_t nowNs = now_ns();
            if (firstRun || nowNs - lastCheck > 400'000'000LL) {
                firstRun = false;
                lastCheck = nowNs;
                why.clear();
                sev = DiagnoseOutput(cfg, why);
                if (why.empty()) {
                    sev = 0;
                    why.push_back("Ready. Everything needed to move the mouse "
                                  "is in place.");
                }
            }
            ui::AlertBox(sev, why);
        }

        ui::SliderFloat("Sensitivity", &cfg.sensitivity, 0.0f, 2.0f, "%.2f",
            "How fast the output closes the gap to the target, per second. "
            "Frame rate does not affect it.");

        ui::Toggle("Scale with target size", &cfg.boxScaleOn,
            "Reduces sensitivity as the target's box gets smaller.\n\n"
            "Box height stands in for distance: a target twice as far away "
            "is half as tall. A correction that feels right up close is then "
            "far too eager at range, because the same screen pixels cover "
            "much more of the world -- so the aim overshoots and hunts. This "
            "makes the feel consistent at any distance rather than tuned for "
            "one.\n\n"
            "It only ever reduces. Speeding up on a large close target would "
            "make the twitchiest case twitchier.");

        if (cfg.boxScaleOn) {
            ui::BeginGroupBox();
            ui::SliderFloat("Below box size", &cfg.boxScaleRefPx,
                20.0f, 600.0f, "%.0f px",
                "The target size that counts as close, and gets your full "
                "Sensitivity. Anything smaller than this is treated as "
                "further away and eased off.\n\n"
                "Measured across the whole box rather than its height alone, "
                "so a crouched or partly clipped target still reads at the "
                "right distance. The live figure below tells you what the "
                "target you are looking at measures.");

            ui::SliderFloat("Slow scale", &cfg.boxScaleStrength,
                0.0f, 1.0f, "%.2f",
                "How quickly sensitivity drops as targets get smaller.\n\n"
                "At 0.00 nothing changes with size at all. Around 0.33 it is "
                "roughly proportional -- half the size, half the sensitivity. "
                "At 1.00 a distant target crawls.");

            // The floor cannot exceed the sensitivity it is a floor for, so
            // the slider stops there rather than allowing a setting that
            // silently does nothing.
            ui::SliderFloat("Stay above", &cfg.boxScaleFloor,
                0.0f, std::max(0.01f, cfg.sensitivity), "%.2f",
                "The slowest the aim may get, in the same units as "
                "Sensitivity above.\n\n"
                "However small a target is, it will not drop below this -- so "
                "a distant one is still tracked rather than effectively "
                "abandoned. The track ends at your current Sensitivity, "
                "because a floor above it would never apply.");
            if (cfg.boxScaleFloor > cfg.sensitivity)
                cfg.boxScaleFloor = cfg.sensitivity;

            {
                const auto bs = g_control.state();
                ImGui::PushFont(ui::fontSmall);
                if (bs.hasTarget)
                    ImGui::TextDisabled("this target measures %.0f px  ->  "
                                        "sensitivity %.2f",
                                        bs.boxScaleSize,
                                        cfg.sensitivity * bs.boxScaleMult);
                else
                    ImGui::TextDisabled("no target to measure");
                ImGui::PopFont();
                ui::Hint("Watch this while aiming at something near and then "
                         "far; the two readings are what you are tuning.");
            }
            ui::EndGroupBox();
        }

        ui::SliderConfidence("Confidence", &cfg.confThresh,
            "Detections below this are ignored. Too low and noise gets "
            "treated as a target; too high and real ones are dropped. Both "
            "ends of the track are red for that reason.");

        ui::Toggle("Measure response", &cfg.autoResponse,
            "Works out how many screen pixels one unit of output moves the "
            "view, by watching how the world reacts. Every move is scaled by "
            "this, so it is not optional -- only whether it is measured or "
            "typed in.");
        if (cfg.autoResponse) {
            ImGui::PushFont(ui::fontSmall);
            if (g_control.responseLearned())
                ImGui::TextDisabled("measured: %.3f px per unit",
                                    g_control.responseScale());
            else
                ImGui::TextDisabled("measuring... hold the key and let it move");
            ImGui::PopFont();
            ui::Hint("This matters more than it looks. Once the aim is "
                     "tracking a moving target, the box barely moves on "
                     "screen because the output is cancelling its motion, so "
                     "almost the whole velocity estimate comes from adding "
                     "that output back. If this figure is wrong the target "
                     "reads as stationary and gets no lead.");
            ImGui::PushFont(ui::fontSmall);
            ImGui::PopFont();
        } else {
            ui::SliderFloat("Response", &cfg.responseScale, 0.02f, 8.0f, "%.3f",
                "Screen pixels per unit of output. Too small and every move "
                "is multiplied; too large and it never arrives.");
        }
        ui::Toggle("Lag compensation", &cfg.lagCompOn,
            "Subtracts movement already sent but not yet visible in a "
            "detection. Without it the same error is corrected twice.");

        ImGui::Dummy(ImVec2(0, 4));
        ui::SliderFloat("Aim offset X", &cfg.aimOffXPct, -50.0f, 50.0f, "%+.0f %%",
            "Where inside the box to aim, as a share of its own width. Using "
            "the box rather than the screen means the same setting lands in "
            "the same place on a near target and a distant one.");
        ui::SliderFloat("Aim offset Y", &cfg.aimOffYPct, -50.0f, 50.0f, "%+.0f %%",
            "Share of the box height, positive upward. Capped at 50% so the "
            "aim point cannot leave the box.");

        ui::SliderInt("Deadzone", &cfg.deadzonePx, 0, 40, 1, "%d px",
            "Stops correcting once the target is this close, so it settles "
            "instead of hunting.");

        // Deliberately quiet: most people already know, and a full warning
        // block here would cry wolf every time the slider is dragged past.
        if (cfg.confThresh < 0.20f || cfg.confThresh > 0.85f) {
            ImGui::PushFont(ui::fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(ui::col::warn));
            ImGui::TextWrapped(cfg.confThresh < 0.20f
                ? "Confidence is low; noise may be treated as a target."
                : "Confidence is high; real detections may be dropped.");
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        const char* rates[] = {"250 Hz", "500 Hz", "1000 Hz", "2000 Hz"};
        static const int rateVals[] = {250, 500, 1000, 2000};
        int ri = 1;
        for (int i = 0; i < 4; ++i) if (rateVals[i] == cfg.controlRateHz) ri = i;
        if (ui::ComboRow("Output rate", &ri, rates, 4,
                "How often movement is emitted, independent of the frame "
                "rate. Detections arrive a few hundred times a second at "
                "best; ticking faster and interpolating between them is what "
                "makes the motion smooth."))
        {
            cfg.controlRateHz = rateVals[ri];
        }

        // One rate drives both.
        //
        // The board's packet rate used to be a separate setting, which meant
        // the real output rate was whichever of the two was lower and the
        // other number was a decoy. There is no case where wanting a faster
        // control loop does not also mean wanting packets to keep up with
        // it, so the second setting has gone and this one carries both.
        cfg.serialRateHz = cfg.controlRateHz;

        if (cfg.inputMethod == InputArduino && cfg.controlRateHz > 500)
            ui::Notice(1, "A 32u4 doing pass-through as well struggles past "
                          "about 500 Hz, and flooding it makes the mouse feel "
                          "worse rather than better. If the board stutters, "
                          "drop a step.");

        if (cfg.controlEnabled && !g_engine.ready())
            ui::Notice(1, "No engine is loaded, so there is nothing to track.");
        }});
    cards.push_back({"Field of view", [&] {
        const char* anchors[] = {"Screen centre", "Follow mouse"};
        ui::ComboRow("Anchor", &cfg.roiMode, anchors, 2,
            "Where the capture region sits and what the output steers "
            "toward. Both use the same origin, so they cannot disagree.");
        ui::SliderInt("FOV", &cfg.fov, 128, 1280, 16, "%d px",
            "The detection region. Cost scales with area, so halving this is "
            "roughly a quarter of the work.");
        ui::Hint(cfg.roiMode == ROI_CENTER
                 ? "Fixed at the centre of the screen."
                 : "Following the cursor.");

        {
            // Live cursor position in follow mode, so the preview shows where
            // the region really is instead of implying it is always central.
            int fx = -1, fy = -1;
            if (cfg.roiMode == ROI_MOUSE) {
                POINT pt{};
                // Shown relative to the captured monitor, since that is what
                // the preview draws. On a second display the raw desktop
                // coordinate would put the marker off the edge.
                if (GetCursorPos(&pt)) {
                    fx = pt.x - g_capture.screenLeft();
                    fy = pt.y - g_capture.screenTop();
                }
            }
            ui::FovPreview(g_capture.screenWidth(), g_capture.screenHeight(),
                           cfg.fov, cfg.actionFovOn, cfg.actionFov, fx, fy);

        }

        ImGui::Dummy(ImVec2(0, 6));
        ui::Toggle("Action FOV", &cfg.actionFovOn,
            "Splits detection from action. The model keeps watching the whole "
            "field of view, but movement only starts once a box reaches this "
            "smaller region.");
        if (cfg.actionFovOn) {
            const int amax = std::max(64, cfg.fov);
            ui::SliderInt("Action size", &cfg.actionFov, 32, amax, 8, "%d px",
                "Touching counts, not just containing: a target entering the "
                "edge is already acted on.");
            if (cfg.actionFov >= cfg.fov)
                ui::Notice(1, "The action region is as large as the detection "
                              "region, so it has no effect.");
        }
        }});
    cards.push_back({"Targeting", [&] {
        // The dropdowns go inside a child of a fixed width, and the preview
        // sits beside it.
        //
        // PushItemWidth does not help here: these row widgets lay themselves
        // out from GetContentRegionAvail rather than from the item width, so
        // they kept taking the whole card and pushing the preview off the
        // right edge. A child region is the only thing that actually changes
        // what they measure.
        const float previewW = 86.0f;
        const float gapW     = 10.0f;
        const float availW   = ImGui::GetContentRegionAvail().x;
        const float leftW    = std::max(150.0f, availW - previewW - gapW);
        const bool  roomForPreview = availW > previewW + gapW + 150.0f;

        // Zero padding: the child inherits the card's, which would indent
        // these two rows away from everything else in it.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##targetleft", ImVec2(roomForPreview ? leftW : availW, 0),
                          ImGuiChildFlags_AutoResizeY);
        ImGui::PopStyleVar();
        const char* prios[] = {"Middle", "Largest box",
                               "Smallest box", "Highest confidence",
                               "Under the origin", "Closest, smallest overlap",
                               "Sticky", "Where you are heading"};
        ui::Toggle("Stay on one target", &cfg.stickyTarget,
            "Keeps whatever is already being tracked, whichever priority is "
            "chosen above.\n\n"
            "Sticky used to be a priority of its own, which meant picking it "
            "gave up every other way of choosing. It is really a modifier: "
            "hold what you have, and use the rule above only when deciding "
            "what to hold. A held target simply looks nearer than it is, so "
            "the rule still decides -- it just needs a margin to overcome "
            "before it swaps.");
        if (cfg.stickyTarget && cfg.advancedMode)
            ui::SliderFloat("How firmly", &cfg.stickyBiasPx, 10.0f, 400.0f,
                "%.0f px",
                "How much nearer a held target appears. Larger holds through "
                "more distraction; too large and it will not let go of "
                "something that has stopped mattering.");

        ui::ComboRow("Priority", &cfg.targetPriority, prios, 8,
            "Which detection to steer toward when several are visible. "
            "Largest usually means nearest, smallest usually furthest. "
            "Closest, smallest overlap picks the nearest target but prefers "
            "the smaller box wherever two overlap, since an enclosing box is "
            "rarely the thing worth aiming at. Sticky holds whatever it "
            "started on. Where you are heading prefers the target your own "
            "hand is already moving toward, which is a statement of intent "
            "no geometric rule has access to -- it falls back to nearest "
            "while the hand is still.");

        if (cfg.targetPriority == PrioSticky)
            ui::Hint("Holds one box until it disappears, the key is "
                     "released, or its confidence collapses. While held, the "
                     "threshold for that box alone drops to %.2f, so a dip "
                     "for a frame or two does not lose it.",
                     std::max(0.05f, cfg.confThresh * 0.6f));

        const char* anchors2[] = {"Centre", "Top edge", "Bottom edge",
                                  "Steadiest edge"};
        ui::ComboRow("Anchor", &cfg.boxAnchor, anchors2, 4,
            "Which part of the box the aim is measured from. Box edges "
            "rarely wobble equally, and measuring from the centre inherits "
            "half of whichever edge is noisy as movement on a target that "
            "has not moved. Steadiest edge picks whichever is currently "
            "quieter.");

        ImGui::EndChild();

        if (roomForPreview) {
            ImGui::SameLine(0.0f, gapW);
            ui::AnchorPreview(cfg.boxAnchor, cfg.aimOffXPct, cfg.aimOffYPct, 70.0f);
        } else {
            // Too narrow to sit beside them, so it goes underneath rather
            // than being squeezed into a sliver.
            ui::AnchorPreview(cfg.boxAnchor, cfg.aimOffXPct, cfg.aimOffYPct, 70.0f);
        }

        // The engine reports class ids and nothing else, so the only honest
        // list is what has actually come out of it. Anything never seen
        // cannot be offered.
        const uint32_t seen = g_engine.ready() ? g_engine.seenClasses() : 0u;
        const int seenCount = (int)std::bitset<32>(seen).count();

        if (seenCount <= 1) {
            ui::Hint(seenCount == 1
                     ? "One class detected so far, so there is nothing to "
                       "prioritise between."
                     : "No classes seen yet. Once detections come in, any "
                       "extra classes appear here.");
        } else {
            ImGui::Dummy(ImVec2(0, 4));
            ui::Toggle("Class priority", &cfg.classFilterOn,
                "Choose which classes may be targeted, and in what order. A "
                "class higher in the list always wins, whatever the geometry "
                "says.");

            if (cfg.classFilterOn) {
                // Fold in anything newly seen, keeping the existing order.
                for (int c = 0; c < 32; ++c) {
                    if (!(seen & (1u << c))) continue;
                    bool present = false;
                    for (int i = 0; i < cfg.classOrderCount; ++i)
                        if (cfg.classOrder[i] == c) { present = true; break; }
                    if (!present && cfg.classOrderCount < 32)
                        cfg.classOrder[cfg.classOrderCount++] = c;
                }

                int moveUp = -1, moveDown = -1;
                for (int i = 0; i < cfg.classOrderCount; ++i) {
                    const int c = cfg.classOrder[i];
                    if (!(seen & (1u << c))) continue;
                    ImGui::PushID(i);

                    bool on = (cfg.classMask & (1u << c)) != 0;
                    ImGui::AlignTextToFramePadding();
                    if (ImGui::Checkbox("##on", &on)) {
                        if (on) cfg.classMask |= (1u << c);
                        else    cfg.classMask &= ~(1u << c);
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(on ? ui::col::text
                                                          : ui::col::textFaint));
                    ImGui::Text("%d. %s", i + 1, ClassLabel(c).c_str());
                    ImGui::PopStyleColor();

                    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 56);
                    if (ui::GhostButton("^", ImVec2(24, 0)) && i > 0) moveUp = i;
                    ImGui::SameLine();
                    if (ui::GhostButton("v", ImVec2(24, 0)) &&
                        i < cfg.classOrderCount - 1) moveDown = i;
                    ImGui::PopID();
                }
                if (moveUp > 0)
                    std::swap(cfg.classOrder[moveUp], cfg.classOrder[moveUp - 1]);
                if (moveDown >= 0 && moveDown < cfg.classOrderCount - 1)
                    std::swap(cfg.classOrder[moveDown], cfg.classOrder[moveDown + 1]);

                if (cfg.classMask == 0)
                    ui::Notice(1, "Every class is switched off, so nothing "
                                  "will ever be targeted.");
                ui::Hint("Names come from a .names or .txt file beside the "
                         "model. Without one they are numbered.");
            }
        }
        }});
    cards.push_back({"Prediction", [&] {
        ui::Toggle("Enable", &cfg.predictionOn,
            "Aims where the target will be rather than where it was.");
        const char* methods[] = {"Linear", "Smoothed", "Acceleration",
                                 "Alpha-beta", "Edge consensus", "Kalman"};
        ui::ComboRow("Method", &cfg.predictionMethod, methods, 6,
            "Kalman is the one to use for leading a moving target: it "
            "carries a covariance, so it weighs each measurement against how "
            "certain it currently is instead of blending by a fixed amount. "
            "Edge consensus suits boxes that resize on a stationary target. "
            "Linear is the raw frame difference, Smoothed a filtered version "
            "of it, Acceleration second order, Alpha-beta the steadiest.");

        if (cfg.predictionMethod == PredKalman) {
            if (cfg.advancedMode) {
        ui::SliderFloat("Manoeuvre", &cfg.kalmanProcess, 50.0f, 6000.0f,
                "%.0f",
                "How much the filter expects the target to change speed "
                "between frames, in pixels per second squared.\n\n"
                "Raise it and the filter trusts each new detection more, so "
                "the lead swings quickly onto a target that cuts sideways -- "
                "and also onto detector noise. Lower it and the filter "
                "believes its own steady-motion model, so the lead is smooth "
                "and confident but arrives late after a real direction "
                "change.");
            ui::SliderFloat("Detector noise", &cfg.kalmanMeasure, 1.0f, 80.0f,
                "%.0f",
                "How far the box centre is expected to wobble frame to "
                "frame, in pixels, when nothing is actually moving.\n\n"
                "This is the counterweight to Manoeuvre. Raise it when boxes "
                "are jittery and the filter will ignore small movements "
                "rather than leading on them. Set it too high and genuine "
                "motion is dismissed as noise, so the lead falls behind.");
            ui::Hint("Only the ratio matters. Manoeuvre over Detector noise "
                     "high means believe the measurement, low means believe "
                     "the model.");
        }

        if (cfg.predictionMethod == PredEdgeConsensus) {
            ui::SliderFloat("Jitter reject", &cfg.jitterReject, 0.0f, 15.0f,
                "%.1f %%",
                "Movement smaller than this share of the box size is treated "
                "as detector noise rather than travel. Raise it if a "
                "stationary target still shows velocity; lower it if slow "
                "movement is being ignored.");

            const float jr = g_control.jitterRatio();
            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("box wobble rejected: %.0f%% of raw movement",
                                jr * 100.0f);
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0, 4));
        ui::Toggle("Follow the hand", &cfg.userAssistOn,
            "Scales the pull by whether your own movement agrees with it.\n\n"
            "Moving toward the target while being pulled toward it "
            "overshoots; moving away while being pulled back is the aim "
            "fighting your hand. Reading both and scaling accordingly makes "
            "it behave like assistance rather than a second hand on the "
            "mouse. Your own movement is read from the mouse directly, so it "
            "is never confused with the output this program sends.");
        if (cfg.userAssistOn) {
            ui::BeginGroupBox();
            ui::SliderFloat("When agreeing", &cfg.assistWithPct, 50.0f, 300.0f,
                "%.0f %%",
                "Gain while your hand moves the same way as the correction. "
                "Above 100 helps it get there; too high and you overshoot "
                "together.");
            ui::SliderFloat("When opposing", &cfg.assistAgainstPct, 0.0f, 100.0f,
                "%.0f %%",
                "Gain while your hand moves against the correction. Low "
                "values let you pull away without a fight; zero hands "
                "control back entirely for as long as you are moving.");
            ui::SliderFloat("Full effect at", &cfg.assistSpeedRef, 10.0f,
                2000.0f, "%.0f px/s",
                "Hand speed at which the adjustment is applied in full. "
                "Below it the effect fades in, so a resting hand leaves the "
                "gain exactly where the sensitivity slider put it.");
            ui::EndGroupBox();
        }

        {
            const auto stp = g_control.state();
            const float smoothShown = cfg.leadSmoothScales
                ? std::max(cfg.leadSmoothMs, cfg.predictionMs * 0.6f)
                : cfg.leadSmoothMs;
            ui::LeadPreview(cfg.predictionMs * 0.001f * cfg.predictionGain,
                            cfg.leadTrustOn ? stp.leadTrust : 1.0f,
                            smoothShown, cfg.predictionOn);
        }

        ui::Toggle("Sideways only", &cfg.leadHorizontalOnly,
            "Discards the vertical part of the lead entirely.\n\n"
            "Vertical box edges move with the detector far more than with the "
            "target: a box breathing a few pixels between frames reads as "
            "vertical velocity, and a lead multiplies that into a visible "
            "offset above or below the target. Most targets worth leading "
            "move mainly sideways, so this removes a whole class of "
            "misbehaviour and costs very little. Turn it off if you are "
            "tracking something that genuinely climbs or falls.");

        ui::SliderFloat("Ignore minor axis", &cfg.minorAxisCut, 0.0f, 100.0f,
            "%.0f %%",
            "How much to distrust a lead on whichever axis is moving "
            "less.\n\n"
            "Real movement is nearly always dominated by one direction, so a "
            "target crossing sideways has no meaningful vertical velocity and "
            "any vertical lead is measuring the box wobbling. A lead is a "
            "position offset, so that lands directly on the aim as bobbing. "
            "Scaled by the ratio between the axes, so genuine diagonal "
            "movement is left alone.");

        ui::SliderFloat("Lead steadiness", &cfg.leadSmoothMs, 0.0f, 400.0f,
            "%.0f ms",
            "How much to steady the velocity before it becomes a lead.\n\n"
            "The lead is an offset proportional to velocity, so noise in the "
            "velocity lands directly on where the aim points -- and an aim "
            "chasing its own jitter is what orbiting is. Higher is steadier "
            "and a fraction slower to react to a real change of direction. "
            "Lower follows sharp turns and shakes on noisy detections.");

        ui::SliderFloat("Self-motion floor", &cfg.selfMotionFloor, 0.0f, 40.0f,
            "%.0f %%",
            "How much of the aim's own movement to treat as uncertainty when "
            "working out what the target did.\n\n"
            "Recovering real motion means adding the aim's own movement back, "
            "and what survives that is dominated by the error in the response "
            "scale rather than by the target. Anything smaller than this "
            "share of the correction is not counted as motion.\n\n"
            "Too low and a stationary target appears to drift in whatever "
            "direction the aim approached from, and the lead chases it. Too "
            "high and genuinely slow movement is ignored. It is widened "
            "automatically while the response scale is still being measured.");

        // Three quarters of the travel covers nought to two hundred, the
        // rest reaches a second. The useful range is bunched at the low end
        // but the far end is occasionally needed, and a linear track either
        // wastes most of its length or makes the common range unsettable.
        }   // end of the advanced-only block above

        // Always shown: this is the one setting that has to be right, and
        // everything above it exists to make this one behave.
        ui::SliderPiecewise("Lead time", &cfg.predictionMs, 0.0f, 200.0f,
                            1000.0f, 0.75f, "%.0f ms",
            "How far into the future to aim.\n\n"
            "A detection describes where the target was when the frame was "
            "captured, and by the time an input lands it has moved on. Set "
            "this near your real end-to-end latency: the sum of frame "
            "cadence and inference on the Debug tab is a good starting "
            "figure. Too little and the aim trails; too much and it sits "
            "ahead of the target.\n\n"
            "The upper quarter of this slider reaches a second. That far "
            "ahead the lead multiplies any wobble in the velocity by the same "
            "amount, which is what used to force the intensity right down to "
            "stay usable. Steadiness and Trust below handle that directly, "
            "so the intensity can stay where it belongs.");

        if (cfg.advancedMode) {
        ui::Toggle("Trust steady velocity only", &cfg.leadTrustOn,
            "Applies the lead in proportion to how consistent the velocity "
            "has been.\n\n"
            "A lead multiplies velocity by a horizon, and so multiplies the "
            "error in it. Comparing the spread of the velocity against its "
            "mean separates a target genuinely travelling -- mean far larger "
            "than spread, full lead kept -- from one whose velocity is mostly "
            "noise, whose lead is cut in proportion. Turning the intensity "
            "down instead weakens the lead when the estimate is good as well "
            "as when it is bad.");

        ui::Toggle("Steady in step with the lead", &cfg.leadSmoothScales,
            "Widens the steadying window as the lead time grows. Predicting a "
            "second ahead while reacting to every tenth of a second of "
            "velocity is incoherent: a target that changes direction that "
            "fast makes the prediction wrong whatever it is smoothed by.");
        ui::SliderFloat("Intensity", &cfg.predictionGain, 0.0f, 3.0f, "%.2f",
            "Multiplies the lead after it is worked out.\n\n"
            "1.0 uses the estimate as measured. Below that hedges against a "
            "velocity reading you do not fully trust; above it overdrives, "
            "which can compensate for latency the lead time does not "
            "account for but makes overshoot worse on a target that stops "
            "suddenly.");
        }

        if (!cfg.advancedMode)
            ui::Hint("Lead time is the setting that matters here. How the "
                     "velocity is steadied, which axis is trusted and how "
                     "much of your own movement is discounted are all in "
                     "Advanced mode, on the Settings tab.");
        }});
    cards.push_back({"Auto-tune", [&] {
        ui::Hint("Finds a sensitivity and a deadzone by experiment. Put a "
                 "stationary object the model detects confidently in the "
                 "middle of the region, press start, then click into the "
                 "game during the five second countdown and leave it focused "
                 "and the mouse alone. Most games ignore mouse input while "
                 "another window has focus, and a measurement taken that way "
                 "is worthless.");

        const bool tuning = g_tuner.busy();
        if (!tuning) {
            if (ui::PrimaryButton("Find best sensitivity", ImVec2(200, 0))) {
                if (!g_engine.ready()) {
                    ui::Toast("Load a model first.");
                } else {
                    // Switching these on is what the user meant by pressing
                    // the button, so do it rather than refusing.
                    // Control alone is enough now; the model follows it.
                    cfg.controlEnabled = true;
                    g_tuner.start(&g_control, cfg, g_shared.log);
                }
            }
        } else {
            if (ui::GhostButton("Cancel", ImVec2(110, 0))) g_tuner.cancel();
        }

        const auto ts = g_tuner.status();
        if (tuning) {
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextUnformatted(ts.phase.c_str());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                ImGui::ColorConvertU32ToFloat4(ui::col::accent));
            ImGui::ProgressBar(ts.trialsTotal ? (float)ts.trialsDone / ts.trialsTotal : 0.0f,
                               ImVec2(-1, 6), "");
            ImGui::PopStyleColor();
            if (!ts.note.empty()) ui::Hint("%s", ts.note.c_str());
            ui::Notice(1, "Click into the game now and leave it focused. "
                          "Output sent while loopcore is the focused window "
                          "does not reach the game, and every measurement "
                          "reads as zero. The view will jerk around once it "
                          "starts -- that is the measurement, not a fault.");
        } else if (ts.bestGain > 0.0f) {
            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("best %.2f  -  settles %.0f ms, overshoot %.1f px",
                                ts.bestGain, ts.bestSettleMs, ts.bestOvershoot);
            ImGui::PopFont();

            const auto trials = g_tuner.results();
            if (!trials.empty()) {
                std::vector<float> scores;
                for (const auto& t : trials) scores.push_back(t.score);
                ImGui::PlotLines("##tune", scores.data(), (int)scores.size(), 0,
                                 nullptr, 0.0f, FLT_MAX, ImVec2(-1, 40));
                ui::Hint("Score across the gains tried, lower is better. It "
                         "weighs overshoot heavily: arriving sooner is worth "
                         "little if the aim swings past on the way.");
            }
        }
        }});

    cards.push_back({"Action", [&] {
        ui::Toggle("Fire an action", &cfg.actionOn,
            "Presses a mouse button when something happens to the target.\n\n"
            "The trigger is a geometric fact about where the aim is relative "
            "to the target, and the output is a mouse button. Nothing wider: "
            "a general macro engine is a different program.");

        if (!cfg.actionOn) {
            ui::Hint("Off, nothing is ever pressed.");
        } else {
            const char* trigs[] = {"Within a distance", "Inside the box",
                                   "Inside part of the box",
                                   "Target appears", "Target lost",
                                   "Arrived and settled"};
            ui::ComboRow("When", &cfg.actionTrigger, trigs, 6,
                "Within a distance measures from the aim point inside the "
                "box, so the offsets set under Targeting are already "
                "accounted for.\n\n"
                "Inside the box is a different question: a large box can be "
                "entered while the aim is still far from its centre.\n\n"
                "Inside part of the box scales that region about the aim "
                "point rather than the box centre, so shrinking it tightens "
                "toward where you actually want to hit. Arrived and settled "
                "additionally waits for the aim to stop closing, so it does "
                "not fire while sweeping past.");

            if (cfg.actionTrigger == TrigWithinPixels ||
                cfg.actionTrigger == TrigSettled)
                ui::SliderInt("Distance", &cfg.actionRadiusPx, 1, 120, 1,
                    "%d px",
                    "How close the aim has to be to the aim point, measured "
                    "from the point your X and Y offsets put inside the box "
                    "rather than from its centre.");

            if (cfg.actionTrigger == TrigInsideBoxScaled)
                ui::SliderFloat("How much of it", &cfg.actionBoxPct,
                    5.0f, 200.0f, "%.0f %%",
                    "The share of the box that counts, scaled about the aim "
                    "point. A hundred is the box as detected; below that "
                    "tightens toward the aim point, above extends past the "
                    "edges.");

            const char* btns[] = {"Left", "Right", "Middle"};
            ui::ComboRow("Press", &cfg.actionButton, btns, 3,
                "Which mouse button.");
            ui::Hint(cfg.inputMethod == InputArduino
                     ? "Sent to the board on COM%d, the same path movement "
                       "takes. A synthetic press would not reach a game that "
                       "ignores synthetic movement."
                     : "Sent as synthetic input, the same path movement "
                       "takes.", cfg.serialPort);
            if (cfg.inputMethod == InputArduino)
                ui::Notice(1, "Button support needs the firmware from this "
                              "version. An older sketch ignores the command "
                              "entirely, so nothing will happen -- reflash "
                              "from the Settings tab.");

            const char* kinds[] = {"Click", "Double click",
                                   "Hold while true", "Toggle on entry"};
            ui::ComboRow("How", &cfg.actionKind, kinds, 4,
                "Click and Double click are one-off events, repeated no more "
                "often than the cooldown allows. Hold keeps the button down "
                "for as long as the condition lasts. Toggle presses once each "
                "time the condition becomes true and never repeats while it "
                "stays true.");

            ui::Toggle("Only while a key is held", &cfg.actionNeedsKey,
                "Requires the activation key, so nothing is pressed while you "
                "are not asking for anything. Worth keeping on: an action "
                "that fires unprompted is surprising in a way nothing else "
                "here is.");

            if (cfg.actionNeedsKey) {
                if (cfg.actionKey > 0) {
                    ui::KeybindRow("Action key", &cfg.actionKey,
                        "Click, then press any key or mouse button. This is "
                        "separate from the activation key, so the aim and the "
                        "action can be asked for independently.");
                    if (ui::GhostButton("Use the aim key", ImVec2(150, 0)))
                        cfg.actionKey = 0;
                } else {
                    ui::Hint("Using the activation key, so the action fires "
                             "while you are aiming.");
                    if (ui::GhostButton("Set a separate key", ImVec2(160, 0)))
                        cfg.actionKey = cfg.activationKey > 0
                                      ? cfg.activationKey : VK_XBUTTON2;
                }
            }

            ui::SliderFloat("Ignore below", &cfg.actionMinConf, 0.0f, 1.0f,
                "%.2f",
                "Confidence the target must reach before this can fire. A "
                "marginal detection is a poor reason to press a button.");

            ui::SliderInt("Must hold for", &cfg.actionHoldMs, 0, 500, 10,
                "%d ms",
                "How long the condition has to persist first. Zero fires on "
                "the first frame that qualifies, which a target crossing the "
                "threshold for one frame can satisfy by accident.");

            ui::SliderInt("Cooldown", &cfg.actionCooldownMs, 0, 2000, 10,
                "%d ms",
                "The shortest gap between firings.");

            if (cfg.advancedMode)
                ui::SliderInt("Press length", &cfg.actionGapMs, 5, 200, 5,
                    "%d ms",
                    "How long the button stays down, and the spacing between "
                    "the two presses of a double click.\n\n"
                    "Pressing and releasing within one tick can produce an "
                    "event a game never sees: many read input once a frame, "
                    "and a press already released by then never happened.");

            {
                const auto ast = g_control.state();
                ImGui::PushFont(ui::fontSmall);
                if (ast.hasTarget)
                    ImGui::TextDisabled("%.0f px from firing", ast.actionDistPx);
                ImGui::TextDisabled("%s   fired %llu times",
                                    ast.actionArmed ? "armed" : "not armed",
                                    (unsigned long long)ast.actionFires);
                ImGui::PopFont();
            }

            if (cfg.actionKind == ActionHold)
                ui::Notice(1, "Hold keeps the button down while the condition "
                              "lasts. It is released when the condition ends, "
                              "when control is switched off, and on exit -- "
                              "but a button held down is the one thing here "
                              "you cannot undo from inside the program, so it "
                              "is worth knowing that is what this does.");
        }
        }});

    cards.push_back({"Safety limits", [&] {
        ui::Toggle(cfg.safetyOn ? "Enabled" : "Disabled", &cfg.safetyOn,
            "Turns every limit below off at once. They exist for a reason; "
            "off, one bad frame can move the view several hundred pixels.");
        // Dimmed rather than disabled: the values stay adjustable so they can
        // be set up before the limits are switched on.
        // Read once, after the toggle. Testing cfg directly means a click
        // changes the answer between the push and the pop, which unbalances
        // the stack -- exactly what ImGui was reporting.
        const bool dimmed = !cfg.safetyOn;
        if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        ui::Hint("Bounds on what one bad frame can do. Every one of these "
                 "exists because an unbounded value upstream can turn a "
                 "single misread into a movement of several hundred pixels.");
        ui::SliderFloat("Max step", &cfg.maxStepPx, 2.0f, 200.0f, "%.0f px",
            "The most a single output tick may move, whatever the maths "
            "produced. The last line of defence.");
        ui::SliderFloat("Aim speed limit", &cfg.maxAimSpeedPx,
            500.0f, 60000.0f, "%.0f px/s",
            "How fast the aim itself may travel.\n\n"
            "This is a backstop against a broken response estimate throwing "
            "the view across the screen, not a tuning control -- that failure "
            "is far beyond any speed real aiming needs. Set it low and it "
            "will cap ordinary corrections, and raising Sensitivity will then "
            "appear to do nothing because every tick is already at the "
            "ceiling.");

        ui::SliderFloat("Max speed", &cfg.maxSpeedPx, 200.0f, 10000.0f, "%.0f px/s",
            "Ceiling on the velocity estimate. Two detections arriving very "
            "close together can otherwise imply a speed in the tens of "
            "thousands, which prediction then multiplies by the lead time.");
        ui::SliderFloat("Jump reject", &cfg.jumpRejectPx, 20.0f, 1200.0f, "%.0f px",
            "A target that appears to have moved further than this between "
            "frames is treated as a different object, not as something "
            "moving fast. Its position is taken; no velocity is inferred.");
        ui::SliderInt("Drop after", &cfg.staleMs, 40, 1000, 10, "%d ms",
            "A detection older than this stops being a target. Carrying an "
            "old one forward is how a brief dropout becomes a flick.");
        if (dimmed) ImGui::PopStyleVar();
        }});
    cards.push_back({"Movement", [&] {
        // How the aim travels, kept apart from how fast it travels.
        //
        // Sensitivity and the safety limits answer "how quickly"; this card
        // is for "along what path", which is a separate question and was
        // previously spread between Smoothing and nothing at all.
        // Fifteen paths in one dropdown, with the explanation below it.
        //
        // A row of toggles or a grid would put every option on screen at
        // once, which sounds helpful and is not: they are mutually exclusive
        // and most people want one. A list plus a description of the current
        // choice is the shape that scales -- adding a sixteenth costs one
        // line, not a redesign.
        {
            static const char* names[PathCount];
            for (int i = 0; i < PathCount; ++i) names[i] = MovePathName(i);
            ui::ComboRow("Path", &cfg.movePath, names, (int)PathCount,
                "How the aim travels to the target, as distinct from how "
                "fast. Every one of these is bound by the same safety limits "
                "-- a path chooses a route, it cannot move further or faster "
                "than the limits allow.");

            ImGui::PushFont(ui::fontSmall);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", MovePathBlurb(cfg.movePath));
            ImGui::PopTextWrapPos();
            ImGui::PopFont();

            if (cfg.movePath != PathDefault) {
                ui::Toggle("Stronger at distance", &cfg.movePathRamp,
                    "Lets the path assert itself in proportion to how far "
                    "there is to go.\n\n"
                    "A long sweep has room for character; a two-pixel "
                    "correction does not, and a path that bows or throws on "
                    "one is just imprecision. Measured in target widths, so "
                    "\"far\" means the same whether the target is close and "
                    "large or distant and small.\n\n"
                    "It only scales how much character the path adds, not "
                    "how fast the aim travels.");

                ui::SliderFloat("Amount", &cfg.movePathAmount, 0.0f, 1.0f,
                    "%.2f",
                    "How strongly the path departs from a straight, even "
                    "approach. Zero is close to Default whatever is selected "
                    "above, which makes it a way to dial a path back rather "
                    "than turn it off.");
            }
        }

        ImGui::Dummy(ImVec2(0, 6));
        ui::SectionHeader("SMOOTHING");

        ui::Toggle("EMA smoothing", &cfg.emaOn,
            "Removes detection jitter at the cost of lag, which prediction "
            "exists to cancel.");

        // The two axis sliders only exist to shape the smoothing, so they
        // are hidden when there is none. A disabled control that still takes
        // a row is worse than one that steps aside.
        if (cfg.emaOn) {
            ui::SliderFloat("Horizontal", &cfg.emaIntensity, 0.0f, 0.98f,
                "%.2f",
                "Smoothing across the screen. Sideways movement is usually "
                "the target genuinely moving, so this normally wants to stay "
                "low: filtering it costs lead.");
            ui::SliderFloat("Vertical", &cfg.emaIntensityY, 0.0f, 0.98f,
                "%.2f",
                "Smoothing up and down. Vertical box edges move mostly with "
                "the detector rather than with the target, so this can "
                "usually be much higher than the horizontal figure without "
                "costing anything.");
            ui::Hint("A time constant, so both behave the same at any output "
                     "rate.");
        }
        }});

    cards.push_back({"Live", [&] {
        const auto st = g_control.state();
        ImGui::PushFont(ui::fontSmall);
        ImGui::TextDisabled("output      %s%s", st.sinkName.c_str(),
                            st.sinkReady ? "" : "  (not ready)");
        if (st.hasTarget) {
            ImGui::TextDisabled("target      %.0f, %.0f px", st.targetX, st.targetY);
            // Both, because when the controller is tracking well the box
            // barely moves on screen while the target is moving quickly. The
            // second line is the one prediction uses.
            ImGui::TextDisabled("box on screen  %.0f, %.0f px/s",
                                st.screenVelX, st.screenVelY);
            ImGui::TextDisabled("target really  %.0f, %.0f px/s",
                                st.velX, st.velY);
            ImGui::TextDisabled("lead applied   %+.0f, %+.0f px",
                                st.leadX, st.leadY);
            if (cfg.leadTrustOn)
                ImGui::TextDisabled("velocity steady %.0f%%",
                                    st.leadTrust * 100.0f);
            ImGui::TextDisabled("lead settles in %.0f ms", st.leadLagMs);
            if (cfg.drawLeadDot)
                ImGui::TextDisabled("(lead marker is on, so this is visible "
                                    "on screen too)");

            const float screenSpeed = std::sqrt(st.screenVelX * st.screenVelX +
                                                st.screenVelY * st.screenVelY);
            const float realSpeed   = std::sqrt(st.velX * st.velX +
                                                st.velY * st.velY);
            if (st.hasTarget && st.engaged && screenSpeed < 40.0f &&
                realSpeed < 40.0f && !g_control.responseLearned())
                ui::Notice(1, "Both readings are near zero while engaged. "
                              "That is expected while the aim is holding a "
                              "still target, but if the target is moving it "
                              "means the response scale has not been "
                              "measured, so the movement being cancelled by "
                              "the output cannot be added back. Run Auto-tune "
                              "or hold the key through a few large sweeps.");
            ImGui::TextDisabled("last move   %+.0f, %+.0f", st.lastDx, st.lastDy);
        } else {
            ImGui::TextDisabled("no target");
        }
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 2));
        ui::StatusPill(st.engaged ? "ENGAGED" : "STANDBY",
                       st.engaged ? ui::col::accent : ui::col::textFaint);
        }});
    AdvancedOnly(cards, cfg.advancedMode);
    RenderCards(0, cfg, cards);
}

static void TabVisual(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Field of view", [&] {
        ui::Toggle("Show FOV", &cfg.drawRoiRect,
            "Draws the detection boundary, and the action region when that "
            "is on. Size and position live on the General tab.");
        ui::SliderInt("Border", &cfg.fovThickness, 1, 8, 1, "%d px",
            "Thickness of the FOV outlines, separate from the detection "
            "boxes.");
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::ColorEdit3("##roicol", cfg.roiColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine(); ImGui::TextUnformatted("FOV colour");
        ui::Hint("%d x %d at %+d, %+d px from centre.", cfg.fov, cfg.fov,
                 (int)(cfg.roiOffXPct * 0.01f * g_capture.screenWidth()),
                 (int)(cfg.roiOffYPct * 0.01f * g_capture.screenHeight()));
        if (g_engine.ready()) {
            const float ar = (float)g_engine.inputW() / (float)g_engine.inputH();
            if (std::abs(ar - 1.0f) > 0.08f)
                ui::Notice(1, "The region is square but the engine expects "
                              "%.2f:1. The mismatch becomes padding and "
                              "wasted compute.", ar);
        }
        }});
    cards.push_back({"Bounding boxes overlay", [&] {
        ui::Toggle("Show boxes", &cfg.overlayOn,
            "Draws detection boxes on screen. With this on the model runs "
            "continuously; with it off it only runs while the activation key "
            "is held.");
        ui::Toggle("Confidence", &cfg.drawConfText,
            "Prints the confidence above each box, drawn with a pixel font "
            "because layered windows cannot render normal text.");
        ui::Toggle("Class marker", &cfg.drawLabels,
            "Encodes the class as the width of a tab above each box.");
        ui::Toggle("Confidence fill", &cfg.drawConfidence,
            "Fills that tab in proportion to the score.");
        ui::Toggle("Centre dot", &cfg.drawCenterDot);
        ui::Toggle("Mouse path", &cfg.drawPath,
            "Draws the route the aim has just taken, coloured by speed -- "
            "green where it is moving slowly, blue where it is quickest.\n\n"
            "Movement paths differ in ways that are hard to feel and easy to "
            "see. Switch this on, pick a path on the Movement card, and the "
            "shape of the line is the difference between them: an overshoot "
            "doubles back on itself, an arc bows, a two-phase move has a long "
            "blue run and a short green tail.");

        ui::Toggle("Lead marker", &cfg.drawLeadDot,
            "Draws a cross where prediction is aiming, as opposed to where "
            "the target is.\n\n"
            "It staying inside the box is normal: a target crossing at 400 "
            "px/s with a 40 ms lead is only 16 px ahead, which is less than "
            "most boxes are wide. Raise Lead time if the aim is trailing -- "
            "it wants to be near the real end-to-end latency, which the "
            "Debug tab reports as cadence plus inference.");
        ui::SliderInt("Box thickness", &cfg.boxThickness, 1, 8, 1, "%d px",
            "Detection boxes only. Also scales the score text.");
        ui::SliderFloat("Opacity", &cfg.overlayAlpha, 0.05f, 1.0f, "%.2f");

        ui::SliderFloat("Hide below", &cfg.overlayMinConf, 0.0f, 1.0f, "%.2f",
            "A separate floor for drawing, so marginal detections can be "
            "watched without entering the control path.");

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::ColorEdit3("##boxcol", cfg.boxColor,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SameLine(); ImGui::TextUnformatted("Box colour");


        }});
    cards.push_back({"Appearance", [&] {
        if (ui::ThemePicker("Theme", &cfg.uiTheme))
            ui::ApplyTheme(cfg.uiTheme);

        const char* styles[ui::StyleCount];
        for (int i = 0; i < ui::StyleCount; ++i) styles[i] = ui::StyleName(i);
        if (ui::ComboRow("Surfaces", &cfg.uiStyle, styles, ui::StyleCount,
                "How panels are drawn, separate from what colour they "
                "are.\n\n"
                "Flat is a border and a fill. Raised lights each card from "
                "above and drops a shadow beneath it. Soft presses them out "
                "of the panel behind, with a light edge on one side and a "
                "dark one on the other. Glass makes them translucent with a "
                "bright rim -- there is no real blur behind it, so the rim "
                "carries the effect."))
            ui::SetUiStyle(cfg.uiStyle);
        ui::Hint("%s. Palette only: layout and contrast are the same in all "
                 "of them.", ui::ThemeName(cfg.uiTheme));

        }});
    cards.push_back({"Performance monitor", [&] {
        ui::Toggle("Show performance monitor", &cfg.perfMonOn,
            "A small always-on-top readout of loop time, frame rate, and "
            "detections. Drag it anywhere; it snaps to the nearest edge.");
        const char* edges[] = {"Top", "Right", "Bottom", "Left"};
        if (ui::ComboRow("Position", &cfg.perfMonEdge, edges, 4,
                "Which screen edge it clings to."))
            g_perf.snapToEdge(cfg.perfMonEdge, cfg.perfMonEdgeT);
        if (ui::SliderFloat("Along edge", &cfg.perfMonEdgeT, 0.0f, 1.0f, "%.2f",
                "Where along that edge it sits. 0 and 1 are the corners, and "
                "a drop near either end snaps to them. Dragging the window "
                "sets both of these."))
            g_perf.snapToEdge(cfg.perfMonEdge, cfg.perfMonEdgeT);
        ui::SliderFloat("Size", &cfg.perfMonScale, 0.6f, 2.5f, "%.2fx",
            "Scales the readout on top of whatever the display scaling "
            "already applies. Text is redrawn at the new size rather than "
            "stretched.");

        ui::Toggle("Resource meters", &cfg.perfMonMeters,
            "Adds bars for processor, graphics and video memory load, plus "
            "the graphics clock and temperature. Answers whether a slow "
            "moment is the machine or the program without switching windows. "
            "Off, the readout shrinks back to just the timings.");
        ui::SliderFloat("Graph span", &cfg.perfGraphSecs, 0.5f, 20.0f, "%.1f s",
            "How much history the sparkline covers. Short reacts quickly but "
            "streaks past; long is smooth but slow to show a change.");
        ui::Hint("Redraws at 10 Hz, so it costs nothing measurable against "
                 "what it is measuring.");
        }});
    RenderCards(1, cfg, cards);
}

static void TabSettings(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Displays", [&] {
        // Everything about which screen is which, in one place.
        //
        // These were spread across three tabs, which made a mismatch between
        // them hard to even notice. They default to the primary display and
        // most people will never open this.
        const auto& disp = g_disk.displays;
        if (disp.size() <= 1) {
            if (!disp.empty())
                ui::Hint("One display, %d x %d. Nothing to choose between.",
                         disp[0].width, disp[0].height);
        } else {
            std::vector<const char*> items;
            for (const auto& d : disp) items.push_back(d.label.c_str());
            const int n = (int)disp.size();

            int cap = ResolveDisplay(cfg.captureMonitor);
            if (ui::ComboRow("Model watches", &cap, items.data(), n,
                    "Which display the model looks at. Capturing one screen "
                    "while working on another is a normal arrangement, and "
                    "capturing the wrong one is what makes the region appear "
                    "in the wrong place at the wrong size.")) {
                cfg.captureMonitor = cap;
                g_pending.switchMonitor = cap;
            }

            int pm = ResolveDisplay(cfg.perfMonDisplay);
            if (ui::ComboRow("Readout sits on", &pm, items.data(), n,
                "Which display the performance readout clings to. It has no "
                "bearing on what is captured."))
                cfg.perfMonDisplay = pm;
        }

        // What each source thinks, together, because a disagreement between
        // them is the whole diagnosis when the overlay is misplaced.
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushFont(ui::fontSmall);
        ImGui::TextDisabled("capture reports   %d x %d",
                            g_capture.detectedWidth(), g_capture.detectedHeight());
        {
            HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(mon, &mi))
                ImGui::TextDisabled("windows reports   %d x %d",
                                    (int)(mi.rcMonitor.right - mi.rcMonitor.left),
                                    (int)(mi.rcMonitor.bottom - mi.rcMonitor.top));
        }
        ImGui::TextDisabled("in use            %d x %d",
                            g_capture.screenWidth(), g_capture.screenHeight());
        ImGui::PopFont();

        ui::Toggle("Override the size", &cfg.resOverride,
            "Forces the screen size everything positional is worked out "
            "against.\n\n"
            "This should not be needed: the capture item reports the true "
            "pixel count whatever Windows thinks. It exists so a wrong "
            "detection is a setting rather than a dead end.");
        if (cfg.resOverride) {
            static const struct { const char* name; int w, h; } kPresets[] = {
                {"1920 x 1080", 1920, 1080}, {"2560 x 1440", 2560, 1440},
                {"3440 x 1440", 3440, 1440}, {"3840 x 2160", 3840, 2160},
                {"2560 x 1080", 2560, 1080}, {"1280 x 720",  1280, 720},
            };
            const char* names[6];
            int shown = 0;
            for (int i = 0; i < 6; ++i) {
                names[i] = kPresets[i].name;
                if (kPresets[i].w == cfg.resOverrideW &&
                    kPresets[i].h == cfg.resOverrideH) shown = i;
            }
            if (ui::ComboRow("Preset", &shown, names, 6,
                    "Common sizes. The two below take any value.")) {
                cfg.resOverrideW = kPresets[shown].w;
                cfg.resOverrideH = kPresets[shown].h;
            }
            ui::SliderInt("Width",  &cfg.resOverrideW, 640, 7680, 16, "%d px");
            ui::SliderInt("Height", &cfg.resOverrideH, 480, 4320, 16, "%d px");
        }
        }});
    AdvancedOnly(cards, cfg.advancedMode);

    cards.push_back({"Interface", [&] {
        const bool advBefore = cfg.advancedMode;
        ui::Toggle("Advanced mode", &cfg.advancedMode,
            "Shows the cards and settings that only matter once you are "
            "measuring things: the timing breakdowns, the engine internals, "
            "the estimator knobs.\n\n"
            "Hiding them changes nothing about how any of it behaves. "
            "Whatever you set stays set, and turning this back on shows it "
            "exactly as you left it.");
        // Switching into advanced mode turns the explanations off.
        //
        // Someone asking for the advanced view has already read them, and
        // the hint under every control is what makes the panel long. Only
        // the transition does this, so turning tooltips back on afterwards
        // sticks.
        if (!advBefore && cfg.advancedMode) cfg.tooltipsOn = false;

        if (!cfg.advancedMode)
            ui::Hint("Several cards are hidden. Nothing is switched off.");

        ui::Toggle("Show tooltips", &cfg.tooltipsOn,
            "Shows the explanatory line under each control. With this off the "
            "panel is far more compact; hovering any control still explains "
            "it either way.");
        ui::Hint("Off, this line and every one like it disappears.");
        
        ImGui::Dummy(ImVec2(0, 6));
        ui::SectionHeader("WINDOW");

        if (ui::Toggle("Always on top", &cfg.alwaysOnTop))
            g_pending.applyTopmost = true;

        ui::Toggle("Visible to capture", &cfg.visibleToCapture,
            "One switch for everything loopcore draws: this panel, the boxes, "
            "the region outline and the readout. Off, none of them appear in "
            "a recording or in the frames the model looks at.");
        if (cfg.visibleToCapture) {
            ui::Notice(1, "Everything is now recordable, including the boxes "
                          "the model then sees itself. Useful for capturing "
                          "a demo, misleading while tuning.");
        } else {
            ui::Hint("Nothing loopcore draws is captured, so it can sit on "
                     "the same monitor being tested without getting in the "
                     "way. Some recorders refuse to record the desktop at "
                     "all in this state and will say so.");
        }
        const char* prio[] = {"Normal", "Above normal", "High"};
        if (ui::ComboRow("Priority", &cfg.priorityMode, prio, 3,
                "Normal is right for almost everything."))
            g_pending.applyPriority = true;
        if (cfg.priorityMode == 2)
            ui::Notice(1, "High priority can starve the desktop: the taskbar "
                          "may stop responding while loopcore runs.");
        }});
    cards.push_back({"Reset", [&] {
        if (ui::GhostButton("Restore defaults", ImVec2(150, 0))) {
            const bool keepTop = cfg.alwaysOnTop;
            const int  keepPri = cfg.priorityMode;
            cfg = Config{};
            cfg.alwaysOnTop  = keepTop;
            cfg.priorityMode = keepPri;
            g_shared.log.info("Settings restored to defaults.");
            ui::Toast("Settings restored to defaults.");
        }
        ui::Hint("Window position and priority are kept.");
        }});

    cards.push_back({"Detection", [&] {
        ui::SliderConfidence("Confidence", &cfg.confThresh,
            "Detections below this score are discarded before anything else "
            "sees them. Both ends of the track are red: too low treats noise "
            "as a target, too high drops real ones.");
        ui::SliderInt("Max boxes", &cfg.maxDets, 1, 300, 1, "%d",
            "How many boxes are drawn. Targeting always sees every "
            "detection, so setting this to 1 no longer stops it following "
            "one target while a higher-scoring one is on screen.");
        ui::Toggle("Run the pipeline", &cfg.pipelineOn,
            "Turning this off stops capture and inference entirely.");
        }});
    cards.push_back({"Input method", [&] {
        const char* methods[] = {"Mouse", "Arduino", "Makcu"};
        // Switching to Arduino with nothing chosen picks a device once, so
        // the common case needs no extra button press.
        static int lastMethod = -1;
        if (lastMethod != cfg.inputMethod) {
            lastMethod = cfg.inputMethod;
            if ((cfg.inputMethod == InputArduino ||
                 cfg.inputMethod == InputMakcu) && cfg.serialPort <= 0) {
                RefreshPorts();
                const int found = AutoPickPort(g_ports);
                if (found > 0) {
                    cfg.serialPort = found;
                    for (const auto& pt : g_ports)
                        if (pt.number == found) {
                            const int b = BoardFromFriendlyName(pt.friendlyName);
                            if (b >= 0) cfg.boardType = b;
                        }
                    ui::Toast(("Auto-selected COM" +
                               std::to_string(found) + ".").c_str());
                }
            }
        }

        ui::ComboRow("Send via", &cfg.inputMethod, methods, 3,
            "Mouse injects relative movement into Windows. Arduino sends "
            "packets over serial to a board acting as a USB HID device.\n\n"
            "Makcu is for running the model on a second machine: the device "
            "plugs into the machine being played on and appears there as a "
            "real mouse, while this one only writes to a COM port. Capture "
            "and inference happen here, so the machine under load and the "
            "machine being aimed on need not be the same.");

        if (cfg.inputMethod == InputMouse) {
            ui::Hint("Relative movement via SendInput. Good for the sim; "
                     "many games ignore synthetic input.");
        } else if (cfg.inputMethod == InputMakcu) {
            ui::Hint("The Makcu is on this machine's COM port, and its mouse "
                     "end plugs into the other one. Nothing is injected "
                     "locally, so this machine can be doing nothing but "
                     "capturing and inferring.");
            ui::Notice(0, "Board input only and the firmware options below do "
                          "not apply to a Makcu -- it is not an Arduino and "
                          "nothing here flashes it.");
        }
        if (cfg.inputMethod != InputMouse) {
            // A list of what is actually attached, not a number to guess.
            // Every value a slider passes through on the way is a real port
            // that something else may own, and opening it has consequences.
            std::vector<std::string> labels;
            std::vector<int> numbers;
            labels.push_back("None");
            numbers.push_back(0);
            for (const auto& p : g_ports) {
                labels.push_back(p.friendlyName.empty() ? p.name : p.friendlyName);
                numbers.push_back(p.number);
            }
            std::vector<const char*> items;
            for (const auto& l : labels) items.push_back(l.c_str());

            int sel = 0;
            for (size_t i = 0; i < numbers.size(); ++i)
                if (numbers[i] == cfg.serialPort) sel = (int)i;

            if (ui::ComboRow("Device", &sel, items.data(), (int)items.size(),
                    "Serial devices currently attached. Anything that looks "
                    "like a board is detected by its USB vendor ID."))
                cfg.serialPort = numbers[sel];

            if (ui::GhostButton("Rescan", ImVec2(84, 0))) {
                // Read from the local copy, not the shared one.
                //
                // A block of startup code had been spliced into this handler
                // -- original indentation and all -- so pressing Rescan read
                // shared config without the lock and did a filesystem search
                // for arduino-cli in the middle of a draw. The lines are
                // back where they belong, at startup.
                if (cfg.boardDevice[0]) rawin::SetPreferred(cfg.boardDevice);
                RefreshPorts();
                ui::Toast("Rescanned serial devices.");
            }
            ImGui::SameLine();
            if (ui::GhostButton("Auto detect", ImVec2(112, 0))) {
                RefreshPorts();
                const int found = AutoPickPort(g_ports);
                if (found > 0) {
                    cfg.serialPort = found;
                    // The port name usually says what the board is, so there
                    // is no reason to make the user pick it again.
                    for (const auto& pt : g_ports) {
                        if (pt.number != found) continue;
                        const int b = BoardFromFriendlyName(pt.friendlyName);
                        if (b >= 0) cfg.boardType = b;
                    }
                    ui::Toast(("Selected COM" + std::to_string(found) + ".").c_str());
                } else {
                    ui::Toast("No board-like device found. Plug it in and rescan.");
                }
            }

            if (g_ports.empty())
                ui::Notice(1, "No serial devices found. Plug the board in and "
                              "press Rescan.");

            const char* bauds[] = {"9600", "115200", "250000", "500000", "1000000"};
            static const int baudVals[] = {9600, 115200, 250000, 500000, 1000000};
            int bi = 1;
            for (int i = 0; i < 5; ++i) if (baudVals[i] == cfg.baudRate) bi = i;
            if (ui::ComboRow("Baud", &bi, bauds, 5,
                    "How fast bytes move down the wire. On a Leonardo, Micro, "
                    "Pro Micro or Teensy this is ignored entirely: those talk "
                    "USB directly and run at USB speed whatever you set. It "
                    "only matters on boards with a separate USB-to-serial "
                    "chip, like an Uno or a CH340 clone, where it has to "
                    "match Serial.begin() in the sketch. 115200 if unsure."))
                cfg.baudRate = baudVals[bi];

            ui::Toggle("Board input only", &cfg.arduinoOnlyInput,
                "Only accept activation clicks that came through the board. "
                "Without this any mouse on the desk triggers it, because "
                "Windows merges them all into one button state.");

        if (cfg.arduinoOnlyInput) {
                // Guessing the device from its vendor id fails on firmware
                // that does not advertise one, which is most prebuilt hex.
                // Learning it from a click is unambiguous.
                const std::string prefNow = rawin::Preferred();
                if (rawin::Learning()) {
                    ui::Notice(0, "Now click any button on the mouse that is "
                                  "plugged into the board.");
                } else {
                    // Stays available once a device is chosen. Reflashing the
                    // board changes how it enumerates, so the choice has to
                    // be redone often enough that hiding the button would be
                    // a nuisance.
                    if (ui::PrimaryButton(prefNow.empty()
                                          ? "Click to set Arduino mouse"
                                          : "Set Arduino mouse again",
                                          ImVec2(220, 0)))
                        rawin::LearnNextDevice();
                    if (!prefNow.empty()) {
                        ImGui::SameLine();
                        if (ui::GhostButton("Forget", ImVec2(80, 0))) {
                            rawin::SetPreferred("");
                            cfg.boardDevice[0] = '\0';
                        }
                    }
                }

                const std::string pref = rawin::Preferred();
                ImGui::PushFont(ui::fontSmall);
                if (pref.empty())
                    ImGui::TextDisabled("no device chosen, using vendor id guess");
                else
                    ImGui::TextDisabled("device: %s",
                        pref.size() > 60 ? pref.substr(pref.size() - 60).c_str()
                                         : pref.c_str());
                const auto& devs = g_disk.devices;
                for (const auto& d : devs)
                    ImGui::TextDisabled("  %s%s%s", d.label.c_str(),
                                        d.isBoard ? "  <- board" : "",
                                        d.active ? "  (seen)" : "");
                ImGui::PopFont();

                if (!rawin::BoardSeen())
                    ui::Notice(1, "No device has identified itself as the "
                                  "board yet, so every mouse is still "
                                  "accepted. Press Learn and click with the "
                                  "one on the shield.");
            }

            const char* protos[] = {"loopcore binary", "ASCII  x,y,click"};
            if (cfg.serialProtocol == ProtoBinary)
                ui::Notice(1, "Binary only works with the bundled sketch. The "
                              "prebuilt YesHostShield and NoHostShield hex "
                              "files speak ASCII and will ignore these "
                              "packets entirely, so nothing moves.");
            ui::ComboRow("Protocol", &cfg.serialProtocol, protos, 2,
                "Binary suits the bundled sketch: 7 fixed bytes with a "
                "checksum, so a dropped byte cannot desync it. ASCII suits "
                "the prebuilt community firmwares, including the "
                "YesHostShield and NoHostShield hex files.");

            ui::Hint("Packets go out at the output rate set on the General "
                     "tab, currently %d Hz. Having the same number in two "
                     "places only created a way for them to disagree.",
                     cfg.serialRateHz);

            ui::Hint("A 7-byte packet: header, dx, dy, count, checksum. Fixed "
                     "length, so the receiver can resync after a dropped "
                     "byte. DTR is held low, so opening the port does not "
                     "reset the board.");

            const auto st = g_control.state();
            if (cfg.serialPort > 0 && !st.sinkReady && cfg.controlEnabled)
                ui::Notice(2, "COM%d did not open. Something else may have it "
                              "-- close any serial monitor and press Rescan.",
                           cfg.serialPort);
        }
        }});
    cards.push_back({"Firmware", [&] {
            const std::string& cli = g_disk.arduinoCli;
            const std::string& sketch = g_disk.sketchDir;
            const bool flashing = g_flasher.busy();

            const char* boards[BoardCount];
            for (int i = 0; i < BoardCount; ++i) boards[i] = BoardName(i);
            ui::ComboRow("Board", &cfg.boardType, boards, BoardCount,
                "Which board is attached. Detected from the port name where "
                "possible.");

            ui::Toggle("USB Host Shield", &cfg.fwPassthrough,
                "Build with host shield support, so a physical mouse plugged "
                "into the shield passes through. This is also what powers the "
                "shield's downstream port: without it a mouse plugged in "
                "there gets no power at all.");
            if (!cfg.fwPassthrough) {
                ui::Hint("Off, the board only injects movement. Plug the mouse "
                         "straight into the PC: fewer hops, less latency, and "
                         "nothing to misread. Only turn this on if the mouse "
                         "has to route through the board.");
            } else {
                const char* layouts[] = {
                    "Guess from length",
                    "8-bit  (btn, X8, Y8, wheel)",
                    "16-bit (btn, X16, Y16, wheel)",
                    "16-bit with report id",
                };
                ui::ComboRow("Report layout", &cfg.fwMouseLayout, layouts, 4,
                    "How the mouse's HID report is arranged. Guessing suits "
                    "most mice. If the pointer dashes across the screen, or "
                    "moving sideways scrolls, the guess is wrong for yours.");

                // --- work out this mouse's report layout -------------------
                ImGui::Dummy(ImVec2(0, 4));
                if (!g_sketch.running()) {
                    if (ui::GhostButton("Identify this mouse", ImVec2(190, 0))) {
                        g_sketch.begin();
                        g_pending.sketchCmd = "s1";
                    }
                    ui::Hint("Watches the mouse do a short list of things and "
                             "works out which byte of its report carries each "
                             "one. Needs firmware with Dump reports on, and "
                             "the mouse plugged into the shield.");
                    if (!g_sketchResult.empty()) {
                        ImGui::PushFont(ui::fontSmall);
                        ImGui::TextUnformatted(g_sketchResult.c_str());
                        ImGui::PopFont();
                        if (ui::GhostButton("Copy layout", ImVec2(130, 0))) {
                            ImGui::SetClipboardText(g_sketchResult.c_str());
                            ui::Toast("Layout copied.");
                        }
                        ImGui::SameLine();
                        ImGui::PushItemWidth(150);
                        ImGui::InputTextWithHint("##sketchname", "profile name",
                                                 g_sketchName, sizeof(g_sketchName));
                        ImGui::PopItemWidth();
                        ImGui::SameLine();
                        if (ui::PrimaryButton("Save", ImVec2(80, 0)))
                            g_pending.saveSketch = g_sketchName[0] ? g_sketchName
                                                                  : "mouse";
                    }
                } else {
                    const int st = g_sketch.step();
                    ImGui::PushFont(ui::fontSmall);
                    ImGui::TextDisabled("step %d of %d", st + 1, (int)StepCount);
                    ImGui::PopFont();

                    ui::Notice(0, "%s", SketchStepTitle(st));
                    ui::Hint("%s", SketchStepDetail(st));

                    // An arrow, because "left" is ambiguous when the mouse is
                    // in your hand and the screen is in front of you.
                    if (const int arrow = SketchStepArrow(st)) {
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        const ImVec2 o = ImGui::GetCursorScreenPos();
                        const float w = ImGui::GetContentRegionAvail().x;
                        const float cx = o.x + w * 0.5f, cy = o.y + 26.0f;
                        const float L = 26.0f;
                        ImVec2 tip, tail, w1, w2;
                        switch (arrow) {
                        case 1: tip = {cx - L, cy}; tail = {cx + L, cy};
                                w1 = {cx - L + 12, cy - 10}; w2 = {cx - L + 12, cy + 10}; break;
                        case 2: tip = {cx + L, cy}; tail = {cx - L, cy};
                                w1 = {cx + L - 12, cy - 10}; w2 = {cx + L - 12, cy + 10}; break;
                        case 3: tip = {cx, cy - L}; tail = {cx, cy + L};
                                w1 = {cx - 10, cy - L + 12}; w2 = {cx + 10, cy - L + 12}; break;
                        default: tip = {cx, cy + L}; tail = {cx, cy - L};
                                w1 = {cx - 10, cy + L - 12}; w2 = {cx + 10, cy + L - 12}; break;
                        }
                        dl->AddLine(tail, tip, ui::col::accent, 3.0f);
                        dl->AddTriangleFilled(tip, w1, w2, ui::col::accent);
                        ImGui::Dummy(ImVec2(w, 56.0f));
                    }

                    const int got = g_sketch.gathered();
                    const int want = g_sketch.wanted();
                    ImGui::ProgressBar(std::min(1.0f, (float)got / (float)std::max(1, want)),
                                       ImVec2(-1, 6), "");
                    ImGui::PushFont(ui::fontSmall);
                    ImGui::TextDisabled("%d of %d samples", std::min(got, want), want);
                    ImGui::PopFont();

                    if (got == 0)
                        ui::Notice(1, "No reports are arriving. The firmware "
                                      "needs Dump reports switched on, and "
                                      "the mouse has to be plugged into the "
                                      "shield rather than the PC.");

                    const bool ready = g_sketch.stepSatisfied();
                    if (ready) {
                        if (ui::PrimaryButton("Next", ImVec2(110, 0))) {
                            if (!g_sketch.advance()) {
                                g_sketchResult = g_sketch.explain();
                                g_pending.sketchCmd = "s0";
                            }
                        }
                    } else if (ui::GhostButton("Skip this step", ImVec2(140, 0))) {
                        // Skipping is legitimate: not every mouse has side
                        // buttons, and forcing a sample that cannot happen
                        // would strand the whole sequence.
                        if (!g_sketch.advance()) {
                            g_sketchResult = g_sketch.explain();
                            g_pending.sketchCmd = "s0";
                        }
                    }
                    ImGui::SameLine();
                    if (ui::GhostButton("Cancel", ImVec2(100, 0))) {
                        g_sketch.cancel();
                        g_pending.sketchCmd = "s0";
                    }
                }
                ImGui::Dummy(ImVec2(0, 4));

                ui::Toggle("Dump reports", &cfg.fwDumpReports,
                    "Makes the board print its raw HID bytes back over serial. "
                    "They appear in the Debug log as 'board:' lines, which is "
                    "how to work out the right layout.");

                ui::Hint("Reads the mouse in report protocol, so the wheel and "
                         "buttons 4 and 5 come through as well as movement. "
                         "Tilt wheel does not: the HID descriptor has no pan "
                         "axis. If the board's LED stays lit the shield is not "
                         "responding, which is wiring or the VBUS jumper.");
            }

            if (cli.empty()) {
                ui::Notice(1, "arduino-cli is not installed, so flashing is "
                              "unavailable. Install it, or drop "
                              "arduino-cli.exe into bin\\tools.");
                if (ui::GhostButton("Get arduino-cli", ImVec2(140, 0)))
                    g_pending.openFolder =
                        "https://arduino.github.io/arduino-cli/latest/installation/";
                ImGui::SameLine();
                if (ui::GhostButton("Open tools", ImVec2(112, 0))) {
                    CreateDirectoryA((store::binDir() + "\\tools").c_str(), nullptr);
                    g_pending.openFolder = store::binDir() + "\\tools";
                }
            } else if (sketch.empty()) {
                ui::Notice(2, "The firmware folder is missing. Re-extract the "
                              "zip so firmware\\loopcore_mouse sits beside "
                              "loopcore.exe.");
            } else {
                if (flashing) ImGui::BeginDisabled();
                ui::Hint("The board is asked to reset itself first. If it "
                         "does not -- which happens after a bad flash, or "
                         "when something else holds the port -- press the "
                         "reset button on it and the upload starts on its "
                         "own within a tenth of a second. There is no "
                         "waiting to time.");

                if (ui::PrimaryButton("Flash board", ImVec2(132, 0))) {
                    if (cfg.serialPort <= 0) {
                        ui::Toast("Pick a device first.");
                    } else {
                        // Released, and given a moment to actually become
                        // free: closing a handle and the port becoming
                        // available are not the same instant, and the
                        // flasher's first action is to open it.
                        g_control.releaseOutput();
                        Sleep(150);
                        g_flasher.start(cfg.serialPort, cfg.boardType,
                                        cfg.fwPassthrough, cfg.fwMouseLayout,
                                        cfg.fwDumpReports, g_shared.log);
                    }
                }
                if (flashing) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ui::GhostButton("Open sketch", ImVec2(116, 0)))
                    g_pending.openFolder = sketch;

                ImGui::Dummy(ImVec2(0, 6));
                ImGui::TextDisabled("Prebuilt firmware");
                {
                    const std::string& yes = g_disk.hexShield;
                    const std::string& no  = g_disk.hexNoShield;
                    auto flashHex = [&](const std::string& path) {
                        if (cfg.serialPort <= 0) { ui::Toast("Pick a device first."); return; }
                        if (path.empty()) { ui::Toast("That hex is missing from firmware\\hex."); return; }
                        g_control.releaseOutput();
                        g_flasher.startHex(cfg.serialPort, path, g_shared.log);
                    };
                    if (ui::GhostButton("Host shield", ImVec2(112, 0))) flashHex(yes);
                    ImGui::SameLine();
                    if (ui::GhostButton("No shield", ImVec2(104, 0))) flashHex(no);
                    ui::Hint("Known-good builds that speak the ASCII protocol. "
                             "Set Protocol to ASCII when using either.");
                }

                ImGui::Dummy(ImVec2(0, 4));
                if (ui::GhostButton("Flash another .hex", ImVec2(160, 0))) {
                    char file[512] = {0};
                    OPENFILENAMEA ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner   = g_hwnd;
                    ofn.lpstrFilter = "Firmware (*.hex)\0*.hex\0All files\0*.*\0";
                    ofn.lpstrFile   = file;
                    ofn.nMaxFile    = sizeof(file);
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameA(&ofn)) {
                        if (cfg.serialPort <= 0) {
                            ui::Toast("Pick a device first.");
                        } else {
                            g_control.releaseOutput();
                            g_flasher.startHex(cfg.serialPort, file, g_shared.log);
                        }
                    }
                }
                ui::Hint("Writes an already-compiled firmware with avrdude, "
                         "nothing is built. Use this for a hex someone else "
                         "produced -- set Protocol to match whatever it "
                         "expects, usually ASCII.");

                ui::Hint("Compiles and uploads loopcore_mouse.ino. Without it "
                         "the packets arrive and nothing acts on them. The "
                         "first run downloads board support, which takes a "
                         "minute.");
            }

            if (flashing || g_flasher.state() == FlashState::Failed) {
                ImGui::Dummy(ImVec2(0, 6));
                const auto fs = g_flasher.status();
                if (flashing) {
                    ImGui::TextUnformatted(fs.phase.c_str());
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                        ImGui::ColorConvertU32ToFloat4(ui::col::accent));
                    ImGui::ProgressBar(fs.fraction, ImVec2(-1, 6), "");
                    ImGui::PopStyleColor();
                    if (!fs.detail.empty()) ui::Hint("%s", fs.detail.c_str());
                } else {
                    ui::Notice(2, "Flashing failed. The Debug tab has the "
                                  "full output.");
                }
            }
            }, cfg.inputMethod == InputArduino});
    RenderCards(3, cfg, cards);
}

static void TabModel(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Load new model", [&] {

    // Buttons on their own row, the path on the full width below them.
    //
    // Sharing a row meant the path competed with two fixed-width buttons for
    // space, and a path is exactly the kind of string that is useless when
    // truncated: a path cut off after the drive and one folder tells you
    // nothing about which file it is. Giving it the whole width costs one
    // row of height and makes it readable.
    const bool busy = g_builder.busy();
    const bool needsBuild = ModelBuilder::needsConversion(g_modelPath);

    const float browseW = 88.0f;
    const float retryW  = 104.0f;

    if (ui::GhostButton("Browse", ImVec2(browseW, 0))) g_pending.browse = true;
    ImGui::SameLine();
    if (busy) ImGui::BeginDisabled();
    // Kept as a way to retry the same file without browsing again, which is
    // the only reason to press it now that choosing a file acts at once.
    // The same width the field reserved for it, not a second copy of the
    // number -- two literals that have to agree is how the row broke before.
    if (ui::GhostButton(needsBuild ? "Re-Convert" : "Reload",
                        ImVec2(retryW, 0)))
        g_pending.loadModel = g_modelPath;
    if (busy) ImGui::EndDisabled();

    // Only while there is something to stop.
    //
    // A permanently visible Cancel that does nothing most of the time is
    // worse than one that appears when it applies: it takes space on the row
    // and invites a click that has no effect.
    if (busy) {
        ImGui::SameLine();
        if (ui::GhostButton("Cancel", ImVec2(88, 0))) {
            g_builder.requestCancel();
            ui::Toast("Stopping the build.");
        }
        if (g_builder.cancelRequested())
            ui::Hint("Stopping. TensorRT only checks between optimisation "
                     "steps, so this ends at the next one rather than "
                     "instantly.");
    }

    ImGui::PushItemWidth(-1);
    ImGui::InputText("##model", g_modelPath, sizeof(g_modelPath));
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered() && g_modelPath[0])
        ImGui::SetTooltip("%s", g_modelPath);

    ui::Hint("Choosing a file uses it straight away. An .engine loads; an "
             ".onnx is built into one here; a .pt goes through ultralytics "
             "first and is then built.");

    if (busy || g_builder.state() == BuildState::Failed) {
        ImGui::Dummy(ImVec2(0, 8));
        auto pr = g_builder.progress();
        if (busy) {
            // The spinner, always, with the phase beside it.
            //
            // The bar is gone rather than shown when a fraction happens to be
            // available: TensorRT reports one for part of a build and nothing
            // for the rest, so a bar appeared, filled, vanished, and came
            // back -- which reads as several operations rather than one. A
            // mark that turns for the whole build says the true thing.
            ui::Spinner(20.0f);
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(pr.phase.c_str());
            if (!pr.detail.empty()) ui::Hint("%s", pr.detail.c_str());
            if (pr.etaSeconds > 0.0) {
                const int total = (int)(pr.etaSeconds + 0.5);
                const char* qual = pr.etaIsGuess ? " (rough)" : "";
                if (total >= 60)
                    ui::Hint("about %d min %d s remaining%s",
                             total / 60, total % 60, qual);
                else
                    ui::Hint("about %d s remaining%s", total, qual);
            }
        } else {
            ui::Notice(2, "Conversion failed. The Debug tab has the full output.");
        }
    }

        }});
    cards.push_back({"Loaded engine", [&] {
        // The model timed on its own, against the model timed in the loop.
        //
        // The live gpu figure is elapsed time on the stream, so a model
        // preempted by a game reads as a slower model. Without something to
        // compare against there is no way to tell those apart, and I spent
        // several rounds tuning engine quality for what turned out to be
        // contention.
        if (ui::GhostButton("Benchmark the model", ImVec2(190, 0)))
            g_pending.benchmark = true;
        ui::Hint("Runs it sixty times on a fixed input and reports the "
                 "median. Do it once with nothing else running, then again "
                 "with the game up.");

        if (g_benchResult.ran) {
            static std::vector<double> bq;
            const auto live = g_shared.timings.stage[ST_GPU].summary(bq);
            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("alone: %.2f ms   (best %.2f, worst %.2f)",
                                g_benchResult.medianMs, g_benchResult.bestMs,
                                g_benchResult.worstMs);
            if (live.n > 100 && live.p50 > 0.0)
                ImGui::TextDisabled("in the loop: %.2f ms", live.p50);
            ImGui::PopFont();

            if (live.n > 100 && live.p50 > g_benchResult.medianMs * 1.25)
                ui::Notice(0, "The model finishes slower in the loop than on "
                              "its own, which is the card being shared rather "
                              "than the engine being slow. Rebuilding will not "
                              "help that.");
        }
        ImGui::Dummy(ImVec2(0, 6));

    if (g_engine.ready()) {
        // What the sidecar says this engine was made from and on. Written at
        // build time, because none of it can be read back out of a plan.
        {
            const auto it = g_disk.meta.find(g_modelPath);
            const store::EngineMeta meta = it != g_disk.meta.end()
                                         ? it->second : store::EngineMeta{};
            if (meta.valid) {
                ImGui::PushFont(ui::fontSmall);
                ImGui::TextDisabled("built on %s, compute %d.%d, TensorRT %d.%d",
                                    meta.gpuName.c_str(), meta.smMajor,
                                    meta.smMinor, meta.trtMajor, meta.trtMinor);
                if (!meta.builtAt.empty())
                    ImGui::TextDisabled("%s%s", meta.builtAt.c_str(),
                                        meta.fp16 ? "   fp16 requested" : "");
                if (meta.portable)
                    ImGui::TextDisabled("portable: runs on Ampere and newer");
                if (meta.smokeOk)
                    ImGui::TextDisabled("verified at build: one inference in "
                                        "%.2f ms", meta.smokeMs);
                ImGui::PopFont();
                if (!meta.smokeOk && meta.smokeMs > 0.0)
                    ui::Notice(2, "This engine failed its check when it was "
                                  "built: it loaded but produced no usable "
                                  "output.");
            } else {
                ui::Hint("No build record beside this engine. It was made by "
                         "an older version, or copied here from elsewhere.");
            }
        }

        if (ui::GhostButton("Verify now", ImVec2(110, 0)))
            g_pending.verifyEngine = true;
        ui::Hint("Runs one inference on synthetic input and reports whether "
                 "the output is plausible. A build reporting success only "
                 "means TensorRT was willing to serialise something.");
        ImGui::Dummy(ImVec2(0, 4));

        ImGui::TextUnformatted(g_engine.describe().c_str());

        ImGui::PushFont(ui::fontSmall);
        for (const auto& line : g_engine.tensorReport())
            ImGui::TextDisabled("%s", line.c_str());
        ImGui::PopFont();

        ImGui::Dummy(ImVec2(0, 6));
        const char* layouts[LayoutCount];
        for (int i = 0; i < LayoutCount; ++i) layouts[i] = LayoutName(i);
        int lay = cfg.outputLayout;
        if (ui::ComboRow("Output layout", &lay, layouts, LayoutCount,
                "How the network's output is arranged. Auto reads it from the "
                "tensor shapes. Getting this wrong does not fail loudly, it "
                "decodes noise into plausible-looking boxes.")) {
            cfg.outputLayout = lay;
            { std::lock_guard<std::mutex> el(g_engineMutex);
              g_engine.setLayoutOverride(lay, g_shared.log); }
        }

        const auto d = g_engine.diag();
        ImGui::PushFont(ui::fontSmall);
        ImGui::TextDisabled("last frame: %d candidates, %d kept, best score %.3f",
                            d.rawCandidates, d.kept, d.maxScore);
        if (d.kept > 0)
            ImGui::TextDisabled("box sizes %.0f to %.0f px", d.minBoxPx, d.maxBoxPx);
        ImGui::PopFont();

        if (d.rawCandidates > 0 && d.maxScore > 0.999f)
            ui::Notice(1, "Every score is pinned at 1.0, which usually means "
                          "the output is being read with the wrong layout.");
    } else {
        ui::Hint("Nothing loaded.");
    }

        }});
    // Hidden outright when there is nothing loaded.
    //
    // An empty card explaining that it is empty is worse than no card: it
    // takes a column slot and says nothing the absence would not.
    if (!cards.empty() && !g_engine.ready()) cards.back().visible = false;
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Library", [&] {
    if (g_library.empty()) {
        ui::Hint("Empty. Built engines land in bin\\models and appear here. "
                 "The .pt and .onnx files they came from are kept in "
                 "bin\\converted.");
    } else {
        // Tall enough for what is in it, up to a point.
        //
        // A fixed 148 px meant four models and a scrollbar inside a card that
        // already scrolls, so the user scrolled a list inside a list. Sizing
        // to the contents removes the inner scroll entirely until the library
        // is genuinely long, and the ceiling stops one enormous card from
        // pushing everything else off the tab.
        // Sized to the contents, up to six models.
        //
        // A fixed height meant scrolling a list inside a card that already
        // scrolls, which is two scrollbars for one list. Growing with the
        // library removes the inner one entirely for anyone with a handful
        // of engines, which is most people.
        //
        // Six rather than unlimited: past that the card starts pushing
        // everything else off the tab, and an inner scroll is the lesser
        // annoyance. Six is also about what fits beside the other cards
        // without the column looking lopsided.
        const float rowPitch = 42.0f;   // row height plus its gap
        const int   shown    = std::clamp((int)g_library.size(), 1, 6);
        ImGui::BeginChild("##lib",
                          ImVec2(0, rowPitch * (float)shown + 8.0f),
                          LC_CHILD_BORDER);
        for (const auto& m : g_library) {
            ImGui::PushID(m.path.c_str());
            const bool current = (m.path == g_modelPath);

            // No AlignTextToFramePadding here.
            //
            // It nudges the cursor down by half a frame's padding before
            // anything is drawn, and the rectangle below is measured from the
            // cursor -- so the frame started lower than the row it was meant
            // to enclose, and everything placed relative to that frame
            // inherited the offset. The row does its own vertical centring
            // now, which is what that call was approximating.

            // The row is laid out from a rectangle worked out up front.
            //
            // Everything used to be positioned with SameLine offsets taken
            // from GetContentRegionAvail, which is measured from wherever the
            // cursor happens to be -- so after the name had been drawn the
            // numbers referred to a different origin, and the buttons ended
            // up outside the outline that was supposed to contain them.
            // Deciding the geometry once, before anything is drawn, is what
            // makes the frame and its contents agree.
            // Equal margins all round, and one gap value used everywhere.
            //
            // The previous numbers were arithmetically correct -- the buttons
            // did sit inside the frame -- but the spacing was inconsistent:
            // the margin left of the name, the gap between the buttons and
            // the margin right of them were three different values, so the
            // group read as pushed against the edge even though it was not.
            // A single inset used on every side is what makes it look
            // deliberate.
            const float inset  = 12.0f;   // frame edge to any content
            const float gapIn  = 8.0f;    // between the two buttons

            // The button height comes from the font and the style, not from
            // a number chosen by eye.
            //
            // ImGui draws a button's label into the box shrunk by
            // FramePadding on every side, and clips whatever will not fit.
            // FramePadding.y is 6 here, so a 22 px button left 10 px for a 13
            // px line and the text was cut off along the bottom -- which
            // looked like the label falling out of the button, because that
            // is effectively what it was doing.
            const float bh     = ImGui::GetTextLineHeight() +
                                 ImGui::GetStyle().FramePadding.y * 2.0f;
            const float rowH   = bh + 12.0f;
            const float btnW   = 66.0f;
            const float delW   = 28.0f;
            // The glow reaches four pixels beyond the frame, so the frame is
            // inset by that much or the outermost ring is clipped by the
            // child window and the row looks cut off on the right.
            const float glowPad = 5.0f;
            const ImVec2 rowP  = ImGui::GetCursorScreenPos();
            const float rowW   = ImGui::GetContentRegionAvail().x;
            const ImVec2 r0(rowP.x + glowPad, rowP.y);
            const ImVec2 r1(rowP.x + rowW - glowPad, rowP.y + rowH);

            const auto lmIt = g_disk.meta.find(m.path);
            const store::EngineMeta lm = lmIt != g_disk.meta.end()
                                       ? lmIt->second : store::EngineMeta{};
            // A portable engine built elsewhere is fine here as long as this
            // card is Ampere or newer, so it is not flagged as foreign.
            const bool foreign = lm.valid && g_gpu.valid &&
                                 (lm.smMajor != g_gpu.major ||
                                  lm.smMinor != g_gpu.minor) &&
                                 !(lm.portable && g_gpu.major >= 8);
            const auto srcIt = g_disk.source.find(m.path);
            const std::string srcHere = (foreign && srcIt != g_disk.source.end())
                                      ? srcIt->second : std::string();

            // --- the frame -------------------------------------------------
            {
                ImDrawList* rdl = ImGui::GetWindowDrawList();
                if (current) {
                    // The glow is several rounded rectangles fading outward.
                    // A single wider border reads as a thick line; the
                    // falloff is what makes it look lit.
                    for (int g = 4; g >= 1; --g) {
                        const int alpha = 26 - g * 4;
                        if (alpha <= 0) continue;
                        rdl->AddRect(ImVec2(r0.x - g, r0.y - g),
                                     ImVec2(r1.x + g, r1.y + g),
                                     (ui::col::accent & 0x00FFFFFF) |
                                     ((ImU32)alpha << 24),
                                     7.0f + g, 0, 1.0f);
                    }
                    rdl->AddRectFilled(r0, r1,
                                       (ui::col::accent & 0x00FFFFFF) | 0x1A000000,
                                       7.0f);
                    rdl->AddRect(r0, r1, ui::col::accent, 7.0f, 0, 1.6f);
                } else {
                    rdl->AddRect(r0, r1,
                                 (ui::col::border & 0x00FFFFFF) | 0x77000000,
                                 7.0f, 0, 1.0f);
                }
            }

            // --- the name, vertically centred in the frame -----------------
            {
                // The whole row, less the buttons. The size used to take a
                // fixed 78 px out of the middle of every row, and it is the
                // one thing here nobody needs at a glance -- it is on hover
                // with the path and the date, where it belongs.
                const float nameW = std::max(40.0f,
                    (r1.x - r0.x) - btnW - delW - gapIn - inset * 3.0f);
                // Centred on the frame, using the font's full line height so
                // descenders are not clipped.
                const float lineH = ImGui::GetTextLineHeight();
                const ImVec2 at(r0.x + inset,
                                r0.y + (rowH - lineH) * 0.5f);
                ImGui::SetCursorScreenPos(at);

                if (current)
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(ui::col::accent));
                // The extension is dropped: every entry here is a .engine, so
                // it is four characters of noise repeated down the column.
                std::string shown = m.name;
                const size_t dot = shown.find_last_of('.');
                if (dot != std::string::npos && dot > 0) shown.resize(dot);

                // Clipped horizontally only; a tight vertical clip was
                // cutting the descenders off names like "RustV9Harlan".
                // The marker is measured first and its width taken out of
                // the name, rather than drawn after it.
                //
                // Placing it past the name's clip edge would put it straight
                // on top of the buttons now that the name runs the full
                // width. Reserving the space keeps both inside the frame,
                // and only costs the name anything on the rare row that has
                // a marker at all.
                const char* mark = !foreign ? nullptr
                                 : (srcHere.empty() ? "[other GPU]" : "[rebuild]");
                const float markW = mark
                    ? ImGui::CalcTextSize(mark).x + 6.0f : 0.0f;
                const float textW = std::max(30.0f, nameW - markW);

                // Double-clicking the name turns it into a field.
                //
                // Editing in place rather than through a dialog, because the
                // thing being renamed is right there and a dialog would only
                // ask the same question somewhere else.
                if (g_renaming == m.path) {
                    if (current) ImGui::PopStyleColor();
                    ImGui::SetNextItemWidth(textW);
                    if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                    const bool done = ImGui::InputText(
                        "##rename", g_renameBuf, sizeof(g_renameBuf),
                        ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                    // Committed on Enter, abandoned on Escape or on clicking
                    // away -- the same three ways every other rename works.
                    // Enter commits, and so does clicking away.
                    //
                    // Losing focus used to throw the edit away, which is the
                    // opposite of what every other text field does: if the
                    // user typed a name and clicked elsewhere, they meant the
                    // name. Escape is the way to abandon it, and it is
                    // checked first so it wins over the deactivation that
                    // follows in the same frame.
                    const bool escaped = ImGui::IsKeyPressed(ImGuiKey_Escape);
                    const bool leftIt  = ImGui::IsItemDeactivatedAfterEdit();

                    if (escaped) {
                        g_renaming.clear();
                    } else if (done || leftIt) {
                        g_pending.renameModel = m.path;
                        g_pending.renameTo = g_renameBuf;
                        g_renaming.clear();
                    } else if (ImGui::IsItemDeactivated()) {
                        // Deactivated without an edit: nothing was changed,
                        // so there is nothing to write.
                        g_renaming.clear();
                    }
                } else {
                    ImGui::PushClipRect(ImVec2(at.x, r0.y),
                                        ImVec2(at.x + textW, r1.y), true);
                    ImGui::TextUnformatted(shown.c_str());
                    ImGui::PopClipRect();
                    if (current) ImGui::PopStyleColor();

                    if (ImGui::IsMouseHoveringRect(
                            ImVec2(at.x, r0.y), ImVec2(at.x + textW, r1.y)) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        g_renaming = m.path;
                        strncpy_s(g_renameBuf, sizeof(g_renameBuf),
                                  shown.c_str(), _TRUNCATE);
                    }
                }

                if (mark) {
                    ImGui::SetCursorScreenPos(
                        ImVec2(at.x + textW + 6.0f, at.y));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(
                            srcHere.empty() ? ui::col::danger : ui::col::warn));
                    ImGui::TextUnformatted(mark);
                    ImGui::PopStyleColor();
                }

            }

            // --- the buttons, inside the frame -----------------------------
            {
                const float by = r0.y + (rowH - bh) * 0.5f;
                // Laid out from the right edge inward, so the margin after
                // the last button is the same as the margin before the name.
                const float delX = r1.x - inset - delW;
                const float btnX = delX - gapIn - btnW;

                ImGui::SetCursorScreenPos(ImVec2(btnX, by));
                if (foreign && !srcHere.empty()) {
                    if (ui::GhostButton("Rebuild", ImVec2(btnW, bh)))
                        g_pending.loadModel = srcHere;
                } else if (ui::GhostButton(current ? "Reload" : "Load",
                                           ImVec2(btnW, bh))) {
                    g_pending.loadModel = m.path;
                }

                ImGui::SetCursorScreenPos(ImVec2(delX, by));
                if (ui::GhostButton("X", ImVec2(delW, bh)))
                    ImGui::OpenPopup("##delmodel");
            }

            if (ImGui::BeginPopup("##delmodel")) {
                ImGui::PushFont(ui::fontSmall);
                ImGui::TextUnformatted("Delete this model?");
                ImGui::TextDisabled("%s", m.name.c_str());
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 4));
                if (ui::PrimaryButton("Delete", ImVec2(90, 0))) {
                    g_pending.deleteModel = m.path;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ui::GhostButton("Keep", ImVec2(80, 0)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            // The cursor is put back below the frame; everything above moved
            // it about explicitly.
            ImGui::SetCursorScreenPos(ImVec2(rowP.x, r1.y + 5.0f));
            ImGui::Dummy(ImVec2(rowW, 0.0f));

            if (ImGui::IsMouseHoveringRect(r0, r1))
                ImGui::SetTooltip("%s\n%s   %s", m.path.c_str(),
                                  m.sizeText.c_str(), m.dateText.c_str());

            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    if (ui::GhostButton("Models", ImVec2(90, 0)))
        g_pending.openFolder = store::modelsDir();
    ImGui::SameLine();
    if (ui::GhostButton("Sources", ImVec2(92, 0)))
        g_pending.openFolder = store::convertedDir();
    ImGui::SameLine();
    if (ui::GhostButton("Refresh", ImVec2(84, 0))) g_pending.refreshAll = true;
        }});
    cards.push_back({"Build options", [&] {
    ui::SliderInt("Engine width",  &cfg.engineW, 128, 1280, 32, "%d");
    ui::SliderInt("Engine height", &cfg.engineH, 128, 1280, 32, "%d");
    if (ui::GhostButton("Match to field of view", ImVec2(180, 0))) {
        cfg.engineW = cfg.fov;
        cfg.engineH = cfg.fov;
    }
    ui::Hint("Matching these to the capture region means no letterbox "
             "padding, so no compute is spent on grey pixels.");

    ImGui::Dummy(ImVec2(0, 6));
    ui::Toggle("FP16", &cfg.buildFp16);
    {
        // The two things that actually move the number, stated where the
        // engine is built rather than left to be inferred from a warning.
        static std::vector<double> sg, si;
        const auto g = g_shared.timings.stage[ST_GPU].summary(sg);
        const auto i2 = g_shared.timings.stage[ST_INFER].summary(si);
        if (g.n > 200 && i2.p50 > 0.0) {
            ui::Hint("Right now: %.1f ms on the card, %.1f ms in total for "
                     "inference. The difference is this thread waiting for "
                     "the result and then decoding it.", g.p50, i2.p50);
        }
    }

    if (g_engine.ready() && g_engine.needsCpuNms())
        ui::Notice(1, "This engine returns raw anchors, so every frame copies "
                      "the whole output back and suppresses it on the "
                      "processor while the loop waits. Re-exporting with "
                      "nms=True moves that onto the card and returns a "
                      "handful of boxes instead of thousands -- it is the "
                      "single largest saving available here, and costs "
                      "nothing at runtime.");

    ui::Hint("Input size is the largest lever on inference cost, and it "
             "scales with the area: 512 is about a third less work than 640, "
             "416 about half. Small targets suffer first, so it is worth "
             "measuring rather than assuming. Export at a different imgsz "
             "and convert that.");

    ui::Toggle("Portable engine", &cfg.buildPortable,
        "Not needed, and not recommended.\n\n"
        "A plan is normally compiled for the exact card it was built on, "
        "which is why copying one between machines is rejected. This relaxes "
        "that so the engine runs on other cards too.\n\n"
        "Two things to know before using it. It only reaches Ampere and "
        "newer -- 30, 40 and 50 series. TensorRT has no setting that extends "
        "backwards to Turing, so a 20 series card will still refuse it "
        "whichever machine built it. And it cannot use kernels specific to "
        "one architecture, so it is slower than a native build, often "
        "noticeably.\n\n"
        "Converting the source on each machine costs a couple of minutes and "
        "gives a faster engine. This is for when that is not possible.");
    ui::Hint("There is no way to build for a different card on purpose. "
             "TensorRT picks its kernels by timing candidates on the device "
             "in front of it, so the hardware has to be present. To move a "
             "model between machines, copy the .onnx or .pt from "
             "bin\\converted and convert it there -- a couple of minutes, "
             "and the result is faster than any portable build.");
    if (cfg.buildPortable) {
        ui::Notice(1, "Portable builds run on Ampere and newer only. Turing "
                      "and older will still reject the result, and it will be "
                      "slower than a native build on every card.");
        if (g_gpu.valid && g_gpu.major < 8)
            ui::Notice(2, "This machine is compute %d.%d, which is older than "
                          "Ampere. It can build a portable engine but cannot "
                          "run one, so the result is untestable here.",
                       g_gpu.major, g_gpu.minor);
    }

    // Always available, whatever is currently selected.
    //
    // This was disabled unless the chosen model was a .pt, which was correct
    // about when it has an effect and wrong about when it needs setting.
    // Picking a file starts the conversion immediately, so by the time a .pt
    // is selected the build is already running and the setting is out of
    // reach -- the one moment it mattered was the one moment it could not be
    // changed. A build option belongs to the next build, not to whatever
    // happens to be loaded now.
    ui::SliderInt("Build effort", &cfg.buildOptLevel, 0, 5, 1, "level %d",
        "How hard TensorRT searches for kernels when building.\n\n"
        "The default is 3. Level 5 tries far more tactics for each layer, "
        "which is where tuned fp16 tensor-core kernels get chosen over "
        "generic ones -- often worth ten to thirty per cent of inference "
        "time on a small model.\n\n"
        "It costs build time and nothing else. The engine is written once "
        "and used for as long as the model lasts, so minutes here buy "
        "milliseconds on every frame afterwards.");

    if (cfg.buildOptLevel >= 5)
        ui::Hint("Level 5 can take two to three times as long to build as "
                 "the default. Worth it once per model; tedious if you are "
                 "iterating.");
    else if (cfg.buildOptLevel <= 2)
        ui::Hint("Below the default. Builds finish sooner and the engine is "
                 "slower every frame after that.");

    ui::Toggle("Embed NMS", &cfg.buildEmbedNms,
        "Asks ultralytics to build the box filtering into the model during "
        "export.\n\n"
        "Only applies when converting from a .pt -- an .onnx already has its "
        "graph decided and nothing here can change it.\n\n"
        "Worth measuring rather than assuming. The NMS operator often forces "
        "neighbouring layers back to FP32, and on a card whose speed comes "
        "from fp16 tensor cores that can cost more than the host copy it "
        "saves. Compare the gpu figure with it on and off.");

    if (ModelBuilder::extensionOf(g_modelPath) != "pt")
        ui::Hint("The model selected now is not a .pt, so this will apply to "
                 "the next .pt you convert.");

    if (cfg.buildEmbedNms) {
        static std::vector<double> scq;
        const auto infq = g_shared.timings.stage[ST_INFER].summary(scq);
        if (infq.n > 200 && infq.p50 > 4.0)
            ui::Notice(1, "Inference is %.1f ms, which is slow for this input "
                          "size. Rebuilding with Embed NMS off is the first "
                          "thing to try.", infq.p50);
    }
    ui::Hint("FP16 stores weights and does arithmetic in 16-bit floats "
             "instead of 32-bit. Half the memory traffic, and on cards with "
             "tensor cores roughly double the throughput. Detection accuracy "
             "is almost never affected.");
    if (!g_advice.fp16Reason.empty()) {
        const bool agrees = (cfg.buildFp16 == g_advice.fp16);
        ui::Notice(agrees ? 0 : 1, "%s%s", g_advice.fp16Reason.c_str(),
                   agrees ? "" : "  Your current setting differs.");
    }

        }});
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Memory", [&] {
        // The recommendation is drawn on the track instead of written out
        // underneath it: a paragraph explaining a number is worse than a
        // green patch showing where it is.
        // Ceiling raised to match what the card can actually offer. The old
        // 8 GB limit was below TensorRT's own default on a card this size.
        ui::SliderSweetSpot("Workspace", &cfg.workspaceMB, 256,
            std::max(8192, g_advice.workspaceMB), 256,
            g_advice.workspaceMB, 0.16f, "%d MB",
            "Scratch memory TensorRT is allowed to use WHILE BUILDING an "
            "engine, and only then.\n\n"
            "It is not reserved, not held while the model runs, and has no "
            "effect on anything once the engine exists. Building happens once "
            "per model, with nothing else of yours running; after that this "
            "number does nothing at all. Raising it cannot cost you frames in "
            "a game.\n\n"
            "What it does affect is which kernels the builder may consider. A "
            "tactic needing more scratch than this is skipped however fast it "
            "would have been, so a small workspace quietly produces a slower "
            "engine -- permanently, since the engine is written once.\n\n"
            "The green band is about four fifths of the memory free right "
            "now, close to what TensorRT uses when nothing sets a limit. "
            "Lower it only if a build runs out of memory.");

        if (ui::GhostButton("Use suggested settings", ImVec2(190, 0))) {
            cfg.buildFp16   = g_advice.fp16;
            cfg.workspaceMB = g_advice.workspaceMB;
        }
        }});
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Profiles", [&] {
        ui::Hint("A profile stores every setting plus the model in use. "
                 "Loading one also loads its model, which is the only thing "
                 "that does.");

        // The name field takes the row; the two buttons share the next one.
        // All three on one line pushed Refresh past the edge of the card,
        // which is why it appeared to be missing.
        ImGui::PushItemWidth(-1);
        ImGui::InputTextWithHint("##profname", "profile name",
                                 g_profileName, sizeof(g_profileName));
        ImGui::PopItemWidth();

        if (ui::PrimaryButton("Save", ImVec2(88, 0)))
            g_pending.saveProfile = g_profileName[0] ? g_profileName : "profile";
        ImGui::SameLine();
        if (ui::GhostButton("Refresh", ImVec2(88, 0))) {
            RefreshProfiles();
            RefreshDiskCache(true);
            ui::Toast("Profile list refreshed.");
        }
        ImGui::SameLine();
        if (ui::GhostButton("Folder", ImVec2(88, 0)))
            g_pending.openFolder = store::configDir();
        ui::Hint("Refresh picks up a config dropped into the folder by hand.");

        if (g_profiles.empty()) {
            ui::Hint("No saved profiles yet.");
        } else {
            ImGui::BeginChild("##profiles", ImVec2(0, 112), LC_CHILD_BORDER);
            for (const auto& p : g_profiles) {
                ImGui::PushID(p.path.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(p.name.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 104);
                if (ui::GhostButton("Load", ImVec2(58, 0)))
                    g_pending.loadProfile = p.path;
                ImGui::SameLine();
                if (ui::GhostButton("X", ImVec2(26, 0)))
                    g_pending.deleteProfile = p.path;
                ImGui::Separator();
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
        }});

    RenderCards(5, cfg, cards);
}

static void TabCapture(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Capture", [&] {
        ui::Toggle("Enable capture", &cfg.captureEnabled,
            "Off, the hotkey does nothing. Worth having when the capture key "
            "is shared with the activation key and you only want to aim.");
        if (!cfg.captureEnabled) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        const bool capOff = !cfg.captureEnabled;

        const bool on = g_capwriter.running();
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImGui::ColorConvertU32ToFloat4(on ? ui::col::accent : ui::col::textDim));
        ImGui::TextUnformatted(on ? "Recording" : "Stopped");
        ImGui::PopStyleColor();

        if (!on) {
            if (ui::PrimaryButton("Start", ImVec2(96, 0))) {
                if (!g_engine.ready() && cfg.captureMode != CapTimed)
                    ui::Toast("This mode needs a model. Load one, or use Timed.");
                else if (g_capwriter.start(cfg, g_shared.log))
                    g_capture.setHostReadback(true);
            }
        } else {
            if (ui::GhostButton("Stop", ImVec2(96, 0))) {
                g_capwriter.stop(g_shared.log);
                g_capture.setHostReadback(false);
            }
        }
        ImGui::SameLine();
        if (ui::GhostButton("Open folder", ImVec2(122, 0)))
            g_pending.openFolder = g_capwriter.running()
                                 ? g_capwriter.sessionDir()
                                 : (cfg.capturePath[0] ? std::string(cfg.capturePath)
                                                       : store::binDir() + "\\captures");

        ui::KeybindRow("Hotkey", &cfg.captureKey,
            "Starts and stops capture without switching to this window.");
        if (cfg.captureKey != cfg.activationKey) {
            if (ui::GhostButton("Use the activation key", ImVec2(190, 0)))
                cfg.captureKey = cfg.activationKey;
        } else {
            ui::Notice(1, "Sharing the activation key means every engagement "
                          "writes images: a readback off the GPU, an encode "
                          "and a disk write, while the loop is trying to be "
                          "quick. Expect the delay to climb the longer the "
                          "key is held. Fine for gathering data, not for "
                          "measuring latency.");
        }

        ui::Hint("Frames come from the same region the model sees, so an "
                 "image and its labels always agree. Capture costs one "
                 "readback per frame and only runs while recording.");
        if (capOff) ImGui::PopStyleVar();
        }});
    cards.push_back({"Trigger", [&] {
        // Said here rather than on the General tab: this is a fact about
        // capturing, and it belongs beside the thing it is a consequence of.
        if (cfg.captureEnabled &&
            (cfg.captureKey == cfg.activationKey ||
             (cfg.activationKey2 > 0 && cfg.captureKey == cfg.activationKey2)))
            ui::Notice(0, "Capture shares the activation key, so writing "
                          "images adds a little delay while you aim.");

        const char* modes[] = {"Timed", "On detection", "Burst on loss"};
        ui::ComboRow("Mode", &cfg.captureMode, modes, 3,
            "Timed saves at a fixed rate. On detection saves only frames with "
            "something in them. Burst on loss captures nothing while tracking "
            "is solid and hardest just after the target is lost.");

        if (cfg.captureMode != CapBurstOnLoss)
            ui::SliderInt("Interval", &cfg.captureDelayMs, 20, 5000, 10, "%d ms",
                "Minimum gap between saves. On detection also needs a "
                "detection, so this is a ceiling on rate, not a schedule.");

        if (cfg.captureMode == CapTimed) {
            ui::Hint("Saves regardless of what is on screen. Simple, and it "
                     "collects a lot of frames a model already handles.");
        } else if (cfg.captureMode == CapOnDetection) {
            ui::Hint("Only frames the model found something in. Good for "
                     "growing a set of positives, poor for teaching it what "
                     "it currently misses.");
            if (!g_engine.ready())
                ui::Notice(1, "This mode needs a model loaded.");
        } else {
            ui::Hint("The useful one. Frames a model is already confident on "
                     "teach it nothing, so those are skipped; the rate climbs "
                     "as confidence decays and bursts once the target is "
                     "lost, which is exactly where the failures are.");
            if (!g_engine.ready())
                ui::Notice(1, "This mode needs a model loaded.");
        }
        }});
    cards.push_back({"Ladder", [&] {
            ui::Hint("Each step says: at or above this confidence, capture "
                     "this often. Zero means capture nothing, which is how "
                     "the top step skips frames the model already handles.");

            int remove = -1;
            for (int i = 0; i < cfg.ladderCount; ++i) {
                ImGui::PushID(i);
                char lbl[32];
                snprintf(lbl, sizeof(lbl), "Step %d", i + 1);

                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(
                        cfg.ladderMs[i] == 0 ? ui::col::textFaint : ui::col::text));
                ImGui::TextUnformatted(lbl);
                ImGui::PopStyleColor();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 26);
                if (cfg.ladderCount > 1 && ui::GhostButton("X", ImVec2(24, 0)))
                    remove = i;

                // Shown as a percentage, which is how confidence is talked
                // about everywhere else in the app.
                float pct = cfg.ladderConf[i] * 100.0f;
                if (ui::SliderFloat("Above", &pct, 0.0f, 100.0f, "%.0f %%",
                        "Confidence at or above which this step applies."))
                    cfg.ladderConf[i] = pct * 0.01f;
                ui::SliderInt("Timing", &cfg.ladderMs[i], 0, 1000, 10, "%d ms",
                    "Zero captures nothing at this confidence.");

                if (cfg.ladderMs[i] == 0)
                    ui::Hint("skipped");
                else
                    ui::Hint("about %.1f images a second while here",
                             1000.0f / (float)cfg.ladderMs[i]);

                ImGui::Separator();
                ImGui::PopID();
            }

            if (remove >= 0) {
                for (int i = remove; i < cfg.ladderCount - 1; ++i) {
                    cfg.ladderConf[i] = cfg.ladderConf[i + 1];
                    cfg.ladderMs[i]   = cfg.ladderMs[i + 1];
                }
                --cfg.ladderCount;
            }

            if (cfg.ladderCount < Config::kLadderMax &&
                ui::GhostButton("Add a step", ImVec2(130, 0))) {
                const int i = cfg.ladderCount;
                // Slot the new one below the last, which is where an extra
                // step is almost always wanted.
                cfg.ladderConf[i] = std::max(0.0f, cfg.ladderConf[i - 1] * 0.5f);
                cfg.ladderMs[i]   = cfg.ladderMs[i - 1] > 0
                                  ? cfg.ladderMs[i - 1] : 200;
                ++cfg.ladderCount;
            }
            ImGui::SameLine();
            if (ui::GhostButton("Sort", ImVec2(80, 0))) {
                // Steps are matched top down, so an out-of-order list would
                // silently shadow the ones below it.
                for (int i = 0; i < cfg.ladderCount; ++i)
                    for (int j = i + 1; j < cfg.ladderCount; ++j)
                        if (cfg.ladderConf[j] > cfg.ladderConf[i]) {
                            std::swap(cfg.ladderConf[i], cfg.ladderConf[j]);
                            std::swap(cfg.ladderMs[i], cfg.ladderMs[j]);
                        }
            }

            bool ordered = true;
            for (int i = 1; i < cfg.ladderCount; ++i)
                if (cfg.ladderConf[i] > cfg.ladderConf[i - 1]) ordered = false;
            if (!ordered)
                ui::Notice(1, "Steps are checked from the top down, so a "
                              "higher confidence lower in the list never "
                              "fires. Press Sort.");

            ui::SectionHeader("AFTER A LOSS");
            ui::SliderInt("Frames", &cfg.burstFrames, 0, 30, 1, "%d",
                "How many images to take once a target that was really there "
                "disappears. A fixed count rather than a duration, so the "
                "burst is the same size whatever the frame rate is.");
            ui::SliderInt("Interval", &cfg.burstMs, 0, 200, 5, "%d ms");
            ui::Hint("These are the frames the model got wrong, which makes "
                     "them the ones worth having.");
        }, cfg.captureMode == CapBurstOnLoss});
    cards.push_back({"Output", [&] {
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 40);
        ImGui::InputTextWithHint("##cappath", "bin\\captures",
                                 cfg.capturePath, sizeof(cfg.capturePath));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ui::GhostButton("[..]", ImVec2(32, 0))) g_pending.browseCapture = true;
        ui::Hint("Leave empty for bin\\captures beside the exe.");

        // The written size, deliberately separate from the field of view.
        //
        // The region is a tuning setting and moves around; a dataset wants
        // every image the same size, or the labels have to be rescaled
        // before anything can be trained on them.
        static const int kSizes[] = {320, 416, 512, 640, 800, 960, 1280};
        const char* sizeNames[] = {"320", "416", "512", "640",
                                   "800", "960", "1280"};
        int si = 3;
        for (int i = 0; i < 7; ++i) if (kSizes[i] == cfg.captureSize) si = i;
        if (ui::ComboRow("Image size", &si, sizeNames, 7,
                "The size images are written at, cropped about the centre of "
                "the region.\n\n"
                "Larger than the region has no effect, since there is no more "
                "image to take -- it is capped rather than padded."))
            cfg.captureSize = kSizes[si];
        if (cfg.captureSize > cfg.fov)
            ui::Notice(1, "The region is only %d px, so images are written at "
                          "that size rather than %d.", cfg.fov, cfg.captureSize);

        const char* fmts[] = {"PNG  lossless", "JPEG  smaller"};
        ui::ComboRow("Format", &cfg.captureFormat, fmts, 2,
            "PNG keeps every pixel, which matters if the set will be "
            "re-cropped or re-scaled later. JPEG is a fraction of the size.");
        if (cfg.captureFormat == ImgJpeg)
            ui::SliderInt("Quality", &cfg.captureQuality, 40, 100, 1, "%d");

        ui::Toggle("Write labels", &cfg.captureLabels,
            "Saves a YOLO .txt beside each image, from the detections in that "
            "frame. Only as good as the model that produced them, so treat it "
            "as pre-labelling to correct rather than ground truth.");
        ui::Toggle("Session folders", &cfg.captureSessionFolders,
            "One folder per run of the program, named for whatever "
            "application was in front for most of it. Switching capture off "
            "and on again keeps writing to the same folder; only restarting "
            "loopcore starts a new one.");
        }});
    cards.push_back({"Progress", [&] {
        const auto st = g_capwriter.stats();
        const auto tot = g_capwriter.totals();
        ImGui::PushFont(ui::fontSmall);
        ImGui::Text("this run   %llu images, %llu labels",
                    (unsigned long long)st.saved, (unsigned long long)st.labels);
        ImGui::Text("all runs   %llu images, %llu labels, %llu sessions",
                    (unsigned long long)tot.saved,
                    (unsigned long long)tot.labels,
                    (unsigned long long)tot.sessions);
        ImGui::Text("queued     %d", st.queued);
        ImGui::Text("dropped    %llu", (unsigned long long)st.dropped);
        ImGui::Text("failed     %llu", (unsigned long long)st.failed);
        if (!st.tier.empty()) ImGui::Text("last tier  %s", st.tier.c_str());
        if (!st.lastFile.empty()) ImGui::TextDisabled("%s", st.lastFile.c_str());
        ImGui::PopFont();

        if (st.dropped > 0)
            ui::Notice(1, "Frames were dropped because encoding could not "
                          "keep up. Lower the rate, or switch to JPEG.");
        }});
    PinnedFullWidth(cards);
    AdvancedOnly(cards, cfg.advancedMode);
    RenderCards(2, cfg, cards);
}


static void StageTable() {
    static std::vector<double> scratch;
    ImGui::PushFont(ui::fontSmall);
    if (ImGui::BeginTable("stages", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthStretch, 1.7f);
        ImGui::TableSetupColumn("mean");
        ImGui::TableSetupColumn("p50");
        ImGui::TableSetupColumn("p90");
        ImGui::TableSetupColumn("p99");
        ImGui::TableSetupColumn("max");
        ImGui::TableHeadersRow();

        for (int s = 0; s < ST_COUNT; ++s) {
            auto sum = g_shared.timings.stage[s].summary(scratch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool key = (s == ST_TOTAL);
            if (key) ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(ui::col::accent));
            ImGui::TextUnformatted(stage_name(s));
            if (key) ImGui::PopStyleColor();
            if (sum.n == 0) {
                for (int c = 0; c < 5; ++c) { ImGui::TableNextColumn(); ImGui::TextDisabled("--"); }
                continue;
            }
            const double v[5] = {sum.mean, sum.p50, sum.p90, sum.p99, sum.max};
            for (double d : v) { ImGui::TableNextColumn(); ImGui::Text("%.3f", d); }
        }
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ui::Hint("milliseconds, over the last %d samples", Ring::CAP);
}

static void TabDebug(Config& cfg) {
    std::vector<CardDef> cards;
    cards.push_back({"Stage timings", [&] {
        StageTable();

        // What the model is costing whatever is in front.
        {
            const double idleFps = g_shared.load.idleFps();
            const double busyFps = g_shared.load.busyFps();
            const double cost = g_shared.load.costPct();
            const int stride = g_shared.load.stride.load();

            ImGui::PushFont(ui::fontSmall);
            if (busyFps > 0.0)
                ImGui::TextDisabled("foreground frame rate  %.0f fps while "
                                    "inferring", busyFps);
            if (idleFps > 0.0)
                ImGui::TextDisabled("                       %.0f fps on "
                                    "skipped frames", idleFps);
            if (stride > 1)
                ImGui::TextDisabled("inferring on one arrival in %d", stride);
            ImGui::PopFont();

            if (cost > 8.0)
                ui::Notice(1, "Running the model is costing the application "
                              "in front about %.0f%% of its frame rate. "
                              "Adaptive load, below, gives some of that back "
                              "by inferring less often.", cost);
            else if (busyFps > 0.0 && idleFps > 0.0)
                ui::Hint("Running the model is costing the application in "
                         "front roughly %.0f%% of its frame rate.", cost);

            ui::Hint("Measured from how often the screen changes, so it is "
                     "the rate that application is presenting at. It cannot "
                     "read above the display refresh, and it means nothing "
                     "while the screen is still.");
        }

        // The comparison that answers "why is it slower when I tab in".
        {
            static std::vector<double> sf, sb;
            const auto fg = g_shared.timings.stage[ST_GPU_FG].summary(sf);
            const auto bg = g_shared.timings.stage[ST_GPU_BG].summary(sb);
            if (fg.n > 100 && bg.n > 100) {
                const double ratio = fg.p50 > 0.0 ? bg.p50 / fg.p50 : 1.0;
                if (ratio > 1.25) {
                    ui::Notice(1, "The same GPU work takes %.1f ms while "
                                  "another window is in front and %.1f ms "
                                  "while loopcore is. That is the graphics "
                                  "card being shared: the application in "
                                  "front is served first, and nothing here "
                                  "can outrank it. The fixes are to make the "
                                  "work smaller -- a smaller engine input, a "
                                  "smaller region -- or to leave the other "
                                  "application more headroom by capping its "
                                  "frame rate.",
                               bg.p50, fg.p50);
                } else if (bg.p50 > 0.0) {
                    ui::Hint("GPU work takes %.1f ms with another window in "
                             "front and %.1f ms with loopcore in front, so "
                             "the card is not being contended for.",
                             bg.p50, fg.p50);
                }
            }
        }

        // The cadence row is the one people misread. It is not a cost, it is
        // a ceiling: nothing downstream can be fresher than the rate frames
        // arrive at, and the desktop only presents when something changes.
        {
            static std::vector<double> scc;
            const auto cad2 = g_shared.timings.stage[ST_CADENCE].summary(scc);
            if (cad2.p50 > 0.0) {
                const double fps = 1000.0 / cad2.p50;
                static std::vector<double> sci;
                const auto inf2 = g_shared.timings.stage[ST_INFER].summary(sci);
                ui::Hint("Frames arrive at %.0f a second, so a detection is "
                         "already %.1f ms old before anything is done with "
                         "it. With %.1f ms of inference on top, the aim is "
                         "acting on roughly %.0f ms of history -- and no "
                         "amount of output rate changes that.",
                         fps, cad2.p50, inf2.p50, cad2.p50 + inf2.p50);

                if (fps < 70.0 && fps > 50.0)
                    ui::Notice(1, "Frames are arriving at almost exactly 60 a "
                                  "second, which is the desktop refresh rate "
                                  "rather than anything loopcore chose. "
                                  "Capture cannot go faster than the display "
                                  "presents: raising the monitor's refresh "
                                  "rate is the only thing that lowers this "
                                  "number.");
            }
        }
        if (ui::GhostButton("Reset", ImVec2(90, 0))) {
            g_shared.timings.reset();
            g_shared.counters.reset();
        }
        }});
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Drift", [&] {
        // The complaint "it got slower" is hard to see in a percentile that
        // covers the whole session, because the early good frames hold it
        // down. Comparing a short recent window against a baseline taken
        // once the run had settled makes it obvious.
        static std::vector<float> recent;
        static double baseline = 0.0;
        static int64_t baseAt = 0;
        static uint64_t baseFrames = 0;

        g_shared.timings.stage[ST_TOTAL].tail(recent, 240);
        double recentMed = 0.0;
        if (!recent.empty()) {
            std::vector<float> sorted(recent);
            std::sort(sorted.begin(), sorted.end());
            recentMed = sorted[sorted.size() / 2];
        }

        const uint64_t frames = g_shared.counters.frames.load();
        if (baseline <= 0.0 && frames > 600) {
            baseline = recentMed;
            baseAt = now_ns();
            baseFrames = frames;
        }

        ImGui::PushFont(ui::fontSmall);
        if (baseline > 0.0) {
            const double pct = 100.0 * (recentMed - baseline) / baseline;
            const double mins = (now_ns() - baseAt) / 6e10;
            ImGui::Text("baseline      %.2f ms", baseline);
            ImGui::Text("recent        %.2f ms", recentMed);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertU32ToFloat4(
                    pct > 25.0 ? ui::col::danger
                               : (pct > 10.0 ? ui::col::warn : ui::col::accent)));
            ImGui::Text("change        %+.0f%% over %.1f min", pct, mins);
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("collecting a baseline...");
        }
        ImGui::PopFont();

        if (ui::GhostButton("Re-baseline", ImVec2(120, 0))) {
            baseline = 0.0;
            baseFrames = 0;
        }
        ui::Hint("Recent median against a baseline taken once the run "
                 "settled. A large positive change with nothing altered "
                 "points at the card downclocking rather than at anything "
                 "here.");
        }});
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Loop time", [&] {
        // Sampled by wall clock, so the trace scrolls at a constant rate
        // whatever the frame rate happens to be doing.
        static std::vector<TimeSeries::Column> loopCols, gpuCols;
        const int nc = std::clamp((int)ImGui::GetContentRegionAvail().x, 80, 420);

        g_shared.traces.loop.sample(cfg.perfGraphSecs, nc, loopCols);
        ui::TraceGraph("##loop", loopCols, 92.0f, cfg.perfGraphSecs, "ms");
        ui::Hint("Total loop time. The band is the spread within each column "
                 "and the line is the mean, so a figure alternating between "
                 "one and eight looks different from one steady at four -- "
                 "which a single line through the mean cannot show.");

        g_shared.traces.gpu.sample(cfg.perfGraphSecs, nc, gpuCols);
        ui::TraceGraph("##gpu", gpuCols, 58.0f, cfg.perfGraphSecs, "ms", true);
        ui::Hint("Device time alone. A gap between this and the loop above is "
                 "the thread waiting rather than the model working.");

        if (ui::PrimaryButton("Copy timing report", ImVec2(200, 0)))
            g_pending.copyReport = true;
        ui::Hint("Copies every stage, the counters, the current detections "
                 "and the recent trace. Take one while holding the "
                 "activation key so the loaded case is captured.");
        }});
    cards.push_back({"Session log", [&] {
        ui::Toggle("Record this session", &cfg.sessionTracking,
            "Writes what the program was doing, not just how fast it was, "
            "into bin\\logs.\n\n"
            "The timing report answers how fast it is now. This answers the "
            "question that keeps coming up instead: how fast was it under "
            "what conditions. The same measurements are kept several times "
            "over -- split by whether the key was held, which window had "
            "focus, and how many targets were on screen -- so a difference "
            "between two of those blocks is the answer rather than the start "
            "of an investigation.");

        if (g_session.running()) {
            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("%.0f s engaged, mostly in %s",
                                g_session.engagedSeconds(),
                                g_session.dominantForeground().c_str());
            ImGui::PopFont();
            ui::Hint("Written when loopcore closes, so let it exit normally "
                     "rather than ending the process.");
        }

        if (ui::GhostButton("Open logs", ImVec2(110, 0)))
            g_pending.openFolder = store::logsDir();
        ImGui::SameLine();
        if (ui::GhostButton("Mark a moment", ImVec2(140, 0))) {
            g_session.note("marked by hand");
            ui::Toast("Marked in the session log.");
        }
        ui::Hint("Mark a moment writes a timestamped line, so something odd "
                 "can be found again in the log afterwards.");
        }});

    cards.push_back({"Resources", [&] {
        static int64_t lastPoll = 0;
        static double cpuPct = 0.0;
        static size_t workingSet = 0, privateBytes = 0;
        static size_t gpuUsed = 0, gpuTotal = 0;
        static ULONGLONG prevKernel = 0, prevUser = 0, prevWall = 0;
        static int handles = 0;
        static gpumon::Sample gs;
        static bool lowClockLatched = false;

        const int64_t nowNs = now_ns();
        if (nowNs - lastPoll > 500'000'000LL) {
            lastPoll = nowNs;

            FILETIME ct{}, et{}, kt{}, ut{};
            if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                const ULONGLONG k = ((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
                const ULONGLONG u = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
                FILETIME nf{};
                GetSystemTimeAsFileTime(&nf);
                const ULONGLONG w = ((ULONGLONG)nf.dwHighDateTime << 32) | nf.dwLowDateTime;
                if (prevWall) {
                    SYSTEM_INFO si{};
                    GetSystemInfo(&si);
                    const double dw = (double)(w - prevWall);
                    const double dc = (double)((k - prevKernel) + (u - prevUser));
                    if (dw > 0.0)
                        cpuPct = 100.0 * dc / dw / (double)si.dwNumberOfProcessors;
                }
                prevKernel = k; prevUser = u; prevWall = w;
            }

            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                                     (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                workingSet   = pmc.WorkingSetSize;
                privateBytes = pmc.PrivateUsage;
            }

            size_t freeB = 0, totalB = 0;
            if (cudaMemGetInfo(&freeB, &totalB) == cudaSuccess) {
                gpuUsed = totalB - freeB;
                gpuTotal = totalB;
            }

            DWORD hc = 0;
            if (GetProcessHandleCount(GetCurrentProcess(), &hc)) handles = (int)hc;

            gs = gpumon::Poll();

            // Latched with a gap between the thresholds, so a clock sitting
            // near one cannot flicker the card several times a second.
            if (gs.valid && gs.coreMaxMHz > 0) {
                const float frac = (float)gs.coreMHz / (float)gs.coreMaxMHz;
                if (!lowClockLatched && frac < 0.70f) lowClockLatched = true;
                else if (lowClockLatched && frac > 0.82f) lowClockLatched = false;
            }
        }

        ImGui::PushFont(ui::fontSmall);
        ImGui::Text("cpu           %.1f%% of all cores", cpuPct);
        ImGui::Text("working set   %.0f MB", workingSet / (1024.0 * 1024.0));
        ImGui::Text("private       %.0f MB", privateBytes / (1024.0 * 1024.0));
        if (gpuTotal)
            ImGui::Text("gpu memory    %.0f of %.0f MB, whole device",
                        gpuUsed / (1024.0 * 1024.0), gpuTotal / (1024.0 * 1024.0));
        ImGui::Text("handles       %d", handles);
        if (gs.valid) {
            ImGui::Text("gpu clock     %d of %d MHz", gs.coreMHz, gs.coreMaxMHz);
            ImGui::Text("gpu load      %d%%   memory %d%%",
                        gs.utilGpuPct, gs.utilMemPct);
            if (gs.fanPct >= 0)
                ImGui::Text("temperature   %d C   fan %d%%", gs.tempC, gs.fanPct);
            else
                ImGui::Text("temperature   %d C", gs.tempC);
            if (gs.powerLimitW > 0)
                ImGui::Text("power         %d of %d W", gs.powerW, gs.powerLimitW);
        }
        ImGui::PopFont();

        if (gs.valid && gs.throttled) {
            ui::Notice(1, "The card is throttling: %s.", gs.throttle.c_str());
        } else if (gs.valid && !cfg.keepGpuBoosted && lowClockLatched) {
            ui::Notice(1, "The card is at %d of %d MHz with %d%% load and no "
                          "throttling. It has clocked down because the work "
                          "arrives in short bursts, which makes each burst "
                          "slower. Keep model running, below, holds the "
                          "clocks up; the NVIDIA power mode setting does the "
                          "same thing permanently and more reliably.",
                       gs.coreMHz, gs.coreMaxMHz, gs.utilGpuPct);
        }

        ImGui::Dummy(ImVec2(0, 4));
        ui::Toggle("Adaptive load", &cfg.adaptiveLoad,
            "Infers less often when the application in front is struggling."
            "\n\n"
            "Doing less work is the only thing that actually returns "
            "performance: a priority change moves work around rather than "
            "removing any. Below the floor the model runs on every second or "
            "third arrival instead of every one. Detections then arrive less "
            "often, which the controller already copes with because it "
            "interpolates between them -- but a fast-moving target is "
            "tracked from staler information, so this trades aim quality for "
            "frame rate rather than getting both.");
        if (cfg.adaptiveLoad) {
            ui::SliderInt("Protect", &cfg.fpsFloor, 30, 360, 5, "%d fps",
                "The frame rate to defend. Above this nothing changes; below "
                "it, inference is thinned until the rate recovers.");
            ui::SliderInt("Skip at most", &cfg.maxStride, 2, 6, 1, "one in %d",
                "How far the thinning may go. At one in three the model sees "
                "a third of the frames, so detections are three times as far "
                "apart and prediction is doing more of the work.");
        }

        ui::Toggle("Sleep while waiting for the GPU", &cfg.yieldWhileWaiting,
            "How the thread waits for a frame to finish inferring.\n\n"
            "CUDA spins by default: the thread busy-waits until the GPU is "
            "done. That is the fastest choice on an idle machine and the "
            "wrong one here, because waiting seven milliseconds a frame at "
            "two hundred frames a second pins a whole core doing nothing -- "
            "taken from the game running alongside it.\n\n"
            "Sleeping costs tens of microseconds of wakeup instead. Turn it "
            "off only if nothing else is running and you want the last "
            "fraction of a millisecond.\n\n"
            "Takes effect on the next launch: the choice has to be made "
            "before the graphics context is created.");

        ui::Toggle("Keep model running", &cfg.keepGpuBoosted,
            "Runs the model when nothing needs the result, purely to stop the "
            "card lowering its clocks.\n\n"
            "Worth knowing before turning it on: holding the clocks up means "
            "giving the card real work, and the rate that achieves it settles "
            "at roughly half the card's time spent on inferences nobody asked "
            "for. That competes with whatever is in front, which is the same "
            "cost it was meant to avoid. Setting the NVIDIA power mode to "
            "prefer maximum performance holds the same clocks for nothing, "
            "and is the better answer wherever it is available.\n\n"
            "This looks wasteful and often is not. A card fed short bursts of "
            "work never raises its clocks, so each burst runs at a fraction of "
            "the boost speed. Holding it busy keeps the clocks up and each "
            "inference finishes sooner. The clock reading above is the thing "
            "to watch: turn this off and see whether it falls.");
        ui::Hint("GPU memory is the device total, not this process alone: "
                 "CUDA offers no per-process figure.");
        }});

    cards.push_back({"Counters", [&] {
        const uint64_t arr = g_shared.counters.arrivals.load();
        const uint64_t frm = g_shared.counters.frames.load();
        const uint64_t skp = g_shared.counters.skipped.load();
        const uint64_t det = g_shared.counters.detections.load();
        const uint64_t emp = g_shared.counters.empty.load();

        ImGui::PushFont(ui::fontSmall);
        ImGui::Text("arrived     %llu", (unsigned long long)arr);
        ImGui::Text("processed   %llu", (unsigned long long)frm);
        ImGui::Text("dropped     %llu  (%.1f%%)",
                    (unsigned long long)skp, arr ? 100.0 * skp / arr : 0.0);
        ImGui::Text("boxes       %llu  (%.2f per frame)",
                    (unsigned long long)det, frm ? (double)det / frm : 0.0);
        ImGui::Text("empty       %llu  (%.1f%%)",
                    (unsigned long long)emp, frm ? 100.0 * emp / frm : 0.0);
        ImGui::Text("idle skips  %llu",
                    (unsigned long long)g_shared.counters.idle.load());
        ImGui::Text("errors      %llu",
                    (unsigned long long)g_shared.counters.errors.load());
        ImGui::PopFont();

        if (arr > 0 && (double)skp / arr > 0.05)
            ui::Notice(1, "Frames are being dropped because the loop is "
                          "slower than the source is presenting. Lower the "
                          "field of view, or rebuild at a smaller input size.");
        }});
    AdvancedOnly(cards, cfg.advancedMode);
    cards.push_back({"Log", [&] {
        if (ui::GhostButton("Copy", ImVec2(80, 0))) {
            std::string all;
            all.reserve(16384);
            all += "loopcore log\n";
            if (g_engine.ready()) {
                all += "engine: " + g_engine.describe() + "\n";
                for (const auto& line : g_engine.tensorReport())
                    all += "  " + line + "\n";
                const auto d = g_engine.diag();
                char b[192];
                snprintf(b, sizeof(b), "diag: %d candidates, %d kept, best %.3f\n",
                         d.rawCandidates, d.kept, d.maxScore);
                all += b;
            }
            if (g_gpu.valid)
                all += "gpu: " + g_gpu.name + " (" + g_gpu.archName() + ")\n";
            all += "---\n";
            char line[1200];
            for (const auto& e : g_shared.log.snapshot()) {
                snprintf(line, sizeof(line), "%8.2f  %s  %s\n", e.t,
                         e.level == LOG_ERROR ? "ERR " :
                         e.level == LOG_WARN  ? "WARN" : "info", e.text.c_str());
                all += line;
            }
            ImGui::SetClipboardText(all.c_str());
            ui::Toast("Log copied to the clipboard.");
        }
        ImGui::SameLine();
        if (ui::GhostButton("Clear", ImVec2(80, 0))) g_shared.log.clear();
        ImGui::SameLine();
        if (ui::GhostButton("Folder", ImVec2(88, 0)))
            g_pending.openFolder = store::binDir();

        ImGui::BeginChild("log", ImVec2(0, 300), LC_CHILD_BORDER);
        ImGui::PushFont(ui::fontSmall);
        for (const auto& e : g_shared.log.snapshot()) {
            const ImU32 c = e.level == LOG_ERROR ? ui::col::danger
                          : e.level == LOG_WARN  ? ui::col::warn
                                                 : ui::col::textDim;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(c));
            ImGui::TextWrapped("%7.2f  %s", e.t, e.text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        }});
    cards.push_back({"Hardware", [&] {
        if (g_gpu.valid) {
            ImGui::TextUnformatted(g_gpu.name.c_str());
            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("%s   compute %d.%d   %d SMs",
                                g_gpu.archName().c_str(), g_gpu.major,
                                g_gpu.minor, g_gpu.smCount);
            ImGui::TextDisabled("%s total, %s free",
                                g_gpu.totalText().c_str(), g_gpu.freeText().c_str());
            ImGui::PopFont();
        } else {
            ui::Notice(2, "No CUDA device: %s", g_gpu.error.c_str());
        }
        }});
    cards.push_back({"Folders", [&] {
        if (ui::GhostButton("Models", ImVec2(96, 0)))
            g_pending.openFolder = store::modelsDir();
        ImGui::SameLine();
        if (ui::GhostButton("Sources", ImVec2(96, 0)))
            g_pending.openFolder = store::convertedDir();
        if (ui::GhostButton("Config", ImVec2(96, 0)))
            g_pending.openFolder = store::configDir();
        ImGui::SameLine();
        if (ui::GhostButton("Logs", ImVec2(96, 0)))
            g_pending.openFolder = store::binDir();
        ui::Hint("Everything loopcore keeps lives beside the exe, in bin.");
        }});
    cards.push_back({"Environment", [&] {
        if (g_engine.ready()) {
            // The model name comes from the file; everything else is
            // inferred from the engine itself, since a serialised engine no
            // longer knows what it was built from.
            const char* leaf = strrchr(g_modelPath, '\\');
            ImGui::TextUnformatted(leaf ? leaf + 1 : g_modelPath);

            static std::vector<double> sc;
            const auto inf = g_shared.timings.stage[ST_INFER].summary(sc);
            const auto cx = g_engine.complexity(inf.p50);

            ImGui::PushFont(ui::fontSmall);
            ImGui::TextDisabled("%s", g_engine.describe().c_str());
            ImGui::TextDisabled("%s class model   %.1f MB engine",
                                cx.tier.c_str(), cx.engineBytes / (1024.0 * 1024.0));
            ImGui::TextDisabled("%.2f MP input   %d anchors   %d classes",
                                cx.megapixels, cx.anchors,
                                cx.classes > 0 ? cx.classes : (int)std::bitset<32>(
                                    g_engine.seenClasses()).count());
            ImGui::TextDisabled("~%.0f GFLOP per frame, order of magnitude",
                                cx.gflopsEst);
            ImGui::PopFont();

            if (!cx.note.empty()) ui::Hint("%s", cx.note.c_str());
            ui::Hint("Size and anchor count are read off the engine. A "
                     "serialised engine no longer knows its parameter count, "
                     "so the tier is inferred from its weight footprint and "
                     "the GFLOP figure is a rough guide only. The inference "
                     "time beside it is the measured number.");
        } else {
            ui::Hint("No model loaded.");
        }

        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushFont(ui::fontSmall);
        ImGui::TextDisabled("TensorRT %d.%d", NV_TENSORRT_MAJOR, NV_TENSORRT_MINOR);
        ImGui::TextDisabled("Dear ImGui %s", IMGUI_VERSION);
        // Three sources, shown together because when they disagree the
        // disagreement is the diagnosis.
        ImGui::TextDisabled("capture reports   %d x %d",
                            g_capture.detectedWidth(), g_capture.detectedHeight());
        {
            HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{sizeof(MONITORINFO)};
            if (GetMonitorInfoW(mon, &mi))
                ImGui::TextDisabled("windows reports   %d x %d",
                                    (int)(mi.rcMonitor.right - mi.rcMonitor.left),
                                    (int)(mi.rcMonitor.bottom - mi.rcMonitor.top));
        }
        ImGui::TextDisabled("in use            %d x %d",
                            g_capture.screenWidth(), g_capture.screenHeight());
        ImGui::PopFont();

        ImGui::PushFont(ui::fontSmall);
        ImGui::PopFont();
        }});

    RenderCards(4, cfg, cards);
}

// ------------------------------------------------------------------ help

static void HelpSection(const char* title, const char* body) {
    ui::SectionHeader(title);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::ColorConvertU32ToFloat4(ui::col::textDim));
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
}

static void HelpWindow() {
    if (!g_showHelp) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));

    if (ImGui::Begin("How loopcore works", &g_showHelp,
                     ImGuiWindowFlags_NoCollapse)) {

        HelpSection("WHAT THIS DOES",
            "Captures a square region of the screen the moment Windows "
            "presents a new frame, crops it on the GPU, runs a TensorRT "
            "detection model over it, and reports where things are. Nothing "
            "is copied back to the CPU except the final list of boxes.");

        HelpSection("FIELD OF VIEW",
            "The size of the captured square. Compute scales with area, not "
            "width, so going from 640 to 320 is about four times less work, "
            "not two. Anchor it to the screen centre or let it follow the "
            "mouse, and nudge it with the percentage offsets.");

        HelpSection("MODELS",
            "Three formats. A .engine loads immediately. An .onnx is compiled "
            "here into an engine. A .pt is exported to .onnx by ultralytics "
            "first, which needs Python and ultralytics installed. Compiling "
            "takes minutes and runs in the background.\n\n"
            "Engines are tied to one GPU model and one TensorRT version. One "
            "built on another machine will not load.");

        HelpSection("THE BIN FOLDER",
            "bin\\models holds engines you can load. bin\\converted keeps the "
            ".pt and .onnx files they were built from. bin\\config holds "
            "profiles. Everything is copied in, so the library still works "
            "after you move or delete the original.");

        HelpSection("PROFILES",
            "Save every setting under a name, then load it back later. A "
            "profile also records which model was in use and loads it. "
            "Settings alone are restored automatically on start; the model is "
            "not, so launching never spends time compiling or loading "
            "something you did not ask for.");

        HelpSection("FP16 AND WORKSPACE",
            "FP16 does the arithmetic in 16-bit floats instead of 32-bit. On "
            "a card with tensor cores that is roughly twice the throughput "
            "for no meaningful accuracy loss. Workspace is scratch memory "
            "TensorRT may use while choosing kernels; it is not held at "
            "runtime. The Model tab suggests values for your card.");

        HelpSection("READING THE DEBUG TAB",
            "Watch p99, not the mean: a steady 4 ms drives better than a 2 ms "
            "average with 18 ms spikes. Dropped frames mean the loop is "
            "slower than the source is presenting, so it is falling behind. "
            "Frame cadence tells you the rate frames are actually arriving.");

        HelpSection("IF THINGS FEEL SLOW",
            "Lower the field of view first, then rebuild the engine at a "
            "smaller input size, then enable FP16. Match the engine size to "
            "the field of view so no compute is spent on padding. Leave "
            "process priority on Normal unless you are measuring.");

        HelpSection("WHERE THE LOGS ARE",
            "bin\\logs\\loopcore.log has everything from this session, written as "
            "it happens. If the app crashes, bin\\crash.log records where. "
            "Both are plain text.");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------ draw

static void DrawUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar);

    TitleBar(vp->WorkSize.x);

    // Draw against a copy. The lock is only held for the read and the write,
    // never across the UI, so nothing the UI calls can deadlock against it.
    Config cfg = g_shared.snapshotConfig();
    ui::showHints = cfg.tooltipsOn;
    static int appliedTheme = -1;
    if (appliedTheme != cfg.uiTheme) { ui::ApplyTheme(cfg.uiTheme); appliedTheme = cfg.uiTheme; }

    ImGui::SetCursorPosX(16);
    const float contentTop = ImGui::GetCursorPosY() + 8.0f;
    ImGui::SetCursorPosY(contentTop);
    // The scrollbar is always present, even when there is nothing to scroll.
    //
    // Left to appear on demand it forms a loop with the layout: content just
    // taller than the panel raises a scrollbar, the scrollbar takes width
    // away, narrower cards wrap more text and grow taller -- or, coming from
    // the other side, content that just fits drops the scrollbar, the cards
    // widen, the content shrinks, and the scrollbar is dropped again. Sitting
    // near that threshold it flips every frame, and every flip moves every
    // card below it. The springs are then being driven at the frame rate,
    // which is why the shifting grew rather than settling, and why resizing
    // the window stopped it: that moves the layout off the threshold.
    //
    // Reserving the space unconditionally costs a strip of empty pixels on
    // short tabs and removes the feedback path entirely.
    ImGui::BeginChild("##content",
                      ImVec2(ImGui::GetWindowWidth() - 32,
                             ImGui::GetWindowHeight() - contentTop - 12),
                      0,
                      ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // The content slides in from the side the new tab lies on and fades up.
    //
    // Switching instantly leaves no sense of where the previous page went,
    // and every card animating into place from nothing at once reads as a
    // glitch rather than a transition. Moving the whole page as one, in the
    // direction of travel, says what happened.
    {
        static int lastTab = -1;
        static float slide = 0.0f;      // 1 at the start of a switch, 0 at rest
        static float slideDir = 1.0f;

        if (lastTab != g_tab) {
            slideDir = (lastTab >= 0 && g_tab < lastTab) ? -1.0f : 1.0f;
            if (lastTab >= 0) slide = 1.0f;
            lastTab = g_tab;

            // Card positions are deliberately left alone.
            //
            // They were being reset on every switch, which discards the
            // layout each card had already settled into -- so returning to a
            // tab re-ran the whole arrangement from scratch and every card
            // visibly slid into place again. Their positions do not depend
            // on which tab is showing, so there was nothing to reset: the
            // page slide is the transition, and the cards should simply be
            // where they were.
        }

        const float dt2 = ImGui::GetIO().DeltaTime;
        slide = std::max(0.0f, slide - dt2 * 5.0f);

        // Cubic ease-out: most of the distance covered early, so it feels
        // quick without arriving abruptly.
        const float e = 1.0f - slide;
        const float eased = 1.0f - (1.0f - e) * (1.0f - e) * (1.0f - e);
        const float offset = slideDir * (1.0f - eased) * 34.0f;

        if (slide > 0.0f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                ImGui::GetStyle().Alpha * std::max(0.15f, eased));
        }

        switch (g_tab) {
        case 0:  TabGeneral(cfg);  break;
        case 1:  TabVisual(cfg);   break;
        case 2:  TabModel(cfg);    break;
        case 3:  TabSettings(cfg); break;
        case 4:  TabCapture(cfg);  break;
        // Guarded rather than left as a fallthrough: an out-of-range index
        // would otherwise draw Debug, which is the one tab that may not
        // exist.
        default:
            if (cfg.advancedMode) TabDebug(cfg);
            else                  TabGeneral(cfg);
            break;
        }

        if (slide > 0.0f) ImGui::PopStyleVar();
    }

    ImGui::EndChild();
    ImGui::End();

    HelpWindow();

    // Applied here rather than in the message handler: rebuilding the font
    // atlas mid-frame would invalidate textures already referenced.
    {
        const float want = g_pendingDpiScale.exchange(0.0f);
        if (want > 0.0f) {
            ui::SetUiScale(want);
            ImGui_ImplDX11_InvalidateDeviceObjects();
        }
    }

    ui::DrawSplash();

    // Refreshed on its own timer, so no card body ever touches the disk.
    RefreshDiskCache();

    // The overlay is sized and positioned from the screen dimensions, so it
    // has to be rebuilt when those change rather than left describing a
    // desktop that no longer exists.
    if (g_capture.takeSizeChanged()) {
        g_overlay.destroy();
        g_overlay.create(GetModuleHandleW(nullptr), 0, 0,
                         g_capture.screenWidth(), g_capture.screenHeight(),
                         g_shared.log);
        // Re-placed too: its edge position is a fraction of a screen that is
        // now a different size.
        g_perf.snapToEdge(g_perf.edge(), g_perf.edgeT(), false);
        ui::Toast("Display size changed; overlay rebuilt.");
    }

    // Whatever the board said, logged from here rather than from the
    // control thread, where taking the log's mutex was never appropriate.
    for (const auto& line : g_control.takeBoardLines()) {
        // A report line feeds the sketcher rather than the log while one is
        // running: at the rate sketch mode sends them, logging every line
        // would bury everything else and achieve nothing.
        if (g_sketch.running() && line.rfind("#rpt ", 0) == 0) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                uint8_t bytes[32];
                int n = 0;
                const char* p2 = line.c_str() + colon + 1;
                while (*p2 && n < 32) {
                    while (*p2 == ' ') ++p2;
                    if (!isxdigit((unsigned char)*p2)) break;
                    bytes[n++] = (uint8_t)strtol(std::string(p2, 2).c_str(),
                                                 nullptr, 16);
                    p2 += 2;
                }
                if (n > 0) g_sketch.feed(bytes, n);
            }
            continue;
        }
        g_shared.log.info("board: " + line);
    }

    if (g_control.takeDisabledWarning())
        ui::Toast("The AI is switched off. Enable it on the General tab.", 3.0);
    ui::DrawToast();

    {
        std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
        g_shared.cfg = cfg;
    }
    // A device learned by clicking lives in raw input; mirror it into the
    // config so it survives a restart.
    {
        const std::string pref = rawin::Preferred();
        if (!pref.empty() && pref != cfg.boardDevice)
            strncpy_s(cfg.boardDevice, sizeof(cfg.boardDevice), pref.c_str(), _TRUNCATE);
    }

    RunPending(cfg);
    {
        std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
        g_shared.cfg = cfg;     // pending actions may have changed it
    }

    // Opening a serial port can block, so it happens here on the UI thread
    // rather than on the capture callback. Cheap when nothing changed.
    //
    // Opened whenever Arduino output is selected, not only when control is
    // enabled: the board reports back on the same line, and none of that is
    // visible if the port is only opened once output starts.
    // Not while a flash is running: arduino-cli and avrdude both need the
    // port, and reopening it here right after releaseOutput() is what made
    // the first upload attempt fail with "Access is denied".
    if (!g_flasher.busy() &&
        (cfg.controlEnabled || cfg.inputMethod == InputArduino ||
         cfg.inputMethod == InputMakcu))
        g_control.configure(cfg, g_shared.log);
}

// ------------------------------------------------------------------ main

// Establishes the strongest DPI awareness this Windows offers.
//
// This matters more here than in most programs. Without it Windows lies
// about coordinates: the cursor position, monitor rectangles and window
// placement all come back in scaled units while the captured frame is in
// real pixels. The region would then sit somewhere other than where the
// cursor is, by exactly the scaling factor, and the overlay would be drawn
// in the wrong place at the wrong size.
//
// Declared before any window exists, because it cannot be changed afterwards.
static const char* EstablishDpiAwareness() {
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        using PFN_Ctx = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set = (PFN_Ctx)GetProcAddress(user32,
                                               "SetProcessDpiAwarenessContext")) {
            if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                return "per-monitor v2";
            if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                return "per-monitor";
        }
    }
    // Older Windows. System awareness still gives real pixels on the primary
    // display, which is the case that matters.
    if (HMODULE shcore = LoadLibraryA("shcore.dll")) {
        using PFN_Aw = HRESULT (WINAPI*)(int);
        if (auto set = (PFN_Aw)GetProcAddress(shcore, "SetProcessDpiAwareness")) {
            if (SUCCEEDED(set(2))) return "per-monitor, legacy";
            if (SUCCEEDED(set(1))) return "system";
        }
    }
    if (SetProcessDPIAware()) return "system, legacy";
    return "none";
}

// The scale a window should draw itself at, where 1.0 is 96 dpi.
static float ScaleForWindow(HWND hwnd) {
    UINT dpi = 96;
    if (HMODULE user32 = GetModuleHandleA("user32.dll")) {
        using PFN_Dpi = UINT (WINAPI*)(HWND);
        if (auto get = (PFN_Dpi)GetProcAddress(user32, "GetDpiForWindow"))
            dpi = get(hwnd);
    }
    if (dpi == 0) dpi = 96;
    return (float)dpi / 96.0f;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    const char* dpiMode = EstablishDpiAwareness();
    timeBeginPeriod(1);
    // Deliberately not HIGH_PRIORITY_CLASS. See ApplyPriority.
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);

    HICON iconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_LOOPCORE),
                                      IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    HICON iconSmall = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_LOOPCORE),
                                        IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON),
                                        GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0,
                   inst, iconBig, LoadCursor(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"loopcore", iconSmall};
    RegisterClassExW(&wc);

    // Sized in scaled pixels. Under per-monitor awareness these are real
    // ones, so a fixed size would come out physically smaller the higher the
    // display scale is set.
    float bootScale = 1.0f;
    {
        POINT origin{140, 140};
        HMONITOR mon = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        UINT dx = 96, dy = 96;
        if (HMODULE shcore = LoadLibraryA("shcore.dll")) {
            using PFN_Mon = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (auto get = (PFN_Mon)GetProcAddress(shcore, "GetDpiForMonitor"))
                get(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy);
        }
        if (dx) bootScale = std::clamp((float)dx / 96.0f, 1.0f, 3.0f);
    }

    // Settings are read before the window exists, because its position and
    // size are among them. The model is deliberately not restored: launching
    // should not silently spend a minute loading something that was not
    // asked for. A profile load is the explicit request that does.
    // How the thread waits for the GPU.
    //
    // By default CUDA spins: the thread that calls a synchronise busy-waits
    // until the work finishes. That is the lowest-latency choice on an idle
    // machine and the wrong one here, because the whole point is to run
    // alongside a game. Waiting seven milliseconds a frame at two hundred
    // frames a second means a core pinned at a hundred per cent doing
    // nothing, taken from the eight the game is sharing.
    //
    // Blocking sync sleeps instead and is woken by the driver. It costs tens
    // of microseconds of wakeup against milliseconds of spin.
    //
    // Set before any CUDA context exists, because the flags apply to the
    // context created afterwards and are ignored once one is current.
    // Read straight from the file rather than from the loaded config.
    //
    // This has to happen before anything creates a CUDA context, and the
    // config is not loaded until after that point -- so asking the config
    // object here would always see the default and the setting would never
    // do anything. One small read is the price of getting the ordering right.
    {
        bool yieldWait = true;
        if (FILE* cf = nullptr; fopen_s(&cf, store::lastSessionPath().c_str(),
                                        "rb") == 0 && cf) {
            char line[256];
            while (fgets(line, sizeof(line), cf)) {
                if (strncmp(line, "yieldWhileWaiting=", 18) == 0) {
                    yieldWait = (atoi(line + 18) != 0);
                    break;
                }
            }
            fclose(cf);
        }
        cudaSetDeviceFlags(yieldWait ? cudaDeviceScheduleBlockingSync
                                     : cudaDeviceScheduleSpin);
    }


    {
        std::string model;
        std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
        if (store::loadConfig(store::lastSessionPath(), g_shared.cfg, model,
                              g_shared.log)) {
            if (!model.empty())
                strncpy_s(g_modelPath, sizeof(g_modelPath), model.c_str(), _TRUNCATE);
        } else {
            // First launch: the card's own recommendation, not a fixed guess.
            //
            // This ran before hw::Recommend had been called, so the advice it
            // copied was a default-constructed zero and the workspace came up
            // empty on a fresh install. The query is cheap and has no
            // dependencies, so it happens here rather than being ordered
            // around later.
            if (!g_gpu.valid) {
                g_gpu    = hw::Query();
                g_advice = hw::Recommend(g_gpu);
            }
            g_shared.cfg.buildFp16   = g_advice.fp16;
            g_shared.cfg.workspaceMB = g_advice.workspaceMB;
            g_shared.log.info("First launch: workspace set to " +
                              std::to_string(g_advice.workspaceMB) +
                              " MB for this card.");
        }
    }

    // The engine from last session, loaded rather than merely remembered.
    //
    // The path was restored but nothing was loaded, so the library showed the
    // model highlighted with a Reload button while the status read IDLE. Both
    // were telling the truth and they contradicted each other, which is worse
    // than either being wrong.
    //
    // The reasoning against auto-loading was that startup should not spend a
    // minute on something nobody asked for -- but that is the cost of
    // *building* an engine. Loading a built one is a fraction of a second,
    // and it is what someone who left a model selected expects to come back
    // to. Only an .engine is loaded here; anything needing a build is left
    // alone, which is the case the original concern was actually about.
    if (g_modelPath[0]) {
        const std::string mp(g_modelPath);
        const bool isEngine =
            mp.size() > 7 &&
            _stricmp(mp.c_str() + mp.size() - 7, ".engine") == 0;
        const DWORD att = GetFileAttributesA(mp.c_str());
        const bool present = (att != INVALID_FILE_ATTRIBUTES) &&
                             !(att & FILE_ATTRIBUTE_DIRECTORY);
        if (isEngine && present) {
            g_pendingStartupLoad = mp;
        } else if (!isEngine) {
            // Remembered, not loaded: say so rather than leaving the library
            // looking as though it is running.
            g_shared.log.info("The remembered model needs building, so it has "
                              "not been loaded. Press Re-Convert to build it.");
        }
    }

    // Where it was last time, if that is still somewhere it can be seen.
    int wx = 140, wy = 140;
    int ww = (int)(705 * bootScale), wh = (int)(585 * bootScale);
    bool wantMax = false;
    {
        const Config c0 = g_shared.snapshotConfig();
        if (c0.windowW > 0 && c0.windowH > 0) {
            // Re-scaled by the ratio between the scaling it was saved at and
            // the scaling now, so a window moved between a 100% and a 150%
            // monitor comes back the same apparent size rather than the same
            // pixel count.
            const float ratio = (c0.windowScale > 0.1f)
                              ? bootScale / c0.windowScale : 1.0f;
            ww = (int)(c0.windowW * ratio);
            wh = (int)(c0.windowH * ratio);
            wx = c0.windowX;
            wy = c0.windowY;
            wantMax = c0.windowMaximised;

            // A saved position is only used if a monitor still covers it.
            // Displays get unplugged and rearranged, and restoring onto one
            // that is gone puts the window somewhere unreachable.
            const RECT probe{wx, wy, wx + ww, wy + wh};
            if (!MonitorFromRect(&probe, MONITOR_DEFAULTTONULL)) {
                wx = 140;
                wy = 140;
            }
        }
        g_tab = std::clamp(c0.activeTab, 0, 5);
    }

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"loopcore",
                             WS_OVERLAPPEDWINDOW,
                             wx, wy, ww, wh, nullptr, nullptr, inst, nullptr);
    chrome::Initialise(g_hwnd);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr,
                    L"Could not create a Direct3D 11 device. Update the GPU driver.",
                    L"loopcore", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_hwnd, wantMax ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::ApplyTheme(0);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    // Now that there is a context, size the panel for the display it opened
    // on. Fonts are rasterised at the final size rather than stretched.
    ui::SetUiScale(ScaleForWindow(g_hwnd));

    {
        char m[160];
        snprintf(m, sizeof(m),
                 "DPI awareness: %s. Display scale %.0f%%.",
                 dpiMode, ScaleForWindow(g_hwnd) * 100.0f);
        g_shared.log.info(m);
        if (std::string(dpiMode) == "none")
            g_shared.log.warn("Could not become DPI aware. If Windows display "
                              "scaling is not 100%, the region will not line "
                              "up with the cursor.");
    }

    // The board chosen last time, restored before any input is read.
    //
    // The device was saved and shown in the config, but nothing ever handed
    // it back to raw input at startup -- so the filter began each session
    // with no preference, the card read "no device has been chosen", and a
    // setting the user had made looked like it had been forgotten.
    {
        const Config c1 = g_shared.snapshotConfig();
        if (c1.boardDevice[0]) rawin::SetPreferred(c1.boardDevice);
    }

    // If arduino-cli is installed anywhere sensible, take a copy now so the
    // Firmware card is usable without the user hunting for it. Doing this
    // once at startup rather than from a button keeps a filesystem search
    // off the draw path.
    AdoptArduinoCli(g_shared.log);

    if (!rawin::Init(g_hwnd))
        g_shared.log.warn("Raw input registration failed, so activation "
                          "clicks cannot be attributed to a specific mouse.");

    // Created before anything asks to write into them. ensureDirs makes
    // logs alongside the rest, and a stream onto a missing folder fails
    // silently rather than complaining.
    store::ensureDirs(g_shared.log);
    // Everything written for later reading lives in one place.
    g_shared.log.setFile(store::logsDir() + "\\loopcore.log");
    InstallCrashHandler(store::logsDir());
    g_shared.log.info("loopcore started.");

    gpumon::Init();
    g_gpu    = hw::Query();
    g_advice = hw::Recommend(g_gpu);
    if (g_gpu.valid)
        g_shared.log.info("GPU: " + g_gpu.name + " (" + g_gpu.archName() + ", " +
                          g_gpu.totalText() + ")");

    // Anything that had to wait for a window to exist.
    {
        const Config c1 = g_shared.snapshotConfig();
        if (c1.alwaysOnTop)
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE);
        ApplyPriority(c1.priorityMode);
    }
    RefreshLibrary();
    RefreshProfiles();
    RefreshPorts();
    {
        std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
        if (g_shared.cfg.serialPort <= 0) {
            const int found = AutoPickPort(g_ports);
            if (found > 0) {
                g_shared.cfg.serialPort = found;
                g_shared.log.info("Auto-selected COM" + std::to_string(found) +
                                  " for serial output.");
            }
        }
    }

    // The configured display, not whichever enumerates first.
    //
    // This was hardcoded to zero, so the setting was ignored at launch and
    // capture landed on whatever monitor happened to come back first --
    // often the secondary. Choosing one by hand afterwards was the only
    // thing that made it right, which is exactly what was reported.
    if (!g_capture.start(ResolveDisplay(g_shared.snapshotConfig().captureMonitor),
                         g_shared, OnFrame))
        g_shared.log.error("Capture did not start. The panel still works; fix "
                           "the issue above and restart.");
    else
        g_overlay.create(inst, 0, 0, g_capture.screenWidth(),
                         g_capture.screenHeight(), g_shared.log);
    g_perf.create(inst, g_shared.log);
    // The readout draws the same trace the Debug tab does.
    g_perf.setTrace(&g_shared.traces.loop);
    {
        Config c0 = g_shared.snapshotConfig();
        g_perf.snapToEdge(c0.perfMonEdge, c0.perfMonEdgeT, false);
        ui::ApplyTheme(c0.uiTheme);
        ui::SetUiStyle(c0.uiStyle);
        if (g_engine.ready() && c0.outputLayout != LayoutAuto) {
            std::lock_guard<std::mutex> el(g_engineMutex);
            g_engine.setLayoutOverride(c0.outputLayout, g_shared.log);
        }
    }

    bool done = false;
    int64_t lastOverlay = 0;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // The stride, adjusted from what the foreground application is
        // managing.
        //
        // Raised one step at a time and lowered the same way, with a gap
        // between the two thresholds. Reacting instantly to a single slow
        // frame would oscillate between striding and not, which costs more
        // than either state.
        {
            Config lc2 = g_shared.snapshotConfig();
            static int64_t lastAdj = 0;
            const int64_t nowL = now_ns();
            if (nowL - lastAdj > 700'000'000LL) {
                lastAdj = nowL;
                if (!lc2.adaptiveLoad) {
                    g_shared.load.stride = 1;
                } else {
                    const double fps = g_shared.load.busyFps();
                    int stride = g_shared.load.stride.load();
                    if (fps > 1.0) {
                        const double floorF = (double)std::max(15, lc2.fpsFloor);
                        if (fps < floorF * 0.95)
                            stride = std::min(std::max(1, lc2.maxStride), stride + 1);
                        else if (fps > floorF * 1.15)
                            stride = std::max(1, stride - 1);
                    }
                    g_shared.load.stride = stride;
                }
            }
        }

        // The model follows control, and is not separately switchable.
        //
        // Running the model with control off spends the card on a result
        // nobody reads, and control with the model off cannot do anything at
        // all -- so the two were never independent in any useful way. One
        // switch instead of two removes a state that could only ever be
        // wrong, and the greyed-out Control card that came with it.
        //
        // The tuner is the exception: it needs detections while control is
        // off, and asks for them directly.
        {
            std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
            g_shared.cfg.aiEnabled =
                g_shared.cfg.controlEnabled || Tuner::WantsInference();
        }

        // Conditions recorded alongside timings, a few times a second.
        //
        // The cost is a handful of running means; the value is being able to
        // ask afterwards whether a slowdown tracked the key, the focus or
        // the number of targets, which a single set of percentiles cannot
        // answer because all three are averaged into the same numbers.
        {
            static int64_t lastTrack = 0;
            const int64_t nowT = now_ns();
            const Config tc = g_shared.snapshotConfig();

            if (tc.sessionTracking && !g_session.running()) {
                g_session.start(tc, g_shared.log);
            } else if (!tc.sessionTracking && g_session.running()) {
                // The toggle only ever started it.
                //
                // Switching it off left the recorder running for the rest of
                // the session -- still sampling, still writing the file every
                // ten seconds -- so the setting appeared to do nothing and
                // the log kept growing. Stopping here also writes the file
                // out, which is the right moment: what was recorded up to the
                // point the user stopped is worth keeping.
                g_session.stop(tc, g_shared.log);
            }

            if (g_session.running() && nowT - lastTrack > 200'000'000LL) {
                lastTrack = nowT;
                static std::vector<double> sa, sb, sc3, sd;
                SessionSample ss;
                ss.loopMs    = g_shared.timings.stage[ST_TOTAL].summary(sa).p50;
                ss.gpuMs     = g_shared.timings.stage[ST_GPU].summary(sb).p50;
                ss.cadenceMs = g_shared.timings.stage[ST_CADENCE].summary(sc3).p50;
                ss.inferMs   = g_shared.timings.stage[ST_INFER].summary(sd).p50;
                static std::vector<double> spre;
                ss.preMs     = g_shared.timings.stage[ST_PRE].summary(spre).p50;
                static std::vector<double> sarr, scop, smap, spos, spub, sctl;
                ss.arriveMs  = g_shared.timings.stage[ST_ARRIVE].summary(sarr).p50;
                ss.copyMs    = g_shared.timings.stage[ST_COPY].summary(scop).p50;
                ss.mapMs     = g_shared.timings.stage[ST_MAP].summary(smap).p50;
                ss.postMs    = g_shared.timings.stage[ST_POST].summary(spos).p50;
                ss.publishMs = g_shared.timings.stage[ST_PUBLISH].summary(spub).p50;
                ss.controlMs = g_shared.timings.stage[ST_CONTROL].summary(sctl).p50;

                const auto cst = g_control.state();
                ss.engaged   = cst.engaged;
                ss.hasTarget = cst.hasTarget;
                ss.errPx     = std::sqrt(cst.errX * cst.errX +
                                         cst.errY * cst.errY);
                ss.leadPx    = std::sqrt(cst.leadX * cst.leadX +
                                         cst.leadY * cst.leadY);
                ss.leadTrust = cst.leadTrust;
                ss.responseScale = g_control.responseScale();
                ss.foregroundFps = (float)g_shared.load.busyFps();
                ss.stride = g_shared.load.stride.load();

                {
                    std::lock_guard<std::mutex> lk(g_shared.results.m);
                    ss.targets = (int)g_shared.results.dets.size();
                    for (const auto& d : g_shared.results.dets)
                        ss.topScore = std::max(ss.topScore, d.score);
                }

                // Which application the user is actually in, by process
                // name. The window title would change constantly and say
                // less.
                HWND fgw = GetForegroundWindow();
                ss.ourWindowFocused = (fgw == g_hwnd);
                if (fgw) {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(fgw, &pid);
                    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                FALSE, pid)) {
                        char buf[MAX_PATH]{};
                        DWORD n = MAX_PATH;
                        if (QueryFullProcessImageNameA(hp, 0, buf, &n)) {
                            const std::string full(buf);
                            const size_t sl = full.find_last_of("\\/");
                            ss.foreground = (sl == std::string::npos)
                                          ? full : full.substr(sl + 1);
                        }
                        CloseHandle(hp);
                    }
                }

                const auto gsn = gpumon::Poll();
                if (gsn.valid) {
                    ss.gpuMHz    = (float)gsn.coreMHz;
                    ss.gpuMaxMHz = (float)gsn.coreMaxMHz;
                    ss.cpuPct    = -1.0f;
                    ss.gpuPct    = (float)gsn.utilGpuPct;
                    ss.throttled = gsn.throttled;
                }

                g_session.sample(ss);

                // The capture folder is named once, from whatever has had
                // the foreground longest by then.
                CaptureWriter::SetSessionLabel(g_session.dominantForeground());
                // Written out every ten seconds, so a crash or a killed
                // process still leaves a usable record.
                {
                    // Same reasoning as the timing report: writing the file
                    // is disk work, and the control thread must not wait
                    // behind it.
                    const int prev = GetThreadPriority(GetCurrentThread());
                    SetThreadPriority(GetCurrentThread(),
                                      THREAD_PRIORITY_BELOW_NORMAL);
                    g_session.flush(tc);
                    SetThreadPriority(GetCurrentThread(), prev);
                }
            }
        }

        // Settings are saved as they change, not only on exit. Losing an
        // evening of tuning to a power cut or a task-kill is not acceptable,
        // and the write is a few kilobytes to a temporary file.
        {
            // Recorded before the fingerprint is taken, so moving or
            // resizing the window is itself a change worth saving.
            if (g_hwnd) {
                WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
                if (GetWindowPlacement(g_hwnd, &wp)) {
                    std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                    g_shared.cfg.windowMaximised = (wp.showCmd == SW_SHOWMAXIMIZED);
                    // The normal-position rectangle, not the current one:
                    // saving a maximised window's bounds would restore it
                    // filling the screen with no way back.
                    const RECT& r = wp.rcNormalPosition;
                    g_shared.cfg.windowX = r.left;
                    g_shared.cfg.windowY = r.top;
                    g_shared.cfg.windowW = r.right - r.left;
                    g_shared.cfg.windowH = r.bottom - r.top;
                    g_shared.cfg.windowScale = ui::UiScale();
                    g_shared.cfg.activeTab = g_tab;
                }
            }

            static uint64_t lastPrint = 0;
            static int64_t  lastSave = 0;

            // Hashed from the canonical object, not from a copy. A copy's
            // padding bytes are indeterminate, so hashing one changed the
            // answer every frame and rewrote the config to disk endlessly.
            uint64_t fp;
            Config snap;
            {
                std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                fp = store::configFingerprint(g_shared.cfg, g_modelPath);
                snap = g_shared.cfg;
            }

            const int64_t nowNs = now_ns();
            if (fp != lastPrint && nowNs - lastSave > 1'500'000'000LL) {
                lastPrint = fp;
                lastSave = nowNs;
                store::saveConfig(store::lastSessionPath(), snap, g_modelPath,
                                  g_shared.log);
            }
        }

        // A board plugged in at launch is often still enumerating when the
        // program starts, so the first attempt at the port fails and the
        // setting looks broken. Retried for the first half minute.
        {
            Config c = g_shared.snapshotConfig();
            static int64_t lastTry = 0;
            static int tries = 0;
            const int64_t nowNs = now_ns();
            if ((c.inputMethod == InputArduino ||
                 c.inputMethod == InputMakcu) && tries < 12 &&
                nowNs - lastTry > 2'500'000'000LL) {
                const auto cs = g_control.state();
                if (!cs.sinkReady) {
                    lastTry = nowNs;
                    ++tries;
                    RefreshPorts();
                    if (c.serialPort <= 0) {
                        const int found = AutoPickPort(g_ports);
                        if (found > 0) {
                            std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                            g_shared.cfg.serialPort = found;
                            g_shared.log.info("Found the board on COM" +
                                              std::to_string(found) + ".");
                        }
                    }
                } else {
                    tries = 12;    // connected, so stop trying
                }
            }
        }

        // Capture hotkey, edge triggered so holding it does not thrash.
        {
            Config c = g_shared.snapshotConfig();
            static bool wasDown = false;
            const bool down = c.captureEnabled && c.captureKey > 0 &&
                              (GetAsyncKeyState(c.captureKey) & 0x8000) != 0;

            // When the capture key is the activation key, capture runs while
            // it is held rather than toggling. Toggling on the same button
            // used for aiming means every engagement flips it.
            // Either activation key counts, since either one aims.
            const bool holdMode = (c.captureKey == c.activationKey) ||
                                  (c.activationKey2 > 0 &&
                                   c.captureKey == c.activationKey2);
            if (holdMode) {
                if (down && !g_capwriter.running()) {
                    if (g_capwriter.start(c, g_shared.log))
                        g_capture.setHostReadback(true);
                } else if (!down && g_capwriter.running()) {
                    g_capwriter.stop(g_shared.log);
                    g_capture.setHostReadback(false);
                }
                wasDown = down;
            } else if (down && !wasDown) {
                if (g_capwriter.running()) {
                    g_capwriter.stop(g_shared.log);
                    g_capture.setHostReadback(false);
                    ui::Toast("Capture stopped.");
                } else if (g_capwriter.start(c, g_shared.log)) {
                    g_capture.setHostReadback(true);
                    ui::Toast("Capture started.");
                }
                wasDown = down;
            } else {
                wasDown = down;
            }
        }

        {
            Tuner::Result tuned;
            if (g_tuner.takeResult(tuned)) {
                std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                g_shared.cfg.sensitivity  = tuned.sensitivity;
                g_shared.cfg.deadzonePx   = tuned.deadzonePx;
                g_shared.cfg.emaOn        = tuned.emaOn;
                g_shared.cfg.emaIntensity = tuned.emaIntensity;
                g_shared.cfg.lagCompOn    = tuned.lagCompOn;
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "Applied: sensitivity %.2f, deadzone %d px.",
                         tuned.sensitivity, tuned.deadzonePx);
                ui::Toast(msg);
            }
        }

        if (g_flasher.takeCompletion()) {
            // The board re-enumerates after a flash, so the old port number
            // may no longer be the right one.
            RefreshPorts();
            ui::Toast("Firmware flashed.");
        }

        std::string builtEngine;
        if (g_builder.takeCompletion(builtEngine)) {
            strncpy_s(g_modelPath, sizeof(g_modelPath), builtEngine.c_str(), _TRUNCATE);
            std::lock_guard<std::mutex> el(g_engineMutex);
            if (g_engine.load(builtEngine, g_shared.log)) {
                g_classNames = store::loadClassNames(builtEngine);

                // Prove it runs before calling the build a success, and
                // record what it was built on so the next machine to see
                // this file can say something useful about it.
                const auto smoke = g_engine.smokeTest(g_capture.stream(),
                                                      g_shared.log);

                store::EngineMeta meta;
                meta.gpuName  = g_gpu.valid ? g_gpu.name : "unknown";
                meta.smMajor  = g_gpu.major;
                meta.smMinor  = g_gpu.minor;
                meta.trtMajor = NV_TENSORRT_MAJOR;
                meta.trtMinor = NV_TENSORRT_MINOR;
                meta.source   = g_builder.sourcePath();
                meta.fp16     = g_shared.cfg.buildFp16;
                meta.portable = g_shared.cfg.buildPortable;
                meta.inputW   = g_engine.inputW();
                meta.inputH   = g_engine.inputH();
                meta.layout   = g_engine.describe();
                meta.smokeMs  = smoke.ms;
                meta.smokeOk  = smoke.ok;
                {
                    SYSTEMTIME st{};
                    GetLocalTime(&st);
                    char when[32];
                    snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d",
                             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
                    meta.builtAt = when;
                }
                store::saveEngineMeta(builtEngine, meta);

                if (!meta.smokeOk)
                    g_shared.log.error("The engine built and loaded, but it "
                                       "does not produce usable output. "
                                       "Treat the build as failed: the source "
                                       "model is the thing to look at.");
            }
            RefreshLibrary();
        }

        // The overlay is updated before the panel is drawn, not after.
        //
        // It is what the user is actually looking at while playing, and the
        // panel is not. Servicing it first means a slow frame in the panel
        // delays the panel rather than the boxes.
        const int64_t t = now_ns();
        if (t - lastOverlay > 16'000'000) {
            lastOverlay = t;
            Config cfg = g_shared.snapshotConfig();
            g_perf.setGraphSpan(cfg.perfGraphSecs);
            g_perf.setMeters(cfg.perfMonMeters);
            g_perf.setUserScale(cfg.perfMonScale);
            g_perf.setDisplay(ResolveDisplay(cfg.perfMonDisplay));
            g_capture.setSizeOverride(cfg.resOverride ? cfg.resOverrideW : 0,
                                      cfg.resOverride ? cfg.resOverrideH : 0);
            g_perf.setCaptureVisible(cfg.visibleToCapture);
            g_perf.setVisible(cfg.perfMonOn);
            // A drag writes straight to the window, so read the placement
            // back rather than fighting the user for it.
            if (g_perf.edge() != cfg.perfMonEdge ||
                std::fabs(g_perf.edgeT() - cfg.perfMonEdgeT) > 0.001f) {
                std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                g_shared.cfg.perfMonEdge  = g_perf.edge();
                g_shared.cfg.perfMonEdgeT = g_perf.edgeT();
            }
            if (cfg.perfMonOn) {
                static std::vector<double> sc;
                PerfSnapshot ps;
                const auto tot = g_shared.timings.stage[ST_TOTAL].summary(sc);
                const auto cad = g_shared.timings.stage[ST_CADENCE].summary(sc);
                ps.loopP50 = tot.p50;
                ps.loopP99 = tot.p99;
                ps.cadenceHz = cad.p50 > 0 ? 1000.0 / cad.p50 : 0.0;

                // Machine load, polled here rather than in the monitor so
                // the readout and the Debug tab cannot disagree, and so the
                // cost is paid once at the same slow rate.
                {
                    static int64_t lastRes = 0;
                    static float cpuHeld = -1.0f;
                    static gpumon::Sample gsHeld;
                    static ULONGLONG pk = 0, pu = 0, pw = 0;

                    const int64_t nowRes = now_ns();
                    if (nowRes - lastRes > 500'000'000LL) {
                        lastRes = nowRes;

                        FILETIME ct{}, et{}, kt{}, ut{};
                        if (GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut)) {
                            const ULONGLONG k = ((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
                            const ULONGLONG u = ((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
                            FILETIME nf{};
                            GetSystemTimeAsFileTime(&nf);
                            const ULONGLONG w = ((ULONGLONG)nf.dwHighDateTime << 32) | nf.dwLowDateTime;
                            if (pw) {
                                SYSTEM_INFO si{};
                                GetSystemInfo(&si);
                                const double dw = (double)(w - pw);
                                const double dcpu = (double)((k - pk) + (u - pu));
                                if (dw > 0.0)
                                    cpuHeld = (float)(100.0 * dcpu / dw /
                                                      (double)si.dwNumberOfProcessors);
                            }
                            pk = k; pu = u; pw = w;
                        }
                        gsHeld = gpumon::Poll();
                    }

                    ps.cpuPct = cpuHeld;
                    if (gsHeld.valid) {
                        ps.gpuPct    = (float)gsHeld.utilGpuPct;
                        ps.gpuMHz    = gsHeld.coreMHz;
                        ps.gpuMaxMHz = gsHeld.coreMaxMHz;
                        ps.gpuTempC  = gsHeld.tempC;
                        ps.throttled = gsHeld.throttled;
                    }
                    size_t freeB = 0, totalB = 0;
                    if (cudaMemGetInfo(&freeB, &totalB) == cudaSuccess && totalB)
                        ps.vramPct = (float)(100.0 * (totalB - freeB) / totalB);
                }

                const uint64_t arr = g_shared.counters.arrivals.load();
                const uint64_t skp = g_shared.counters.skipped.load();
                ps.dropPct = arr ? 100.0 * skp / arr : 0.0;
                ps.fgFps   = (float)g_shared.load.busyFps();
                ps.stride  = g_shared.load.stride.load();
                // Only meaningful while the feature is on and something is
                // being tracked; otherwise the readout has nothing to say.
                {
                    const auto ss2 = g_control.state();
                    // The toggle, not the writer -- same reasoning as the
                    // title bar mark.
                    ps.capturing = cfg.captureEnabled;
                    ps.sensScale = (cfg.boxScaleOn && ss2.hasTarget)
                                 ? ss2.boxScaleMult : -1.0f;
                }

                const auto cs = g_control.state();
                ps.engaged = cs.engaged;
                {
                    std::lock_guard<std::mutex> lk(g_shared.results.m);
                    ps.detections = (int)g_shared.results.dets.size();
                    for (const auto& dd : g_shared.results.dets)
                        ps.topScore = std::max(ps.topScore, dd.score);
                }
                // History lives in the monitor now, sampled on a wall clock.
                g_perf.update(ps);
            }
            g_overlay.setCaptureVisible(cfg.visibleToCapture);
            {
                const auto cs2 = g_control.state();
                if (cfg.drawLeadDot && cs2.hasTarget)
                    g_overlay.setLeadMarker(cs2.targetX + cs2.leadX,
                                            cs2.targetY + cs2.leadY);
                else
                    g_overlay.setLeadMarker(-1.0f, -1.0f);

                if (cfg.drawPath && cs2.pathCount > 1) {
                    Overlay::PathPt pts[Controller::State::kPathMax];
                    for (int i = 0; i < cs2.pathCount; ++i)
                        pts[i] = { cs2.pathPts[i].x, cs2.pathPts[i].y,
                                   cs2.pathPts[i].speed };
                    g_overlay.setPath(pts, cs2.pathCount);
                } else {
                    g_overlay.setPath(nullptr, 0);
                }
            }

            // The FOV outline and the boxes are independent. Drawing the
            // region needs no detections at all, so it must not depend on
            // the boxes being on or on the model running.
            const bool wantOverlay = (cfg.overlayOn || cfg.drawRoiRect) &&
                                     cfg.pipelineOn;
            g_overlay.setVisible(wantOverlay);
            if (wantOverlay) {
                std::vector<Det> dets;
                if (cfg.overlayOn) {
                    std::lock_guard<std::mutex> lk(g_shared.results.m);
                    dets = g_shared.results.dets;
                }
                // Max boxes applies to what is drawn. The controller has
                // already seen the full set, which is what lets it keep
                // following one target while a louder one is on screen.
                if ((int)dets.size() > cfg.maxDets) {
                    std::partial_sort(dets.begin(), dets.begin() + cfg.maxDets,
                                      dets.end(),
                                      [](const Det& a, const Det& b) {
                                          return a.score > b.score;
                                      });
                    dets.resize(cfg.maxDets);
                }

                // Computed from the config rather than read back from the
                // last detection, so the region still draws when nothing is
                // being inferred.
                const int sw = g_capture.screenWidth();
                const int sh = g_capture.screenHeight();
                int cx = sw / 2, cy = sh / 2;
                if (cfg.roiMode == ROI_MOUSE) {
                    POINT pt{};
                    if (GetCursorPos(&pt)) { cx = pt.x; cy = pt.y; }
                }
                cx += (int)(cfg.roiOffXPct * 0.01f * sw);
                cy += (int)(cfg.roiOffYPct * 0.01f * sh);
                const int hw = cfg.fov / 2;
                const int l  = std::clamp(cx - hw, 0, std::max(0, sw - cfg.fov));
                const int tp = std::clamp(cy - hw, 0, std::max(0, sh - cfg.fov));
                // Paced, rather than redrawn every time round the loop.
                //
                // This is a layered window: each render is a GDI blit of the
                // whole region and an UpdateLayeredWindow call. The loop
                // spins at about 250 Hz when unfocused, so the overlay was
                // being redrawn that often -- roughly four times per display
                // refresh, three of which nobody ever sees.
                //
                // Free when nothing else wants the machine, which is why it
                // never showed up in testing. With a game in front competing
                // for the same GPU it is not free at all, and the cost lands
                // on the loop that also drives the readout and the widgets --
                // which is exactly when they were reported as stuttering.
                // Redrawn when there is something new to show, not on a
                // timer and not every iteration.
                //
                // This is a layered window: each render blits the whole
                // region and calls UpdateLayeredWindow. It was happening
                // every time round the loop -- about 250 times a second --
                // while detections only change when a frame is inferred, and
                // the region only moves when the mouse does. Most of those
                // redraws painted the identical picture.
                //
                // Free when nothing else wants the machine, which is why it
                // never showed up in testing. With a game in front competing
                // for the same GPU it is not free, and the cost lands on the
                // loop that also drives the readout -- which is exactly when
                // the stutter was reported.
                static int64_t lastShownStamp = 0;
                static int lastL = -999999, lastT = -999999;
                int64_t stampNow = 0;
                {
                    std::lock_guard<std::mutex> lk(g_shared.results.m);
                    stampNow = g_shared.results.stamp;
                }
                if (stampNow != lastShownStamp || l != lastL || tp != lastT) {
                    lastShownStamp = stampNow;
                    lastL = l; lastT = tp;
                    g_overlay.render(cfg, dets, l, tp, l + cfg.fov, tp + cfg.fov);
                }
            }
        }

        // Whether the panel is drawn at all this iteration.
        //
        // The loop itself keeps running at full rate, because the overlay is
        // serviced from it and that is what the user is looking at. Only the
        // panel is skipped: nobody is reading it while another window is in
        // front, and drawing it costs real work on the card the model and
        // the game are sharing.
        const bool fgNow = (GetForegroundWindow() == g_hwnd);
        bool drawPanel = true;
        if (!fgNow) {
            static int64_t lastPanel = 0;
            const int64_t nowPanel = now_ns();
            // Thirty a second rather than ten.
            //
            // The cost is a panel redraw, which is a few hundred microseconds
            // of a card layout and one small draw call -- next to inference
            // at four milliseconds a frame it is not the thing worth saving.
            // Ten was chosen to be obviously cheap and it reads as a stutter,
            // which is a poor trade for something the user is looking at
            // while deciding whether the program is working.
            if (nowPanel - lastPanel < 33'000'000LL) drawPanel = false;

            // Nothing at all while the window cannot be seen.
            //
            // A fullscreen game in front means every pixel drawn here is
            // discarded, so the whole panel -- layout, geometry, present --
            // is pure cost taken from the card the game and the model are
            // sharing. Once every widget draws a surface that is no longer a
            // rounding error.
            //
            // Polled a few times a second so the panel comes back promptly
            // when the game is minimised or alt-tabbed away from. A test
            // present is cheap; a full frame is not.
            if (g_occluded) {
                static int64_t lastProbe = 0;
                if (nowPanel - lastProbe < 300'000'000LL) {
                    drawPanel = false;
                } else {
                    lastProbe = nowPanel;
                    const HRESULT t = g_swap->Present(0, DXGI_PRESENT_TEST);
                    g_occluded = (t == DXGI_STATUS_OCCLUDED);
                    if (g_occluded) drawPanel = false;
                }
            }
            else lastPanel = nowPanel;
        }

        // Advanced every iteration, before the panel can be skipped.
        //
        // It was called after the present, which the skip jumps over -- so
        // the readout stopped gliding the moment another window came
        // forward, which is precisely when it is being watched.
        g_perf.animate();

        if (!drawPanel) {
            // Waited on, rather than spun on.
            //
            // Sleep(4) put this loop round about 250 times a second, and
            // every one of those iterations did the full frame-side work --
            // snapshotting config, reading counters, redrawing the overlay --
            // to produce something visible only 30 times a second. With
            // nothing else running that waste is invisible. With a game in
            // front it is a background thread waking 250 times a second and
            // touching the GPU, competing with the thing the user is actually
            // looking at.
            //
            // Sleeping until the panel is next due keeps the widgets at the
            // rate they are drawn at and gives the rest of the machine back.
            // Short enough that the overlay still tracks a moving target,
            // long enough that the loop is not spinning. The overlay above
            // only repaints when something changed, so most of these
            // iterations now cost almost nothing.
            Sleep(3);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        const float clear[4] = {0.055f, 0.063f, 0.075f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // No wait when unfocused: the compositor throttles a background
        // window's present rate, and this loop also services the overlay.
        // The panel is already drawn only a few times a second by the check
        // above, so there is nothing left to throttle here.
        // Occlusion is detected and the panel stops being drawn.
        //
        // Present tells us when nothing it produced can be seen -- a
        // fullscreen game in front, or the window minimised. Until now that
        // return was discarded, so the UI kept rendering at thirty frames a
        // second into a surface nobody was looking at, on the same card the
        // model and the game are sharing.
        //
        // That was affordable when a card was a flat rectangle. It stopped
        // being affordable when every toggle, slider and dropdown started
        // drawing a rounded fill and an edge band, which is a lot of
        // geometry to throw away thirty times a second.
        const HRESULT pres = g_swap->Present(fgNow ? 1 : 0, 0);
        g_occluded = (pres == DXGI_STATUS_OCCLUDED);


        // The panel is part of "everything loopcore draws". On one monitor
        // it is the window most in the way, so it follows the same switch.
        {
            static int applied = -1;
            const Config vc = g_shared.snapshotConfig();
            const int want = vc.visibleToCapture ? 1 : 0;
            if (applied != want) {
                applied = want;
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
                SetWindowDisplayAffinity(g_hwnd,
                    want ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
            }
        }


    }

    // Final save. Geometry is re-read here rather than trusted from the last
    // frame: the window can be moved or resized between that frame and the
    // close, and those are the very last changes a user makes.
    {
        if (g_hwnd) {
            WINDOWPLACEMENT wp{sizeof(WINDOWPLACEMENT)};
            if (GetWindowPlacement(g_hwnd, &wp)) {
                std::lock_guard<std::mutex> lk(g_shared.cfgMutex);
                g_shared.cfg.windowMaximised = (wp.showCmd == SW_SHOWMAXIMIZED);
                const RECT& r = wp.rcNormalPosition;
                g_shared.cfg.windowX = r.left;
                g_shared.cfg.windowY = r.top;
                g_shared.cfg.windowW = r.right - r.left;
                g_shared.cfg.windowH = r.bottom - r.top;
                g_shared.cfg.windowScale = ui::UiScale();
                g_shared.cfg.activeTab = g_tab;
            }
        }
        Config cfg = g_shared.snapshotConfig();
        store::saveConfig(store::lastSessionPath(), cfg, g_modelPath, g_shared.log);
    }

    g_tuner.cancel();
    {
        const Config fc = g_shared.snapshotConfig();
        g_session.stop(fc, g_shared.log);
    }
    gpumon::Shutdown();
    g_shared.log.flush();
    g_capwriter.stop(g_shared.log);
    g_control.close();
    g_perf.destroy();
    g_capture.stop();
    g_overlay.destroy();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    timeEndPeriod(1);
    return 0;
}
