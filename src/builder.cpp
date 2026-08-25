// builder.cpp
#include "builder.h"

#include <windows.h>

#include <NvInfer.h>
#include <NvInferPlugin.h>
#include <NvOnnxParser.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <mutex>
#include <vector>

using namespace nvinfer1;

namespace lc {

namespace {

class BuildLogger : public ILogger {
public:
    Log* sink = nullptr;
    void log(Severity s, const char* msg) noexcept override {
        if (!sink) return;
        if (s == Severity::kERROR || s == Severity::kINTERNAL_ERROR)
            sink->error(std::string("build: ") + msg);
        else if (s == Severity::kWARNING)
            sink->warn(std::string("build: ") + msg);
    }
};

#if NV_TENSORRT_MAJOR >= 10
// TensorRT reports nested phases: "Building engine" contains "Computing
// costs", which contains per-layer work. A monitor that remembers only the
// most recent phase divides a child's step count by the parent's total,
// which is where "137 / 4" and a bar running off the end come from.
//
// So: keep the whole stack. The headline stays on the outermost phase, which
// is stable, and the innermost supplies the detail line. Reports are
// throttled, because TensorRT emits thousands per second and the UI cannot
// usefully show that.
class Monitor : public IProgressMonitor {
public:
    ModelBuilder* owner = nullptr;

    void phaseStart(char const* phase, char const* parent,
                    int32_t nbSteps) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        const std::string name = phase ? phase : "";
        if (!parent || !*parent) stack_.clear();      // a new root
        else unwindTo(parent);
        stack_.push_back({name, nbSteps > 0 ? nbSteps : 1, 0});
        report(true);
    }

    bool stepComplete(char const* phase, int32_t step) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        const std::string name = phase ? phase : "";
        for (auto& p : stack_)
            if (p.name == name) { p.step = step; break; }
        report(false);
        // The one place a build can be stopped: TensorRT checks this return
        // between optimisation steps and unwinds if it is false.
        return !owner || !owner->cancelRequested();
    }

    void phaseFinish(char const* phase) noexcept override {
        std::lock_guard<std::mutex> lk(m_);
        const std::string name = phase ? phase : "";
        for (size_t i = 0; i < stack_.size(); ++i) {
            if (stack_[i].name == name) { stack_.resize(i); break; }
        }
        report(true);
    }

private:
    struct Phase { std::string name; int32_t total; int32_t step; };

    void unwindTo(const std::string& parent) {
        for (size_t i = 0; i < stack_.size(); ++i)
            if (stack_[i].name == parent) { stack_.resize(i + 1); return; }
    }

    // Overall fraction: each level contributes its own progress scaled by
    // the slice of the parent it occupies, so the result only ever climbs.
    void report(bool force) {
        if (!owner || stack_.empty()) return;

        const int64_t now = now_ns();
        if (!force && now - lastReport_ < 100'000'000) return;   // 10 Hz
        lastReport_ = now;

        double frac = 0.0, scale = 1.0;
        for (size_t i = 0; i < stack_.size() && i < 3; ++i) {
            const Phase& p = stack_[i];
            const double t = (double)p.total;
            frac  += scale * (double)p.step / t;
            scale /= t;
        }
        frac = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);

        const Phase& deepest = stack_.back();
        char detail[192];
        if (stack_.size() > 1)
            snprintf(detail, sizeof(detail), "%s  %d / %d",
                     deepest.name.c_str(), deepest.step, deepest.total);
        else
            snprintf(detail, sizeof(detail), "%d / %d", deepest.step, deepest.total);

        owner->setPhase(stack_.front().name, (float)frac, detail);
    }

    std::mutex          m_;
    std::vector<Phase>  stack_;
    int64_t             lastReport_ = 0;
};
#endif

std::string dirOf(const std::string& p) {
    size_t i = p.find_last_of("\\/");
    return i == std::string::npos ? std::string(".") : p.substr(0, i);
}

std::string stemOf(const std::string& p) {
    size_t a = p.find_last_of("\\/");
    a = (a == std::string::npos) ? 0 : a + 1;
    size_t b = p.find_last_of('.');
    if (b == std::string::npos || b < a) b = p.size();
    return p.substr(a, b - a);
}

bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs a command, streaming its output into the log a line at a time.
bool runCommand(const std::string& cmd, Log& log, ModelBuilder* mb,
                const char* phaseLabel)
{
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        log.error("CreatePipe failed while starting the exporter.");
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        log.error("Could not start Python. Install it and ultralytics, or "
                  "export the model yourself and load the .onnx or .engine.");
        return false;
    }
    CloseHandle(wr);

    std::string line;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = '\0';
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (!line.empty()) {
                    log.info(line);
                    if (mb) mb->setPhase(phaseLabel, -1.0f, line);
                    line.clear();
                }
            } else {
                line.push_back(buf[i]);
            }
        }
    }
    if (!line.empty()) log.info(line);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

} // namespace

// -------------------------------------------------------------- helpers

std::string ModelBuilder::extensionOf(const std::string& path) {
    size_t i = path.find_last_of('.');
    if (i == std::string::npos) return {};
    std::string e = path.substr(i + 1);
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e;
}

bool ModelBuilder::needsConversion(const std::string& path) {
    const std::string e = extensionOf(path);
    return e == "pt" || e == "onnx";
}

ModelBuilder::~ModelBuilder() {
    if (worker_.joinable()) worker_.join();
}

void ModelBuilder::markBuildStart() {
    // Cleared here rather than at the end of the previous build: a cancel
    // arriving just as one finished would otherwise kill the next one.
    cancelReq_ = false;

    std::lock_guard<std::mutex> lk(m_);
    buildStartNs_ = now_ns();
    marks_.clear();
    etaSmooth_ = -1.0;
    etaStampNs_ = 0;
    progressMax_ = 0.0f;
    roughTotalSec_ = 0.0;
    srcBytes_ = 0;
    // The source is already recorded as a member by run(); this function
    // takes no arguments, so reading it from there is the only correct way
    // to get at it.
    if (!srcPath_.empty()) {
        std::ifstream f(srcPath_, std::ios::binary | std::ios::ate);
        if (f) srcBytes_ = (int64_t)f.tellg();
    }
}

void ModelBuilder::setPhase(const std::string& phase, float fraction,
                            const std::string& detail) {
    std::lock_guard<std::mutex> lk(m_);
    prog_.phase    = phase;
    prog_.fraction = fraction;
    prog_.detail   = detail;

    // Two separate problems made this misbehave. Estimating from the average
    // rate since the start makes the number climb, because TensorRT's early
    // phases move the fraction slowly. And its fraction is not monotonic --
    // a new root phase resets it -- so any slope taken across that reset is
    // negative, which is why the estimate vanished mid-build.
    if (fraction > progressMax_) progressMax_ = fraction;
    const float p = progressMax_;
    prog_.fraction = p;

    const int64_t now = now_ns();

    // Once there is an estimate, count it down on the wall clock. Phases
    // like timing graph nodes report no progress for many seconds, and a
    // countdown that freezes or disappears during them reads as broken.
    // A rough figure from experience, used until the measured one settles.
    //
    // TensorRT reports no progress at all during some phases, so an estimate
    // derived only from its own reporting is absent exactly when someone is
    // wondering whether the thing has hung. A build of this kind takes a few
    // minutes on typical hardware; saying so is far more use than saying
    // nothing, provided it is offered as the guess it is.
    if (roughTotalSec_ <= 0.0) {
        double guess = 180.0;
        // Bigger inputs and bigger sources cost more, roughly in proportion.
        if (srcBytes_ > 0) guess += (double)srcBytes_ / (2.0 * 1024 * 1024) * 12.0;
        roughTotalSec_ = std::clamp(guess, 45.0, 900.0);
    }

    if (etaSmooth_ > 0.0 && etaStampNs_ > 0) {
        const double spent = (now - etaStampNs_) / 1e9;
        const double left = etaSmooth_ - spent;
        // Once the countdown runs out, the estimate was simply wrong. Saying
        // so is better than parking at one second and pretending, which is
        // what it did before.
        prog_.etaSeconds = (left > 0.5) ? left : -1.0;
    } else {
        // Falls back to the rough figure less whatever has already elapsed,
        // floored so it never claims to be finishing imminently.
        const double spent = (now - buildStartNs_) / 1e9;
        prog_.etaSeconds = (buildStartNs_ > 0)
                         ? std::max(15.0, roughTotalSec_ - spent) : -1.0;
        prog_.etaIsGuess = true;
    }

    if (buildStartNs_ <= 0 || p <= 0.0f || p >= 0.999f) return;

    marks_.push_back({now, p});
    while (marks_.size() > 2 &&
           (now - marks_.front().ns) > 25'000'000'000LL)   // 25 s window
        marks_.pop_front();

    if (marks_.size() < 2) return;
    const double dtSec = (now - marks_.front().ns) / 1e9;
    const double dFrac = (double)p - marks_.front().frac;
    if (dtSec < 4.0 || dFrac <= 1e-4) return;   // not enough signal to revise

    const double raw = (1.0 - p) * dtSec / dFrac;

    // Asymmetric: drop quickly when the build speeds up, rise slowly when it
    // stalls. A countdown that jumps upward reads as broken even when it is
    // technically more accurate.
    if (etaSmooth_ < 0.0) etaSmooth_ = raw;
    else if (raw < etaSmooth_) etaSmooth_ = etaSmooth_ * 0.7 + raw * 0.3;
    else                       etaSmooth_ = etaSmooth_ * 0.93 + raw * 0.07;

    etaStampNs_ = now;
    prog_.etaSeconds = etaSmooth_;
    prog_.etaIsGuess = false;
}

ModelBuilder::Progress ModelBuilder::progress() const {
    std::lock_guard<std::mutex> lk(m_);
    return prog_;
}

std::string ModelBuilder::outputPath() const {
    std::lock_guard<std::mutex> lk(m_);
    return outPath_;
}

std::string ModelBuilder::sourcePath() const {
    std::lock_guard<std::mutex> lk(m_);
    return srcPath_;
}

bool ModelBuilder::takeCompletion(std::string& enginePathOut) {
    bool expected = true;
    if (!completionPending_.compare_exchange_strong(expected, false)) return false;
    enginePathOut = outputPath();
    return true;
}

// ---------------------------------------------------------------- start

bool ModelBuilder::start(const std::string& inputPath, const BuildOptions& opts,
                         Log& log)
{
    if (state_.load() == BuildState::Running) return false;
    if (worker_.joinable()) worker_.join();

    state_ = BuildState::Running;
    setPhase("Starting", 0.0f);
    worker_ = std::thread(&ModelBuilder::run, this, inputPath, opts, &log);
    return true;
}

void ModelBuilder::run(std::string input, BuildOptions opts, Log* logp) {
    {
        std::lock_guard<std::mutex> lk(m_);
        srcPath_ = input;
    }
    Log& log = *logp;

    if (!fileExists(input)) {
        log.error("No file at " + input);
        setPhase("Failed", 0.0f, "file not found");
        state_ = BuildState::Failed;
        return;
    }

    const std::string ext = extensionOf(input);
    std::string onnx = input;

    if (ext == "pt") {
        log.info("Converting " + input + " via ultralytics.");
        if (!exportPtToOnnx(input, opts, onnx, log)) {
            setPhase("Failed", 0.0f, "PyTorch export failed");
            state_ = BuildState::Failed;
            return;
        }
    } else if (ext != "onnx") {
        log.error("Cannot convert a ." + ext + " file. Give a .pt, .onnx, or .engine.");
        state_ = BuildState::Failed;
        return;
    }

    const std::string outDir = opts.outputDir.empty() ? dirOf(input)
                                                      : opts.outputDir;
    const std::string enginePath = outDir + "\\" + stemOf(input) + ".engine";

    if (!buildEngine(onnx, opts, enginePath, log)) {
        setPhase("Failed", 0.0f, "engine build failed");
        state_ = BuildState::Failed;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_);
        outPath_ = enginePath;
    }
    setPhase("Done", 1.0f, enginePath);
    log.info("Engine written to " + enginePath);
    completionPending_ = true;
    state_ = BuildState::Done;
}

// --------------------------------------------------------- pt -> onnx

bool ModelBuilder::exportPtToOnnx(const std::string& pt, const BuildOptions& o,
                                  std::string& onnxOut, Log& log)
{
    setPhase("Exporting PyTorch model", -1.0f, "starting ultralytics");

    std::ostringstream py;
    py << "python -c \""
       << "from ultralytics import YOLO; "
       << "m = YOLO(r'" << pt << "'); "
       << "m.export(format='onnx', imgsz=(" << o.height << "," << o.width << "), "
       << "opset=17, nms=" << (o.embedNms ? "True" : "False")
       << ", simplify=True, dynamic=False)"
       << "\"";

    log.info("Running: " + py.str());
    if (!runCommand(py.str(), log, this, "Exporting PyTorch model")) {
        log.error("ultralytics export failed. Check that Python and "
                  "ultralytics are installed:  pip install ultralytics");
        return false;
    }

    onnxOut = dirOf(pt) + "\\" + stemOf(pt) + ".onnx";
    if (!fileExists(onnxOut)) {
        log.error("Export reported success but " + onnxOut + " is not there.");
        return false;
    }
    return true;
}

// ------------------------------------------------------ onnx -> engine

bool ModelBuilder::buildEngine(const std::string& onnx, const BuildOptions& o,
                               const std::string& enginePath, Log& log)
{
    static BuildLogger blogger;
    blogger.sink = &log;

    static bool pluginsReady = false;
    if (!pluginsReady) pluginsReady = initLibNvInferPlugins(&blogger, "");

    setPhase("Parsing ONNX", -1.0f, onnx);

    auto* builder = createInferBuilder(blogger);
    if (!builder) { log.error("createInferBuilder failed."); return false; }

    auto* network = builder->createNetworkV2(0);
    if (!network) { log.error("createNetworkV2 failed."); delete builder; return false; }

    auto* parser = nvonnxparser::createParser(*network, blogger);
    if (!parser) {
        log.error("Could not create the ONNX parser.");
        delete network; delete builder; return false;
    }

    if (!parser->parseFromFile(onnx.c_str(), (int)ILogger::Severity::kWARNING)) {
        for (int i = 0; i < parser->getNbErrors(); ++i)
            log.error(std::string("ONNX: ") + parser->getError(i)->desc());
        log.error("The ONNX file could not be parsed. If it came from an older "
                  "ultralytics, re-export with opset=17.");
        delete parser; delete network; delete builder;
        return false;
    }

    auto* config = builder->createBuilderConfig();
    if (!config) {
        // Dereferenced on the next line, so a null here is a crash rather
        // than a failed build. Every other TensorRT object in this function
        // is checked; this one was missed.
        log.error("createBuilderConfig failed. The card is probably out of "
                  "memory -- close anything else using it and try again.");
        delete parser; delete network; delete builder;
        return false;
    }

    config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE,
                               size_t(o.workspaceMB) * 1024ull * 1024ull);

    // How exhaustively TensorRT searches for kernels.
    //
    // The default is 3 and this was never set, so every engine built here
    // has been built at it. Level 5 tries far more tactics per layer -- it
    // is where the tuned fp16 tensor-core kernels get chosen over generic
    // ones, which is the difference this card cares about most.
    //
    // It costs build time and nothing else: the engine is written once and
    // used for as long as the model lasts, so trading minutes at build for
    // milliseconds every frame is the right way round.
#if NV_TENSORRT_MAJOR >= 9 || (NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR >= 6)
    {
        const int lvl = std::clamp(o.optLevel, 0, 5);
        config->setBuilderOptimizationLevel(lvl);
        log.info("Builder optimization level " + std::to_string(lvl) +
                 " (the default is 3; higher searches more kernels and takes "
                 "longer to build).");
    }
#endif

    if (o.fp16) {
        if (builder->platformHasFastFp16()) {
            config->setFlag(BuilderFlag::kFP16);
            log.info("FP16 enabled.");
        } else {
            log.warn("This GPU has no fast FP16 path; building FP32.");
        }
    }

    if (o.portable) {
#if NV_TENSORRT_MAJOR >= 9 || (NV_TENSORRT_MAJOR == 8 && NV_TENSORRT_MINOR >= 6)
        // Ampere and newer only. TensorRT offers no level that reaches back
        // to Turing, so an engine built this way runs on a 30, 40 or 50
        // series card and is still rejected by a 20 series one.
        config->setHardwareCompatibilityLevel(
            HardwareCompatibilityLevel::kAMPERE_PLUS);
        log.warn("Portable build: this plan will run on any Ampere or newer "
                 "card, and will still be rejected by Turing and older. It "
                 "cannot use kernels specific to this GPU, so expect it to be "
                 "slower than a native build -- often noticeably.");
#else
        log.warn("Portable builds need TensorRT 8.6 or newer. Building "
                 "normally for this card instead.");
#endif
    }

    // Dynamic inputs need a profile, or the build fails with a shape error.
    bool anyDynamic = false;
    for (int i = 0; i < network->getNbInputs(); ++i) {
        Dims d = network->getInput(i)->getDimensions();
        for (int k = 0; k < d.nbDims; ++k) if (d.d[k] < 0) anyDynamic = true;
    }
    if (anyDynamic) {
        auto* profile = builder->createOptimizationProfile();
        for (int i = 0; i < network->getNbInputs(); ++i) {
            auto* in = network->getInput(i);
            Dims d = in->getDimensions();
            if (d.nbDims == 4) {
                Dims fixed = d;
                fixed.d[0] = 1;
                fixed.d[1] = d.d[1] > 0 ? d.d[1] : 3;
                fixed.d[2] = o.height;
                fixed.d[3] = o.width;
                profile->setDimensions(in->getName(), OptProfileSelector::kMIN, fixed);
                profile->setDimensions(in->getName(), OptProfileSelector::kOPT, fixed);
                profile->setDimensions(in->getName(), OptProfileSelector::kMAX, fixed);
            }
        }
        config->addOptimizationProfile(profile);
        log.info("Dynamic input detected; pinned to " +
                 std::to_string(o.width) + "x" + std::to_string(o.height) + ".");
    }

#if NV_TENSORRT_MAJOR >= 10
    Monitor mon;
    mon.owner = this;
    config->setProgressMonitor(&mon);
#endif

    markBuildStart();
    setPhase("Building engine", -1.0f, "this can take several minutes");
    auto* plan = builder->buildSerializedNetwork(*network, *config);

    if (!plan) {
        // A cancelled build fails the same way an unsuccessful one does, so
        // the two are told apart by the flag rather than by the result.
        // Reporting a deliberate stop as a failure would send someone
        // looking for a problem that does not exist.
        if (cancelReq_.load())
            log.info("Build cancelled. Nothing was written.");
        else
            log.error("Engine build failed. If the log mentions memory, lower "
                      "the workspace on the Model tab.");
        delete config; delete parser; delete network; delete builder;
        return false;
    }

    setPhase("Writing engine", 0.98f, enginePath);
    std::ofstream f(enginePath, std::ios::binary);
    if (!f) {
        log.error("Could not write " + enginePath + ". Check folder permissions.");
        delete plan; delete config; delete parser; delete network; delete builder;
        return false;
    }
    f.write(static_cast<const char*>(plan->data()),
            static_cast<std::streamsize>(plan->size()));
    f.close();

    delete plan; delete config; delete parser; delete network; delete builder;
    return true;
}

} // namespace lc
