// hwinfo.cpp
#include "hwinfo.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>

namespace lc::hw {

namespace {
std::string gb(size_t bytes) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}
}

std::string GpuInfo::archName() const {
    switch (major) {
    case 5:  return "Maxwell";
    case 6:  return "Pascal";
    case 7:  return minor >= 5 ? "Turing" : "Volta";
    case 8:  return minor >= 9 ? "Ada Lovelace" : "Ampere";
    case 9:  return "Hopper";
    case 10:
    case 12: return "Blackwell";
    default: return "unknown";
    }
}

std::string GpuInfo::totalText() const { return gb(totalBytes); }
std::string GpuInfo::freeText()  const { return gb(freeBytes);  }

GpuInfo Query() {
    GpuInfo g;

    int count = 0;
    cudaError_t e = cudaGetDeviceCount(&count);
    if (e != cudaSuccess || count == 0) {
        g.error = (e != cudaSuccess) ? cudaGetErrorString(e)
                                     : "no CUDA device found";
        return g;
    }

    int dev = 0;
    cudaGetDevice(&dev);

    cudaDeviceProp p{};
    if (cudaGetDeviceProperties(&p, dev) != cudaSuccess) {
        g.error = "cudaGetDeviceProperties failed";
        return g;
    }

    g.valid      = true;
    g.name       = p.name;
    g.major      = p.major;
    g.minor      = p.minor;
    g.smCount    = p.multiProcessorCount;
    g.totalBytes = p.totalGlobalMem;

    size_t freeB = 0, totalB = 0;
    if (cudaMemGetInfo(&freeB, &totalB) == cudaSuccess) {
        g.freeBytes = freeB;
        if (totalB) g.totalBytes = totalB;
    } else {
        g.freeBytes = g.totalBytes;
    }
    return g;
}

Advice Recommend(const GpuInfo& g) {
    Advice a;

    if (!g.valid) {
        a.fp16 = true;
        a.fp16Reason = "No GPU detected, so this is a guess. Leave FP16 on and "
                       "turn it off if the build complains.";
        a.workspaceMB = 2048;
        a.workspaceReason = "Default, since free memory could not be read.";
        return a;
    }

    // --- FP16 -----------------------------------------------------------
    // Tensor cores arrive at compute capability 7.0. Before that, half
    // precision is either emulated or runs at a small fraction of FP32.
    const int cc = g.major * 10 + g.minor;

    if (cc >= 70) {
        a.fp16 = true;
        a.fp16Strong = true;
        a.fp16Reason = g.archName() + " has tensor cores, so FP16 typically "
                       "runs 2-3x faster than FP32 with no meaningful accuracy "
                       "loss for detection. Leave this on.";
    } else if (cc == 61 || cc == 62) {
        a.fp16 = false;
        a.fp16Strong = true;
        a.fp16Reason = "Pascal consumer cards run FP16 at a fraction of FP32 "
                       "speed. Turning this on would make inference slower, "
                       "not faster.";
    } else if (cc >= 60) {
        a.fp16 = true;
        a.fp16Reason = "This Pascal part has usable FP16 throughput. Worth "
                       "trying, but measure it against FP32.";
    } else {
        a.fp16 = false;
        a.fp16Strong = true;
        a.fp16Reason = g.archName() + " predates useful FP16 support. Build "
                       "FP32.";
    }

    // --- workspace ------------------------------------------------------
    // Most of free memory, not a quarter of it.
    //
    // The workspace is scratch TensorRT may use while trying kernels, and a
    // tactic needing more than the pool allows is not considered at all --
    // however fast it would have been. The hungry tactics are precisely the
    // tuned fp16 tensor-core convolutions worth having.
    //
    // The old figure was a quarter of free, chosen on the reasoning that more
    // "rarely builds a faster engine". That was wrong in an important way:
    // TensorRT's own default, when nothing sets a limit, is the entire
    // device. So this was not a cautious middle -- it was a fourfold
    // restriction below what any other tool building the same model would
    // use, which is a good explanation for engines built here running slower
    // than the same model built elsewhere.
    //
    // It is still bounded rather than unlimited, because the build has to
    // coexist with whatever else is on the card.
    const double freeMB = g.freeBytes / (1024.0 * 1024.0);
    int ws = (int)(freeMB * 0.80);
    ws = (ws / 256) * 256;
    ws = std::clamp(ws, 512, 32768);
    a.workspaceMB = ws;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "About four fifths of the %.1f GB currently free. TensorRT "
             "would use all of it if left alone, and a tactic needing more "
             "scratch than this is skipped however fast it is -- so a small "
             "workspace quietly costs inference time. Lower it only if a "
             "build runs out of memory.",
             freeMB / 1024.0);
    a.workspaceReason = buf;

    return a;
}

} // namespace lc::hw
