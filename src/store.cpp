// store.cpp
#include "store.h"

// For PathCount. This header depends on nothing but <cstdint>, so including
// it here does not drag the controller in the way control.h would.
#include "paths.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <sstream>

namespace lc::store {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

std::string fileName(const std::string& p) {
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? p : p.substr(i + 1);
}

std::string stem(const std::string& p) {
    std::string n = fileName(p);
    size_t d = n.find_last_of('.');
    return d == std::string::npos ? n : n.substr(0, d);
}

std::string extOf(const std::string& p) {
    std::string n = fileName(p);
    size_t d = n.find_last_of('.');
    return d == std::string::npos ? "" : toLower(n.substr(d + 1));
}

bool dirExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string humanSize(uint64_t b) {
    char buf[64];
    if (b >= 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024));
    else if (b >= 1024)
        snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)b);
    return buf;
}

std::string humanTime(const FILETIME& ft) {
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc)) return {};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buf;
}

} // namespace

// ------------------------------------------------------------- locations

std::string appDir() {
    char buf[MAX_PATH]{};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string(".") : p.substr(0, i);
}

std::string binDir()       { return appDir() + "\\bin"; }
std::string modelsDir()    { return binDir() + "\\models"; }
std::string convertedDir() { return binDir() + "\\converted"; }
std::string logsDir() { return binDir() + "\\logs"; }

std::string configDir()    { return binDir() + "\\config"; }

bool ensureDirs(Log& log) {
    const std::string dirs[5] = { binDir(), modelsDir(), convertedDir(),
                                  configDir(), logsDir() };
    for (const auto& d : dirs) {
        if (dirExists(d)) continue;
        if (!CreateDirectoryA(d.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            log.error("Could not create " + d +
                      ". Move loopcore somewhere writable, or run it as "
                      "administrator.");
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- models

bool isInside(const std::string& path, const std::string& dir) {
    return toLower(path).rfind(toLower(dir), 0) == 0;
}

std::vector<ModelEntry> listModels() {
    std::vector<ModelEntry> out;
    const std::string pattern = modelsDir() + "\\*";

    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ModelEntry e;
        e.name = fd.cFileName;
        e.ext  = extOf(e.name);
        // The library lists loadable engines only. Sources live in
        // bin/converted and are reachable from the Open folder button.
        if (e.ext != "engine") continue;
        e.path  = modelsDir() + "\\" + e.name;
        e.bytes = (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                   fd.ftLastWriteTime.dwLowDateTime;
        e.sizeText = humanSize(e.bytes);
        e.dateText = humanTime(fd.ftLastWriteTime);
        out.push_back(std::move(e));
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(),
              [](const ModelEntry& a, const ModelEntry& b) { return a.mtime > b.mtime; });
    return out;
}

bool importInto(const std::string& src, const std::string& destDir,
                std::string& dstOut, Log& log) {
    if (!ensureDirs(log)) return false;

    if (isInside(src, destDir)) {    // already there, nothing to copy
        dstOut = src;
        return true;
    }
    if (!fileExists(src)) {
        log.error("No file at " + src);
        return false;
    }

    const std::string base = stem(src);
    const std::string ext  = extOf(src);
    std::string dst = destDir + "\\" + base + "." + ext;

    // Never overwrite: an identically named model may be a different network.
    int n = 1;
    while (fileExists(dst)) {
        dst = destDir + "\\" + base + "_" + std::to_string(n++) + "." + ext;
        if (n > 999) { log.error("Too many copies of " + base); return false; }
    }

    if (!CopyFileA(src.c_str(), dst.c_str(), TRUE)) {
        log.error("Could not copy " + fileName(src) + " (error " +
                  std::to_string(GetLastError()) + ").");
        return false;
    }
    log.info("Copied " + fileName(dst) + " into " + fileName(destDir) + ".");
    dstOut = dst;
    return true;
}

bool renameModel(const std::string& path, const std::string& newStem, Log& log) {
    const std::string clean = sanitiseName(newStem);
    if (clean.empty()) {
        log.warn("That name has no usable characters in it.");
        return false;
    }

    const size_t slash = path.find_last_of("\\/");
    const size_t dot   = path.find_last_of('.');
    const std::string dir = (slash == std::string::npos)
                          ? std::string() : path.substr(0, slash + 1);
    const std::string ext = (dot != std::string::npos && dot > slash)
                          ? path.substr(dot) : std::string();
    const std::string dst = dir + clean + ext;

    if (dst == path) return true;
    if (fileExists(dst)) {
        log.warn("A model called " + clean + " already exists.");
        return false;
    }

    if (!MoveFileA(path.c_str(), dst.c_str())) {
        log.error("Could not rename " + path + ". It may be in use.");
        return false;
    }

    // The sidecar follows, or the engine loses the record of what built it.
    // Its absence is not fatal, so a failure here is reported and not undone.
    const std::string metaOld = path + ".meta";
    if (fileExists(metaOld)) {
        const std::string metaNew = dst + ".meta";
        if (!MoveFileA(metaOld.c_str(), metaNew.c_str()))
            log.warn("Renamed the engine but not its .meta sidecar.");
    }

    log.info("Renamed to " + clean + ext + ".");
    return true;
}

bool removeModel(const std::string& path, Log& log) {
    if (!isInside(path, modelsDir()) && !isInside(path, convertedDir())) {
        log.warn("Refusing to delete a file outside the library.");
        return false;
    }
    if (!DeleteFileA(path.c_str())) {
        log.error("Could not delete " + fileName(path) +
                  ". It may be loaded, or open elsewhere.");
        return false;
    }
    log.info("Removed from library: " + fileName(path));
    return true;
}

std::vector<std::string> loadClassNames(const std::string& modelPath) {
    std::vector<std::string> names;
    const std::string dir  = modelPath.substr(0, modelPath.find_last_of("\\/") == std::string::npos
                                              ? 0 : modelPath.find_last_of("\\/"));
    const std::string base = stem(modelPath);

    const std::string candidates[] = {
        dir + "\\" + base + ".names",
        dir + "\\" + base + ".txt",
        dir + "\\classes.txt",
    };
    for (const auto& c : candidates) {
        std::ifstream f(c);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (!line.empty()) names.push_back(line);
        }
        if (!names.empty()) return names;
    }
    return names;
}

static std::string metaPathFor(const std::string& enginePath) {
    return enginePath + ".meta";
}

std::string findSourceFor(const std::string& enginePath) {
    // Shadowing the helper with a variable of the same name would hide it,
    // so the result gets its own.
    const std::string base = stem(enginePath);
    const std::string dir  = convertedDir();

    // The recorded source path is from the machine that built it, so only
    // the file name is portable. Both export formats are tried because
    // either could have produced the engine.
    const std::string candidates[] = {
        dir + "\\" + base + ".onnx",
        dir + "\\" + base + ".pt",
    };
    for (const auto& c : candidates) {
        std::ifstream f(c, std::ios::binary);
        if (f) return c;
    }

    // The meta may name something whose stem differs from the engine's.
    const EngineMeta m = loadEngineMeta(enginePath);
    if (!m.source.empty()) {
        const size_t slash = m.source.find_last_of("\\/");
        const std::string leaf = (slash == std::string::npos)
                               ? m.source : m.source.substr(slash + 1);
        const std::string c = dir + "\\" + leaf;
        std::ifstream f(c, std::ios::binary);
        if (f) return c;
    }
    return {};
}

bool saveEngineMeta(const std::string& enginePath, const EngineMeta& m) {
    std::ofstream f(metaPathFor(enginePath));
    if (!f) return false;
    f << "gpu=" << m.gpuName << "\n"
      << "sm=" << m.smMajor << "." << m.smMinor << "\n"
      << "trt=" << m.trtMajor << "." << m.trtMinor << "\n"
      << "source=" << m.source << "\n"
      << "builtAt=" << m.builtAt << "\n"
      << "fp16=" << (m.fp16 ? 1 : 0) << "\n"
      << "portable=" << (m.portable ? 1 : 0) << "\n"
      << "input=" << m.inputW << "x" << m.inputH << "\n"
      << "layout=" << m.layout << "\n"
      << "buildSeconds=" << m.buildSeconds << "\n"
      << "smokeMs=" << m.smokeMs << "\n"
      << "smokeOk=" << (m.smokeOk ? 1 : 0) << "\n";
    return true;
}

EngineMeta loadEngineMeta(const std::string& enginePath) {
    EngineMeta m;
    std::ifstream f(metaPathFor(enginePath));
    if (!f) return m;

    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if      (k == "gpu")     m.gpuName = v;
        else if (k == "source")  m.source = v;
        else if (k == "builtAt") m.builtAt = v;
        else if (k == "layout")  m.layout = v;
        else if (k == "fp16")    m.fp16 = (v == "1");
        else if (k == "portable") m.portable = (v == "1");
        else if (k == "smokeOk") m.smokeOk = (v == "1");
        else if (k == "buildSeconds") m.buildSeconds = atof(v.c_str());
        else if (k == "smokeMs")      m.smokeMs = atof(v.c_str());
        else if (k == "sm")  sscanf(v.c_str(), "%d.%d", &m.smMajor, &m.smMinor);
        else if (k == "trt") sscanf(v.c_str(), "%d.%d", &m.trtMajor, &m.trtMinor);
        else if (k == "input") sscanf(v.c_str(), "%dx%d", &m.inputW, &m.inputH);
    }
    m.valid = !m.gpuName.empty();
    return m;
}

// -------------------------------------------------------------- profiles

std::string sanitiseName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ')
            out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.empty()) out = "profile";
    if (out.size() > 48) out.resize(48);
    return out;
}

std::string profilePath(const std::string& name) {
    return configDir() + "\\" + sanitiseName(name) + ".cfg";
}

std::string lastSessionPath() { return configDir() + "\\last.cfg"; }

std::vector<Profile> listProfiles() {
    std::vector<Profile> out;
    const std::string pattern = configDir() + "\\*.cfg";

    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Profile p;
        p.name = stem(fd.cFileName);
        if (toLower(p.name) == "last") continue;   // session state, not a profile
        p.path = configDir() + "\\" + fd.cFileName;
        out.push_back(std::move(p));
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(),
              [](const Profile& a, const Profile& b) { return toLower(a.name) < toLower(b.name); });
    return out;
}

// ---------------------------------------------------------- serialisation

// One list drives both directions, so a field can never be saved but not
// loaded, or renamed in one place only.
#define LC_CONFIG_FIELDS(INT, FLT, BOOLEAN)      \
    INT(roiMode)        INT(fov)                 \
    INT(maxDets)        INT(boxThickness)        \
    INT(engineW)        INT(engineH)             \
    INT(workspaceMB)    INT(priorityMode)        \
    INT(inputMethod)    INT(serialPort)          \
    INT(baudRate)       INT(activationMode)      \
    INT(activationKey)  INT(predictionMethod)    \
    INT(uiTheme)        INT(uiStyle)             \
    INT(fpsFloor)       INT(maxStride)           \
    INT(perfMonEdge)                             \
    INT(perfMonDisplay) INT(activeTab)            \
    INT(resOverrideW)   INT(resOverrideH)        \
    INT(captureMonitor)                          \
    INT(windowX)        INT(windowY)             \
    INT(windowW)        INT(windowH)             \
    INT(outputLayout)   INT(targetPriority)      \
    INT(controlRateHz)  INT(actionFov)           \
    INT(classOrderCount) INT(serialRateHz)        \
    INT(boardType)      INT(fwMouseLayout)        \
    INT(serialProtocol) INT(staleMs)              \
    INT(captureMode)    INT(captureDelayMs)      \
    INT(captureFormat)  INT(captureQuality)      \
    INT(actionTrigger)  INT(actionKind)          \
    INT(actionButton)   INT(actionRadiusPx)      \
    INT(actionHoldMs)   INT(actionCooldownMs)    \
    INT(actionGapMs)    INT(actionKey)            \
    INT(activationKey2)                          \
    INT(captureSize)    INT(movePath)             \
    INT(buildOptLevel)                           \
    INT(captureKey)     INT(burstMs)             \
    INT(burstFrames)    INT(ladderCount)         \
    INT(fovThickness)   INT(deadzonePx)          \
    INT(boxAnchor)                               \
    FLT(roiOffXPct)     FLT(roiOffYPct)          \
    FLT(confThresh)     FLT(overlayAlpha)        \
    FLT(overlayMinConf) FLT(sensitivity)         \
    FLT(predictionMs)   FLT(emaIntensity)        \
    FLT(emaIntensityY)                           \
    FLT(predictionGain) FLT(responseScale)        \
    FLT(jitterReject)   FLT(perfGraphSecs)        \
    FLT(perfMonEdgeT)   FLT(maxSpeedPx)           \
    FLT(perfMonScale)                            \
    FLT(windowScale)                             \
    FLT(maxStepPx)      FLT(jumpRejectPx)        \
    FLT(aimOffXPct)     FLT(aimOffYPct)          \
    FLT(kalmanProcess)  FLT(kalmanMeasure)       \
    FLT(selfMotionFloor) FLT(leadSmoothMs)        \
    FLT(actionMinConf)  FLT(actionBoxPct)         \
    FLT(boxScaleRefPx)  FLT(boxScaleStrength)    \
    FLT(boxScaleFloor)  FLT(movePathAmount)       \
    FLT(maxAimSpeedPx)                           \
    FLT(stickyBiasPx)                            \
    FLT(assistWithPct)   FLT(assistAgainstPct)    \
    FLT(assistSpeedRef)  FLT(minorAxisCut)         \
    BOOLEAN(overlayOn)      BOOLEAN(drawRoiRect)      \
    BOOLEAN(drawLabels)     BOOLEAN(drawConfidence)   \
    BOOLEAN(drawCenterDot)  BOOLEAN(pipelineOn)       \
    BOOLEAN(alwaysOnTop)    BOOLEAN(buildFp16)        \
    BOOLEAN(controlEnabled)                      \
    BOOLEAN(predictionOn)   BOOLEAN(emaOn)            \
    BOOLEAN(tooltipsOn)                          \
    BOOLEAN(perfMonOn)      BOOLEAN(drawConfText)     \
    BOOLEAN(captureEnabled) BOOLEAN(keepGpuBoosted)    \
    BOOLEAN(userAssistOn)   BOOLEAN(drawLeadDot)     \
    BOOLEAN(sessionTracking) BOOLEAN(adaptiveLoad)   \
    BOOLEAN(advancedMode)   BOOLEAN(actionOn)        \
    BOOLEAN(movePathRamp)        \
    BOOLEAN(drawPath)        \
    BOOLEAN(yieldWhileWaiting)        \
    BOOLEAN(boxScaleOn)        \
    BOOLEAN(stickyTarget)        \
    BOOLEAN(actionNeedsKey)     \
    BOOLEAN(leadTrustOn)    BOOLEAN(leadSmoothScales) \
    BOOLEAN(leadHorizontalOnly)    \
    BOOLEAN(perfMonMeters)  BOOLEAN(windowMaximised)  \
    BOOLEAN(resOverride)     \
    BOOLEAN(actionFovOn)    BOOLEAN(aiEnabled)        \
    BOOLEAN(classFilterOn)  BOOLEAN(visibleToCapture)  \
    BOOLEAN(fwPassthrough)  BOOLEAN(fwDumpReports)    \
    BOOLEAN(arduinoOnlyInput) BOOLEAN(autoResponse) \
    BOOLEAN(lagCompOn)  \
    BOOLEAN(safetyOn)       BOOLEAN(captureLabels)    \
    BOOLEAN(buildEmbedNms)  BOOLEAN(buildPortable)   \
    BOOLEAN(captureSessionFolders)

bool saveConfig(const std::string& path, const Config& cfg,
                const std::string& modelPath, Log& log)
{
    if (!ensureDirs(log)) return false;

    // Written to a temporary first. Writing in place means an interrupted
    // save leaves a half-finished file, and the settings from the whole
    // session are gone.
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp);
    if (!f) {
        log.error("Could not write " + tmp);
        return false;
    }

    f << "# loopcore configuration\n";
    f << "version=1\n";
    f << "model=" << modelPath << "\n";

#define W_INT(name)  f << #name "=" << cfg.name << "\n";
#define W_FLT(name)  f << #name "=" << cfg.name << "\n";
#define W_BOOL(name) f << #name "=" << (cfg.name ? 1 : 0) << "\n";
    LC_CONFIG_FIELDS(W_INT, W_FLT, W_BOOL)
#undef W_INT
#undef W_FLT
#undef W_BOOL

    f << "boxColor=" << cfg.boxColor[0] << "," << cfg.boxColor[1] << ","
                     << cfg.boxColor[2] << "\n";
    f << "roiColor=" << cfg.roiColor[0] << "," << cfg.roiColor[1] << ","
                     << cfg.roiColor[2] << "\n";
    f << "capturePath=" << cfg.capturePath << "\n";
    f << "ladder=";
    for (int i = 0; i < cfg.ladderCount && i < Config::kLadderMax; ++i)
        f << (i ? "," : "") << cfg.ladderConf[i] << ":" << cfg.ladderMs[i];
    f << "\n";
    f << "boardDevice=" << cfg.boardDevice << "\n";
    for (int t = 0; t < Config::kOrderTabs; ++t) {
        f << "cardFold" << t << "=" << cfg.cardCollapsed[t] << "\n";
        f << "cardSplit" << t << "=" << cfg.cardSplit[t] << "\n";
        f << "cardOrder" << t << "=";
        for (int i = 0; i < cfg.cardOrderCount[t]; ++i)
            f << (i ? "," : "") << cfg.cardOrder[t][i];
        f << "\n";
    }
    f << "classMask=" << (unsigned long)cfg.classMask << "\n";
    f << "classOrder=";
    for (int i = 0; i < cfg.classOrderCount && i < 32; ++i)
        f << (i ? "," : "") << cfg.classOrder[i];
    f << "\n";

    f.flush();
    f.close();
    if (!f) { log.error("Failed while writing " + tmp); return false; }

    // MoveFileEx with REPLACE_EXISTING is atomic enough for this: either the
    // old file or the new one exists, never a mixture of the two.
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        log.error("Could not replace " + path);
        return false;
    }
    return true;
}

uint64_t configFingerprint(const Config& cfg, const std::string& modelPath) {
    // Copied through a zeroed buffer first. Hashing the struct directly
    // includes its padding bytes, and a memberwise copy leaves those
    // indeterminate -- so the hash changed every single frame and the
    // config was being rewritten to disk every 1.5 seconds forever.
    static thread_local std::vector<unsigned char> buf;
    buf.assign(sizeof(Config), 0);
    std::memcpy(buf.data(), &cfg, sizeof(Config));

    uint64_t h = 1469598103934665603ull;
    for (unsigned char b : buf) { h ^= b; h *= 1099511628211ull; }
    for (char c : modelPath) { h ^= (unsigned char)c; h *= 1099511628211ull; }
    return h;
}

bool loadConfig(const std::string& path, Config& cfg,
                std::string& modelPathOut, Log& log)
{
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key.empty()) continue;

        if (key == "model") { modelPathOut = val; continue; }

        bool handled = false;
#define R_INT(name)  if (!handled && key == #name) { cfg.name = std::atoi(val.c_str()); handled = true; }
#define R_FLT(name)  if (!handled && key == #name) { cfg.name = (float)std::atof(val.c_str()); handled = true; }
#define R_BOOL(name) if (!handled && key == #name) { cfg.name = std::atoi(val.c_str()) != 0; handled = true; }
        LC_CONFIG_FIELDS(R_INT, R_FLT, R_BOOL)
#undef R_INT
#undef R_FLT
#undef R_BOOL
        if (handled) continue;

        if (key.rfind("cardFold", 0) == 0 && key.size() == 9) {
            const int t = key[8] - '0';
            if (t >= 0 && t < Config::kOrderTabs)
                cfg.cardCollapsed[t] = (unsigned)strtoul(val.c_str(), nullptr, 10);
            continue;
        }
        if (key.rfind("cardSplit", 0) == 0 && key.size() == 10) {
            const int t = key[9] - '0';
            if (t >= 0 && t < Config::kOrderTabs)
                cfg.cardSplit[t] = std::atoi(val.c_str());
            continue;
        }
        // A saved order from a build with more cards is discarded rather
        // than partially applied: the indices refer to a list that no longer
        // exists, and honouring some of them would scatter the rest.
        if (key.rfind("cardOrder", 0) == 0 && key.size() == 10) {
            const int t = key[9] - '0';
            if (t >= 0 && t < Config::kOrderTabs) {
                std::istringstream ss(val);
                std::string part;
                cfg.cardOrderCount[t] = 0;
                while (std::getline(ss, part, ',') &&
                       cfg.cardOrderCount[t] < Config::kOrderMax) {
                    if (part.empty()) continue;
                    cfg.cardOrder[t][cfg.cardOrderCount[t]++] =
                        std::clamp(std::atoi(part.c_str()), 0, Config::kOrderMax - 1);
                }
            }
            continue;
        }
        if (key == "boardDevice") {
            strncpy_s(cfg.boardDevice, sizeof(cfg.boardDevice),
                      val.c_str(), _TRUNCATE);
            continue;
        }
        if (key == "ladder") {
            std::istringstream ss(val);
            std::string part;
            cfg.ladderCount = 0;
            while (std::getline(ss, part, ',') &&
                   cfg.ladderCount < Config::kLadderMax) {
                const size_t colon = part.find(':');
                if (colon == std::string::npos) continue;
                cfg.ladderConf[cfg.ladderCount] =
                    (float)std::atof(part.substr(0, colon).c_str());
                cfg.ladderMs[cfg.ladderCount] =
                    std::atoi(part.substr(colon + 1).c_str());
                ++cfg.ladderCount;
            }
            continue;
        }
        if (key == "capturePath") {
            strncpy_s(cfg.capturePath, sizeof(cfg.capturePath),
                      val.c_str(), _TRUNCATE);
            continue;
        }
        if (key == "classMask") {
            cfg.classMask = (uint32_t)strtoul(val.c_str(), nullptr, 10);
            continue;
        }
        if (key == "classOrder") {
            std::istringstream ss(val);
            std::string part;
            cfg.classOrderCount = 0;
            while (std::getline(ss, part, ',') && cfg.classOrderCount < 32) {
                if (part.empty()) continue;
                cfg.classOrder[cfg.classOrderCount++] =
                    std::clamp(std::atoi(part.c_str()), 0, 31);
            }
            continue;
        }
        if (key == "boxColor" || key == "roiColor") {
            float* dst = (key == "boxColor") ? cfg.boxColor : cfg.roiColor;
            std::istringstream ss(val);
            std::string part;
            for (int i = 0; i < 3 && std::getline(ss, part, ','); ++i)
                dst[i] = (float)std::atof(part.c_str());
        }
    }

    // Guard against a hand-edited or corrupted file putting the app into a
    // state with no way back through the UI.
    cfg.fov         = std::clamp(cfg.fov, 128, 1280);
    cfg.engineW     = std::clamp(cfg.engineW, 128, 1280);
    cfg.engineH     = std::clamp(cfg.engineH, 128, 1280);
    cfg.maxDets     = std::clamp(cfg.maxDets, 1, 300);
    cfg.confThresh  = std::clamp(cfg.confThresh, 0.01f, 0.95f);
    cfg.workspaceMB = std::clamp(cfg.workspaceMB, 256, 8192);
    cfg.roiMode     = std::clamp(cfg.roiMode, 0, 1);
    cfg.priorityMode= std::clamp(cfg.priorityMode, 0, 2);
    cfg.roiOffXPct  = std::clamp(cfg.roiOffXPct, -50.0f, 50.0f);
    cfg.roiOffYPct  = std::clamp(cfg.roiOffYPct, -50.0f, 50.0f);
    cfg.boxAnchor   = std::clamp(cfg.boxAnchor, 0, 3);
    cfg.aimOffXPct  = std::clamp(cfg.aimOffXPct, -50.0f, 50.0f);
    cfg.aimOffYPct  = std::clamp(cfg.aimOffYPct, -50.0f, 50.0f);
    cfg.inputMethod = std::clamp(cfg.inputMethod, 0, 2);
    cfg.serialPort  = std::clamp(cfg.serialPort, 1, 64);
    cfg.sensitivity = std::clamp(cfg.sensitivity, 0.0f, 2.0f);
    cfg.emaIntensity= std::clamp(cfg.emaIntensity, 0.0f, 0.98f);
    cfg.activationMode   = std::clamp(cfg.activationMode, 0, 2);
    // Every key code, not just the first.
    //
    // These go straight to GetAsyncKeyState, which takes a virtual-key code
    // in 0..254. The original was bounded when it was the only one; the three
    // added since were not, so a hand-edited or corrupted config could put an
    // out-of-range value into a Windows call.
    cfg.activationKey    = std::clamp(cfg.activationKey, 0, 254);
    cfg.activationKey2   = std::clamp(cfg.activationKey2, 0, 254);
    cfg.actionKey        = std::clamp(cfg.actionKey, 0, 254);
    cfg.captureKey       = std::clamp(cfg.captureKey, 0, 254);
    cfg.predictionMethod = std::clamp(cfg.predictionMethod, 0, 5);
    cfg.jitterReject     = std::clamp(cfg.jitterReject, 0.0f, 15.0f);
    // The lead reaches a second now, so the old ceiling would silently undo
    // anything set past it.
    cfg.predictionMs     = std::clamp(cfg.predictionMs, 0.0f, 1000.0f);
    cfg.minorAxisCut     = std::clamp(cfg.minorAxisCut, 0.0f, 100.0f);
    cfg.serialRateHz     = std::clamp(cfg.serialRateHz, 60, 1000);
    cfg.boardType        = std::clamp(cfg.boardType, 0, 3);
    cfg.serialProtocol   = std::clamp(cfg.serialProtocol, 0, 1);
    cfg.perfGraphSecs    = std::clamp(cfg.perfGraphSecs, 0.5f, 20.0f);
    cfg.predictionGain   = std::clamp(cfg.predictionGain, 0.0f, 3.0f);
    cfg.uiTheme          = std::clamp(cfg.uiTheme, 0, 7);
    cfg.uiStyle          = std::clamp(cfg.uiStyle, 0, 3);

    cfg.outputLayout     = std::clamp(cfg.outputLayout, 0, 4);
    // Eight options now. The ceiling was left at four when the later ones
    // were added, so anything past "under the origin" was quietly reset to
    // the first entry on every load -- which reads as the setting not saving
    // at all.
    cfg.targetPriority   = std::clamp(cfg.targetPriority, 0, 7);
    cfg.controlRateHz    = std::clamp(cfg.controlRateHz, 60, 2000);
    cfg.actionFov        = std::clamp(cfg.actionFov, 32, 1280);
    cfg.fovThickness     = std::clamp(cfg.fovThickness, 1, 8);
    cfg.responseScale    = std::clamp(cfg.responseScale, 0.02f, 20.0f);
    cfg.deadzonePx       = std::clamp(cfg.deadzonePx, 0, 60);
    cfg.maxSpeedPx       = std::clamp(cfg.maxSpeedPx, 100.0f, 20000.0f);
    cfg.maxStepPx        = std::clamp(cfg.maxStepPx, 2.0f, 400.0f);
    cfg.staleMs          = std::clamp(cfg.staleMs, 40, 2000);
    cfg.jumpRejectPx     = std::clamp(cfg.jumpRejectPx, 20.0f, 4000.0f);
    cfg.classOrderCount  = std::clamp(cfg.classOrderCount, 0, 32);
    cfg.captureMode      = std::clamp(cfg.captureMode, 0, 2);
    cfg.captureDelayMs   = std::clamp(cfg.captureDelayMs, 10, 60000);
    cfg.captureFormat    = std::clamp(cfg.captureFormat, 0, 1);
    cfg.captureQuality   = std::clamp(cfg.captureQuality, 1, 100);
    cfg.captureSize      = std::clamp(cfg.captureSize, 64, 1600);
    cfg.actionTrigger    = std::clamp(cfg.actionTrigger, 0, (int)TrigCount - 1);
    cfg.actionKind       = std::clamp(cfg.actionKind, 0, (int)ActionKindCount - 1);
    cfg.actionButton     = std::clamp(cfg.actionButton, 0, 2);
    // Literal rather than FwKindCount: that enum lives in flasher.h, which
    // this file does not include, and pulling the flasher in to bound one
    // integer would be the wrong trade.
    cfg.actionRadiusPx   = std::clamp(cfg.actionRadiusPx, 1, 300);
    cfg.actionMinConf    = std::clamp(cfg.actionMinConf, 0.0f, 1.0f);
    cfg.actionBoxPct     = std::clamp(cfg.actionBoxPct, 5.0f, 200.0f);
    cfg.boxScaleRefPx    = std::clamp(cfg.boxScaleRefPx, 8.0f, 2000.0f);
    cfg.boxScaleStrength = std::clamp(cfg.boxScaleStrength, 0.0f, 1.0f);
    // Never above the sensitivity it is a floor for, whatever the file says.
    // Derived from the enum, not a literal. A hardcoded ceiling is exactly
    // how targetPriority ended up silently resetting anything past the
    // fourth option when four more were added.
    // The list was reordered by how closely each path resembles a hand, so
    // a number written before that reorder now names a different one.
    //
    // Nothing is migrated: the setting is a day old, its default is zero,
    // and zero still means Default. Anyone who had picked something else
    // gets a different path rather than a broken one, which is a smaller
    // cost than carrying a version map for a field with one release behind
    // it. Said here so the next person reading this does not conclude the
    // omission was an oversight.
    cfg.movePath         = std::clamp(cfg.movePath, 0, (int)PathCount - 1);
    cfg.buildOptLevel    = std::clamp(cfg.buildOptLevel, 0, 5);
    // Four layouts. This becomes a -D define on the firmware build, so an
    // out-of-range value would compile a sketch against a layout that does
    // not exist -- a build failure with no obvious cause.
    cfg.fwMouseLayout    = std::clamp(cfg.fwMouseLayout, 0, 3);
    cfg.movePathAmount   = std::clamp(cfg.movePathAmount, 0.0f, 1.0f);
    cfg.boxScaleFloor    = std::clamp(cfg.boxScaleFloor, 0.0f,
                                      std::max(0.01f, cfg.sensitivity));
    cfg.maxAimSpeedPx    = std::clamp(cfg.maxAimSpeedPx, 500.0f, 200000.0f);
    cfg.actionHoldMs     = std::clamp(cfg.actionHoldMs, 0, 2000);
    cfg.actionCooldownMs = std::clamp(cfg.actionCooldownMs, 0, 5000);
    cfg.actionGapMs      = std::clamp(cfg.actionGapMs, 5, 400);

    // Bounds for everything a slider offers but the loader did not police.
    //
    // A config is a text file people edit, and it survives across versions
    // where a field's meaning may have changed. Anything read from it and
    // then used as a divisor, a texture dimension or a loop bound needs a
    // range here, not just on the slider that usually writes it.
    //
    // kalmanMeasure is the one that actually mattered: it is used raw as the
    // measurement noise, and a zero there drives the innovation covariance
    // to zero and the filter gain to infinity, so the prediction would jump
    // to wherever the first detection landed and stay there.
    cfg.kalmanMeasure    = std::clamp(cfg.kalmanMeasure, 0.5f, 400.0f);
    cfg.kalmanProcess    = std::clamp(cfg.kalmanProcess, 1.0f, 60000.0f);
    cfg.assistSpeedRef   = std::clamp(cfg.assistSpeedRef, 5.0f, 20000.0f);
    cfg.assistWithPct    = std::clamp(cfg.assistWithPct, 0.0f, 1000.0f);
    cfg.assistAgainstPct = std::clamp(cfg.assistAgainstPct, 0.0f, 1000.0f);
    cfg.selfMotionFloor  = std::clamp(cfg.selfMotionFloor, 0.0f, 100.0f);
    cfg.leadSmoothMs     = std::clamp(cfg.leadSmoothMs, 0.0f, 5000.0f);
    cfg.stickyBiasPx     = std::clamp(cfg.stickyBiasPx, 0.0f, 5000.0f);
    cfg.minorAxisCut     = std::clamp(cfg.minorAxisCut, 0.0f, 100.0f);
    cfg.overlayAlpha     = std::clamp(cfg.overlayAlpha, 0.02f, 1.0f);
    cfg.overlayMinConf   = std::clamp(cfg.overlayMinConf, 0.0f, 1.0f);
    cfg.emaIntensity     = std::clamp(cfg.emaIntensity, 0.0f, 0.99f);
    cfg.emaIntensityY    = std::clamp(cfg.emaIntensityY, 0.0f, 0.99f);
    cfg.boxThickness     = std::clamp(cfg.boxThickness, 1, 16);
    cfg.fovThickness     = std::clamp(cfg.fovThickness, 1, 16);
    cfg.maxStride        = std::clamp(cfg.maxStride, 1, 16);
    cfg.fpsFloor         = std::clamp(cfg.fpsFloor, 15, 1000);
    // Texture dimensions: an absurd value here is an allocation failure at
    // best and a driver reset at worst.
    cfg.resOverrideW     = std::clamp(cfg.resOverrideW, 320, 16384);
    cfg.resOverrideH     = std::clamp(cfg.resOverrideH, 240, 16384);
    cfg.ladderCount      = std::clamp(cfg.ladderCount, 1, Config::kLadderMax);
    cfg.burstFrames      = std::clamp(cfg.burstFrames, 0, 500);
    cfg.burstMs          = std::clamp(cfg.burstMs, 5, 2000);
    for (int i = 0; i < Config::kLadderMax; ++i) {
        cfg.ladderConf[i] = std::clamp(cfg.ladderConf[i], 0.0f, 1.0f);
        cfg.ladderMs[i]   = std::clamp(cfg.ladderMs[i], 0, 10000);
    }
    cfg.perfMonEdge      = std::clamp(cfg.perfMonEdge, 0, 3);
    cfg.activeTab        = std::clamp(cfg.activeTab, 0, 5);
    // Sizes are clamped, positions are not: a negative x is legitimate on a
    // monitor arranged to the left of the primary one.
    cfg.windowW          = std::clamp(cfg.windowW, 480, 8000);
    cfg.windowH          = std::clamp(cfg.windowH, 360, 8000);
    cfg.windowScale      = std::clamp(cfg.windowScale, 0.5f, 4.0f);
    cfg.perfMonEdgeT     = std::clamp(cfg.perfMonEdgeT, 0.0f, 1.0f);
    cfg.perfMonScale     = std::clamp(cfg.perfMonScale, 0.6f, 2.5f);

    log.info("Loaded settings from " + fileName(path));
    return true;
}

bool removeProfile(const std::string& path, Log& log) {
    if (!DeleteFileA(path.c_str())) {
        log.error("Could not delete " + fileName(path));
        return false;
    }
    log.info("Deleted profile " + stem(path));
    return true;
}

} // namespace lc::store
