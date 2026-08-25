// paths.h -- how the aim travels, as distinct from how fast.
//
// The controller holds a plan: a remaining distance to the aim point, paid
// out a fraction at a time. That produces a straight line at an exponentially
// decaying speed, which is correct and looks nothing like a hand.
//
// Everything here shapes that payout without replacing it. Each path answers
// two questions per tick:
//
//   how much of the remaining distance to release now, relative to what the
//   plain exponential would have released, and
//
//   how far to step sideways, perpendicular to the direction of travel.
//
// Keeping it to those two lets every path share the safety limits, the
// prediction, and the sub-pixel carry that already exist -- a path cannot
// steer the aim somewhere the controller would not have gone, only take a
// different route to the same place. That matters: a path that could set an
// arbitrary position would bypass the speed ceiling and the step clamp.
#pragma once

#include <cstdint>

namespace lc {

// Ordered by how closely each reproduces the way a hand actually moves,
// most convincing first after Default.
//
// The ranking is not a quality judgement about the maths -- Linear is a
// perfectly good curve -- but about how much of real pointing behaviour each
// one captures. What reads as human is, roughly in order: the structure of a
// movement (a throw and a correction, an overshoot), then its speed profile,
// then its texture, and only last its shape in space. A perfectly smooth arc
// is still obviously not a hand; a slightly wrong two-phase move is not.
//
// Default stays first because it is the reference: it is what everything
// else is a departure from, and the one to come back to.
enum MovePath {
    PathDefault = 0,   // exponential approach; the original behaviour

    // --- closest to real pointing -------------------------------------
    PathTwoPhase,      // fast throw, then a separate slow correction
    PathMinJerk,       // the quintic human arms actually follow
    PathOvershoot,     // passes the target, comes back
    PathLogNormal,     // asymmetric speed curve of real pointing
    PathAdaptive,      // picks throw or direct by distance and size

    // --- plausible, and cheap to combine with the above ---------------
    PathTremor,        // exponential plus a small hand wobble
    PathFitts,         // duration from distance and target width
    PathMomentum,      // carries speed across target switches
    PathEaseInOut,     // slow, fast, slow

    // --- shaped in space rather than in time --------------------------
    PathCatmullRom,    // follows where the target has been, not the chord
    PathCubicBezier,   // bowed off the straight line by two control points
    PathWind,          // wandering drift perpendicular to travel
    PathArc,           // constant-radius bow, alternating side

    // --- least like a hand --------------------------------------------
    PathLinear,        // constant speed, stops dead

    PathCount
};

const char* MovePathName(int p);
const char* MovePathBlurb(int p);

// What a path decides for one tick.
struct PathStep {
    // Multiplies the fraction of the plan released this tick. One leaves the
    // controller's own rate untouched.
    float rate = 1.0f;
    // Sideways movement in output units, perpendicular to travel. Added to
    // the step, so it is bounded by the same limits as everything else.
    float lateral = 0.0f;
};

// Everything a path needs to remember between ticks.
//
// Held by the controller and reset when a plan is abandoned, so a new
// engagement starts cleanly rather than inheriting the middle of the last
// one's curve.
struct PathState {
    float startDist = 0.0f;    // plan length when it began, in pixels
    float progress = 0.0f;     // 0 at the start, 1 at the aim point
    float phase = 0.0f;        // free-running, for tremor and wind
    float carry = 0.0f;        // momentum kept across switches
    int   side = 1;            // which way an arc bows
    bool  corrected = false;   // whether a two-phase move has switched over
    bool  passed = false;      // whether an overshoot has turned back
    uint32_t seed = 1;         // per-engagement, so paths do not repeat
    // The sideways offset currently applied, in output units.
    //
    // Paths describe where the aim should be relative to the straight line;
    // the controller can only be told how far to step. Keeping the current
    // offset here lets the step be the difference between the two, which is
    // what makes a bow come back to the line instead of drifting off it.
    float lateralNow = 0.0f;

    void begin(float distPx, uint32_t s);
    void clear();
};

// `distPx` is what remains, `boxPx` the target's apparent size, `dt` seconds.
//
// `rampWithDistance` makes the path assert itself in proportion to how far
// there is to go. It pairs with the fade that already happens on approach:
// that one stops a path interfering with the last few pixels, this one stops
// it doing much of anything on a short correction while leaving it free on a
// long sweep. Together they put the character where there is room for it.
PathStep ShapePath(int path, PathState& st, float distPx, float boxPx,
                   float dt, float strength, bool rampWithDistance = false);

} // namespace lc
