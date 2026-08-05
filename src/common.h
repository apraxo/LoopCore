// common.h -- shared configuration, per-stage timing, and the error log.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace lc {

// ---------------------------------------------------------------- clock

inline int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- stages

enum Stage {
    ST_ARRIVE = 0,   // FrameArrived -> we start work (queue delay)
    ST_COPY,         // CopySubresourceRegion of the ROI
    ST_MAP,          // CUDA map of the shared texture
    ST_PRE,          // letterbox + colour convert + normalise kernel
    ST_INFER,        // TensorRT enqueue + sync
    ST_POST,         // NMS decode + device->host of the box list
    ST_PUBLISH,      // hand results to the UI / overlay
    ST_TOTAL,        // whole callback
    ST_CADENCE,      // wall time between consecutive FrameArrived events
    ST_COUNT
};

inline const char* stage_name(int s) {
    static const char* n[ST_COUNT] = {
        "arrive", "roi copy", "cuda map", "preprocess",
        "inference", "postprocess", "publish", "TOTAL", "frame cadence"
    };
    return n[s];
}

// Fixed-capacity ring of durations in milliseconds.
class Ring {
public:
    static constexpr int CAP = 4096;

    void add(double ms) {
        std::lock_guard<std::mutex> lk(m_);
        buf_[head_ % CAP] = ms;
        ++head_;
    }

    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        head_ = 0;
    }

    // Copies out, then sorts a scratch vector. Called from the UI thread only.
    struct Summary {
        int    n    = 0;
        double mean = 0, p50 = 0, p90 = 0, p99 = 0, max = 0, last = 0;
    };

    Summary summary(std::vector<double>& scratch) const {
        Summary s;
        {
            std::lock_guard<std::mutex> lk(m_);
            int n = (int)std::min<uint64_t>(head_, CAP);
            if (n == 0) return s;
            scratch.assign(buf_.begin(), buf_.begin() + n);
            s.n    = n;
            s.last = buf_[(head_ - 1) % CAP];
        }
        std::sort(scratch.begin(), scratch.end());
        double sum = 0;
        for (double v : scratch) sum += v;
        auto pct = [&](double p) {
            size_t i = (size_t)(p * (scratch.size() - 1) + 0.5);
            return scratch[std::min(i, scratch.size() - 1)];
        };
        s.mean = sum / scratch.size();
        s.p50  = pct(0.50);
        s.p90  = pct(0.90);
        s.p99  = pct(0.99);
        s.max  = scratch.back();
        return s;
    }

    // Last N samples in chronological order, for the sparkline.
    void tail(std::vector<float>& out, int count) const {
        std::lock_guard<std::mutex> lk(m_);
        int n = (int)std::min<uint64_t>(head_, CAP);
        count = std::min(count, n);
        out.resize(count);
        for (int i = 0; i < count; ++i)
            out[i] = (float)buf_[(head_ - count + i) % CAP];
    }

private:
    mutable std::mutex   m_;
    std::array<double, CAP> buf_{};
    uint64_t             head_ = 0;
};

struct Timings {
    Ring stage[ST_COUNT];
    void reset() { for (auto& r : stage) r.reset(); }
};

// Scoped timer. Writes elapsed ms into a Ring on destruction.
class ScopedTimer {
public:
    explicit ScopedTimer(Ring& r) : r_(r), t0_(now_ns()) {}
    ~ScopedTimer() { r_.add((now_ns() - t0_) / 1e6); }
private:
    Ring&   r_;
    int64_t t0_;
};

// --------------------------------------------------------------- counters

struct Counters {
    std::atomic<uint64_t> frames{0};        // pipeline iterations completed
    std::atomic<uint64_t> arrivals{0};      // FrameArrived events seen
    std::atomic<uint64_t> skipped{0};       // arrivals dropped (pipeline busy)
    std::atomic<uint64_t> detections{0};    // total boxes emitted
    std::atomic<uint64_t> empty{0};         // iterations with zero boxes
    std::atomic<uint64_t> errors{0};

    void reset() {
        frames = arrivals = skipped = detections = empty = errors = 0;
    }
};

// ------------------------------------------------------------------ log

enum LogLevel { LOG_INFO = 0, LOG_WARN, LOG_ERROR };

struct LogEntry {
    double      t;       // seconds since start
    LogLevel    level;
    std::string text;
};

class Log {
public:
    void add(LogLevel lv, std::string text) {
        std::lock_guard<std::mutex> lk(m_);
        if (entries_.size() >= 500) entries_.pop_front();
        entries_.push_back({elapsed(), lv, std::move(text)});
        dirty_ = true;
    }
    void info(std::string s)  { add(LOG_INFO,  std::move(s)); }
    void warn(std::string s)  { add(LOG_WARN,  std::move(s)); }
    void error(std::string s) { add(LOG_ERROR, std::move(s)); }

    std::vector<LogEntry> snapshot() {
        std::lock_guard<std::mutex> lk(m_);
        dirty_ = false;
        return std::vector<LogEntry>(entries_.begin(), entries_.end());
    }
    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        entries_.clear();
    }
    bool dirty() const { return dirty_; }

private:
    double elapsed() const {
        static const int64_t t0 = now_ns();
        return (now_ns() - t0) / 1e9;
    }
    mutable std::mutex   m_;
    std::deque<LogEntry> entries_;
    std::atomic<bool>    dirty_{false};
};

// --------------------------------------------------------------- config

enum RoiMode { ROI_CENTER = 0, ROI_MOUSE };

struct Config {
    // region of interest
    int  roiMode   = ROI_CENTER;
    int  roiW      = 640;
    int  roiH      = 384;
    int  roiOffX   = 0;
    int  roiOffY   = 0;

    // detection
    float confThresh = 0.35f;
    int   maxDets    = 100;

    // overlay
    bool  overlayOn      = true;
    bool  drawRoiRect    = true;
    bool  drawLabels     = true;
    bool  drawConfidence = true;
    bool  drawCenterDot  = false;
    int   boxThickness   = 2;
    float boxColor[3]    = {0.15f, 1.0f, 0.35f};
    float roiColor[3]    = {1.0f, 0.55f, 0.10f};
    float overlayAlpha   = 0.85f;
    float overlayMinConf = 0.35f;

    // pipeline
    bool  useCudaGraph   = false;
    bool  pipelineOn     = true;
};

// Detection in *screen* pixel coordinates.
struct Det {
    float x1, y1, x2, y2;
    float score;
    int   cls;
};

// Snapshot of what the pipeline last produced. UI + overlay read this.
struct Results {
    std::mutex        m;
    std::vector<Det>  dets;
    int               roiL = 0, roiT = 0, roiR = 0, roiB = 0;
    int64_t           stamp = 0;
};

// Everything the UI and the pipeline both touch.
struct Shared {
    std::mutex   cfgMutex;
    Config       cfg;
    Timings      timings;
    Counters     counters;
    Log          log;
    Results      results;
    std::atomic<bool> running{true};

    Config snapshotConfig() {
        std::lock_guard<std::mutex> lk(cfgMutex);
        return cfg;
    }
};

} // namespace lc
