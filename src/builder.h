// builder.h -- turns .pt or .onnx into a .engine, off the UI thread.
//
//   .engine  loaded directly
//   .onnx    built natively with the TensorRT ONNX parser
//   .pt      exported to .onnx by ultralytics, then built natively
//
// Building takes minutes, so all of it runs on a worker thread and reports
// phase and percentage back for the UI to draw.
#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "common.h"

namespace lc {

enum class BuildState { Idle, Running, Done, Failed };

struct BuildOptions {
    int  width   = 640;
    int  height  = 640;
    bool fp16    = true;
    bool embedNms = true;
    // Build a plan that runs on cards other than this one.
    //
    // TensorRT normally compiles for the exact compute capability it is
    // building on, which is why a plan copied between machines is rejected.
    // Hardware compatibility relaxes that, at a real cost in speed, and only
    // across Ampere and newer -- the feature does not extend backwards to
    // Turing or Pascal.
    bool portable = false;
    int  workspaceMB = 2048;
    int  optLevel    = 5;   // TensorRT builder optimization level, 0-5
    // Where the finished .engine goes. Empty means beside the input.
    std::string outputDir;
};

class ModelBuilder {
public:
    ~ModelBuilder();

    // Returns false if a build is already running.
    bool start(const std::string& inputPath, const BuildOptions& opts, Log& log);

    BuildState  state() const { return state_.load(); }
    bool        busy()  const { return state_.load() == BuildState::Running; }

    // Snapshot of progress, safe to call from the UI thread.
    struct Progress {
        std::string phase;             // the stable, top-level phase
        float       fraction = 0.0f;   // < 0 when indeterminate
        std::string detail;            // the innermost phase and its counter
        double      etaSeconds = -1.0;
        // True while the figure is a guess from experience rather than
        // measured from the build's own progress.
        bool        etaIsGuess = false;
    };
    Progress progress() const;

    // Asks the running build to stop.
    //
    // TensorRT has no abort call; the only way out is to return false from
    // the progress monitor, which it checks between optimisation steps. So
    // this sets a flag and the build ends at the next step rather than
    // immediately -- usually within a second, but a long single step will
    // run to its end first.
    void requestCancel() { cancelReq_ = true; }
    bool cancelRequested() const { return cancelReq_.load(); }

    // Valid once state() == Done.
    std::string outputPath() const;
    // What the engine was built from, for recording alongside it.
    std::string sourcePath() const;

    // Set by the worker when it finishes, cleared by the UI once consumed.
    bool takeCompletion(std::string& enginePathOut);

    void setPhase(const std::string& phase, float fraction,
                  const std::string& detail = {});

    // Marks the start of the long phase, so remaining time can be estimated.
    void markBuildStart();

    // True when the given path needs conversion rather than direct loading.
    static bool needsConversion(const std::string& path);
    static std::string extensionOf(const std::string& path);

private:
    void run(std::string input, BuildOptions opts, Log* log);
    bool exportPtToOnnx(const std::string& pt, const BuildOptions& o,
                        std::string& onnxOut, Log& log);
    bool buildEngine(const std::string& onnx, const BuildOptions& o,
                     const std::string& enginePath, Log& log);

    std::thread             worker_;
    std::atomic<BuildState> state_{BuildState::Idle};
    std::atomic<bool>       completionPending_{false};

    mutable std::mutex m_;
    Progress    prog_;
    std::string outPath_;
    std::string srcPath_;
    int64_t     buildStartNs_ = 0;

    // A short history of (time, fraction) so the estimate can use the recent
    // rate of progress rather than the average since the start.
    struct Mark { int64_t ns; float frac; };
    std::deque<Mark> marks_;
    double  etaSmooth_ = -1.0;
    // TensorRT starts new root phases part-way through, which sends its
    // reported fraction back to zero. Keeping the high-water mark makes the
    // number the user sees monotonic, which is what a progress bar promises.
    float   progressMax_ = 0.0f;
    std::atomic<bool> cancelReq_{false};
    double  roughTotalSec_ = 0.0;
    int64_t srcBytes_ = 0;
    int64_t etaStampNs_  = 0;
};

} // namespace lc
