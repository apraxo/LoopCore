// main.cpp -- loopcore control panel.
//
// Three threads that matter:
//   * WinRT thread pool  : FrameArrived -> crop -> CUDA -> TensorRT -> publish
//   * this thread        : Win32 pump, ImGui panel, overlay redraw at ~60 Hz
//   * everything else    : none. Deliberately.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <d3d11.h>
#include <shellscalingapi.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "common.h"
#include "capture.h"
#include "engine.h"
#include "overlay.h"
#include "preprocess.cuh"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace lc;

// --------------------------------------------------------------- globals

static ID3D11Device*           g_dev  = nullptr;
static ID3D11DeviceContext*    g_ctx  = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv  = nullptr;

static Shared   g_shared;
static Capture  g_capture;
static Engine   g_engine;
static Overlay  g_overlay;

static char g_enginePath[512] = "best.engine";
static std::vector<RawDet> g_rawScratch;

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
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp)) return true;
    switch (msg) {
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

// Runs on the WinRT capture thread. Everything here is on the hot path.
static void OnFrame(const MappedRoi& roi) {
    if (!g_engine.ready()) return;
    Config cfg = g_shared.snapshotConfig();

    {
        ScopedTimer t(g_shared.timings.stage[ST_PRE]);
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
            return;
        }
    }

    {
        ScopedTimer t(g_shared.timings.stage[ST_INFER]);
        if (!g_engine.infer(g_capture.stream(), cfg.confThresh, cfg.maxDets,
                            g_rawScratch, g_shared.log)) {
            g_shared.counters.errors++;
            return;
        }
    }

    {
        ScopedTimer t(g_shared.timings.stage[ST_POST]);
        LetterboxMeta m = letterbox_meta(g_engine.inputW(), g_engine.inputH(),
                                          roi.width, roi.height);
        std::vector<Det> dets;
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

        ScopedTimer tp(g_shared.timings.stage[ST_PUBLISH]);
        {
            std::lock_guard<std::mutex> lk(g_shared.results.m);
            g_shared.results.dets.swap(dets);
            g_shared.results.roiL = roi.left;
            g_shared.results.roiT = roi.top;
            g_shared.results.roiR = roi.left + roi.width;
            g_shared.results.roiB = roi.top + roi.height;
            g_shared.results.stamp = now_ns();
            g_shared.counters.detections += g_shared.results.dets.size();
            if (g_shared.results.dets.empty()) g_shared.counters.empty++;
        }
    }
}

// ------------------------------------------------------------------- ui

static void StageTable() {
    static std::vector<double> scratch;
    const ImGuiTableFlags f = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("stages", 6, f)) return;
    ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthStretch, 1.6f);
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
        if (s == ST_TOTAL) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.85f, 0.3f, 1));
        ImGui::TextUnformatted(stage_name(s));
        if (s == ST_TOTAL) ImGui::PopStyleColor();
        if (sum.n == 0) {
            for (int c = 0; c < 5; ++c) { ImGui::TableNextColumn(); ImGui::TextUnformatted("--"); }
            continue;
        }
        double v[5] = {sum.mean, sum.p50, sum.p90, sum.p99, sum.max};
        for (double d : v) { ImGui::TableNextColumn(); ImGui::Text("%.3f", d); }
    }
    ImGui::EndTable();
    ImGui::TextDisabled("milliseconds, last %d samples", Ring::CAP);
}

static void DrawUI() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("loopcore", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- status strip --------------------------------------------------
    {
        bool live = g_capture.running() && g_engine.ready();
        ImGui::PushStyleColor(ImGuiCol_Text, live ? ImVec4(0.3f, 1.0f, 0.45f, 1)
                                                  : ImVec4(1.0f, 0.55f, 0.3f, 1));
        ImGui::Text(live ? "RUNNING" : "STOPPED");
        ImGui::PopStyleColor();
        ImGui::SameLine();

        static std::vector<double> scratch;
        auto cad = g_shared.timings.stage[ST_CADENCE].summary(scratch);
        auto tot = g_shared.timings.stage[ST_TOTAL].summary(scratch);
        ImGui::Text("| loop %.2f ms p50  %.2f ms p99   frame %.1f Hz   dropped %llu",
                    tot.p50, tot.p99,
                    cad.p50 > 0 ? 1000.0 / cad.p50 : 0.0,
                    (unsigned long long)g_shared.counters.skipped.load());
    }
    ImGui::Separator();

    std::lock_guard<std::mutex> cfgLock(g_shared.cfgMutex);
    Config& cfg = g_shared.cfg;

    if (ImGui::BeginTabBar("tabs")) {

        // ------------------------------------------------------- Loop
        if (ImGui::BeginTabItem("Loop")) {
            ImGui::SeparatorText("Model");
            ImGui::SetNextItemWidth(-90);
            ImGui::InputText("##engine", g_enginePath, sizeof(g_enginePath));
            ImGui::SameLine();
            if (ImGui::Button("Load", ImVec2(80, 0)))
                g_engine.load(g_enginePath, g_shared.log);
            if (g_engine.ready())
                ImGui::TextDisabled("%s", g_engine.describe().c_str());
            else
                ImGui::TextDisabled("No engine loaded.");

            ImGui::SeparatorText("Capture region");
            const char* modes[] = { "Centre of screen", "Follow mouse" };
            ImGui::SetNextItemWidth(200);
            ImGui::Combo("Anchor", &cfg.roiMode, modes, 2);
            ImGui::SetNextItemWidth(200);
            ImGui::DragInt("Width",  &cfg.roiW, 8, 64, 3840);
            ImGui::SetNextItemWidth(200);
            ImGui::DragInt("Height", &cfg.roiH, 8, 64, 2160);
            ImGui::SetNextItemWidth(200);
            ImGui::DragInt("Offset X", &cfg.roiOffX, 2, -2000, 2000);
            ImGui::SetNextItemWidth(200);
            ImGui::DragInt("Offset Y", &cfg.roiOffY, 2, -2000, 2000);
            ImGui::TextDisabled("Negative Y lifts the region toward the horizon.");

            if (g_engine.ready()) {
                float want = (float)g_engine.inputW() / g_engine.inputH();
                float have = (float)cfg.roiW / cfg.roiH;
                if (std::abs(want - have) > 0.08f) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.2f, 1));
                    ImGui::TextWrapped(
                        "Region is %.2f:1 but the engine expects %.2f:1. The "
                        "difference becomes grey padding and wasted compute. "
                        "Match them, or re-export at imgsz=[%d,%d].",
                        have, want, cfg.roiH, cfg.roiW);
                    ImGui::PopStyleColor();
                }
            }

            ImGui::SeparatorText("Detection");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Confidence", &cfg.confThresh, 0.01f, 0.95f, "%.2f");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Max boxes", &cfg.maxDets, 1, 300);
            ImGui::Checkbox("Run the pipeline", &cfg.pipelineOn);
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------- Overlay
        if (ImGui::BeginTabItem("Overlay")) {
            ImGui::Checkbox("Show overlay", &cfg.overlayOn);
            ImGui::TextDisabled("Drawn at 60 Hz on the UI thread; never on the loop.");
            ImGui::SeparatorText("What to draw");
            ImGui::Checkbox("Boxes with class width", &cfg.drawLabels);
            ImGui::Checkbox("Confidence as fill",     &cfg.drawConfidence);
            ImGui::Checkbox("Centre dot",             &cfg.drawCenterDot);
            ImGui::Checkbox("Region outline",         &cfg.drawRoiRect);

            ImGui::SeparatorText("Appearance");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderInt("Line thickness", &cfg.boxThickness, 1, 8);
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Opacity", &cfg.overlayAlpha, 0.05f, 1.0f, "%.2f");
            ImGui::SetNextItemWidth(200);
            ImGui::SliderFloat("Hide below confidence", &cfg.overlayMinConf, 0.0f, 1.0f, "%.2f");
            ImGui::ColorEdit3("Box colour",    cfg.boxColor);
            ImGui::ColorEdit3("Region colour", cfg.roiColor);
            ImGui::EndTabItem();
        }

        // ------------------------------------------------------ Debug
        if (ImGui::BeginTabItem("Debug")) {
            if (ImGui::Button("Reset timings")) {
                g_shared.timings.reset();
                g_shared.counters.reset();
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear log")) g_shared.log.clear();

            ImGui::SeparatorText("Stage timings");
            StageTable();

            ImGui::SeparatorText("Loop time, last 256 frames");
            static std::vector<float> tail;
            g_shared.timings.stage[ST_TOTAL].tail(tail, 256);
            if (!tail.empty())
                ImGui::PlotLines("##loopplot", tail.data(), (int)tail.size(), 0,
                                 nullptr, 0.0f, FLT_MAX, ImVec2(-1, 70));

            ImGui::SeparatorText("Counters");
            uint64_t arr = g_shared.counters.arrivals.load();
            uint64_t frm = g_shared.counters.frames.load();
            uint64_t skp = g_shared.counters.skipped.load();
            uint64_t det = g_shared.counters.detections.load();
            uint64_t emp = g_shared.counters.empty.load();
            ImGui::Text("frames arrived   %llu", (unsigned long long)arr);
            ImGui::Text("frames processed %llu", (unsigned long long)frm);
            ImGui::Text("frames dropped   %llu  (%.1f%%)",
                        (unsigned long long)skp, arr ? 100.0 * skp / arr : 0.0);
            ImGui::Text("boxes emitted    %llu  (%.2f per frame)",
                        (unsigned long long)det, frm ? (double)det / frm : 0.0);
            ImGui::Text("empty frames     %llu  (%.1f%%)",
                        (unsigned long long)emp, frm ? 100.0 * emp / frm : 0.0);
            ImGui::Text("errors           %llu",
                        (unsigned long long)g_shared.counters.errors.load());

            if (skp > 0 && arr > 0 && (double)skp / arr > 0.05) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.2f, 1));
                ImGui::TextWrapped(
                    "Frames are being dropped because the loop is slower than the "
                    "sim's present rate. Shrink the region, lower the input "
                    "resolution, or re-export the engine at INT8.");
                ImGui::PopStyleColor();
            }

            ImGui::SeparatorText("Log");
            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Border);
            for (const auto& e : g_shared.log.snapshot()) {
                ImVec4 c = e.level == LOG_ERROR ? ImVec4(1.0f, 0.42f, 0.38f, 1)
                         : e.level == LOG_WARN  ? ImVec4(1.0f, 0.78f, 0.30f, 1)
                                                : ImVec4(0.70f, 0.74f, 0.80f, 1);
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                ImGui::TextWrapped("[%7.2f] %s", e.t, e.text.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}

// ------------------------------------------------------------------ main

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0, 0,
                   inst, nullptr, nullptr, nullptr, nullptr, L"loopcore", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"loopcore", WS_OVERLAPPEDWINDOW,
                              120, 120, 620, 470, nullptr, nullptr, inst, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr, L"Could not create a Direct3D 11 device. Update the GPU driver.",
                    L"loopcore", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.FrameRounding  = 3.0f;
    st.GrabRounding   = 3.0f;
    st.ItemSpacing    = ImVec2(8, 6);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    g_shared.log.info("loopcore started.");
    if (!g_capture.start(0, g_shared, OnFrame))
        g_shared.log.error("Capture did not start. The panel still works; fix the "
                           "issue above and restart.");
    else
        g_overlay.create(inst, 0, 0, g_capture.screenWidth(),
                         g_capture.screenHeight(), g_shared.log);

    g_engine.load(g_enginePath, g_shared.log);

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

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        const float clear[4] = {0.07f, 0.08f, 0.10f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        // Overlay at ~60 Hz, independent of the capture loop.
        int64_t t = now_ns();
        if (t - lastOverlay > 16'000'000) {
            lastOverlay = t;
            Config cfg = g_shared.snapshotConfig();
            g_overlay.setVisible(cfg.overlayOn);
            if (cfg.overlayOn) {
                std::vector<Det> dets;
                int l, tp, r, b;
                {
                    std::lock_guard<std::mutex> lk(g_shared.results.m);
                    dets = g_shared.results.dets;
                    l = g_shared.results.roiL; tp = g_shared.results.roiT;
                    r = g_shared.results.roiR; b  = g_shared.results.roiB;
                }
                g_overlay.render(cfg, dets, l, tp, r, b);
            }
        }
    }

    g_capture.stop();
    g_overlay.destroy();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    timeEndPeriod(1);
    return 0;
}
