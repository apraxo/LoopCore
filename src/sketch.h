// sketch.h -- working out a mouse's HID report layout by watching it.
//
// A USB mouse describes its own report format in a descriptor, but the boards
// this talks to do not parse descriptors -- they copy bytes out of fixed
// offsets. Those offsets differ between mice, which is why the firmware has a
// layout setting at all, and why picking the wrong one produces a mouse that
// moves diagonally, or not at all, or clicks when it should scroll.
//
// Rather than have someone guess between layouts, this watches the mouse do
// things it has been asked to do and works out where each field lives.
//
// The method
// ----------
// Each prompt is a controlled experiment. "Do not touch the mouse" gives a
// baseline of what the report looks like at rest. "Move left" gives a set of
// reports in which exactly one field should be consistently negative. The
// analysis compares the two: a byte that is steady at rest and consistently
// signed during the action is the field being looked for.
//
// Nothing is perfectly clean -- a leftward move always carries some vertical
// noise, and a click often nudges the sensor. So no single report decides
// anything. Every candidate offset is scored across the whole sample and the
// best-scoring one wins, which is what makes the result stable despite the
// mess in any individual report.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace lc {

// One captured report, as bytes.
struct SketchReport {
    uint8_t data[32] = {};
    int     len = 0;
};

// What the analysis concluded about one field.
struct SketchField {
    int  offset = -1;     // byte index into the report
    int  size = 1;        // 1 or 2 bytes
    bool sixteenBit = false;
    bool inverted = false;   // the axis runs opposite to expectation
    int  confidence = 0;     // 0..100, how clearly it separated from the rest
};

// A button, which is a bit rather than a field.
struct SketchButton {
    int     offset = -1;
    uint8_t mask = 0;
    int     confidence = 0;
};

// Everything the sketch works out, and what gets saved as a profile.
struct MouseProfile {
    std::string  name;
    int          reportLen = 0;
    SketchField  x, y, wheel;
    SketchButton left, right, middle, back, forward;
    // The report as it looks with nothing happening. Useful on its own: a
    // mouse whose neutral report is not all zeroes has a field this does not
    // model, and that is worth showing rather than hiding.
    SketchReport neutral;
};

// The prompts, in order. Each names an action and what it is looking for.
enum SketchStep {
    StepNeutral = 0,
    StepLeftClick,
    StepRightClick,
    StepMiddleClick,
    StepBackButton,
    StepForwardButton,
    StepWheelUp,
    StepWheelDown,
    StepMoveLeft,
    StepMoveRight,
    StepMoveUp,
    StepMoveDown,
    StepCount
};

const char* SketchStepTitle(int step);
const char* SketchStepDetail(int step);
// Which way the arrow should point, or 0 for a step with no direction.
//   1 left, 2 right, 3 up, 4 down
int SketchStepArrow(int step);

class MouseSketch {
public:
    // Reports arrive from the serial reader as raw bytes.
    void feed(const uint8_t* data, int len);

    void begin();
    void cancel();
    bool running() const { return running_; }

    int  step() const { return step_; }
    // Samples gathered for the current step, and how many are wanted.
    int  gathered() const { return (int)current_.size(); }
    int  wanted() const;
    bool stepSatisfied() const;

    // Moves to the next prompt, keeping what was gathered. Returns false when
    // the last step is done, at which point result() is filled.
    bool advance();

    const MouseProfile& result() const { return result_; }
    // A human-readable account of what was decided and how clearly, so the
    // conclusion can be judged rather than taken on trust.
    std::string explain() const;

private:
    void analyse();
    void analyseButtons();
    void analyseAxes();
    void analyseWheel();

    bool running_ = false;
    int  step_ = 0;
    std::vector<SketchReport> current_;
    std::vector<SketchReport> byStep_[StepCount];
    MouseProfile result_;
    std::string  notes_;
};

} // namespace lc
