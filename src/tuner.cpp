// tuner.cpp
#include "tuner.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>

#include "control.h"

namespace lc {

namespace {

// How far off target to push before each run. Big enough that the loop has
// real work to do, small enough that the target stays inside the region.
constexpr float kStepPx      = 140.0f;
constexpr float kSettlePx    = 6.0f;    // inside this counts as arrived
constexpr int   kHoldMs      = 70;      // and has to stay there this long
constexpr int   kTrialCapMs  = 1400;    // a gain that cannot finish by now
                                        // has already lost
constexpr int   kRepeats     = 2;       // per candidate, to average out noise
constexpr int   kCountdownS  = 5;       // time to get set before it starts

// Overshoot is weighted heavily on purpose. Arriving 30 ms sooner is worth
// very little if it costs 20 px of swing past the target, because that swing
// is what reads as the aim snapping around.
constexpr float kOvershootWeight = 12.0f;

void sleepMs(int ms) { Sleep((DWORD)ms); }

} // namespace

Tuner::~Tuner() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void Tuner::setPhase(const std::string& phase, float progress,
                     const std::string& note) {
    std::lock_guard<std::mutex> lk(m_);
    st_.phase = phase;
    st_.progress = progress;
    if (!note.empty()) st_.note = note;
}

Tuner::Status Tuner::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

std::vector<TuneTrial> Tuner::results() const {
    std::lock_guard<std::mutex> lk(m_);
    return trials_;
}

bool Tuner::takeResult(Result& out) {
    bool expected = true;
    if (!haveResult_.compare_exchange_strong(expected, false)) return false;
    std::lock_guard<std::mutex> lk(m_);
    out = result_;
    return true;
}

namespace { std::atomic<bool> g_wantsInference{false}; }
bool Tuner::WantsInference() { return g_wantsInference.load(); }

void Tuner::cancel() { cancel_ = true; }

bool Tuner::start(Controller* ctrl, Config snapshot, Log& log) {
    if (state_.load() == TuneState::Running) return false;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        trials_.clear();
        st_ = Status{};
    }
    state_ = TuneState::Running;
    worker_ = std::thread(&Tuner::run, this, ctrl, snapshot, &log);
    return true;
}

// --------------------------------------------------------------- one trial

bool Tuner::runTrial(Controller* ctrl, const Config& cfg, float gain,
                     TuneTrial& out, Log& log)
{
    out = TuneTrial{};
    out.gain = gain;

    // Let go of the target first, so the push is measured from rest.
    ctrl->setForceEngage(false);
    ctrl->setTuneGain(true, gain);
    ctrl->setTunePlain(true);
    sleepMs(120);

    auto st = ctrl->state();
    if (!st.hasTarget) {
        if (++lostCount_ == 1)
            log.warn("Lost the target between runs. It needs to stay "
                     "detected and still for the whole measurement.");
        return false;
    }

    // Convert the desired pixel step into output units using the measured
    // response. Without that this would push a wildly different distance on
    // every setup.
    const float scale = std::max(0.05f, ctrl->responseScale());
    const float units = kStepPx / scale;

    // Push away along X, then wait for the view and the detector to settle.
    ctrl->nudgeRaw(-units, 0.0f);
    sleepMs(260);

    st = ctrl->state();
    if (!st.hasTarget) {
        // Pushed it out of the region. Put it back and let the caller retry
        // with a smaller step rather than scoring a run that never happened.
        ctrl->nudgeRaw(units, 0.0f);
        sleepMs(200);
        if (++outOfRegionCount_ == 1)
            log.warn("The step is pushing the target out of the region. "
                     "A larger field of view, or a target nearer the middle, "
                     "gives the measurement room to work.");
        return false;
    }

    const float startErr = std::sqrt(st.errX * st.errX + st.errY * st.errY);
    if (startErr < kStepPx * 0.35f) {
        ctrl->nudgeRaw(units, 0.0f);
        sleepMs(160);
        // Counted rather than repeated. Sixty identical lines say nothing
        // that one line and a number does not.
        if (++noMoveCount_ == 1) {
            log.warn("The view is not moving when output is sent. The usual "
                     "cause is that loopcore has keyboard focus rather than "
                     "the game: most games ignore mouse input while another "
                     "window is focused, so click into the game during the "
                     "countdown and leave it focused for the whole run.");
        }
        return false;
    }
    const float sign = (st.errX < 0.0f) ? -1.0f : 1.0f;

    // --- engage and watch -------------------------------------------------
    // Sampled per detection, not per tick. Between frames the target has not
    // been re-observed, so the error is a stale number that says nothing
    // about whether the view has actually arrived. Reading it at tick rate
    // makes every gain look like it settles in a millisecond, which is what
    // made the search pick the slowest one.
    const int64_t t0 = now_ns();
    float peakPast = 0.0f;
    int64_t insideSince = 0;
    float settleMs = -1.0f;
    int64_t lastStamp = 0;
    int observations = 0;

    ctrl->setForceEngage(true);

    while (!cancel_.load()) {
        sleepMs(2);
        const int64_t now = now_ns();
        const float elapsedMs = (float)((now - t0) / 1e6);
        if (elapsedMs > kTrialCapMs) break;

        const auto s = ctrl->state();
        if (!s.hasTarget) continue;
        if (s.errStamp == lastStamp) continue;   // nothing new to judge on
        lastStamp = s.errStamp;
        ++observations;

        const float err = std::sqrt(s.errX * s.errX + s.errY * s.errY);

        // Past the target means the error changed sign along the axis we
        // pushed on. Measuring magnitude alone cannot tell overshoot from
        // undershoot.
        const float along = s.errX * sign;
        if (along < 0.0f) peakPast = std::max(peakPast, -along);

        if (err <= kSettlePx) {
            if (insideSince == 0) insideSince = now;
            else if ((now - insideSince) / 1'000'000 >= kHoldMs &&
                     observations >= 4) {
                // At least four observations before it counts as settled.
                //
                // Otherwise a run that never saw the transition at all --
                // because the output never reached the game, or the target
                // was already centred -- scores as instant with no
                // overshoot, which beats every real measurement. That is
                // exactly how the search kept returning the smallest gain it
                // was offered whatever the machine was doing.
                settleMs = (float)((insideSince - t0) / 1e6);
                break;
            }
        } else {
            insideSince = 0;   // left again, so it had not settled
        }
    }

    ctrl->setForceEngage(false);
    sleepMs(80);

    // A run that produced almost no observations measured nothing.
    if (observations < 4) {
        if (++noMoveCount_ == 1)
            log.warn("Too few detections during a run to judge it. The target "
                     "has to stay detected for the whole measurement.");
        return false;
    }

    // A settle time under one frame interval is not a measurement either:
    // nothing can be observed to converge faster than the detections arrive.
    if (settleMs >= 0.0f && settleMs < 8.0f) {
        if (++noMoveCount_ == 1)
            log.warn("A run settled faster than detections arrive, which "
                     "means the step was never actually seen. Check that the "
                     "game is the focused window.");
        return false;
    }

    if (settleMs < 0.0f) {
        // Never arrived. Score it as the worst possible rather than
        // discarding it, so the search still learns this end is bad.
        out.settleMs = (float)kTrialCapMs;
        out.overshootPx = peakPast;
        out.score = (float)kTrialCapMs + peakPast * kOvershootWeight;
        out.valid = true;
        return true;
    }

    out.settleMs = settleMs;
    out.overshootPx = peakPast;
    out.score = settleMs + peakPast * kOvershootWeight;
    out.valid = true;
    return true;
}

// ------------------------------------------------------------------- run

void Tuner::run(Controller* ctrl, Config cfg, Log* logp) {
    Log& log = *logp;

    // Coarse sweep first, then a finer pass around whatever won. A plain
    // fine sweep over the whole range would take four times as long for no
    // better answer.
    // Starts higher than before. The old grid began at 0.08, which is slow
    // enough that a failed measurement returning it looked like a result
    // rather than a symptom.
    const float coarse[] = {0.12f, 0.20f, 0.32f, 0.50f, 0.75f, 1.05f, 1.45f, 1.90f};
    const int nCoarse = (int)(sizeof(coarse) / sizeof(coarse[0]));
    const int nFine = 5;
    const int total = (nCoarse + nFine) * kRepeats;

    {
        std::lock_guard<std::mutex> lk(m_);
        st_.trialsTotal = total;
    }

    // Inference is normally gated on something wanting the output. Nothing
    // does during a tune with the overlays off, so the run asks for it.
    g_wantsInference = true;
    struct ClearWant { ~ClearWant() { g_wantsInference = false; } } clearWant;

    noMoveCount_ = lostCount_ = outOfRegionCount_ = 0;
    log.info("Auto-tune started. Click into the game during the countdown and "
             "leave it focused: output sent while loopcore has focus does not "
             "reach the game, and every measurement then reads as zero.");

    for (int i = kCountdownS; i > 0 && !cancel_.load(); --i) {
        char msg[64];
        snprintf(msg, sizeof(msg), "starting in %d...", i);
        setPhase("Get the target ready", 0.0f, msg);
        sleepMs(1000);
    }
    if (cancel_.load()) {
        setPhase("Cancelled", 0.0f);
        state_ = TuneState::Failed;
        return;
    }

    setPhase("Checking the target", 0.0f);

    // The whole method assumes a stationary target that is actually being
    // detected; without one, every measurement is noise.
    {
        const auto s = ctrl->state();
        if (!s.hasTarget) {
            log.error("No target. Put a stationary object the model detects "
                      "confidently in the middle of the region, then start.");
            setPhase("Failed", 0.0f, "no target");
            state_ = TuneState::Failed;
            return;
        }
        if (!s.sinkReady) {
            log.error("The output is not connected, so nothing can be "
                      "measured.");
            setPhase("Failed", 0.0f, "no output");
            state_ = TuneState::Failed;
            return;
        }
    }

    // Measure how far the world moves per unit of output before anything
    // else. Without it the loop corrects for movement already in flight
    // using a guessed ratio, and every gain above the smallest oscillates --
    // which makes the search conclude that the smallest gain is best.
    setPhase("Measuring response", 0.02f, "");
    // The response scale still has to be measured -- every plan is divided
    // by it -- but lag compensation is left alone. It changes how the loop
    // behaves, and tuning a gain against a moving target is hard enough.
    // Turned on where the controller can see it.
    //
    // Setting it on this copy did nothing: the config arrives by value and
    // the controller reads the shared one. So with auto response switched
    // off, the whole measurement below ran against a fixed manual scale that
    // could never change, and the tuner then reported whatever that scale
    // happened to be as "measured".
    cfg.autoResponse = true;
    ctrl->forceAutoResponse(true);

    // Kept going until the figure is actually in use, not for a fixed count.
    //
    // Six nudges was enough when a single sample moved the estimate. It is
    // not now: the estimator ignores samples that disagree with what it
    // already believes and creeps toward the rest, which is what stopped it
    // collapsing, and the cost is that it needs a few dozen good samples.
    for (int i = 0; i < 60 && !cancel_.load(); ++i) {
        const float scale = std::max(0.05f, ctrl->responseScale());
        const float units = 60.0f / scale;
        ctrl->nudgeRaw((i % 2) ? units : -units, 0.0f);
        sleepMs(120);
        if (i >= 8 && ctrl->responseLearned()) break;
    }
    if (ctrl->responseLearned()) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%.2f px per unit", ctrl->responseScale());
        log.info(std::string("Measured response: ") + msg);
    } else {
        log.warn("Could not measure the response. Results will be rough; "
                 "check that the output is reaching the game.");
    }

    float bestGain = cfg.sensitivity;
    float bestScore = 1e9f;
    int done = 0;
    int usable = 0;

    auto evaluate = [&](float gain, const char* phase, float base, float span) {
        float sum = 0.0f;
        int   got = 0;
        TuneTrial acc{};
        for (int r = 0; r < kRepeats && !cancel_.load(); ++r) {
            char note[96];
            snprintf(note, sizeof(note), "gain %.2f, run %d of %d",
                     gain, r + 1, kRepeats);
            setPhase(phase, base + span * (float)done / (float)total, note);

            TuneTrial t;
            if (runTrial(ctrl, cfg, gain, t, log) && t.valid) {
                sum += t.score;
                acc = t;
                ++got;
            }
            ++done;
            {
                std::lock_guard<std::mutex> lk(m_);
                st_.trialsDone = done;
            }
        }
        if (got == 0) return;
        usable += got;

        TuneTrial avg = acc;
        avg.gain = gain;
        avg.score = sum / got;
        {
            std::lock_guard<std::mutex> lk(m_);
            trials_.push_back(avg);
        }
        if (avg.score < bestScore) {
            bestScore = avg.score;
            bestGain = gain;
            std::lock_guard<std::mutex> lk(m_);
            st_.bestGain = gain;
            st_.bestSettleMs = avg.settleMs;
            st_.bestOvershoot = avg.overshootPx;
        }
    };

    for (int i = 0; i < nCoarse && !cancel_.load(); ++i)
        evaluate(coarse[i], "Coarse sweep", 0.0f, 1.0f);

    if (!cancel_.load()) {
        // Search between the neighbours of the winner, which is where the
        // real optimum sits if the coarse grid straddled it.
        int bi = 0;
        for (int i = 0; i < nCoarse; ++i) if (coarse[i] == bestGain) bi = i;
        const float lo = coarse[std::max(0, bi - 1)];
        const float hi = coarse[std::min(nCoarse - 1, bi + 1)];
        for (int i = 0; i < nFine && !cancel_.load(); ++i) {
            const float g = lo + (hi - lo) * (float)(i + 1) / (float)(nFine + 1);
            evaluate(g, "Refining", 0.0f, 1.0f);
        }
    }

    ctrl->setTuneGain(false, cfg.sensitivity);
    ctrl->setForceEngage(false);
    ctrl->setTunePlain(false);
    // Released on every exit, or the estimator would stay forced on for the
    // rest of the session and quietly override the user's setting.
    ctrl->forceAutoResponse(false);

    if (cancel_.load()) {
        log.info("Auto-tune cancelled.");
        setPhase("Cancelled", 0.0f);
        state_ = TuneState::Failed;
        return;
    }

    // --- deadzone -----------------------------------------------------------
    // Sized from the residual wobble at the best gain, not guessed. Too small
    // and it hunts forever; too large and it stops short of the target.
    setPhase("Sizing the deadzone", 0.95f, "");
    ctrl->setTuneGain(true, bestGain);
    float wobble = 0.0f;
    {
        ctrl->setForceEngage(true);
        const int64_t until = now_ns() + 1'200'000'000LL;
        int64_t stamp = 0;
        int n2 = 0;
        while (now_ns() < until && !cancel_.load()) {
            sleepMs(4);
            const auto s2 = ctrl->state();
            if (!s2.hasTarget || s2.errStamp == stamp) continue;
            stamp = s2.errStamp;
            const float e = std::sqrt(s2.errX * s2.errX + s2.errY * s2.errY);
            wobble = std::max(wobble, e);
            ++n2;
        }
        ctrl->setForceEngage(false);
        if (n2 < 5) wobble = 3.0f;
    }
    const int deadzone = std::clamp((int)(wobble * 1.2f + 0.5f), 2, 30);

    // Smoothing is left off. It filters the measurement, which only helps
    // when detections are noisy; the settle numbers above already show
    // whether they are. Turning it on by default was what made this orbit.
    ctrl->setTuneGain(false, cfg.sensitivity);
    ctrl->setForceEngage(false);
    ctrl->setTunePlain(false);
    // Released on every exit, or the estimator would stay forced on for the
    // rest of the session and quietly override the user's setting.
    ctrl->forceAutoResponse(false);

    {
        std::lock_guard<std::mutex> lk(m_);
        result_.sensitivity  = bestGain;
        result_.deadzonePx   = deadzone;
        result_.emaOn        = false;
        result_.emaIntensity = 0.0f;
        result_.lagCompOn    = cfg.lagCompOn;
    }
    haveResult_ = true;

    if (usable == 0) {
        log.error("No run produced a usable measurement, so nothing was "
                  "learned and the sensitivity is unchanged. The usual cause "
                  "is that the game was not the focused window while this "
                  "ran.");
        setPhase("Failed", 0.0f, "nothing measured");
        state_ = TuneState::Failed;
        return;
    }

    if (noMoveCount_ > 0) {
        char w[256];
        snprintf(w, sizeof(w),
                 "%d of the runs saw no movement at all, so the result below "
                 "is not trustworthy. Almost always this means the game was "
                 "not the focused window.", noMoveCount_);
        log.warn(w);
    }
    if (lostCount_ > 0 || outOfRegionCount_ > 0) {
        char w[192];
        snprintf(w, sizeof(w),
                 "%d runs lost the target and %d pushed it out of the region.",
                 lostCount_, outOfRegionCount_);
        log.warn(w);
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Auto-tune done. Gain %.2f settles in %.0f ms with %.1f px of "
             "overshoot; deadzone %d px from %.1f px of residual wobble.",
             bestGain, st_.bestSettleMs, st_.bestOvershoot, deadzone, wobble);
    log.info(msg);
    setPhase("Done", 1.0f, msg);
    state_ = TuneState::Done;
}

} // namespace lc
