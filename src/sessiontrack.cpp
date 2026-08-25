// sessiontrack.cpp
#include "sessiontrack.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "store.h"

namespace lc {

namespace {

std::string stamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char b[64];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

void writeStat(std::ofstream& f, const char* label, const Stat& s,
               const char* unit) {
    if (!s.any()) {
        f << "  " << label << "  (never happened)\n";
        return;
    }
    char b[200];
    snprintf(b, sizeof(b),
             "  %-22s %8.3f  sd %7.3f   %7.3f to %8.3f %-3s over %llu samples\n",
             label, s.mean, s.sd(), s.mn, s.mx, unit,
             (unsigned long long)s.n);
    f << b;
}

void writeBucket(std::ofstream& f, const char* title,
                 const SessionTracker::Bucket& b) {
    f << "\n" << title << "\n";
    writeStat(f, "loop", b.loop, "ms");
    writeStat(f, "gpu", b.gpu, "ms");
    // Between the two: the model's own device time, and the device time
    // spent on the frame before the model started. Their sum is what the
    // inference row below is waiting for.
    writeStat(f, "pre gpu", b.pre, "ms");
    writeStat(f, "inference", b.infer, "ms");
    // The rest of the pipeline, in the order a frame passes through it.
    writeStat(f, "  arrive", b.arrive, "ms");
    writeStat(f, "  roi copy", b.copy, "ms");
    writeStat(f, "  cuda map", b.map, "ms");
    writeStat(f, "  decode+nms", b.post, "ms");
    writeStat(f, "  publish", b.publish, "ms");
    writeStat(f, "  control", b.control, "ms");
    writeStat(f, "frame gap", b.cadence, "ms");
    writeStat(f, "distance to aim", b.err, "px");
    writeStat(f, "lead applied", b.lead, "px");
}

} // namespace

bool SessionTracker::start(const Config& cfg, Log& log) {
    if (running_.load()) return false;

    // The folder is made here rather than assumed. Tracking starts from the
    // frame loop, which can run before anything else has had cause to
    // create it, and an ofstream onto a missing directory fails silently --
    // which is why no session file appeared.
    CreateDirectoryA(store::binDir().c_str(), nullptr);
    CreateDirectoryA(store::logsDir().c_str(), nullptr);

    std::lock_guard<std::mutex> lk(m_);
    path_ = store::logsDir() + "\\session_" + stamp() + ".txt";
    startNs_ = now_ns();
    lastNs_ = startNs_;
    samples_ = 0;
    throttledSamples_ = 0;
    engagedSec_ = 0.0;
    notes_.clear();
    foreground_.clear();
    fgFpsBusy_ = fgFpsIdle_ = Stat{};
    idle_ = held_ = Bucket{};
    focusOurs_ = focusOther_ = Bucket{};
    noTarget_ = oneTarget_ = fewTargets_ = manyTargets_ = Bucket{};
    trust_ = scale_ = score_ = gpuClock_ = Stat{};

    {
        // Opened immediately, so a path that cannot be written is reported
        // now rather than discovered when the session ends and the whole
        // recording is lost.
        std::ofstream probe(path_);
        if (!probe) {
            log.error("Could not open " + path_ + " for the session log.");
            return false;
        }
        probe << "loopcore session, in progress\n";
    }

    running_ = true;
    log.info("Session tracking to " + path_);
    return true;
}

std::string SessionTracker::path() const {
    std::lock_guard<std::mutex> lk(m_);
    return path_;
}

double SessionTracker::engagedSeconds() const {
    std::lock_guard<std::mutex> lk(m_);
    return engagedSec_;
}

std::string SessionTracker::dominantForeground() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string best;
    double bestSec = 0.0;
    for (const auto& f : foreground_) {
        // loopcore's own window is excluded: the interesting answer is what
        // the user was actually working in.
        if (_stricmp(f.first.c_str(), "loopcore.exe") == 0) continue;
        if (f.second > bestSec) { bestSec = f.second; best = f.first; }
    }
    if (best.empty()) return "desktop";
    const size_t dot = best.find_last_of('.');
    return (dot == std::string::npos) ? best : best.substr(0, dot);
}

void SessionTracker::note(const std::string& event) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lk(m_);
    if (notes_.size() >= 500) return;
    char b[32];
    snprintf(b, sizeof(b), "%8.1f s  ", (now_ns() - startNs_) / 1e9);
    notes_.push_back(std::string(b) + event);
}

void SessionTracker::sample(const SessionSample& s) {
    if (!running_.load()) return;
    std::lock_guard<std::mutex> lk(m_);

    const int64_t now = now_ns();
    const double dt = std::min(1.0, (now - lastNs_) / 1e9);
    lastNs_ = now;
    ++samples_;

    if (s.engaged) engagedSec_ += dt;
    if (s.throttled) ++throttledSamples_;

    // Time in each foreground application, for naming the capture folder and
    // for knowing what the run was actually measuring.
    if (!s.foreground.empty()) {
        bool found = false;
        for (auto& f : foreground_)
            if (f.first == s.foreground) { f.second += dt; found = true; break; }
        if (!found && foreground_.size() < 64)
            foreground_.push_back({s.foreground, dt});
    }

    auto feed = [&](Bucket& b) {
        b.loop.add(s.loopMs);
        b.gpu.add(s.gpuMs);
        b.cadence.add(s.cadenceMs);
        b.infer.add(s.inferMs);
        b.pre.add(s.preMs);
        b.arrive.add(s.arriveMs);
        b.copy.add(s.copyMs);
        b.map.add(s.mapMs);
        b.post.add(s.postMs);
        b.publish.add(s.publishMs);
        b.control.add(s.controlMs);
        if (s.hasTarget) {
            b.err.add(s.errPx);
            b.lead.add(s.leadPx);
        }
    };

    // Every sample lands in one bucket from each dimension, so a slowdown
    // can be attributed to a condition rather than merely observed.
    feed(s.engaged ? held_ : idle_);
    feed(s.ourWindowFocused ? focusOurs_ : focusOther_);
    if (s.targets == 0)      feed(noTarget_);
    else if (s.targets == 1) feed(oneTarget_);
    else if (s.targets <= 3) feed(fewTargets_);
    else                     feed(manyTargets_);

    if (s.hasTarget) {
        trust_.add(s.leadTrust);
        score_.add(s.topScore);
    }
    scale_.add(s.responseScale);
    if (s.gpuMaxMHz > 0.0f) gpuClock_.add(s.gpuMHz);
    // Split by engagement, because the cost of the model is what the
    // difference between these two is measuring.
    if (s.foregroundFps > 1.0f)
        (s.engaged ? fgFpsBusy_ : fgFpsIdle_).add(s.foregroundFps);
}

void SessionTracker::flush(const Config& cfg) {
    // Written out as the run proceeds, not only when it ends.
    //
    // A session that ends in a crash, or with the process killed, is exactly
    // the session worth having a record of -- and holding everything in
    // memory until a clean exit is the one arrangement that guarantees
    // losing it.
    if (!running_.load()) return;
    const int64_t now = now_ns();
    {
        std::lock_guard<std::mutex> lk(m_);
        if (now - lastFlush_ < 10'000'000'000LL) return;
        lastFlush_ = now;
    }
    writeOut(cfg, nullptr);
}

void SessionTracker::stop(const Config& cfg, Log& log) {
    if (!running_.exchange(false)) return;
    writeOut(cfg, &log);
}

void SessionTracker::writeOut(const Config& cfg, Log* log) {
    std::lock_guard<std::mutex> lk(m_);
    std::ofstream f(path_);
    if (!f) {
        if (log) log->error("Could not write " + path_);
        return;
    }

    const double dur = (now_ns() - startNs_) / 1e9;
    char b[400];

    f << "loopcore session\n";
    snprintf(b, sizeof(b), "duration %.1f s, %llu samples, %.1f s engaged "
             "(%.0f%% of the run)\n", dur, (unsigned long long)samples_,
             engagedSec_, dur > 0.0 ? 100.0 * engagedSec_ / dur : 0.0);
    f << b;

    if (samples_ > 0) {
        snprintf(b, sizeof(b), "gpu throttled during %.0f%% of samples\n",
                 100.0 * throttledSamples_ / samples_);
        f << b;
    }

    f << "\nforeground applications, by time\n";
    {
        auto sorted = foreground_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& c) { return a.second > c.second; });
        for (const auto& fg : sorted) {
            snprintf(b, sizeof(b), "  %-28s %7.1f s\n",
                     fg.first.c_str(), fg.second);
            f << b;
        }
    }

    // The comparisons are the point of the file: same measurements, split by
    // the condition in force, so a difference between two blocks is the
    // answer rather than the start of an investigation.
    f << "\n=== split by whether the activation key was held ===";
    writeBucket(f, "not engaged", idle_);
    writeBucket(f, "engaged", held_);

    f << "\n=== split by which window had focus ===";
    writeBucket(f, "loopcore in front", focusOurs_);
    writeBucket(f, "another application in front", focusOther_);

    f << "\n=== split by how many targets were on screen ===";
    writeBucket(f, "none", noTarget_);
    writeBucket(f, "one", oneTarget_);
    writeBucket(f, "two or three", fewTargets_);
    writeBucket(f, "four or more", manyTargets_);

    f << "\n=== estimator health ===\n";
    writeStat(f, "lead trust", trust_, "");
    writeStat(f, "response scale", scale_, "px");
    writeStat(f, "best confidence", score_, "");
    writeStat(f, "gpu clock", gpuClock_, "MHz");
    writeStat(f, "foreground fps, engaged", fgFpsBusy_, "fps");
    writeStat(f, "foreground fps, idle", fgFpsIdle_, "fps");
    if (fgFpsBusy_.any() && fgFpsIdle_.any() && fgFpsIdle_.mean > 1.0) {
        snprintf(b, sizeof(b),
                 "  frame rate cost of running the model: %.0f%%\n",
                 100.0 * (fgFpsIdle_.mean - fgFpsBusy_.mean) / fgFpsIdle_.mean);
        f << b;
    }

    if (!notes_.empty()) {
        f << "\n=== events ===\n";
        for (const auto& n : notes_) f << "  " << n << "\n";
    }

    // Every setting, so a report can be reproduced rather than guessed at.
    //
    // Rendered once and kept, rather than round-tripped through a temporary
    // file on every write. This function runs every ten seconds so the
    // session survives a crash, and writing a config, reading it back and
    // deleting it three times a minute is disk work for a block of text that
    // barely changes.
    f << "\n=== settings in force ===\n";
    {
        const uint64_t fp = store::configFingerprint(cfg, "");
        if (fp != settingsFp_ || settingsText_.empty()) {
            settingsFp_ = fp;
            settingsText_.clear();
            Log quiet;
            const std::string tmp = path_ + ".cfg";
            store::saveConfig(tmp, cfg, "", quiet);
            std::ifstream in(tmp);
            std::string line;
            while (std::getline(in, line)) settingsText_ += "  " + line + "\n";
            in.close();
            DeleteFileA(tmp.c_str());
        }
        f << settingsText_;
    }

    f.flush();
    if (log) log->info("Session written to " + path_);
}

} // namespace lc
