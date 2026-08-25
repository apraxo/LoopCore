// paths.cpp
#include "paths.h"

#include <algorithm>
#include <cmath>

namespace lc {

namespace {

// A cheap deterministic hash, used instead of rand().
//
// The paths that need randomness need it to be repeatable within one
// engagement and different between engagements. A global generator gives
// neither reliably when two of them are running.
float noise1(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float)(x & 0xFFFFFF) / (float)0xFFFFFF;   // 0..1
}

// Smooth value noise: interpolated between integer samples, so it wanders
// rather than jumping. Perlin in spirit, one dimension, no gradients.
float smoothNoise(float t, uint32_t seed) {
    const float f = std::floor(t);
    const int i = (int)f;
    const float frac = t - f;
    const float a = noise1((uint32_t)i * 374761393u + seed);
    const float b = noise1((uint32_t)(i + 1) * 374761393u + seed);
    // Smoothstep, so the joins are not visible as direction changes.
    const float u = frac * frac * (3.0f - 2.0f * frac);
    return (a + (b - a) * u) * 2.0f - 1.0f;           // -1..1
}

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

} // namespace

void PathState::begin(float distPx, uint32_t s) {
    startDist = std::max(1.0f, distPx);
    lateralNow = 0.0f;
    progress = 0.0f;
    carry = 0.0f;
    corrected = false;
    passed = false;
    seed = s ? s : 1;
    // Arcs alternate rather than always bowing the same way, or every
    // movement curves identically and the pattern is the giveaway.
    side = (noise1(seed) > 0.5f) ? 1 : -1;
}

void PathState::clear() {
    lateralNow = 0.0f;
    startDist = 0.0f;
    progress = 0.0f;
    carry = 0.0f;
    corrected = false;
    passed = false;
}

const char* MovePathName(int p) {
    switch (p) {
    case PathLinear:      return "Linear";
    case PathCubicBezier: return "Cubic Bezier";
    case PathCatmullRom:  return "Catmull-Rom";
    case PathArc:         return "Arc";
    case PathEaseInOut:   return "Ease in and out";
    case PathMinJerk:     return "Minimum jerk";
    case PathLogNormal:   return "Log-normal";
    case PathFitts:       return "Fitts timed";
    case PathTwoPhase:    return "Two phase";
    case PathOvershoot:   return "Overshoot and correct";
    case PathTremor:      return "Hand tremor";
    case PathAdaptive:    return "Adaptive";
    case PathMomentum:    return "Momentum";
    case PathWind:        return "Wind";
    default:              return "Default";
    }
}

const char* MovePathBlurb(int p) {
    switch (p) {
    case PathLinear:
        return "Constant speed the whole way, stopping dead on arrival. The "
               "most predictable and the least like a hand -- useful mainly "
               "as a baseline to compare the others against.";
    case PathCubicBezier:
        return "Bows off the straight line through two control points, so the "
               "aim arcs into the target instead of sliding along the "
               "diagonal. Strength sets how far it bends.";
    case PathCatmullRom:
        return "Curves along where the target has recently been rather than "
               "cutting the corner to where it is now. Follows a moving "
               "target's own track, which is what someone watching it does.";
    case PathArc:
        return "A deliberate constant-radius bow, alternating side between "
               "engagements so repeated movements do not trace the same "
               "curve.";
    case PathEaseInOut:
        return "Slow to start, quick through the middle, slow into the "
               "target. The familiar shape of a deliberate hand movement.";
    case PathMinJerk:
        return "The trajectory human arms actually follow, from motor-control "
               "research: the quintic that minimises jerk. Smoothest possible "
               "change of acceleration, and the closest single curve to a "
               "real reach.";
    case PathLogNormal:
        return "Models the movement as overlapping muscle impulses, giving "
               "the slightly lopsided speed curve real pointing has -- "
               "reaching peak speed before the halfway point rather than at "
               "it.";
    case PathFitts:
        return "Takes its time from the distance and the target's size, the "
               "way Fitts's law describes. Far or small targets legitimately "
               "take longer instead of everything arriving at one speed.";
    case PathTwoPhase:
        return "A fast throw covering most of the distance, then a distinct "
               "slower correction at the end. This is measurably what people "
               "do, and it is the structure rather than the smoothness that "
               "reads as human.";
    case PathOvershoot:
        return "Passes slightly beyond the target, then comes back onto it. "
               "Costs a little time and is among the most convincing things "
               "on this list, because almost nothing else overshoots.";
    case PathTremor:
        return "The default approach with a small high-frequency wobble laid "
               "over it. A real hand is never still, and perfectly smooth "
               "travel is one of the clearest signs that something is not "
               "one.";
    case PathAdaptive:
        return "Chooses between a throw-and-correct and a direct approach "
               "based on how far away the target is and how large it appears "
               "-- a long reach to something small gets the correction, a "
               "short nudge does not.";
    case PathMomentum:
        return "Carries speed across a change of target rather than starting "
               "again from rest, so consecutive targets flow into one another "
               "instead of each move beginning from a stop.";
    case PathWind:
        return "Adds a slow wandering drift perpendicular to travel. The path "
               "never repeats, which is the point: identical movements are "
               "more noticeable than imperfect ones.";
    default:
        return "The original behaviour: a straight line, quickest at the "
               "start and easing into the target. Nothing is added, so this "
               "is the one to return to if a path behaves oddly.";
    }
}

PathStep ShapePath(int path, PathState& st, float distPx, float boxPx,
                   float dt, float strength, bool rampWithDistance) {
    PathStep out;
    if (dt <= 0.0f) return out;

    st.phase += dt;

    // Progress along the move. The plan shortens as it is paid out, so how
    // far in we are is what remains against what there was to begin with.
    // Recomputed rather than integrated, because the target keeps moving and
    // an integrated position would drift away from the truth.
    if (st.startDist > 1.0f) {
        // Progress only ever advances.
        //
        // A target moving away lengthens the plan, and computing progress
        // from the remaining distance then sends it backwards -- an ease
        // curve would slow down again halfway through a chase, and a
        // two-phase move could re-enter its throw after already correcting.
        // The question being asked is "how far through this movement are
        // we", and that does not decrease because the destination moved.
        //
        // A genuinely new engagement gets a fresh state from the controller,
        // so nothing is carried over where it should not be.
        const float raw = clamp01(1.0f - distPx / st.startDist);
        if (raw > st.progress) st.progress = raw;
    }

    const float t = st.progress;
    float k = std::clamp(strength, 0.0f, 1.0f);

    // Stronger the further there is to go, when asked.
    //
    // Measured in target widths rather than pixels, because that is what
    // "far" means here -- a hundred pixels to a distant figure is a long
    // reach, and to one filling the screen it is a nudge. Below one width
    // the path is nearly absent; by six it is at full strength.
    //
    // Only the path's own parameter is scaled, not its rate: a path that
    // asked for a slower approach should still get one on a short move. This
    // governs how much character it adds, not how fast it travels.
    if (rampWithDistance) {
        const float widths = distPx / std::max(8.0f, boxPx);
        const float ramp = clamp01((widths - 1.0f) / 5.0f);
        k *= 0.15f + 0.85f * ramp;
    }

    // Where the path wants the aim to sit, sideways, right now. Converted to
    // a step at the end.
    float wantLateral = 0.0f;

    switch (path) {

    case PathLinear: {
        // The controller's own rate decays with the remaining distance, so
        // undoing that decay is what produces constant speed: release a
        // fixed share of the *original* distance each tick.
        // Bounded at source, because the ratio has no upper limit.
        //
        // Constant speed means undoing the controller's own decay, which is
        // a ratio of the original distance to what remains -- and that grows
        // without bound as the aim arrives. At three pixels out on a
        // three-hundred pixel move it asks for thirty-five times the rate,
        // which is not constant speed, it is a slingshot. Capping it keeps
        // the middle of the move honestly linear and lets the approach fade
        // handle the end.
        const float ratio = (distPx > 1.0f) ? (st.startDist / distPx) : 1.0f;
        out.rate = std::min(2.2f, ratio * 0.35f);
        break;
    }

    case PathEaseInOut: {
        // Smoothstep on the velocity, normalised so the total still adds up.
        const float v = 6.0f * t * (1.0f - t);          // 0 at both ends
        out.rate = 0.45f + v * 1.3f;
        break;
    }

    case PathMinJerk: {
        // The minimum-jerk velocity profile: 30t^2(1-t)^2, scaled.
        // The floor matters as much as the peak. The controller's own rate
        // already decays toward the end, so a low multiplier there compounds
        // with it and the last few pixels crawl.
        const float v = 30.0f * t * t * (1.0f - t) * (1.0f - t);
        out.rate = 0.55f + v * 0.75f;
        break;
    }

    case PathLogNormal: {
        // Peak speed before halfway, with a long tail after -- the asymmetry
        // is the whole point, so the peak is deliberately off-centre.
        const float x = std::max(0.02f, t);
        const float lg = std::log(x) + 0.9f;
        const float v = std::exp(-(lg * lg) * 2.2f) / x;
        out.rate = 0.5f + std::min(2.5f, v) * 0.55f;
        break;
    }

    case PathFitts: {
        // Fitts's law gives a duration from distance and target width; the
        // rate that fills that duration is its reciprocal. A wide target
        // finishes sooner, which is the behaviour being modelled.
        const float w = std::max(8.0f, boxPx);
        const float id = std::log2(1.0f + st.startDist / w);   // bits
        const float secs = std::clamp(0.08f + 0.12f * id, 0.08f, 1.2f);
        out.rate = std::clamp((0.45f / secs), 0.25f, 3.0f);
        break;
    }

    case PathTwoPhase: {
        // A throw, then a correction. The switch happens once, on the way in,
        // and is remembered so a target moving during the correction cannot
        // send it back into the throw.
        if (!st.corrected && t > 0.82f) st.corrected = true;
        out.rate = st.corrected ? 0.5f : (1.8f + k * 1.2f);
        break;
    }

    case PathOvershoot: {
        // Past the target first, then back. The overshoot is produced by
        // continuing at speed through the end of the plan rather than by
        // aiming somewhere else, so nothing downstream has to know.
        if (!st.passed && t > 0.94f) st.passed = true;
        if (!st.passed) out.rate = 1.5f + k * 0.9f;
        else            out.rate = 0.55f;
        break;
    }

    case PathMomentum: {
        // Speed decays toward the plain rate rather than restarting, so a
        // new target inherits what the last one had built up.
        st.carry = st.carry * std::exp(-dt * 3.0f) + dt * 3.0f * 1.0f;
        out.rate = 0.7f + st.carry * (0.6f + k);
        break;
    }

    case PathAdaptive: {
        // Long reach to a small target gets the correction; a short nudge to
        // a large one does not. The threshold is in target widths, which is
        // the unit the choice actually depends on.
        const float widths = st.startDist / std::max(8.0f, boxPx);
        if (widths > 2.5f) {
            if (!st.corrected && t > 0.85f) st.corrected = true;
            out.rate = st.corrected ? 0.55f : 1.7f;
        } else {
            out.rate = 1.0f;
        }
        break;
    }

    case PathTremor: {
        // Two frequencies, because a single sine reads as a machine
        // oscillating rather than a hand that is not quite still.
        const float a = std::sin(st.phase * 47.0f) * 0.6f;
        const float b = std::sin(st.phase * 23.0f + 1.7f) * 0.4f;
        wantLateral = (a + b) * k * 2.5f;
        break;
    }

    case PathCubicBezier: {
        // Lateral offset following the bow of a cubic with both control
        // points pushed to one side. Zero at both ends by construction, so
        // the aim still arrives exactly where the plan says.
        const float bow = 3.0f * t * (1.0f - t);
        wantLateral = bow * st.startDist * 0.16f * k * (float)st.side;
        break;
    }

    case PathArc: {
        // A half sine is the constant-radius case: widest in the middle,
        // nothing at the ends.
        const float bow = std::sin(t * 3.14159265f);
        wantLateral = bow * st.startDist * 0.13f * k * (float)st.side;
        break;
    }

    case PathCatmullRom: {
        // Bows toward the side the target has been drifting, which is what
        // following its track rather than the chord amounts to. The drift
        // direction is carried in `side`, set when the plan began.
        const float bow = std::sin(t * 3.14159265f);
        wantLateral = bow * st.startDist * 0.10f * k * (float)st.side;
        out.rate = 0.9f + 0.3f * (1.0f - t);
        break;
    }

    case PathWind: {
        // Smooth noise, so the drift wanders instead of vibrating. Scaled by
        // how far there is left to go, so it settles as the aim arrives
        // rather than jittering on the target.
        const float w = smoothNoise(st.phase * 1.6f, st.seed);
        wantLateral = w * k * st.startDist * 0.08f * (0.25f + 0.75f * (1.0f - t));
        break;
    }

    case PathDefault:
    default:
        break;
    }

    // Nothing may stop the aim outright or run away with it. A path is a
    // route, not a veto, and a rate of zero would strand a plan that the
    // controller believes is being paid out.
    // The step is the change in offset, not the offset itself.
    //
    // Emitting the offset directly integrated it: a bow that should arc out
    // and back instead drifted steadily to one side, because every tick
    // added its full displacement again. The difference is what actually
    // moves the aim, and it returns to the line by construction -- when the
    // offset falls back to zero the steps are negative and cancel what was
    // added.
    // The path's influence fades out as the aim arrives.
    //
    // This is what made almost every path overshoot. A path shapes a journey,
    // and the controller rebuilds the journey every frame -- so while
    // tracking, the remaining distance collapses to a few pixels while the
    // path is still applying whatever rate it chose for a long move. A
    // multiplier of two or three across the last few pixels does not settle
    // on the target, it crosses it, and next frame it crosses back. That is
    // the orbiting.
    //
    // Fading toward 1.0 near the target lets every path behave like the
    // default over the final approach, which is the part that has to be
    // accurate, while keeping its character over the distance, which is the
    // part that has to look human. Linear and Fitts were the worst of them
    // precisely because their rate is a ratio that grows without bound as
    // the distance shrinks.
    {
        // Measured against the target's own size rather than a fixed number
        // of pixels: arriving at a large close target and a small distant one
        // are different journeys, and the last stretch of each scales with
        // how big the thing is.
        const float nearPx = std::max(6.0f, boxPx * 0.25f);
        if (distPx < nearPx) {
            const float w = clamp01(distPx / nearPx);   // 0 at the target
            out.rate = 1.0f + (out.rate - 1.0f) * w;
            wantLateral *= w;
        }
    }

    const float delta = wantLateral - st.lateralNow;
    st.lateralNow = wantLateral;

    // The ceiling is 2.0 rather than 4.0: nothing here needs to pay out four
    // times faster than the sensitivity says, and the extra room only gave
    // the ratio-based paths somewhere to misbehave.
    out.rate = std::clamp(out.rate, 0.05f, 2.0f);
    // A single tick may not lurch, however large the offset it is chasing.
    out.lateral = std::clamp(delta, -6.0f, 6.0f);
    return out;
}

} // namespace lc
