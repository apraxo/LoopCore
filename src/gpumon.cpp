// gpumon.cpp
#include "gpumon.h"

#include <windows.h>

#include <cstdio>

namespace lc::gpumon {

namespace {

// NVML's own headers are not part of the CUDA toolkit install on every
// machine, so the handful of entry points used here are declared directly.
// The ABI is stable and documented; this avoids a header dependency that
// would otherwise stop the build on a perfectly good setup.
typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;

struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };

constexpr int NVML_SUCCESS = 0;
constexpr int NVML_TEMPERATURE_GPU = 0;
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 1;

// Throttle reason bits, from nvml.h.
constexpr unsigned long long kGpuIdle              = 0x0000000000000001ULL;
constexpr unsigned long long kAppClocksSetting     = 0x0000000000000002ULL;
constexpr unsigned long long kSwPowerCap           = 0x0000000000000004ULL;
constexpr unsigned long long kHwSlowdown           = 0x0000000000000008ULL;
constexpr unsigned long long kSyncBoost            = 0x0000000000000010ULL;
constexpr unsigned long long kSwThermalSlowdown    = 0x0000000000000020ULL;
constexpr unsigned long long kHwThermalSlowdown    = 0x0000000000000040ULL;
constexpr unsigned long long kHwPowerBrakeSlowdown = 0x0000000000000080ULL;
constexpr unsigned long long kDisplayClockSetting  = 0x0000000000000100ULL;

using PFN_Init            = nvmlReturn_t (*)();
using PFN_Shutdown        = nvmlReturn_t (*)();
using PFN_GetHandle       = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using PFN_GetTemp         = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_GetFan          = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using PFN_GetClock        = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_GetMaxClock     = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using PFN_GetUtil         = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
using PFN_GetPower        = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using PFN_GetPowerLimit   = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using PFN_GetThrottle     = nvmlReturn_t (*)(nvmlDevice_t, unsigned long long*);

HMODULE          g_lib = nullptr;
nvmlDevice_t     g_dev = nullptr;
bool             g_ready = false;

PFN_Init          p_init = nullptr;
PFN_Shutdown      p_shutdown = nullptr;
PFN_GetHandle     p_handle = nullptr;
PFN_GetTemp       p_temp = nullptr;
PFN_GetFan        p_fan = nullptr;
PFN_GetClock      p_clock = nullptr;
PFN_GetMaxClock   p_maxClock = nullptr;
PFN_GetUtil       p_util = nullptr;
PFN_GetPower      p_power = nullptr;
PFN_GetPowerLimit p_powerLimit = nullptr;
PFN_GetThrottle   p_throttle = nullptr;

template <typename T>
void bind(T& fn, const char* name) {
    fn = reinterpret_cast<T>(GetProcAddress(g_lib, name));
}

} // namespace

bool Init() {
    if (g_ready) return true;

    // The driver installs it here; the plain name works when the driver
    // directory is on the path, which it usually is.
    g_lib = LoadLibraryA("nvml.dll");
    if (!g_lib) {
        char sys[MAX_PATH]{};
        if (GetEnvironmentVariableA("ProgramFiles", sys, MAX_PATH)) {
            const std::string p =
                std::string(sys) + "\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            g_lib = LoadLibraryA(p.c_str());
        }
    }
    if (!g_lib) return false;

    bind(p_init,        "nvmlInit_v2");
    bind(p_shutdown,    "nvmlShutdown");
    bind(p_handle,      "nvmlDeviceGetHandleByIndex_v2");
    bind(p_temp,        "nvmlDeviceGetTemperature");
    bind(p_fan,         "nvmlDeviceGetFanSpeed");
    bind(p_clock,       "nvmlDeviceGetClockInfo");
    bind(p_maxClock,    "nvmlDeviceGetMaxClockInfo");
    bind(p_util,        "nvmlDeviceGetUtilizationRates");
    bind(p_power,       "nvmlDeviceGetPowerUsage");
    bind(p_powerLimit,  "nvmlDeviceGetEnforcedPowerLimit");
    bind(p_throttle,    "nvmlDeviceGetCurrentClocksThrottleReasons");

    if (!p_init || !p_handle) { FreeLibrary(g_lib); g_lib = nullptr; return false; }
    if (p_init() != NVML_SUCCESS) { FreeLibrary(g_lib); g_lib = nullptr; return false; }

    // Device 0. Multi-GPU machines would need the one CUDA picked, but the
    // capture path already pins itself to the display adapter.
    if (p_handle(0, &g_dev) != NVML_SUCCESS || !g_dev) {
        if (p_shutdown) p_shutdown();
        FreeLibrary(g_lib);
        g_lib = nullptr;
        return false;
    }

    g_ready = true;
    return true;
}

void Shutdown() {
    if (!g_lib) return;
    if (p_shutdown) p_shutdown();
    FreeLibrary(g_lib);
    g_lib = nullptr;
    g_dev = nullptr;
    g_ready = false;
}

Sample Poll() {
    Sample s;
    if (!g_ready) return s;

    unsigned int v = 0;
    if (p_temp && p_temp(g_dev, NVML_TEMPERATURE_GPU, &v) == NVML_SUCCESS)
        s.tempC = (int)v;
    if (p_fan && p_fan(g_dev, &v) == NVML_SUCCESS) s.fanPct = (int)v;
    if (p_clock && p_clock(g_dev, NVML_CLOCK_GRAPHICS, &v) == NVML_SUCCESS)
        s.coreMHz = (int)v;
    if (p_clock && p_clock(g_dev, NVML_CLOCK_MEM, &v) == NVML_SUCCESS)
        s.memMHz = (int)v;
    if (p_maxClock && p_maxClock(g_dev, NVML_CLOCK_GRAPHICS, &v) == NVML_SUCCESS)
        s.coreMaxMHz = (int)v;
    if (p_power && p_power(g_dev, &v) == NVML_SUCCESS) s.powerW = (int)(v / 1000);
    if (p_powerLimit && p_powerLimit(g_dev, &v) == NVML_SUCCESS)
        s.powerLimitW = (int)(v / 1000);

    nvmlUtilization_t u{};
    if (p_util && p_util(g_dev, &u) == NVML_SUCCESS) {
        s.utilGpuPct = (int)u.gpu;
        s.utilMemPct = (int)u.memory;
    }

    unsigned long long r = 0;
    if (p_throttle && p_throttle(g_dev, &r) == NVML_SUCCESS) {
        std::string why;
        auto add = [&](const char* t) { if (!why.empty()) why += ", "; why += t; };
        // Idle is not a complaint, so it does not count as throttled.
        if (r & kSwThermalSlowdown)    { add("thermal"); s.throttled = true; }
        if (r & kHwThermalSlowdown)    { add("thermal, hardware"); s.throttled = true; }
        if (r & kSwPowerCap)           { add("power cap"); s.throttled = true; }
        if (r & kHwPowerBrakeSlowdown) { add("power brake"); s.throttled = true; }
        if (r & kHwSlowdown)           { add("hardware slowdown"); s.throttled = true; }
        if (r & kAppClocksSetting)     { add("application clocks"); }
        if (r & kDisplayClockSetting)  { add("display clock"); }
        if (r & kSyncBoost)            { add("sync boost"); }
        if (why.empty() && (r & kGpuIdle)) why = "idle";
        s.throttle = why.empty() ? "none" : why;
    } else {
        s.throttle = "unavailable";
    }

    s.valid = true;
    return s;
}

} // namespace lc::gpumon
