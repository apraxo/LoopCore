// capture_writer.cpp
#include "capture_writer.h"

#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <vector>

#include "store.h"

namespace lc {

namespace {

// WIC is part of Windows, so encoding PNG and JPEG needs no third-party
// library and no bundled DLL.
IWICImagingFactory* g_wic = nullptr;

bool ensureWic() {
    if (g_wic) return true;
    // The writer thread owns this; COM is initialised per-thread.
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&g_wic));
    return SUCCEEDED(hr) && g_wic;
}

std::string timestampStem() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    snprintf(buf, sizeof(buf), "cap_%04d%02d%02d_%02d%02d%02d_%03d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, st.wMilliseconds);
    return buf;
}

std::string todayStamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

std::string randomTag(int n) {
    static const char* kAlphabet = "abcdefghijkmnpqrstuvwxyz23456789";
    static bool seeded = false;
    if (!seeded) { seeded = true; srand((unsigned)GetTickCount64()); }
    std::string out;
    for (int i = 0; i < n; ++i) out.push_back(kAlphabet[rand() % 32]);
    return out;
}

// One folder per run of the program, decided the first time capture starts
// and reused thereafter.
//
// Capture is switched on and off dozens of times in a sitting -- often from
// a key that is also used for something else -- and a folder per press
// buries the images in directory noise. Tying it to the process instead
// means the images from one sitting stay together, and a fresh launch is the
// only thing that starts a new set.
std::string g_sessionDir;
std::string g_sessionLabel;

std::string sessionFolder(const std::string& root) {
    if (!g_sessionDir.empty()) return g_sessionDir;
    std::string name = todayStamp();
    if (!g_sessionLabel.empty()) name += "_" + g_sessionLabel;
    name += "_" + randomTag(4);
    g_sessionDir = root + "\\" + name;
    return g_sessionDir;
}

bool makeDir(const std::string& p) {
    return CreateDirectoryA(p.c_str(), nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

constexpr int kQueueMax = 24;

} // namespace

CaptureWriter::~CaptureWriter() {
    if (running_.load()) {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
}

std::string CaptureWriter::sessionDir() const {
    std::lock_guard<std::mutex> lk(statMutex_);
    return dir_;
}

void CaptureWriter::SetSessionLabel(const std::string& label) {
    // Only meaningful before the folder is created; afterwards the name is
    // already on disk and renaming it would orphan anything written.
    if (!g_sessionDir.empty()) return;
    std::string clean;
    for (char c : label) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_') clean.push_back(c);
        else if (c == ' ') clean.push_back('-');
        if (clean.size() >= 24) break;
    }
    g_sessionLabel = clean;
}

CaptureStats CaptureWriter::totals() const {
    std::lock_guard<std::mutex> lk(statMutex_);
    CaptureStats s;
    s.saved    = totalSaved_ + stats_.saved;
    s.labels   = totalLabels_ + stats_.labels;
    s.sessions = sessions_;
    return s;
}

CaptureStats CaptureWriter::stats() const {
    // The two locks are taken one after the other, never nested.
    //
    // This held statMutex_ and then reached for m_, while submit() on the
    // capture thread holds m_ and reaches for statMutex_ when the queue is
    // full. That is a lock-order inversion: each thread ends up holding what
    // the other is waiting for, and both stop for good. Nothing fails and
    // nothing is logged -- the UI simply never returns, and capture stops
    // with it.
    //
    // It needs a full queue and a UI read in the same instant, which is why
    // it has not been seen. That is not a reason to leave it.
    //
    // Reading them separately means the queue depth can be an instant older
    // than the counters beside it. For a number on a readout that is not a
    // cost worth a deadlock.
    CaptureStats s;
    {
        std::lock_guard<std::mutex> lk(statMutex_);
        s = stats_;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        s.queued = (int)queue_.size();
    }
    return s;
}

bool CaptureWriter::start(const Config& cfg, Log& log) {
    if (running_.load()) return false;

    std::string root = cfg.capturePath[0] ? std::string(cfg.capturePath)
                                          : store::binDir() + "\\captures";
    if (!makeDir(root)) {
        log.error("Could not create " + root + ". Pick a writable folder on "
                  "the Capture tab.");
        return false;
    }

    std::string dir = root;
    if (cfg.captureSessionFolders) {
        dir = sessionFolder(root);
        if (!makeDir(dir)) {
            log.error("Could not create the session folder " + dir);
            return false;
        }
    }
    // Labels live beside the images, which is what every YOLO loader
    // expects; a separate labels/ folder needs extra configuration.

    {
        std::lock_guard<std::mutex> lk(statMutex_);
        dir_ = dir;
        stats_ = CaptureStats{};
        stats_.session = dir;
        stats_.sessions = sessions_;
        format_  = (ImageFormat)cfg.captureFormat;
        quality_ = cfg.captureQuality;
    }

    lastSaveNs_ = 0;
    lastRealNs_ = 0;
    burstLeft_ = 0;
    lastBurstNs_ = 0;
    burstsCancelled_ = 0;
    burstDone_ = false;
    tierName_ = 0;
    hadDetection_ = false;
    index_ = 0;

    running_ = true;
    worker_ = std::thread(&CaptureWriter::writerLoop, this, &log);

    // Only the first run announces where it is going. With the hotkey bound
    // to the activation key this starts and stops constantly, and a line
    // each time buries everything else in the log.
    static bool announced = false;
    if (!announced) {
        announced = true;
        log.info("Capture writing to " + dir);
    }
    return true;
}

void CaptureWriter::stop(Log& log) {
    if (!running_.load()) return;
    running_ = false;
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Nothing is logged per run. The totals live on the Capture tab, which
    // is where anyone would look for them.
    totalSaved_  += stats().saved;
    totalLabels_ += stats().labels;
    ++sessions_;
}

// ------------------------------------------------------------- triggering

void CaptureWriter::offer(const uint8_t* bgra, int width, int height, int stride,
                          const std::vector<Det>& dets, float topScore,
                          int roiLeft, int roiTop, const Config& cfg)
{
    if (!running_.load() || !bgra || width <= 0 || height <= 0) return;

    const int64_t now = now_ns();
    const int64_t delay = (int64_t)cfg.captureDelayMs * 1'000'000LL;
    const bool haveDets = !dets.empty();

    bool save = false;
    const char* tier = "";

    switch (cfg.captureMode) {
    case CapOnDetection:
        // Only frames with something in them, still rate limited so a static
        // scene does not fill the disk with near-duplicates.
        if (haveDets && (now - lastSaveNs_) >= delay) { save = true; tier = "detection"; }
        break;

    case CapBurstOnLoss: {
        // Walk the rungs from the top down and take the first the current
        // confidence reaches. A rung with a zero interval captures nothing,
        // which is how the highest one skips frames the model already
        // handles: those teach it nothing.
        const bool real = haveDets && topScore >= cfg.confThresh;
        if (real) { lastRealNs_ = now; hadDetection_ = true; }

        // Low-confidence noise can otherwise sit in a rung forever after the
        // real target has gone, so give up on it after a grace period.
        const int64_t fadeTimeout = 2'000'000'000LL;
        if (!real && hadDetection_ && (now - lastRealNs_) > fadeTimeout)
            hadDetection_ = false;

        int64_t interval = 0;

        if (haveDets || real) {
            // A burst in progress is abandoned the moment the model is
            // confident again.
            //
            // The point of these frames is the case the model got wrong, and
            // once it has the target back there is nothing further to learn
            // from the sequence -- every additional image is one more
            // near-duplicate to sort through later. Cancelling on a
            // confident detection rather than on any detection at all also
            // stops a low-confidence ghost from cutting a burst short, which
            // is the case actually worth capturing.
            if (real && burstLeft_ > 0) {
                burstLeft_ = 0;
                burstDone_ = true;
                ++burstsCancelled_;
            }
            burstLeft_ = 0;      // still seeing something, so not lost yet
            for (int i = 0; i < cfg.ladderCount && i < Config::kLadderMax; ++i) {
                if (topScore >= cfg.ladderConf[i]) {
                    interval = (int64_t)cfg.ladderMs[i] * 1'000'000LL;
                    tierName_ = i;
                    break;
                }
            }
        } else if (hadDetection_) {
            // Just lost it. Fire a fixed number of frames rather than for a
            // fixed time, so the burst is the same size whatever the frame
            // rate happens to be.
            // Not more often than the burst itself takes to run.
            //
            // A target flickering at the edge of confidence produces a loss
            // every few frames, and without this each one arms a fresh
            // burst: the writer then spends its whole time in back-to-back
            // bursts and the folder fills with the same moment over and
            // over. One burst per settled loss is the intent.
            const int64_t burstSpan =
                (int64_t)std::max(1, cfg.burstFrames) *
                (int64_t)std::max(1, cfg.burstMs) * 1'000'000LL;
            const int64_t cooldown = std::max(burstSpan, 1'000'000'000LL);
            const bool cooled = (lastBurstNs_ == 0) ||
                                (now - lastBurstNs_) >= cooldown;

            if (burstLeft_ == 0 && !burstDone_ && cooled) {
                burstLeft_ = cfg.burstFrames;
                lastBurstNs_ = now;
            }
            if (burstLeft_ > 0) {
                interval = (int64_t)cfg.burstMs * 1'000'000LL;
                tierName_ = -1;
            } else {
                hadDetection_ = false;
                burstDone_ = false;
            }
        }

        if (interval > 0 && (now - lastSaveNs_) >= interval) {
            save = true;
            if (tierName_ < 0) {
                tier = "burst";
                if (burstLeft_ > 0) --burstLeft_;
                if (burstLeft_ == 0) { burstDone_ = true; hadDetection_ = false; }
            } else {
                static char tierBuf[24];
                snprintf(tierBuf, sizeof(tierBuf), "rung %d", tierName_ + 1);
                tier = tierBuf;
            }
        }
        break;
    }

    case CapTimed:
    default:
        if ((now - lastSaveNs_) >= delay) { save = true; tier = "timed"; }
        break;
    }

    if (!save) return;
    lastSaveNs_ = now;

    Job j;
    j.w = width;
    j.h = height;
    j.roiLeft = roiLeft;
    j.roiTop = roiTop;
    j.label = cfg.captureLabels && haveDets;
    j.dets = dets;
    j.stem = timestampStem();

    // Cropped to the configured size, about the same centre.
    //
    // The written size is deliberately not the field of view: the region is
    // a tuning setting that moves around, and a dataset wants every image
    // the same size or the labels have to be rescaled before anything can
    // be trained on them. Larger than the region is meaningless -- there is
    // no more image to take -- so it is capped rather than padded.
    const int want = std::clamp(cfg.captureSize, 64, 1600);
    const int cw = std::min(want, width);
    const int chh = std::min(want, height);
    const int ox = (width - cw) / 2;
    const int oy = (height - chh) / 2;

    j.w = cw;
    j.h = chh;
    j.cropX = ox;
    j.cropY = oy;

    // Pack tightly: the source may be padded, and the encoder wants a
    // contiguous buffer anyway.
    j.rgba.resize((size_t)cw * chh * 4);
    for (int y = 0; y < chh; ++y)
        memcpy(j.rgba.data() + (size_t)y * cw * 4,
               bgra + (size_t)(y + oy) * stride + (size_t)ox * 4,
               (size_t)cw * 4);

    // Neither lock is held while the other is taken.
    //
    // The queue push and the counters are separate steps for one reason:
    // stats() reads statMutex_ then m_, and holding m_ here while reaching
    // for statMutex_ would complete a lock-order inversion between the UI
    // thread and this one. Both would stop, with nothing logged.
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (queue_.size() >= kQueueMax) {
            // Dropping is correct. Blocking would stall the capture thread,
            // and a stalled capture thread is worse than a missing training
            // image.
            dropped = true;
        } else {
            queue_.push_back(std::move(j));
        }
    }
    {
        std::lock_guard<std::mutex> lk(statMutex_);
        if (dropped) stats_.dropped++;
        stats_.tier = tier;
    }
    if (dropped) return;

    cv_.notify_one();
}

// ---------------------------------------------------------------- writing

void CaptureWriter::writerLoop(Log* logp) {
    Log& log = *logp;
    // Balanced: CoUninitialize is only called when the matching initialise
    // actually succeeded. Calling it otherwise decrements a count this
    // thread never incremented, which unloads COM out from under whoever
    // did.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    if (!comOwned)
        log.warn("COM could not be initialised on the writer thread; image "
                 "encoding will not work.");

    if (!ensureWic())
        log.error("WIC could not be created; images cannot be encoded.");

    while (running_.load()) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(100),
                         [&] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) continue;
            j = std::move(queue_.front());
            queue_.pop_front();
        }

        const std::string base = sessionDir() + "\\" + j.stem;
        const std::string img = base + (format_ == ImgJpeg ? ".jpg" : ".png");

        if (writeImage(j, img)) {
            std::lock_guard<std::mutex> lk(statMutex_);
            stats_.saved++;
            stats_.lastFile = j.stem;
        } else {
            std::lock_guard<std::mutex> lk(statMutex_);
            stats_.failed++;
        }

        if (j.label && writeLabel(j, base + ".txt")) {
            std::lock_guard<std::mutex> lk(statMutex_);
            stats_.labels++;
        }
    }

    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
    if (comOwned) CoUninitialize();
}

bool CaptureWriter::writeImage(const Job& j, const std::string& path) {
    if (!g_wic) return false;

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;

    do {
        if (FAILED(g_wic->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE))) break;

        const GUID& fmt = (format_ == ImgJpeg) ? GUID_ContainerFormatJpeg
                                               : GUID_ContainerFormatPng;
        if (FAILED(g_wic->CreateEncoder(fmt, nullptr, &enc))) break;
        if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&frame, &props))) break;

        if (format_ == ImgJpeg && props) {
            PROPBAG2 opt{};
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v{};
            v.vt = VT_R4;
            v.fltVal = std::clamp(quality_, 1, 100) / 100.0f;
            props->Write(1, &opt, &v);
        }
        if (FAILED(frame->Initialize(props))) break;
        if (FAILED(frame->SetSize((UINT)j.w, (UINT)j.h))) break;

        // The format WIC actually accepted, which is not necessarily the
        // one that was asked for.
        //
        // SetPixelFormat takes its argument by pointer and rewrites it when
        // the encoder cannot do what was requested. The JPEG encoder has no
        // 32-bit format at all, so it quietly answers 24bppBGR -- and the
        // call succeeds. Handing it 32-bit rows after that walks the buffer
        // at three quarters of the true stride, which shears every row a
        // little further than the last and produces the fine vertical
        // banding across the whole image.
        WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&pf))) break;

        const bool wants24 =
            (memcmp(&pf, &GUID_WICPixelFormat24bppBGR, sizeof(pf)) == 0);
        const bool wants32 =
            (memcmp(&pf, &GUID_WICPixelFormat32bppBGRA, sizeof(pf)) == 0);

        if (wants32) {
            if (FAILED(frame->WritePixels((UINT)j.h, (UINT)j.w * 4,
                                          (UINT)j.rgba.size(),
                                          const_cast<BYTE*>(j.rgba.data()))))
                break;
        } else if (wants24) {
            // Repacked to three bytes per pixel, dropping the alpha the
            // desktop leaves meaningless anyway.
            std::vector<uint8_t> tight((size_t)j.w * j.h * 3);
            for (int y = 0; y < j.h; ++y) {
                const uint8_t* src = j.rgba.data() + (size_t)y * j.w * 4;
                uint8_t* dst = tight.data() + (size_t)y * j.w * 3;
                for (int x = 0; x < j.w; ++x) {
                    dst[x * 3 + 0] = src[x * 4 + 0];
                    dst[x * 3 + 1] = src[x * 4 + 1];
                    dst[x * 3 + 2] = src[x * 4 + 2];
                }
            }
            if (FAILED(frame->WritePixels((UINT)j.h, (UINT)j.w * 3,
                                          (UINT)tight.size(), tight.data())))
                break;
        } else {
            // Some other format entirely. Writing the wrong layout produces
            // a corrupt file that looks plausible at a glance, which is
            // worse than no file, so this stops instead.
            break;
        }

        if (FAILED(frame->Commit())) break;
        if (FAILED(enc->Commit())) break;
        ok = true;
    } while (false);

    if (props)  props->Release();
    if (frame)  frame->Release();
    if (enc)    enc->Release();
    if (stream) stream->Release();
    return ok;
}

bool CaptureWriter::writeLabel(const Job& j, const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;

    // YOLO format: class, then centre and size as fractions of the image.
    // Detections are in screen coordinates, so shift into the crop first.
    for (const Det& d : j.dets) {
        // Shifted by the crop as well as the region, or every box would be
        // offset by however much was trimmed and the labels would describe
        // an image that was never written.
        const float x1 = d.x1 - j.roiLeft - j.cropX;
        const float y1 = d.y1 - j.roiTop - j.cropY;
        const float x2 = d.x2 - j.roiLeft - j.cropX;
        const float y2 = d.y2 - j.roiTop - j.cropY;

        // Clip to the crop: a box hanging off the edge would otherwise be
        // written with coordinates outside 0..1, which most loaders reject.
        const float cx1 = std::clamp(x1, 0.0f, (float)j.w);
        const float cy1 = std::clamp(y1, 0.0f, (float)j.h);
        const float cx2 = std::clamp(x2, 0.0f, (float)j.w);
        const float cy2 = std::clamp(y2, 0.0f, (float)j.h);
        if (cx2 - cx1 < 2.0f || cy2 - cy1 < 2.0f) continue;

        const float cx = ((cx1 + cx2) * 0.5f) / j.w;
        const float cy = ((cy1 + cy2) * 0.5f) / j.h;
        const float w  = (cx2 - cx1) / j.w;
        const float h  = (cy2 - cy1) / j.h;

        char line[160];
        snprintf(line, sizeof(line), "%d %.6f %.6f %.6f %.6f\n",
                 d.cls, cx, cy, w, h);
        f << line;
    }
    return true;
}

} // namespace lc
