// sketch.cpp
#include "sketch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lc {

namespace {

// A byte read as a signed value, one or two wide.
int readSigned(const SketchReport& r, int off, bool wide) {
    if (off < 0 || off >= r.len) return 0;
    if (!wide) return (int)(int8_t)r.data[off];
    if (off + 1 >= r.len) return 0;
    // Little endian, which is what HID uses.
    return (int)(int16_t)((uint16_t)r.data[off] | ((uint16_t)r.data[off + 1] << 8));
}

// The middle value of a set. Used rather than the mean throughout, because a
// single wild report -- a sensor glitch, or the moment the user grabbed the
// mouse -- moves a mean and does not move a median.
int medianOf(std::vector<int>& v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// How consistently a field carried the expected sign across the sample.
//
// This is the whole trick. A real axis is negative in nearly every report of a
// leftward move; a noise axis is negative in roughly half. Scoring the
// proportion rather than looking for a threshold means the answer does not
// depend on how hard or how far the mouse was moved.
int directionScore(const std::vector<SketchReport>& reps, int off, bool wide,
                   int expectSign, int& outMedian) {
    if (reps.empty()) return 0;

    std::vector<int> vals;
    vals.reserve(reps.size());
    int agree = 0, moved = 0;
    for (const auto& r : reps) {
        const int v = readSigned(r, off, wide);
        vals.push_back(v);
        if (v == 0) continue;
        ++moved;
        if ((v > 0) == (expectSign > 0)) ++agree;
    }
    outMedian = medianOf(vals);

    // A field that never moved is not the field, whatever its sign.
    if (moved < (int)reps.size() / 8 || moved < 3) return 0;

    const double proportion = (double)agree / (double)moved;
    const double magnitude = std::fabs((double)outMedian);

    // Agreement is most of the score; magnitude breaks ties between two
    // fields that both agree, which is what separates a real axis from a
    // low-order byte that happens to correlate.
    double s = (proportion - 0.5) * 2.0;          // 0 at chance, 1 at perfect
    if (s < 0.0) s = 0.0;
    s *= 100.0;
    s += std::min(20.0, magnitude);
    return (int)std::min(100.0, s);
}

} // namespace

const char* SketchStepTitle(int step) {
    switch (step) {
    case StepNeutral:       return "Rest the mouse and do not touch it";
    case StepLeftClick:     return "Left click a few times";
    case StepRightClick:    return "Right click a few times";
    case StepMiddleClick:   return "Click the wheel a few times";
    case StepBackButton:    return "Press the back side button";
    case StepForwardButton: return "Press the forward side button";
    case StepWheelUp:       return "Scroll the wheel up";
    case StepWheelDown:     return "Scroll the wheel down";
    case StepMoveLeft:      return "Move the mouse left";
    case StepMoveRight:     return "Move the mouse right";
    case StepMoveUp:        return "Move the mouse away from you";
    case StepMoveDown:      return "Move the mouse toward you";
    default:                return "Done";
    }
}

const char* SketchStepDetail(int step) {
    switch (step) {
    case StepNeutral:
        return "This records what the mouse says when nothing is happening. "
               "Everything else is measured against it, so it is worth "
               "letting go of the mouse entirely for a moment.";
    case StepLeftClick:
    case StepRightClick:
    case StepMiddleClick:
        return "Press and release several times. Holding it down is fine; "
               "what matters is that the bit changes state more than once.";
    case StepBackButton:
    case StepForwardButton:
        return "If the mouse has no such button, skip this step -- nothing "
               "will be assigned and the rest is unaffected.";
    case StepWheelUp:
    case StepWheelDown:
        return "Several clicks of the wheel in one direction.";
    case StepMoveLeft:
    case StepMoveRight:
        return "A steady sweep, not a flick. Some vertical wander is expected "
               "and does not matter -- the analysis is looking at which field "
               "moves consistently, not at any single report.";
    case StepMoveUp:
    case StepMoveDown:
        return "Same again, along the other axis. Away from you is treated as "
               "up, which is what the screen does.";
    default:
        return "";
    }
}

int SketchStepArrow(int step) {
    switch (step) {
    case StepMoveLeft:  return 1;
    case StepMoveRight: return 2;
    case StepMoveUp:    return 3;
    case StepMoveDown:  return 4;
    default:            return 0;
    }
}

void MouseSketch::begin() {
    running_ = true;
    step_ = 0;
    current_.clear();
    for (auto& v : byStep_) v.clear();
    result_ = MouseProfile{};
    notes_.clear();
}

void MouseSketch::cancel() {
    running_ = false;
    current_.clear();
}

int MouseSketch::wanted() const {
    // Movement needs a long sample because the signal is a proportion; a
    // button flips a bit and needs far less.
    switch (step_) {
    case StepMoveLeft: case StepMoveRight:
    case StepMoveUp:   case StepMoveDown:   return 120;
    case StepNeutral:                       return 40;
    default:                                return 60;
    }
}

bool MouseSketch::stepSatisfied() const {
    return (int)current_.size() >= wanted();
}

void MouseSketch::feed(const uint8_t* data, int len) {
    if (!running_ || len <= 0) return;
    SketchReport r;
    r.len = std::min(len, (int)sizeof(r.data));
    for (int i = 0; i < r.len; ++i) r.data[i] = data[i];

    // Kept beyond the target count: an extra sample never hurts the analysis
    // and the user may keep moving after the bar fills.
    if (current_.size() < 400) current_.push_back(r);
}

bool MouseSketch::advance() {
    if (!running_) return false;
    if (step_ >= 0 && step_ < StepCount) byStep_[step_] = current_;
    current_.clear();

    ++step_;
    if (step_ >= StepCount) {
        analyse();
        running_ = false;
        return false;
    }
    return true;
}

void MouseSketch::analyseButtons() {
    const auto& rest = byStep_[StepNeutral];
    if (rest.empty()) return;

    // The bits that are steady at rest. Anything varying with the mouse
    // untouched is noise or a field this does not model, and must not be
    // mistaken for a button.
    uint8_t restVal[32] = {};
    bool    restSteady[32] = {};
    const int len = rest[0].len;
    for (int b = 0; b < len && b < 32; ++b) {
        restVal[b] = rest[0].data[b];
        restSteady[b] = true;
        for (const auto& r : rest) {
            // A report shorter than the first one cannot be compared at this
            // offset, and reading it there would compare against whatever the
            // buffer held. Treated as unsteady instead: a byte that is not
            // present in every resting report is not one to trust as a
            // baseline, which is exactly what restSteady means.
            if (b >= r.len) { restSteady[b] = false; break; }
            if (r.data[b] != restVal[b]) { restSteady[b] = false; break; }
        }
    }

    auto findBit = [&](int stepIdx, SketchButton& out) {
        const auto& reps = byStep_[stepIdx];
        if (reps.empty()) return;

        int bestScore = 0;
        for (int b = 0; b < len && b < 32; ++b) {
            if (!restSteady[b]) continue;
            for (int bit = 0; bit < 8; ++bit) {
                const uint8_t m = (uint8_t)(1u << bit);
                if (restVal[b] & m) continue;      // already set at rest
                // Each report checked against its own length.
                //
                // The bound came from the neutral capture, and a mouse can
                // send a shorter report for some events -- a boot-protocol
                // report alongside its full one, for instance. Indexing a
                // short report with a long report's width reads whatever
                // happened to be in the buffer, and the analysis would then
                // find a button in the padding.
                int setCount = 0;
                int seen = 0;
                for (const auto& r : reps) {
                    if (b >= r.len) continue;
                    ++seen;
                    if (r.data[b] & m) ++setCount;
                }
                if (setCount == 0 || seen < 3) continue;

                // A button should be set in some reports and clear in others:
                // pressed the whole time is indistinguishable from a stuck
                // bit, and set in every report is more likely a field that
                // simply differs from rest.
                const double frac = (double)setCount / (double)seen;
                const int score = (int)(100.0 * (1.0 - std::fabs(frac - 0.45) * 1.6));
                if (score > bestScore) {
                    bestScore = score;
                    out.offset = b;
                    out.mask = m;
                    out.confidence = std::max(0, std::min(100, score));
                }
            }
        }
    };

    findBit(StepLeftClick,     result_.left);
    findBit(StepRightClick,    result_.right);
    findBit(StepMiddleClick,   result_.middle);
    findBit(StepBackButton,    result_.back);
    findBit(StepForwardButton, result_.forward);
}

void MouseSketch::analyseAxes() {
    const int len = byStep_[StepNeutral].empty() ? 8 : byStep_[StepNeutral][0].len;

    // Each axis is scored twice, once against each direction, and the two
    // must agree on the same offset. A field that looks like X when moving
    // left but not when moving right is not X.
    auto solveAxis = [&](int negStep, int posStep, SketchField& out) {
        int bestScore = -1;
        for (int wide = 0; wide < 2; ++wide) {
            for (int off = 0; off + (wide ? 1 : 0) < len; ++off) {
                int mNeg = 0, mPos = 0;
                const int sNeg = directionScore(byStep_[negStep], off, wide != 0, -1, mNeg);
                const int sPos = directionScore(byStep_[posStep], off, wide != 0, +1, mPos);
                if (sNeg == 0 || sPos == 0) continue;

                // Both directions must be convincing, so the weaker one sets
                // the score. An offset that only works one way is a
                // coincidence.
                const int score = std::min(sNeg, sPos);
                if (score > bestScore) {
                    bestScore = score;
                    out.offset = off;
                    out.size = wide ? 2 : 1;
                    out.sixteenBit = (wide != 0);
                    out.inverted = false;
                    out.confidence = score;
                }
            }
        }

        // Nothing matched in the expected sense; try the axis reversed before
        // giving up, since some mice report Y upward.
        if (bestScore < 0) {
            for (int wide = 0; wide < 2; ++wide) {
                for (int off = 0; off + (wide ? 1 : 0) < len; ++off) {
                    int mNeg = 0, mPos = 0;
                    const int sNeg = directionScore(byStep_[negStep], off, wide != 0, +1, mNeg);
                    const int sPos = directionScore(byStep_[posStep], off, wide != 0, -1, mPos);
                    if (sNeg == 0 || sPos == 0) continue;
                    const int score = std::min(sNeg, sPos);
                    if (score > bestScore) {
                        bestScore = score;
                        out.offset = off;
                        out.size = wide ? 2 : 1;
                        out.sixteenBit = (wide != 0);
                        out.inverted = true;
                        out.confidence = score;
                    }
                }
            }
        }
    };

    // Whether the byte after a field is that field's sign extension.
    //
    // An eight-bit read and a sixteen-bit read of the same offset score
    // identically while every value fits in a byte, which it does at ordinary
    // speeds. Choosing eight then works perfectly until a fast movement
    // exceeds 127 and wraps, which is a hard fault that only appears in play.
    //
    // The high byte settles it: on a real sixteen-bit field it is 0x00 when
    // the low byte is positive and 0xFF when negative, every time. On an
    // eight-bit field the next byte is something else entirely and shows no
    // such relationship.
    auto looksSixteenBit = [&](int stepIdx, int off) {
        const auto& reps = byStep_[stepIdx];
        if (reps.empty()) return false;
        int agree = 0, seen = 0;
        for (const auto& r : reps) {
            // Bounded against this report, not against the first one.
            //
            // The length came from reps[0], and a mouse can send a shorter
            // report for some events -- a boot-protocol report alongside its
            // full one. Indexing a short report with a long one's width reads
            // whatever was left in the buffer, and the analysis then decides
            // the layout from padding. The button scan had the same fault and
            // was fixed; this one was missed because it reads two bytes
            // rather than one, so it did not match the same search.
            if (off + 1 >= r.len) continue;
            const int lo = (int)(int8_t)r.data[off];
            if (lo == 0) continue;
            ++seen;
            const uint8_t hi = r.data[off + 1];
            if ((lo < 0 && hi == 0xFF) || (lo >= 0 && hi == 0x00)) ++agree;
        }
        return seen >= 8 && agree >= (seen * 9) / 10;
    };

    auto widenIfNeeded = [&](SketchField& f, int negStep, int posStep) {
        if (f.offset < 0 || f.sixteenBit) return;
        if (looksSixteenBit(negStep, f.offset) &&
            looksSixteenBit(posStep, f.offset)) {
            f.sixteenBit = true;
            f.size = 2;
        }
    };

    solveAxis(StepMoveLeft, StepMoveRight, result_.x);
    // Toward the user is downward on screen, so the "down" step is the
    // positive one.
    solveAxis(StepMoveUp, StepMoveDown, result_.y);

    widenIfNeeded(result_.x, StepMoveLeft, StepMoveRight);
    widenIfNeeded(result_.y, StepMoveUp, StepMoveDown);

    // X and Y cannot be the same field. If the scoring landed on one offset
    // twice, the weaker of the two is discarded rather than both being
    // reported wrongly.
    if (result_.x.offset >= 0 && result_.x.offset == result_.y.offset) {
        if (result_.x.confidence >= result_.y.confidence) result_.y = SketchField{};
        else                                              result_.x = SketchField{};
    }
}

void MouseSketch::analyseWheel() {
    int bestScore = -1;
    const int len = byStep_[StepNeutral].empty() ? 8 : byStep_[StepNeutral][0].len;
    for (int off = 0; off < len; ++off) {
        if (off == result_.x.offset || off == result_.y.offset) continue;
        int mUp = 0, mDn = 0;
        const int sUp = directionScore(byStep_[StepWheelUp], off, false, +1, mUp);
        const int sDn = directionScore(byStep_[StepWheelDown], off, false, -1, mDn);
        if (sUp == 0 || sDn == 0) continue;
        const int score = std::min(sUp, sDn);
        if (score > bestScore) {
            bestScore = score;
            result_.wheel.offset = off;
            result_.wheel.size = 1;
            result_.wheel.confidence = score;
        }
    }
}

void MouseSketch::analyse() {
    if (!byStep_[StepNeutral].empty()) {
        result_.neutral = byStep_[StepNeutral][0];
        result_.reportLen = result_.neutral.len;
    }
    analyseButtons();
    analyseAxes();
    analyseWheel();
}

std::string MouseSketch::explain() const {
    char b[256];
    std::string out;

    snprintf(b, sizeof(b), "report length %d bytes\n", result_.reportLen);
    out += b;

    auto field = [&](const char* nm, const SketchField& f) {
        if (f.offset < 0) {
            snprintf(b, sizeof(b), "  %-8s not found\n", nm);
        } else {
            snprintf(b, sizeof(b), "  %-8s byte %d, %d-bit%s  (%d%% sure)\n",
                     nm, f.offset, f.sixteenBit ? 16 : 8,
                     f.inverted ? ", inverted" : "", f.confidence);
        }
        out += b;
    };
    auto button = [&](const char* nm, const SketchButton& x) {
        if (x.offset < 0) {
            snprintf(b, sizeof(b), "  %-8s not found\n", nm);
        } else {
            snprintf(b, sizeof(b), "  %-8s byte %d bit 0x%02X  (%d%% sure)\n",
                     nm, x.offset, x.mask, x.confidence);
        }
        out += b;
    };

    field("x", result_.x);
    field("y", result_.y);
    field("wheel", result_.wheel);
    button("left", result_.left);
    button("right", result_.right);
    button("middle", result_.middle);
    button("back", result_.back);
    button("forward", result_.forward);

    out += "\nreport at rest:";
    for (int i = 0; i < result_.neutral.len; ++i) {
        snprintf(b, sizeof(b), " %02X", result_.neutral.data[i]);
        out += b;
    }
    out += "\n";

    // A neutral report that is not all zeroes is worth saying out loud rather
    // than quietly accommodating: it usually means a field this does not
    // model, and it explains a layout that half works.
    bool allZero = true;
    for (int i = 0; i < result_.neutral.len; ++i)
        if (result_.neutral.data[i] != 0) { allZero = false; break; }
    if (!allZero)
        out += "\nThe resting report is not all zeroes, so this mouse sends a "
               "field the analysis does not model. The layout above may still "
               "be right; it is worth checking the axes behave before "
               "trusting it.\n";

    return out;
}

} // namespace lc
