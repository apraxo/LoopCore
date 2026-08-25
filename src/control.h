// control.h -- turns detections into steering output.
//
// The chain, in order:
//   pick a target  ->  predict where it will be  ->  smooth  ->  scale  ->  emit
//
// Prediction sits before smoothing on purpose. Predicting from a smoothed
// signal measures the smoother's lag rather than the target's motion, which
// produces a lead in the wrong direction.
#pragma once

#include <atomic>
#include <deque>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "common.h"
#include "paths.h"
#include "inputtag.h"

namespace lc {

// Not INPUT_MOUSE / INPUT_KEYBOARD: winuser.h already defines those as
// macros for the SendInput API, so an enumerator by that name expands to a
// bare integer literal before the compiler sees it.
enum InputMethod {
    InputMouse = 0,   // SendInput on this machine
    InputArduino,     // a 32u4 speaking loopcore's own protocol
    // A Makcu, for running the model on a second machine.
    //
    // The device is a serial-controlled mouse: it plugs into the machine
    // being played on and appears there as a real mouse, while this machine
    // -- which does the capturing and the inference -- only talks to it over
    // a COM port. Nothing is injected locally, so the machine under load and
    // the machine being aimed on need not be the same one.
    InputMakcu,
    InputMethodCount
};

// How the key gates output.
enum ActivationMode { ActHold = 0, ActToggle, ActAlways };

// Wire format to the board.
//   Binary  loopcore's own 7-byte framed packet, used by the bundled sketch.
//   Ascii   "x,y,click\n", which is what most community firmwares expect,
//           including the prebuilt YesHostShield/NoHostShield hex files.
enum SerialProtocol { ProtoBinary = 0, ProtoAscii };

// How the lead is estimated.
//   Linear      raw frame-to-frame difference. Responsive, noisy.
//   Smoothed    filtered velocity. The sane default.
//   Acceleration second order, leads harder into turns, overshoots straights.
//   AlphaBeta   a steady-state constant-velocity filter. Steadiest, slowest
//               to react to a genuine change of direction.
//   Linear       raw frame-to-frame difference. Responsive, noisy.
//   Smoothed     filtered velocity. Fine when boxes are stable.
//   Acceleration second order, leads harder into turns.
//   AlphaBeta    steady-state constant-velocity filter.
//   EdgeConsensus  ignores the box centre and reads the two edges
//                  separately, because a box that is breathing on a static
//                  object moves its edges in opposite directions while a
//                  box that is genuinely travelling moves them together.
enum PredictMethod {
    PredLinear = 0, PredSmoothed, PredAccel, PredAlphaBeta, PredEdgeConsensus,
    // A proper constant-velocity Kalman filter: it carries a covariance, so
    // it weighs each new measurement against how certain it currently is
    // rather than blending by a fixed constant. Leads well through noise and
    // settles on its own when a target stops.
    PredKalman
};

// Which detection to steer toward when several are visible.
enum TargetPriority {
    PrioClosest = 0,   // nearest the aim origin
    PrioLargest,       // biggest box, usually the nearest object
    PrioSmallest,      // smallest box, usually the furthest
    PrioConfidence,    // highest score
    PrioOverlap,       // the one already under the aim origin
    // Nearest the centre, but where boxes overlap each other the smallest of
    // that cluster wins. A large box enclosing a small one is usually the
    // wrong thing to aim at.
    PrioClosestSmallest,
    // Holds the box it started on until that box goes away, the key is
    // released, or confidence collapses. Everything else is ignored.
    PrioSticky,
    // Whichever target the user's own hand is already heading toward.
    //
    // Where the hand is pointed is a statement of intent that no geometric
    // rule has access to, and it is measured already for the assist. When
    // the hand is still this falls back to nearest, because there is nothing
    // to infer from.
    PrioIntent
};

// Where output goes. Both implementations are fire-and-forget: nothing in
// the hot loop ever waits on an acknowledgement.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual bool open(const Config& cfg, Log& log) = 0;
    virtual void send(float dx, float dy, int targetCount) = 0;
    // Press or release a mouse button. `button` is 0 left, 1 right, 2 middle.
    //
    // Separate from send() because a press is an event rather than a rate:
    // movement is coalesced and dispensed continuously, whereas a button
    // must arrive exactly once and in the order it was asked for.
    virtual void button(int button, bool down) { (void)button; (void)down; }
    // Called every tick whether or not anything is being sent, so a sink
    // with a return channel gets serviced even when output is idle.
    virtual void poll() {}
    // Drop anything queued but not yet sent. Called when the user stops
    // asking for movement, so the aim stops with the key rather than a few
    // packets later.
    virtual void flush() {}
    // Send a raw command line. Only the board understands any of these; the
    // other sinks ignore it rather than pretending to support it.
    virtual void sendLine(const char*) {}
    virtual void close() {}
    virtual const char* name() const = 0;
    virtual bool ready() const = 0;
    // Non-zero when the sink is producing output that never arrives. The
    // mouse sink reports this when Windows refuses injected input; a serial
    // sink has no equivalent, so the default is silence.
    virtual uint64_t rejectedCount() const { return 0; }
    virtual bool     accessBlocked() const { return false; }
};

class Controller {
public:
    ~Controller();

    // Opens or reopens the sink when the method or port changes. Cheap and
    // idempotent otherwise, so it is safe to call every frame.
    void configure(const Config& cfg, Log& log);

    // Called once per processed frame from the capture thread. This only
    // records where the target is; it does not move anything.
    // `stampNs` is when the frame arrived, not when this call happens. The
    // difference is the whole pipeline, and everything that reasons about
    // staleness needs the earlier number.
    void submit(const std::vector<Det>& dets, float originX, float originY,
                const Config& cfg, int64_t stampNs);

    // How much of the recent box movement looked like resizing rather than
    // travel, 0 to 1. Useful for telling "not moving" from "can't tell".
    float jitterRatio() const { return jitterRatio_.load(); }

    // Measured screen pixels moved per unit of output sent. Game sensitivity
    // decides this, so it cannot be assumed; everything that reasons about
    // movement already in flight needs it to be right.
    float responseScale() const { return effectiveScale_.load(); }
    float rawScaleEstimate() const { return scaleEst_.load(); }
    // 0 to 1: how consistent the velocity estimate has been lately.
    float leadTrust() const { return leadTrust_.load(); }
    // How many times the action has fired this session, for the readout.
    uint64_t actionFires() const { return actFires_.load(); }
    // Learned means the loop is actually using the measured figure.
    //
    // This used to report true at eight samples while the blend below does
    // not begin until thirty, so anything asking "has it learned yet" got a
    // yes while every plan was still being divided by the configured value.
    // The tuner asks exactly that question and then stops measuring.
    bool  responseLearned() const { return scaleSamples_.load() >= 30; }

    void close();

    // Drops the output sink without stopping the control thread, so an
    // external tool can take the serial port. The sink reopens by itself on
    // the next configure().
    void releaseOutput();
    // Passes a line straight to the sink, for the few things that are a
    // conversation with the board rather than movement.
    void sendBoardLine(const char* text);

    // Hooks for the auto-tuner. It needs to drive the loop directly: force
    // it to engage without a key, substitute a candidate gain, push the view
    // off target, and read the error back each tick.
    void setTuneGain(bool on, float sensitivity);
    // Suppresses prediction and smoothing while a measurement is running.
    // A step response is only meaningful if nothing is adding a lead to it.
    void setTunePlain(bool on);
    // Forces the estimator on regardless of the stored setting, for the
    // duration of a measurement.
    void forceAutoResponse(bool on) { forceAuto_ = on; }
    void setForceEngage(bool on);
    void nudgeRaw(float dx, float dy);

    struct State {
        bool  hasTarget   = false;
        float errX        = 0, errY = 0;      // what the loop is correcting
        int64_t errStamp  = 0;
        float targetX     = 0, targetY = 0;   // after prediction and smoothing
        float lastDx      = 0, lastDy = 0;    // what was emitted
        // Two velocities, because they answer different questions.
        //
        // screenVelX is what the box appears to do; velX is what the target
        // is actually doing once our own movement is added back. When the
        // controller is tracking well the first is near zero while the
        // second is not, and it is the second that prediction uses.
        float velX        = 0, velY  = 0;     // px/s, compensated
        float screenVelX  = 0, screenVelY = 0;
        float leadX       = 0, leadY = 0;     // pixels of lead applied
        float leadTrust   = 1.0f;             // how steady the velocity is
        float leadLagMs   = 0.0f;             // how far the filter is behind
        // The action, for the readout: whether its preconditions are met,
        // how far the aim is from firing, and how often it has.
        // Output that Windows is discarding, and whether it named a reason.
        // Which activation key the controller is currently seeing down.
        //
        // Separate from `engaged`, which is the result after the mode and
        // any filtering. Knowing the key is registering but nothing is
        // moving points at the output; knowing it is not registering points
        // at the bind or the device filter. Those need opposite fixes, and
        // without this the two look identical.
        // What the box-size scaling is currently doing to sensitivity.
        float    boxScaleMult = 1.0f;
        // What the current target measures, in the same units the setting
        // uses, so the two can be compared directly.
        float    boxScaleSize = 0.0f;
        // Detections that were rejected purely for sitting outside the
        // action region. Distinguishes "nothing was found" from "things were
        // found and none of them counted", which look identical otherwise.
        int      outsideActionFov = 0;

        // The recent path of the aim, for drawing it.
        //
        // Movement paths differ in ways that are hard to feel and easy to
        // see, so the differences were being judged blind. Each point is
        // where the aim was and how fast it was going, which is what a
        // speed-coloured line needs.
        struct PathPoint { float x, y, speed; };
        static constexpr int kPathMax = 96;
        PathPoint pathPts[kPathMax] = {};
        int       pathCount = 0;
        bool     key1Down = false;
        bool     key2Down = false;

        uint64_t rejected = 0;
        bool     outputBlocked = false;

        bool     actionArmed = false;
        float    actionDistPx = 0.0f;
        uint64_t actionFires = 0;
        bool  engaged     = false;            // acting this frame
        std::string sinkName = "none";
        bool  sinkReady   = false;
    };
    State state() const;

    // True once, then cleared: the key was held while the model was off.
    bool takeDisabledWarning();

    // Lines the board sent back, collected without touching the log from
    // the control thread. Drained by the UI.
    std::vector<std::string> takeBoardLines();

private:
    void reset();
    void threadMain();
    void tick(const Config& cfg, float dt);

    // Output runs on its own thread. Frames arrive at a few hundred hertz at
    // best, and stepping the mouse once per frame looks exactly as chunky as
    // it sounds. Ticking at ~1 kHz and interpolating between frames is what
    // makes the movement read as smooth.
    std::thread       thread_;
    std::atomic<bool> running_{false};

    // Latest observation, written by the capture thread.
    std::mutex  obsMutex_;
    bool        obsValid_ = false;
    float       obsX_ = 0, obsY_ = 0;      // raw target centre, screen px
    // The box itself, not just its centre: the edges carry the information
    // that separates a moving object from a wobbling detection.
    float       obsX1_ = 0, obsY1_ = 0, obsX2_ = 0, obsY2_ = 0;
    float       obsOx_ = 0, obsOy_ = 0;    // aim origin, screen px
    int64_t     obsNs_ = 0;
    int         obsCount_ = 0;
    // The chosen target's own confidence. Carried through because the action
    // needs to be able to refuse a marginal detection, and the count alone
    // says nothing about quality.
    // Atomic, because it is the one observation field read outside the
    // snapshot.
    //
    // Everything else in this group is copied under obsMutex_ at the top of
    // tick(); this one is read later, in runAction, where the lock is not
    // held. A float torn between two writes would only misjudge one action
    // trigger, which is why it has never been noticed -- but it costs
    // nothing to make it a load, and leaving one field of a group unguarded
    // is the kind of thing that reads as deliberate later.
    std::atomic<float> obsScore_{0.0f};
    // The size distance-scaling measures against: the chosen box, or the
    // larger one enclosing it when there is one.
    float       obsScaleSize_ = 0.0f;
    Config      obsCfg_{};

    // Replaced by the UI thread and used by the control thread. Without a
    // lock, changing input method or COM port while running frees the sink
    // out from under a send that is already in progress.
    mutable std::mutex sinkMutex_;
    std::unique_ptr<OutputSink> sink_;
    int   sinkMethod_ = -1;
    int   sinkPort_   = -1;
    int   sinkProto_  = -1;

    bool  haveSmooth_ = false;
    float smoothX_ = 0, smoothY_ = 0;
    float prevX_   = 0, prevY_   = 0;
    float velX_    = 0, velY_    = 0;   // filtered, per method
    float rawVelX_ = 0, rawVelY_ = 0;   // last instantaneous measurement
    float accX_    = 0, accY_    = 0;
    float abPosX_  = 0, abPosY_  = 0;   // alpha-beta filter state
    // The compensated path the filter measures, as distinct from the screen
    // position -- which says little once the aim is keeping up, because the
    // box then barely moves however fast the target is going.
    float abPathX_ = 0, abPathY_ = 0;
    bool  haveAb_  = false;

    // Kalman state: position and velocity per axis, with a 2x2 covariance.
    float kx_[2] = {0, 0}, ky_[2] = {0, 0};
    float kPx_[2][2] = {{1, 0}, {0, 1}};
    float kPy_[2][2] = {{1, 0}, {0, 1}};
    bool  haveKalman_ = false;
    // A virtual position accumulating compensated displacement, so the
    // filter measures the target's path rather than its screen position.
    float kalmanPosX_ = 0.0f, kalmanPosY_ = 0.0f;

    // Running variance of each edge's frame-to-frame movement, so the
    // steadier one can be preferred as the anchor.
    float edgeVarTop_ = 0, edgeVarBottom_ = 0;
    float edgeVarLeft_ = 0, edgeVarRight_ = 0;
    // Smoothed box size, so an anchor on one edge does not inherit the
    // other edge's noise through the height.
    float boxW_ = 0, boxH_ = 0;
    bool  haveBox_ = false;
    float lastLeadX_ = 0, lastLeadY_ = 0;

    // Previous box edges, for the consensus estimator.
    float pX1_ = 0, pY1_ = 0, pX2_ = 0, pY2_ = 0;
    bool  havePrevBox_ = false;
    // Stabilised aim point, advanced by consensus translation rather than
    // by wherever the noisy centre happened to land.
    float stabX_ = 0, stabY_ = 0;
    bool  haveStab_ = false;
    std::atomic<float> jitterRatio_{0.0f};
    std::atomic<float> scaleEst_{1.0f};
    // What is actually divided by. Held at the configured value until the
    // estimate has earned trust, then eased across.
    std::atomic<float> effectiveScale_{1.0f};
    std::atomic<int>   scaleSamples_{0};
    // Decaying sums for the least-squares slope. Not atomic: only the
    // observation path touches them.
    float sxx_ = 0.0f, sxy_ = 0.0f;
    float screenVelX_ = 0.0f, screenVelY_ = 0.0f;
    float leadVelX_ = 0.0f, leadVelY_ = 0.0f;
    // Set when a reversal was taken at face value, so the statistics that
    // judge the velocity can be restarted rather than poisoned by it.
    bool  turnedNow_ = false;

    // --- the action state machine ---------------------------------------
    //
    // Kept as explicit state rather than derived each tick, because a press
    // and its release are separated in time and the release must happen even
    // if the condition that caused the press has since gone away. A button
    // left down is the one failure here that the user cannot undo from
    // inside the program.
    // The multiplier the box-size scaling last applied, for the readout.
    std::atomic<float> boxScaleMult_{1.0f};
    std::atomic<float> boxScaleSize_{0.0f};
    // Ring of recent aim positions, written by the control thread.
    struct TraceP { float x, y, speed; };
    TraceP  trace_[State::kPathMax] = {};
    int     traceHead_ = 0;
    int     traceCount_ = 0;
    float   traceX_ = 0.0f, traceY_ = 0.0f;
    std::atomic<int>   outsideFov_{0};
    std::atomic<bool> key1Down_{false};
    std::atomic<bool> key2Down_{false};
    PathState pathState_;
    bool    wasEngaged_   = false;   // to catch the release edge
    bool    actWasValid_  = false;   // a target existed last tick
    bool    actCondition_ = false;   // the condition, as of this tick
    int64_t actSinceNs_   = 0;       // when it became true
    int64_t actLastFire_  = 0;       // when the last firing began
    bool    actHeld_      = false;   // a button is currently down
    int     actHeldButton_ = -1;     // which one, so the right one is released
    int64_t actNextStepNs_ = 0;      // when the next press or release is due
    int     actStep_      = 0;       // where in a click sequence we are
    bool    actToggleState_ = false;
    std::atomic<uint64_t> actFires_{0};

    void runAction(const Config& cfg, bool conditionNow, int64_t now);
    void releaseActionButton();
    // Written by the control thread, read by the capture thread when the
    // intent priority is selecting a target.
    // Accumulated hand movement and the window it covers, so the velocity
    // does not depend on how often this loop happens to look.
    float   handAccX_ = 0.0f, handAccY_ = 0.0f;
    int64_t handWindowNs_ = 0;
    std::atomic<float> userVelX_{0.0f};
    std::atomic<float> userVelY_{0.0f};
    // Running mean and spread of the lead velocity, for judging whether the
    // estimate is steady enough to be worth multiplying by a long horizon.
    float velMeanX_ = 0.0f, velMeanY_ = 0.0f;
    float velVarX_ = 0.0f,  velVarY_ = 0.0f;
    std::atomic<float> leadTrust_{1.0f};
    std::atomic<float> leadLagMs_{0.0f};
    int64_t prevNs_ = 0;

    // Latched state for toggle activation.
    bool  toggleOn_   = false;
    bool  keyWasDown_ = false;

    std::atomic<bool>  tuneOn_{false};
    std::atomic<float> tuneGain_{0.35f};
    std::atomic<bool>  forceEngage_{false};
    std::atomic<bool>  tunePlain_{false};
    std::atomic<bool>  forceAuto_{false};

    // Board chatter, filled on the control thread and drained by the UI.
    mutable std::mutex       lineMutex_;
    std::vector<std::string> boardLines_;

    // Sub-pixel remainder, so slow drift is not lost to integer truncation.
    float carryX_ = 0, carryY_ = 0;

    // The planned move, in output units, and what is left of it.
    //
    // This is the important structural choice. Re-deriving an error from the
    // target position every tick means correcting the same error hundreds of
    // times before a new detection can show it has already been dealt with,
    // and in a loop with ten milliseconds of dead time that is guaranteed to
    // overshoot and ring. Instead each new observation plans a move, and the
    // ticks between observations only dispense what was planned. Nothing is
    // ever corrected twice.
    float remainX_ = 0, remainY_ = 0;
    bool  havePlan_ = false;

    // Track association. Picking the nearest detection to the origin every
    // frame independently is what lets one bad box throw the aim across the
    // screen; keeping continuity with what was already being tracked stops
    // it.
    float trackX_ = 0, trackY_ = 0;
    float trackW_ = 0, trackH_ = 0;
    int64_t trackNs_ = 0;
    bool  haveTrack_ = false;
    // Sticky holds a target across frames, so it needs to know whether the
    // key has been released since it latched on.
    bool  stickyHeld_ = false;

    // Movement already emitted but not yet visible in a detection. Without
    // subtracting this, the controller keeps correcting for an error it has
    // already dealt with, overshoots, corrects back, and orbits the target.
    struct Sent { int64_t ns; float dx, dy; };
    std::deque<Sent> sent_;

    // Set when the activation key is pressed while the model is switched
    // off, so the UI can say something rather than silently doing nothing.
    std::atomic<bool> blockedByDisabled_{false};

    mutable std::mutex m_;
    State st_;
};

} // namespace lc
