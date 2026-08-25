// hwinfo.h -- what GPU is this, and what should the build settings be.
#pragma once

#include <string>

namespace lc::hw {

struct GpuInfo {
    bool        valid = false;
    std::string name;
    int         major = 0, minor = 0;   // compute capability
    int         smCount = 0;
    size_t      totalBytes = 0;
    size_t      freeBytes  = 0;
    std::string error;                  // set when valid == false

    std::string archName() const;       // "Turing", "Ampere", ...
    std::string totalText() const;      // "11.0 GB"
    std::string freeText()  const;
};

GpuInfo Query();

struct Advice {
    bool        fp16 = true;
    int         workspaceMB = 2048;
    std::string fp16Reason;
    std::string workspaceReason;
    bool        fp16Strong = false;     // true when the answer is unambiguous
};

Advice Recommend(const GpuInfo& g);

} // namespace lc::hw
