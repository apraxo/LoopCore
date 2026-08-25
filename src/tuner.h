// tuner.h -- finds a gain by experiment rather than by feel.
//
// The method is a step response, the same one used to tune any servo loop.
// With a stationary target in view it pushes the aim off by a known amount,
// engages, and measures two things: how long the error takes to settle, and
// how far past the target it went on the way. A gain that settles fast but
// overshoots is not better than one that settles a little slower and stops
// where it was told, so the score weighs both.
//
// Everything here depends on the whole chain -- game sensitivity, output
// path, board packet rate, frame rate -- which is exactly why measuring it
// beats guessing.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace lc {

class Controller;

enum class TuneState { Idle, Running, Done, Failed };

struct TuneTrial {
    float gain        = 0.0f;
    float settleMs    = 0.0f;
    float overshootPx = 0.0f;
    float score       = 0.0f;   // lower is better
    bool  valid       = false;
};

class Tuner {
public:
    ~Tuner();

    bool start(Controller* ctrl, Config snapshot, Log& log);
    void cancel();

    TuneState state() const { return state_.load(); }
    bool      busy()  const { return state_.load() == TuneState::Running; }

    struct Status {
        std::string phase;
        float       progress = 0.0f;
        float       bestGain = 0.0f;
        float       bestSettleMs = 0.0f;
        float       bestOvershoot = 0.0f;
        int         trialsDone = 0;
        int         trialsTotal = 0;
        std::string note;
    };
    Status status() const;

    std::vector<TuneTrial> results() const;

    // Everything the run settled on. Applied by the caller in one go.
    struct Result {
        float sensitivity = 0.35f;
        int   deadzonePx  = 3;
        float emaIntensity = 0.0f;
        bool  emaOn       = false;
        bool  lagCompOn   = true;
    };
    bool takeResult(Result& out);

    // True while a run is inferring, so the pipeline stays awake even with
    // every overlay switched off.
    static bool WantsInference();

private:
    void run(Controller* ctrl, Config cfg, Log* log);
    bool runTrial(Controller* ctrl, const Config& cfg, float gain,
                  TuneTrial& out, Log& log);
    void setPhase(const std::string& phase, float progress,
                  const std::string& note = {});

    std::thread            worker_;
    std::atomic<TuneState> state_{TuneState::Idle};
    std::atomic<bool>      cancel_{false};
    std::atomic<bool>      haveResult_{false};
    Result                 result_;

    // Failure counts, so a run that goes wrong reports once with a number
    // rather than sixty identical lines.
    int noMoveCount_ = 0;
    int lostCount_ = 0;
    int outOfRegionCount_ = 0;

    mutable std::mutex m_;
    Status                 st_;
    std::vector<TuneTrial> trials_;
};

} // namespace lc
