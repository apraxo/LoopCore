// sessiontrack.h -- what the program was doing, written down as it happens.
//
// The timing report answers "how fast is it now". This answers the question
// that keeps coming up instead: how fast was it *under what conditions*. A
// delay that only appears with the key held, or only with four targets on
// screen, or only when another window has focus, is invisible in a single
// set of percentiles because every one of those cases is averaged into the
// same numbers.
//
// So the same measurements are kept several times over, split by condition,
// and written out as plain text at the end of the run.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace lc {

// A running mean, spread and extremes. Cheap enough to keep dozens of.
struct Stat {
    void add(double v) {
        if (n == 0) { mn = mx = v; }
        else { mn = (v < mn) ? v : mn; mx = (v > mx) ? v : mx; }
        ++n;
        const double d = v - mean;
        mean += d / (double)n;
        m2 += d * (v - mean);
    }
    double sd() const { return n > 1 ? std::sqrt(m2 / (double)(n - 1)) : 0.0; }
    bool   any() const { return n > 0; }

    uint64_t n = 0;
    double mean = 0.0, m2 = 0.0, mn = 0.0, mx = 0.0;
};

// One sample of everything worth correlating against.
struct SessionSample {
    double loopMs = 0.0;
    double gpuMs = 0.0;
    double cadenceMs = 0.0;
    double inferMs = 0.0;
    // Device time for the capture map and the letterbox kernel -- everything
    // queued ahead of the model on the same stream. Reported because the
    // inference row sits several milliseconds above the gpu row and this is
    // the only thing that can account for the difference.
    double preMs = 0.0;
    // The stages between inference finishing and the frame being done.
    //
    // Four of nine stages were measured but never reported, and the gap
    // between the inference row and the loop row -- which is where the
    // delay actually grew -- fell entirely inside them. Reporting the whole
    // pipeline means the next question of this kind is answered by reading
    // the log rather than by reasoning about which setting changed.
    double arriveMs = 0.0;
    double copyMs = 0.0;
    double mapMs = 0.0;
    double postMs = 0.0;
    double publishMs = 0.0;
    double controlMs = 0.0;

    bool  engaged = false;        // activation key held or latched
    bool  ourWindowFocused = false;
    std::string foreground;       // process the user is actually in

    int   targets = 0;
    float topScore = 0.0f;

    bool  hasTarget = false;
    float errPx = 0.0f;           // distance left to the aim point
    float leadPx = 0.0f;          // how far the lead is displacing it
    float leadTrust = 1.0f;
    float responseScale = 1.0f;

    // What the application in front managed, and how thinned inference was.
    float foregroundFps = 0.0f;
    int   stride = 1;

    float gpuMHz = 0.0f, gpuMaxMHz = 0.0f;
    float cpuPct = -1.0f, gpuPct = -1.0f;
    bool  throttled = false;
};

class SessionTracker {
public:
    bool start(const Config& cfg, Log& log);
    void stop(const Config& cfg, Log& log);
    // Writes the file as the run proceeds, so a crash does not lose it.
    void flush(const Config& cfg);
    bool running() const { return running_.load(); }

    void sample(const SessionSample& s);
    void note(const std::string& event);

    // The process the user spent most of the run inside, and how long the
    // activation key was held. Both used to name the capture folder.
    std::string dominantForeground() const;
    double      engagedSeconds() const;
    std::string path() const;

    // The same measurements, gathered under one set of conditions.
    struct Bucket { Stat loop, gpu, cadence, infer, pre,
                           arrive, copy, map, post, publish, control,
                           err, lead; };

private:
    void writeOut(const Config& cfg, Log* log);

    mutable std::mutex m_;
    std::atomic<bool>  running_{false};
    std::string        path_;
    int64_t            startNs_ = 0;
    int64_t            lastNs_ = 0;
    int64_t            lastFlush_ = 0;
    // The settings block, rendered once and reused until something changes.
    uint64_t           settingsFp_ = 0;
    std::string        settingsText_;

    // The same timings, split by the conditions that were in force.
    Bucket idle_, held_;
    Bucket focusOurs_, focusOther_;
    Bucket noTarget_, oneTarget_, fewTargets_, manyTargets_;
    Stat   trust_, scale_, score_;
    Stat   gpuClock_;
    Stat   fgFpsBusy_, fgFpsIdle_;

    uint64_t samples_ = 0;
    uint64_t throttledSamples_ = 0;
    double   engagedSec_ = 0.0;

    std::vector<std::string> notes_;
    std::vector<std::pair<std::string, double>> foreground_;   // name, seconds
};

} // namespace lc
