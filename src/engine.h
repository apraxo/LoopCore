// engine.h -- TensorRT engine wrapper.
#pragma once

#include <cuda_runtime.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace lc {

// Boxes as the engine emits them: letterboxed input pixel coordinates.
struct RawDet {
    float x1, y1, x2, y2;
    float score;
    int   cls;
};

// How the network's output is arranged. Getting this wrong does not fail
// loudly -- it decodes noise into plausible-looking boxes, so it is worth
// naming explicitly and logging.
enum OutputLayout {
    LayoutAuto = 0,
    LayoutPluginNms,   // 4 tensors: num_dets / boxes / scores / classes
    LayoutEnd2End,     // 1 tensor  (1, N, 6): x1 y1 x2 y2 score class
    LayoutRawCHW,      // 1 tensor  (1, 4+nc, anchors)   -- needs NMS
    LayoutRawHWC,      // 1 tensor  (1, anchors, 4+nc)   -- needs NMS
    LayoutCount
};

const char* LayoutName(int layout);

// What the last inference actually saw. Surfaced in the UI because "no
// detections" and "decoding nonsense" look identical from the outside.
struct EngineDiag {
    int   rawCandidates = 0;   // rows above a floor of 0.01
    int   kept          = 0;   // survived the confidence threshold
    float maxScore      = 0.0f;
    float minBoxPx      = 0.0f;
    float maxBoxPx      = 0.0f;
    // What the GPU actually spent, from device events. The wall clock around
    // enqueue-and-sync includes however long the thread waited to be
    // scheduled, so a gap between the two means contention rather than a
    // slow model.
    float gpuMs         = 0.0f;
    // Device time for everything queued ahead of the model on the same
    // stream: the capture map, the letterbox kernel, the unmap.
    //
    // Measured because the inference row read three milliseconds above the
    // gpu row and there was no way to tell whether that was the kernel, the
    // copy, or the thread simply waiting. Guessing at it produced two wrong
    // conclusions already.
    float preMs         = 0.0f;
};

class Engine {
public:
    Engine() = default;
    ~Engine();

    // Loads a serialised .engine. Returns false and fills `err` on failure.
    bool load(const std::string& path, Log& log);

    // Runs inference on whatever is already in inputBuffer(). Boxes come
    // back in engine-input pixel space; caller maps them to the screen.
    bool infer(cudaStream_t stream, float confThresh, int maxDets,
               std::vector<RawDet>& out, Log& log);

    // Forces a decode layout, or LayoutAuto to detect from the shapes.
    void   setLayoutOverride(int layout, Log& log);
    int    layout() const { return layout_; }
    int    layoutOverride() const { return override_; }

    EngineDiag diag() const;

    // Bitmask of class ids seen since load. The engine file carries no class
    // names or count, so the only honest source is what has come out of it.
    uint32_t seenClasses() const { return seenMask_.load(); }
    void     clearSeenClasses() { seenMask_ = 0; }

    // Every IO tensor, formatted for the log and the UI.
    const std::vector<std::string>& tensorReport() const { return report_; }

    void*  inputBuffer() const { return inputBuf_; }
    int    inputW()      const { return inW_; }
    int    inputH()      const { return inH_; }
    bool   inputIsHalf() const { return inHalf_; }
    bool   hasNmsPlugin()const { return pluginNms_; }
    bool   ready()       const { return ready_; }

    const std::string& describe() const { return desc_; }
    // True when suppression has to be done here rather than in the engine.
    bool needsCpuNms() const {
        return layout_ == LayoutRawCHW || layout_ == LayoutRawHWC;
    }

    // A rough sense of how much work the network is. There is no way to ask
    // TensorRT for a parameter count once an engine is serialised, so this
    // is inferred from what is observable: the input size, the number of
    // anchors the head produces, the engine's own weight footprint, and the
    // measured inference time.
    struct Complexity {
        size_t engineBytes = 0;    // serialised size on disk
        int    anchors     = 0;    // detection positions per frame
        int    classes     = 0;
        double megapixels  = 0.0;  // input pixels per inference
        double gflopsEst   = 0.0;  // very rough, see the note in the source
        std::string tier;          // "nano", "small", ...
        std::string note;
        // The verdict, decided here rather than by each caller.
        //
        // Two places were recomputing it as "ran and finite and non-zero",
        // which is wrong for an engine with NMS built in: an empty result on
        // a blank test image is the correct answer, not a failure. Deciding
        // it once, where the output layout is known, keeps the two from
        // disagreeing.
        bool ok = false;
    };
    Complexity complexity(double inferMs) const;

    // Runs one inference on synthetic input and reports what came back.
    //
    // A build that reports success has only proved that TensorRT was willing
    // to serialise something. Whether the result actually produces plausible
    // numbers is a different question, and the only way to answer it is to
    // run the thing.
    struct SmokeResult {
        bool  ran = false;
        bool  finite = false;      // no NaN or infinity in the output
        bool  nonZero = false;     // not simply all zeros
        float minVal = 0.0f, maxVal = 0.0f;
        double ms = 0.0;
        std::string note;
        // The verdict, decided here rather than by each caller.
        //
        // Two places were recomputing it as "ran and finite and non-zero",
        // which is wrong for an engine with NMS built in: an empty result on
        // a blank test image is the correct answer, not a failure. Deciding
        // it once, where the output layout is known, keeps the two from
        // disagreeing.
        bool ok = false;
    };
    SmokeResult smokeTest(cudaStream_t stream, Log& log);

    // Runs the model repeatedly on a fixed input and reports the median.
    //
    // The point is to separate two things the live figures cannot: how long
    // the model takes to execute, and how long it takes to finish while
    // sharing the card with a game. The device timer measures elapsed time
    // on the stream, so work preempted by another application reads as a
    // slower model -- which sent three rounds of tuning at engine quality
    // when engine quality was never the problem.
    //
    // Run it with nothing else on screen for the engine's own number, then
    // again with the game running to see what contention costs.
    struct BenchResult {
        bool   ran = false;
        double medianMs = 0.0;
        double bestMs = 0.0;
        double worstMs = 0.0;
        int    iterations = 0;
    };
    BenchResult benchmark(cudaStream_t stream, int iterations, Log& log);

    // Marks the start of a frame's device work, before the letterbox kernel.
    // Paired with the events already recorded around the model, this splits
    // "the GPU was busy with our earlier work" from "the model was running".
    void markFrameStart(cudaStream_t stream);

private:
    void  destroy();
    void  chooseLayout(Log& log);
    bool  decodePluginNms(cudaStream_t, float conf, int maxDets, std::vector<RawDet>&, Log&);
    bool  decodeEnd2End(cudaStream_t, float conf, int maxDets, std::vector<RawDet>&, Log&);
    bool  decodeRaw(cudaStream_t, bool chw, float conf, int maxDets,
                    std::vector<RawDet>&, Log&);

    void*  runtime_  = nullptr;   // nvinfer1::IRuntime*
    void*  engine_   = nullptr;   // nvinfer1::ICudaEngine*
    void*  context_  = nullptr;   // nvinfer1::IExecutionContext*

    struct Binding {
        std::string name;
        bool        isInput = false;
        size_t      bytes   = 0;
        void*       dev     = nullptr;
        std::vector<int> dims;
        int         dtype   = 0;  // nvinfer1::DataType as int
    };
    std::vector<Binding> bindings_;

    void*  inputBuf_ = nullptr;
    int    inW_ = 0, inH_ = 0;
    bool   inHalf_ = false;
    bool   pluginNms_ = false;
    bool   ready_ = false;
    int    layout_   = LayoutAuto;
    size_t engineBytes_ = 0;
    int    override_ = LayoutAuto;

    std::vector<std::string> report_;
    mutable std::mutex diagMutex_;
    EngineDiag diag_;
    std::atomic<uint32_t> seenMask_{0};

    // pinned host staging for the small output copy
    void*  evStart_ = nullptr;   // cudaEvent_t
    void*  evStop_  = nullptr;
    void*  evPre_   = nullptr;   // marks the top of the frame's stream work
    void*  hostStage_ = nullptr;
    // Pinned staging for the plugin-NMS outputs. Pageable destinations make
    // cudaMemcpyAsync synchronous, which defeats the point of batching.
    void*  nmsStage_ = nullptr;
    // Decode scratch, kept between frames.
    //
    // These were local to decodeRaw, so every frame allocated a candidate
    // list and a suppression flag array and freed them again. At two hundred
    // and fifty frames a second that is five hundred heap operations a
    // second on the thread whose latency is the entire point of the program.
    std::vector<RawDet> cand_;
    std::vector<uint8_t> dead_;
    size_t nmsStageBytes_ = 0;
    size_t hostStageBytes_ = 0;

    // raw-head geometry
    int    numAnchors_ = 0;
    int    numClasses_ = 0;

    std::string desc_;
};

} // namespace lc
