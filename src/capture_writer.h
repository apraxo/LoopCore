// capture_writer.h -- dataset capture.
//
// Three trigger modes, taken from the tool this replaces:
//
//   Timed         save every N ms, regardless of what is on screen
//   On detection  save only when the model sees something, rate limited
//   Burst on loss a confidence ladder: capture nothing while tracking is
//                 solid, harder as it degrades, hardest right after the
//                 target is lost. Those are the frames a model gets wrong,
//                 and the ones worth having in a training set.
//
// Encoding and disk writes happen on a worker thread. The capture callback
// hands over a copy and returns; nothing here is allowed near the hot loop.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace lc {

enum CaptureMode { CapTimed = 0, CapOnDetection, CapBurstOnLoss };
enum ImageFormat { ImgPng = 0, ImgJpeg };

struct CaptureStats {
    uint64_t saved     = 0;
    uint64_t sessions  = 0;   // runs since launch, so the log need not say
    uint64_t labels    = 0;
    uint64_t dropped   = 0;   // queue was full; the loop never waits
    uint64_t failed    = 0;
    int      queued    = 0;
    std::string session;
    std::string lastFile;
    std::string tier;         // which ladder rung fired, for burst mode
};

class CaptureWriter {
public:
    ~CaptureWriter();

    // `label` names the folder for this run of the program. Empty falls
    // back to the date alone.
    static void SetSessionLabel(const std::string& label);

    bool start(const Config& cfg, Log& log);
    void stop(Log& log);
    bool running() const { return running_.load(); }

    // Called from the capture thread with the ROI already on the host, plus
    // whatever the model found. Returns immediately.
    void offer(const uint8_t* bgra, int width, int height, int stride,
               const std::vector<Det>& dets, float topScore,
               int roiLeft, int roiTop, const Config& cfg);

    CaptureStats stats() const;
    // Across every run since launch, so a hotkey bound to the activation
    // key does not lose the count each time it toggles.
    CaptureStats totals() const;

    // Where this session is writing.
    std::string sessionDir() const;

private:
    struct Job {
        std::vector<uint8_t> rgba;   // tightly packed BGRA
        int w = 0, h = 0;
        std::vector<Det> dets;
        int roiLeft = 0, roiTop = 0;
        // Where the written image sits inside the region, so labels can be
        // moved with it.
        int cropX = 0, cropY = 0;
        bool label = false;
        std::string stem;
    };

    void writerLoop(Log* log);
    bool writeImage(const Job& j, const std::string& path);
    bool writeLabel(const Job& j, const std::string& path);

    std::thread             worker_;
    std::atomic<bool>       running_{false};

    mutable std::mutex      m_;
    std::deque<Job>         queue_;
    std::condition_variable cv_;

    mutable std::mutex      statMutex_;
    CaptureStats            stats_;

    std::string  dir_;
    ImageFormat  format_ = ImgPng;
    int          quality_ = 90;

    // Trigger state
    int64_t lastSaveNs_   = 0;
    int64_t lastRealNs_   = 0;
    int     burstLeft_    = 0;   // frames still owed in a post-loss burst
    int64_t lastBurstNs_  = 0;   // when the last burst began
    uint64_t burstsCancelled_ = 0;   // cut short by the model recovering
    bool    burstDone_    = false;
    int     tierName_     = 0;
    bool    hadDetection_ = false;
    uint64_t index_       = 0;
    uint64_t totalSaved_  = 0;
    uint64_t totalLabels_ = 0;
    uint64_t sessions_    = 0;
};

} // namespace lc
