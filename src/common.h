// common.h -- shared configuration, per-stage timing, and the error log.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
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
    // Device time for everything the frame queues ahead of the model: the
    // CUDA map of the captured texture, the letterbox kernel, and whatever
    // else shares the stream. It used to be the duration of an asynchronous
    // launch call, which measured nothing.
    ST_PRE,
    ST_INFER,        // TensorRT enqueue + sync, as the thread experiences it
    ST_GPU,          // the same work as the GPU measures it
    ST_POST,         // NMS decode + device->host of the box list
    ST_PUBLISH,      // hand results to the UI / overlay
    ST_CONTROL,      // target selection, smoothing, and output
    ST_TOTAL,        // whole callback
    ST_CADENCE,      // wall time between consecutive FrameArrived events
    // The same GPU work, split by whether another application had the
    // foreground. A GPU is shared between processes and the one in front
    // gets served first, so these two answer a question no single number
    // can: is the work slower, or is it waiting.
    ST_GPU_FG,       // loopcore was the foreground window
    ST_GPU_BG,       // something else was
    ST_COUNT
};

inline const char* stage_name(int s) {
    static const char* n[ST_COUNT] = {
        "arrive", "roi copy", "cuda map", "preprocess",
        "inference", "gpu only", "postprocess", "publish", "control",
        "TOTAL", "frame cadence",
        "gpu, ours in front", "gpu, other in front"
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
    std::atomic<uint64_t> idle{0};          // arrivals skipped, nothing wanted output
    std::atomic<uint64_t> errors{0};

    void reset() {
        frames = arrivals = skipped = detections = empty = idle = errors = 0;
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
    // Mirrors every entry to disk from here on. A log that only lives in
    // memory is worthless the moment the process dies.
    void setFile(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        file_.open(path, std::ios::out | std::ios::trunc);
        if (file_) file_ << "loopcore log\n" << std::flush;
    }

    void add(LogLevel lv, std::string text) {
        std::lock_guard<std::mutex> lk(m_);
        const double t = elapsed();
        if (file_) {
            const char* tag = lv == LOG_ERROR ? "ERROR" : lv == LOG_WARN ? "WARN " : "info ";
            file_ << tag << " " << t << "  " << text << "\n";
            // Flushed on anything that might precede a crash, and otherwise
            // at most a few times a second.
            //
            // Flushing every entry meant a synchronous disk write, holding
            // this mutex, on whatever thread happened to log -- including
            // the control thread. A slow write there is a stalled control
            // loop, which is felt directly as the output hitching.
            const int64_t nowNs = now_ns();
            if (lv != LOG_INFO || nowNs - lastFlush_ > 250'000'000LL) {
                file_ << std::flush;
                lastFlush_ = nowNs;
            }
        }
        if (entries_.size() >= 500) entries_.pop_front();
        entries_.push_back({t, lv, std::move(text)});
        dirty_ = true;
    }

    void flush() {
        std::lock_guard<std::mutex> lk(m_);
        if (file_) file_ << std::flush;
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
    std::ofstream        file_;
    int64_t              lastFlush_ = 0;
};

// --------------------------------------------------------------- config

enum RoiMode { ROI_CENTER = 0, ROI_MOUSE };

// A trace indexed by time rather than by sample.
//
// Both graphs previously walked a ring of the last N samples. That looks
// wrong for a reason worth stating: N samples is a different amount of time
// on every frame, so the trace stretches and compresses as the frame rate
// moves, and a column can hold four samples one moment and one the next. The
// result jitters horizontally even when the data is perfectly steady.
//
// Fixed time buckets fix that. Each bucket owns a known slice of wall clock,
// so a column always means the same duration and the trace scrolls at a
// constant rate. Keeping the minimum and maximum per bucket as well as the
// mean means a spike shorter than one bucket is still visible rather than
// averaged into nothing.
class TimeSeries {
public:
    static constexpr int64_t kBucketNs = 16'000'000;   // 16 ms
    static constexpr int     kBuckets  = 2048;         // about 33 s

    struct Bucket { float mn, mx, sum; int n; };

    void add(double value, int64_t nowNs) {
        std::lock_guard<std::mutex> lk(m_);
        const int64_t idx = nowNs / kBucketNs;
        if (idx != head_) {
            // Buckets between the last write and now saw no samples. They
            // are cleared rather than left holding stale values, so a gap in
            // the data reads as a gap.
            const int64_t skip = (head_ == 0) ? kBuckets
                                              : std::min<int64_t>(idx - head_, kBuckets);
            for (int64_t i = 1; i <= skip; ++i)
                b_[(size_t)(((head_ + i) % kBuckets + kBuckets) % kBuckets)] =
                    Bucket{0, 0, 0, 0};
            head_ = idx;
        }
        Bucket& b = b_[(size_t)((idx % kBuckets + kBuckets) % kBuckets)];
        const float v = (float)value;
        if (b.n == 0) { b.mn = b.mx = v; b.sum = v; b.n = 1; }
        else {
            b.mn = std::min(b.mn, v);
            b.mx = std::max(b.mx, v);
            b.sum += v;
            ++b.n;
        }
    }

    struct Column { float mn, mx, mean; bool has; };

    // The last `spanSec` of history, resampled into `cols` columns. Column
    // zero is the oldest. Columns with no data are marked, so the drawing
    // can break the line rather than invent a value.
    void sample(float spanSec, int cols, std::vector<Column>& out) const {
        // Never more columns than there are buckets to fill them.
        //
        // Asking for four hundred columns from a span holding two hundred
        // and fifty buckets leaves most of them empty, and the drawing
        // correctly shows a gap wherever there is no data -- so a perfectly
        // healthy trace came out as isolated marks with holes between them.
        // The resolution of the data is what it is; the graph should not
        // pretend to more.
        // At most one column per two buckets.
        //
        // A column per bucket holds three or four frames, and the spread of
        // three or four frames is mostly the difference between one frame
        // and the next -- noise, drawn at full height. Two buckets per
        // column halves the number of marks and lets each describe a period
        // long enough to mean something.
        const int avail = std::max(2,
            (int)(spanSec * 1e9 / (double)kBucketNs) / 2);
        cols = std::clamp(cols, 1, avail);
        out.assign((size_t)cols, Column{0, 0, 0, false});
        std::lock_guard<std::mutex> lk(m_);
        if (head_ == 0) return;

        const int64_t want = std::max<int64_t>(2,
            (int64_t)(spanSec * 1e9 / (double)kBucketNs));
        const int64_t span = std::min<int64_t>(want, kBuckets - 1);
        const int64_t first = head_ - span;

        for (int c = 0; c < cols; ++c) {
            const int64_t a = first + (int64_t)c * (span + 1) / cols;
            const int64_t z = std::max(a + 1,
                              first + (int64_t)(c + 1) * (span + 1) / cols);
            Column col{0, 0, 0, false};
            float sum = 0.0f;
            int n = 0;
            for (int64_t i = a; i < z; ++i) {
                if (i < 0) continue;
                const Bucket& b = b_[(size_t)((i % kBuckets + kBuckets) % kBuckets)];
                if (b.n == 0) continue;
                if (!col.has) { col.mn = b.mn; col.mx = b.mx; col.has = true; }
                else { col.mn = std::min(col.mn, b.mn); col.mx = std::max(col.mx, b.mx); }
                sum += b.sum;
                n += b.n;
            }
            if (col.has) col.mean = sum / (float)std::max(1, n);
            out[(size_t)c] = col;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& b : b_) b = Bucket{0, 0, 0, 0};
        head_ = 0;
    }

private:
    mutable std::mutex m_;
    std::array<Bucket, kBuckets> b_{};
    int64_t head_ = 0;
};

// The action enums live here, beside the Config fields that store them.
//
// They were in control.h, which store.cpp does not include -- so the loader
// could not name the bounds of the very fields it was clamping and had to be
// given literals instead. Any enum a setting is stored as belongs next to
// the setting, or the two drift apart the first time one of them grows.
// What condition fires the action.
enum ActionTrigger {
    TrigWithinPixels = 0,  // aim is within N px of the aim point
    TrigInsideBox,         // aim is anywhere inside the box
    TrigInsideBoxScaled,   // inside a shrunk or grown version of the box
    TrigAcquired,          // a target appeared where there was none
    TrigLost,              // the tracked target went away
    TrigSettled,           // within range and no longer closing on it
    TrigCount
};

// What it does when it fires. Mouse buttons only.
//
// Prefixed to keep them clear of ActivationMode, which already owns ActHold
// and ActToggle for a different meaning entirely -- one is how the user asks
// for aim, the other is how a button is pressed. Unscoped enums share a
// namespace, so the names had to differ rather than merely the types.
enum ActionKind {
    ActionClick = 0,      // one press and release
    ActionDoubleClick,    // two, spaced
    ActionHold,           // held down for as long as the condition holds
    ActionToggleTap,      // one press and release per entry into the condition
    ActionKindCount
};


struct Config {
    // region of interest -- always square, so one dimension describes it
    int   roiMode  = ROI_CENTER;
    int   fov      = 640;
    // Offsets as a percentage of screen width/height, so a saved profile
    // lands in the same place on a different monitor.
    float roiOffXPct = 0.0f;
    float roiOffYPct = 0.0f;

    // detection
    float confThresh = 0.45f;
    int   maxDets    = 100;

    // overlay
    bool  overlayOn      = false;
    bool  drawRoiRect    = false;
    bool  drawLabels     = true;
    bool  drawConfidence = true;
    bool  drawCenterDot  = false;
    bool  drawConfText   = true;      // score printed above each box
    int   boxThickness   = 2;
    int   fovThickness   = 1;
    float boxColor[3]    = {0.15f, 1.0f, 0.35f};
    float roiColor[3]    = {1.0f, 0.55f, 0.10f};
    float overlayAlpha   = 0.85f;
    float overlayMinConf = 0.35f;

    // control / general
    // Master switch for the model itself. Nothing captures or infers while
    // this is off.
    // Whether the model runs at all. No longer a setting anyone sees.
    //
    // Two switches for one intention was a false choice: nobody wants the
    // model running with control off, and control cannot do anything with
    // the model off. It is now derived from controlEnabled and kept only
    // because the pipeline, the tuner and the diagnostics all ask about it
    // separately, and collapsing those into one flag would tangle the
    // meaning of "is the model loaded and running" with "should output be
    // sent".
    bool  aiEnabled        = true;
    bool  controlEnabled   = false;

    // Two separate things that were once conflated, to bad effect.
    //
    // responseScale converts screen pixels into output units and is always
    // needed; getting it wrong scales every move. autoResponse measures it
    // by watching how the world reacts, otherwise the manual value is used.
    bool  autoResponse     = true;
    float responseScale    = 1.0f;
    // Whether movement already sent but not yet seen is subtracted from the
    // plan. Independent of the scale above.
    // Off by default. It changes how the loop behaves in a way that is hard
    // to reason about while tuning anything else.
    bool  lagCompOn        = false;

    int   deadzonePx       = 3;

    bool  safetyOn         = true;     // master switch for the limits below
    // Safety limits. Every one of these exists because an unbounded value
    // somewhere upstream can turn one bad frame into a movement of several
    // hundred pixels.
    float maxSpeedPx       = 2500.0f;  // velocity estimate ceiling, px/s
    // How fast the aim itself may travel, in pixels a second.
    //
    // Kept apart from maxSpeedPx, which bounds how fast a *target* is
    // believed to be moving. Those are different quantities that happened to
    // share a number, and using the target ceiling to limit output meant the
    // sensitivity slider stopped having any effect above about 0.45 -- every
    // tick was already at the cap.
    //
    // The default is high enough not to bind during ordinary aiming; it
    // exists to stop a collapsed response estimate throwing the view across
    // the screen, and that failure is an order of magnitude beyond this.
    float maxAimSpeedPx    = 24000.0f;
    float maxStepPx        = 45.0f;    // most one output tick may move
    int   staleMs          = 180;      // a detection older than this is dropped
    float jumpRejectPx     = 260.0f;   // an observation further than this is
                                       // a different object, not motion
    // 0 hold the key, 1 the key toggles it, 2 always on while enabled
    int   activationMode   = 0;
    int   activationKey    = 0x02;
    // A second key that does the same thing. Zero means unset.
    //
    // Either one engages: two hands, two grips, or a thumb button that is
    // awkward in some stances and fine in others. They are equals rather
    // than a primary and a fallback, because there is no case where one
    // should work and the other should not.
    int   activationKey2   = 0;    // VK_RBUTTON
    float sensitivity      = 0.35f;   // how quickly a planned move is paid out

    // Ease off on smaller targets.
    //
    // Box height is a proxy for distance: a target twice as far away is half
    // as tall. A correction that feels right up close is then far too eager
    // at range, because the same number of screen pixels covers much more of
    // the world -- so the aim overshoots a distant target and hunts around
    // it. Scaling the approach rate by apparent size makes the behaviour
    // consistent at any distance rather than tuned for one.
    bool  boxScaleOn       = false;
    // The box height at which sensitivity is left alone. Anything smaller is
    // reduced; anything larger is not increased, since being too eager up
    // close is its own problem.
    float boxScaleRefPx    = 140.0f;
    // How sharply it falls away, on a plain 0 to 1 scale.
    //
    // Stored the way it is shown rather than as the exponent it becomes: an
    // exponent is meaningful to the maths and to nobody else, and a setting
    // whose stored value differs from its displayed one is a trap the next
    // person to read the config falls into.
    float boxScaleStrength = 0.35f;
    // The floor, as a sensitivity value rather than a percentage.
    //
    // Expressed in the same units as Sensitivity so the two can be compared
    // directly -- "never slower than 0.20" is a statement someone can check
    // against the number right above it, where "never below 25%" is a
    // quantity they have to work out first.
    float boxScaleFloor    = 0.15f;
    // Where inside the box to aim, as a share of its own size. Positive Y is
    // upward, so +40 means 40% of the box height above its centre.
    float aimOffXPct       = 0.0f;
    float aimOffYPct       = 0.0f;
    // Which part of the box the aim is measured from. A detector's box
    // edges rarely wobble equally: on a person the top is usually steady
    // while the bottom moves with the feet, and the centre inherits half of
    // that as vertical bobbing on a target that has not moved.
    // 0 centre, 1 top edge, 2 bottom edge, 3 whichever edge is steadier.
    //
    // Centre by default. Anchoring to an edge fixes vertical bobbing on a
    // stationary target, but it also moves the aim point around as the box
    // reshapes, which showed up as prediction leading worse than it had. The
    // fix stays available; it is no longer imposed.
    int   boxAnchor        = 0;

    // A second, smaller region. Detection still uses the full field of
    // view; movement only fires once a box reaches this one.
    // On by default. It was never the cause of the aim not moving -- that
    // was a capture fault elsewhere -- and turning it off here was a wrong
    // conclusion from a coincidence.
    bool  actionFovOn      = true;
    int   actionFov        = 150;

    // Class filtering and ordering. The engine reports class ids only, so
    // these are indices; names come from a sidecar file when one exists.
    bool     classFilterOn  = false;
    uint32_t classMask      = 0xFFFFFFFFu;   // which classes may be targeted
    int      classOrder[32] = {0};           // preferred order, class ids
    int      classOrderCount = 0;

    int   targetPriority   = 0;
    // Prefer whatever is already being tracked, whichever priority is set.
    //
    // Sticky used to be a priority of its own, which meant choosing it gave
    // up every other way of picking a target. It is really a modifier: hold
    // what you have, and use the chosen rule only when deciding what to hold.
    bool  stickyTarget     = false;
    // How much a held target is favoured, in pixels of apparent closeness.
    float stickyBiasPx     = 90.0f;
    int   controlRateHz    = 500;     // output tick rate, independent of frames

    // On by default. Off, the aim never leads anything, which is not a
    // sensible starting point for a loop whose whole difficulty is latency.
    // Off by default. Leading a target is a real gain when it is tuned and a
    // source of overshoot when it is not, so it is something to switch on
    // deliberately rather than something to discover fighting.
    bool  predictionOn     = false;
    // 0 linear, 1 smoothed velocity, 2 acceleration, 3 alpha-beta filter
    // Edge consensus by default: detector boxes breathe, and the other
    // estimators read that breathing as velocity.
    // Kalman. Edge consensus was the default, and it is the wrong choice
    // for leading: its purpose is suppressing apparent motion on a target
    // that has not moved, so it starts by disbelieving exactly the signal
    // prediction needs. Consensus is still the right pick when boxes breathe
    // badly on a stationary target.
    int   predictionMethod = 5;
    // Deadband as a percentage of box size. Jitter scales with the box, so
    // a fixed pixel threshold is wrong at both ends of the range.
    float jitterReject     = 2.0f;
    // Kalman tuning. Process noise is how much the target may accelerate
    // between frames; measurement noise is how much the detector wobbles.
    // Their ratio decides whether the filter believes the model or the
    // measurement.
    // How much of the compensation term to treat as uncertainty.
    //
    // Recovering world motion means adding our own movement back, and the
    // result is dominated by the error in the scale rather than by the
    // target. Residuals below this share of the correction are not treated
    // as motion. Too low and a still target appears to drift the way the aim
    // approached it; too high and slow real movement is ignored.
    float selfMotionFloor  = 12.0f;   // per cent
    // Time constant on the velocity that feeds the lead, in milliseconds.
    //
    // The lead is a position offset proportional to velocity, so any noise
    // in the velocity becomes noise in where the aim is pointed -- and the
    // aim chasing its own jitter is what orbiting looks like. Filtering here
    // costs a little responsiveness on a genuine change of direction and
    // buys a lead that holds still.
    float leadSmoothMs     = 90.0f;

    // Respond to what the user's own hand is doing.
    //
    // Moving toward the target and being pulled toward it at the same time
    // overshoots; moving away and being pulled back is the aim fighting the
    // hand. Scaling the pull by whether the two agree makes it feel like
    // assistance rather than a second hand on the mouse.
    bool  userAssistOn     = false;
    float assistWithPct    = 140.0f;  // gain when the hand agrees
    float assistAgainstPct = 35.0f;   // gain when it opposes
    float assistSpeedRef   = 600.0f;  // hand speed at which the effect is full

    // Draw a marker where the lead is aiming.
    // Draws the path the aim has recently taken, coloured by speed.
    bool  drawPath         = false;
    bool  drawLeadDot      = false;

    // How strongly to suppress a lead on the minor axis.
    //
    // A target crossing horizontally has essentially no vertical motion, so
    // any vertical lead is noise -- and a lead is a position offset, so that
    // noise lands straight on the aim as bobbing. Movement is almost always
    // dominated by one axis, which makes the minor one safe to distrust.
    float minorAxisCut     = 70.0f;   // per cent

    // Lead sideways only.
    //
    // Vertical box edges move with the detector far more than with the
    // target: a box breathing by a few pixels between frames is read as
    // vertical velocity, and a lead multiplies that into a visible offset.
    // Most targets that matter move mainly sideways anyway, so discarding
    // the vertical component outright removes a whole class of misbehaviour
    // and costs very little.
    bool  leadHorizontalOnly = true;

    // Cut the lead when the velocity it multiplies is not steady.
    bool  leadTrustOn      = true;
    // Widen the steadying window in step with the horizon.
    bool  leadSmoothScales = true;

    float kalmanProcess    = 900.0f;
    float kalmanMeasure    = 12.0f;
    // 70 ms: roughly the capture-to-screen latency this pipeline actually
    // has, which is the amount of lead that cancels it rather than adds to
    // it. 40 was a conservative guess made before any of it was measured.
    float predictionMs     = 70.0f;   // how far ahead to lead, up to 1000
    float predictionGain   = 1.00f;   // scales the lead

    bool  emaOn            = false;
    // How the aim travels, as distinct from how fast. See paths.h.
    int   movePath         = 0;
    // What the path's own parameter means varies by path; the card explains
    // it per selection rather than pretending one label fits all fifteen.
    float movePathAmount   = 0.5f;
    // Scale the path's character with how far there is to go.
    bool  movePathRamp     = true;
    // Separate per axis. Horizontal motion is usually real and wants little
    // filtering; vertical is mostly the box breathing and wants more.
    float emaIntensity     = 0.65f;   // 0 none, 1 almost frozen -- horizontal
    float emaIntensityY    = 0.65f;

    int   inputMethod      = 0;       // 0 mouse, 1 arduino
    int   serialPort       = 5;       // COMn
    int   baudRate         = 115200;
    // Packets per second to the board. A 32u4 also doing HID work cannot
    // keep up with the control loop's full rate.
    int   serialRateHz     = 500;
    // 0 loopcore's binary packet, 1 ASCII "x,y,click". The prebuilt
    // community hex files speak ASCII.
    int   serialProtocol   = 1;   // ASCII by default: the prebuilt hex speaks it
    // Only accept activation clicks that came through the board. Without
    // this, any mouse on the desk can trigger it.
    bool  arduinoOnlyInput = true;
    // Which physical device counts as the board. A path fragment, learned
    // by clicking rather than guessed from a vendor id.
    char  boardDevice[180] = {0};
    int   boardType        = 0;   // Leonardo / Micro / Pro Micro
    // Build the firmware with USB Host Shield support. This is also what
    // powers the shield's downstream port, so a pass-through mouse is dead
    // without it.
    bool  fwPassthrough    = false;
    int   fwMouseLayout    = 0;      // 0 guess, else an explicit layout
    bool  fwDumpReports    = false;  // print raw HID reports back over serial
    bool  tooltipsOn       = true;
    // Show the cards that only matter once someone is measuring things.
    //
    // Off, a good deal is hidden. Nothing is disabled by hiding it and no
    // setting is touched, so turning this on restores exactly what was
    // there: the flag only decides what is drawn.
    bool  advancedMode     = false;    // inline descriptions under controls
    int   uiTheme          = 0;
    // How surfaces are drawn, kept apart from the palette so a dark theme
    // can be had with any of them.
    int   uiStyle          = 0;

    // Performance readout that clips to a screen corner.
    bool  perfMonOn        = true;
    int   perfMonEdge      = 0;       // 0 top, 1 right, 2 bottom, 3 left
    float perfMonEdgeT     = 1.0f;    // position along that edge, 0 to 1
    float perfGraphSecs    = 4.0f;    // span of the mini graph
    // Override what the screen size is taken to be.
    //
    // Everything positional is computed against this: the region, the
    // overlay, the previews. It should never be needed -- the capture item
    // reports the truth -- but when detection is wrong there is otherwise no
    // way out of it, and being stuck is worse than being wrong.
    // Which display to capture. Not the same as which one the panel is on.
    // -1 means "the primary one", resolved at startup, since enumeration
    // order is not guaranteed to put the primary first.
    int   captureMonitor   = -1;

    bool  resOverride      = false;
    int   resOverrideW     = 2560;
    int   resOverrideH     = 1440;

    bool  perfMonMeters    = true;    // cpu, gpu and memory bars
    float perfMonScale     = 1.0f;    // size, on top of the display scale
    int   perfMonDisplay   = -1;      // which monitor it sits on, -1 primary

    // One switch for every window loopcore owns: the panel, the overlay,
    // the region outline and the readout. Off, none of them appear in a
    // recording or in the frames the model sees. On a single monitor that is
    // the difference between being able to test and not.
    bool  visibleToCapture = false;


    // Card order per tab, as indices into each tab's declared card list.
    // Tabs: 0 General, 1 Visual, 2 Capture, 3 Settings.
    // Where the panel was and what it was showing. Saved in real pixels
    // together with the display scale it was measured at, so restoring onto
    // a differently scaled monitor puts it back at the same apparent size
    // rather than the same pixel count.
    int   windowX          = 140;
    int   windowY          = 140;
    int   windowW          = 705;
    int   windowH          = 585;
    float windowScale      = 1.0f;
    bool  windowMaximised  = false;
    int   activeTab        = 0;

    // Fire a mouse button when something happens.
    //
    // Deliberately narrow: the trigger is a geometric fact about the target,
    // and the output is a mouse button. Anything wider would be a general
    // macro engine, which is a different program.
    bool  actionOn         = false;
    int   actionTrigger    = 0;     // ActionTrigger
    int   actionKind       = 0;     // ActionKind
    int   actionButton     = 0;     // 0 left, 1 right, 2 middle
    int   actionRadiusPx   = 8;     // for the within-pixels trigger
    // How much of the box counts, as a percentage of its size. Below a
    // hundred shrinks it toward the aim point, above grows it.
    float actionBoxPct     = 60.0f;
    // Only while the activation key is held. On by default, because an
    // action that fires when the user is not asking for anything is
    // surprising in a way nothing else here is.
    // Its own key, so the action can be asked for separately from the aim.
    //
    // Zero means "whatever the activation key is", which is the common case
    // and saves keeping two settings in step. A distinct key lets someone
    // aim without firing and fire without aiming.
    int   actionKey        = 0;
    bool  actionNeedsKey   = true;
    float actionMinConf    = 0.5f;  // ignore targets below this
    int   actionHoldMs     = 0;     // condition must persist this long
    int   actionCooldownMs = 250;   // shortest gap between firings
    int   actionGapMs      = 40;    // press-to-release, and between double clicks

    // Card layout, per tab: General, Visual, Capture, Settings, Debug, Model.
    static constexpr int kOrderTabs  = 6;
    // Which cards are folded away, one bit per card index.
    unsigned int cardCollapsed[kOrderTabs] = {};
    // How many cards sit in the left column. -1 means split evenly.
    int   cardSplit[kOrderTabs] = {-1, -1, -1, -1, -1, -1};
    static constexpr int kOrderMax   = 12;
    int   cardOrder[kOrderTabs][kOrderMax] = {};
    int   cardOrderCount[kOrderTabs] = {};

    // ---- dataset capture -------------------------------------------------
    // Master switch. Off, the hotkey does nothing at all, which is what
    // stops an activation key doubling as a capture trigger by accident.
    bool  captureEnabled    = false;
    // Burst on loss by default: the frames worth keeping are the ones the
    // model got wrong, and a timed capture spends most of its disk on frames
    // it already handles correctly.
    int   captureMode       = 2;      // timed / on detection / burst on loss
    int   captureDelayMs    = 200;
    // Off by default: the labels are only as good as the model that made
    // them, and a set of confidently wrong ones is worse than none.
    bool  captureLabels     = false;
    // One folder per launch, named for whatever was in front. Disabling
    // capture and re-enabling it later keeps writing to the same one; only
    // restarting the program starts a new folder.
    bool  captureSessionFolders = true;
    // Record conditions alongside timings, into bin/logs.
    bool  sessionTracking   = true;
    int   captureFormat     = 0;      // 0 png, 1 jpeg
    int   captureQuality    = 100;    // jpeg only
    // The size images are written at, independent of the region the model
    // looks at. A dataset wants a consistent size across sessions; the field
    // of view is a tuning setting and changes freely.
    int   captureSize       = 640;
    char  capturePath[400]  = {0};    // empty means bin\captures
    // F8, deliberately not the activation key.
    //
    // Sharing it means every engagement starts writing images: a readback of
    // the frame off the GPU, an encode, and a disk write, all while the loop
    // is trying to be quick. The latency climbs the longer the key is held.
    // The Capture tab offers to match the activation key for anyone who
    // wants that, with the cost stated.
    int   captureKey        = 0x77;

    // Burst-on-loss ladder, as a list of rungs the user defines rather than
    // a fixed three. Each rung says: at or above this confidence, capture
    // this often. An interval of zero means capture nothing at that
    // confidence, which is how the top rung skips frames the model already
    // handles. Rungs are kept in descending confidence order.
    static constexpr int kLadderMax = 8;
    int   ladderCount       = 3;
    // Confident detections are captured rarely and marginal ones often: a
    // set the model already handles teaches it little, whereas the cases it
    // is unsure about are the ones worth having more of.
    float ladderConf[kLadderMax] = {0.30f, 0.20f, 0.01f};
    int   ladderMs[kLadderMax]   = {1000,  200,   100};

    // After a target that was really there disappears, capture this many
    // frames this fast. These are the examples a model got wrong.
    int   burstFrames       = 6;
    int   burstMs           = 25;

    // Forced decode layout, 0 = detect from the tensor shapes.
    int   outputLayout     = 0;

    // pipeline
    bool  pipelineOn     = true;

    // Keep inferring even when nothing needs the result.
    //
    // Counter-intuitive but measurable: a card fed short bursts of work
    // never raises its clocks, so every burst runs at a fraction of the
    // boost speed. Holding it busy keeps the clocks up and each inference
    // then finishes sooner than it did when the card was mostly idle. On by
    // default because the latency it buys is worth more here than the power
    // it costs. Debug, Resources shows the clock either way.
    // Off by default, and the honest reason is that it is the worse of two
    // ways to solve the same thing.
    //
    // Holding a card's clocks up requires giving it real work, and the loop
    // that finds "just enough" settles at something like half the card's
    // time spent on inferences nobody asked for. That competes with the
    // application in front, which is the very cost it was meant to avoid.
    // Setting the NVIDIA power mode to prefer maximum performance holds the
    // same clocks for free, so this exists only for machines where that is
    // not an option.
    bool  keepGpuBoosted = false;
    // Whether the thread sleeps rather than spins while the GPU works.
    //
    // Read straight from the config file at startup, before any CUDA context
    // exists, because the flag only applies to a context created afterwards.
    bool  yieldWhileWaiting = true;

    // Give frames back to the foreground application when it is struggling.
    //
    // The only lever that actually returns performance is doing less work.
    // Below the floor, inference runs on every second or third arrival
    // instead of every one; detections then arrive less often, which the
    // controller already handles because it interpolates between them.
    bool  adaptiveLoad   = false;
    int   fpsFloor       = 90;    // frames per second to protect
    int   maxStride      = 3;     // most arrivals to skip between inferences

    // misc
    bool  alwaysOnTop    = false;
    // 0 normal, 1 above normal, 2 high. High can starve the desktop, so it
    // is never the default.
    int   priorityMode   = 0;
    int   engineW        = 640;   // build target for model conversion
    int   engineH        = 640;
    bool  buildFp16      = true;
    // Ask ultralytics to embed NMS in the exported graph. Convenient, but
    // the ONNX NMS op often forces surrounding layers back to FP32, which
    // can cost more than the CPU-side NMS it saves.
    bool  buildEmbedNms  = true;
    // Build for other cards as well as this one. Off, and not recommended:
    // it costs speed and still cannot reach Turing or older.
    bool  buildPortable  = false;
    int   workspaceMB    = 2048;
    // How hard TensorRT searches for kernels, 0 to 5.
    //
    // Every build so far used the default of 3, because this was never set.
    // It decides whether a layer gets a tuned fp16 tensor-core kernel or a
    // generic one, which is the likeliest remaining explanation for an
    // engine built here running slower than the same model built elsewhere.
    int   buildOptLevel  = 5;
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
// The traces both graphs read. Written from the frame path, read from the
// UI, and safe for that because TimeSeries locks internally.
// What the foreground application is managing, and what we cost it.
//
// Its frame rate can be measured without touching it. Screen capture
// presents a frame when the desktop content changes, so the gap between
// arrivals is the interval at which that application is putting new frames
// on screen. No hooking, no injection, no reading another process.
//
// Two limits worth stating plainly. The compositor will not present faster
// than the display refreshes, so an application running above the refresh
// rate reads as the refresh rate. And a still screen presents nothing, so
// this only means anything while something is moving.
struct LoadWatch {
    // Frame intervals seen while the model was running, and while it was
    // not. The difference between the two is what loopcore costs.
    std::atomic<double> busyMs{0.0};
    std::atomic<double> idleMs{0.0};
    std::atomic<uint64_t> busyN{0};
    std::atomic<uint64_t> idleN{0};

    // How many arrivals are currently skipped between inferences. One means
    // every frame.
    std::atomic<int> stride{1};

    void note(double gapMs, bool modelRan) {
        // A rolling mean rather than a total, so a long session does not
        // drown a change that happened a minute ago.
        if (modelRan) {
            const double prev = busyMs.load();
            busyMs = (prev <= 0.0) ? gapMs : prev * 0.98 + gapMs * 0.02;
            busyN = busyN.load() + 1;
        } else {
            const double prev = idleMs.load();
            idleMs = (prev <= 0.0) ? gapMs : prev * 0.98 + gapMs * 0.02;
            idleN = idleN.load() + 1;
        }
    }

    double busyFps() const {
        const double v = busyMs.load();
        return v > 0.0 ? 1000.0 / v : 0.0;
    }
    double idleFps() const {
        const double v = idleMs.load();
        return v > 0.0 ? 1000.0 / v : 0.0;
    }
    // Percentage of frame rate lost, or zero when there is nothing to
    // compare against yet.
    double costPct() const {
        const double a = idleFps(), b = busyFps();
        if (a <= 1.0 || b <= 0.0 || idleN.load() < 60 || busyN.load() < 60)
            return 0.0;
        return std::max(0.0, 100.0 * (a - b) / a);
    }
    void reset() {
        busyMs = idleMs = 0.0;
        busyN = idleN = 0;
    }
};

struct Traces {
    TimeSeries loop;      // total loop time
    TimeSeries gpu;       // device time alone
    TimeSeries cadence;   // gap between frames
};

struct Shared {
    std::mutex   cfgMutex;
    Config       cfg;
    Timings      timings;
    Counters     counters;
    Traces       traces;
    LoadWatch    load;
    Log          log;
    Results      results;
    std::atomic<bool> running{true};

    Config snapshotConfig() {
        std::lock_guard<std::mutex> lk(cfgMutex);
        return cfg;
    }
};

} // namespace lc
