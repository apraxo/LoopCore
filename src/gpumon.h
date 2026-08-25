// gpumon.h -- what the GPU is doing to itself.
//
// A run that is fine for several minutes and then gets slower, with nothing
// in the software changing, is the signature of the card downclocking. NVML
// is the only thing that can say so; it ships with the driver, so this loads
// it at runtime rather than linking against it.
#pragma once

#include <string>

namespace lc::gpumon {

struct Sample {
    bool   valid       = false;
    int    tempC       = 0;
    int    fanPct      = -1;      // -1 when the card has no readable fan
    int    coreMHz     = 0;
    int    memMHz      = 0;
    int    coreMaxMHz  = 0;       // the boost clock, for comparison
    int    utilGpuPct  = 0;
    int    utilMemPct  = 0;
    int    powerW      = 0;
    int    powerLimitW = 0;
    std::string throttle;         // why it is not running faster, if it is not
    bool   throttled   = false;   // for a reason other than simply being idle
};

// Loads nvml.dll and initialises. Safe to call once; returns false when the
// driver has no NVML, in which case Poll returns an invalid sample.
bool Init();
void Shutdown();

// Cheap enough for a few times a second, too expensive for every frame.
Sample Poll();

} // namespace lc::gpumon
