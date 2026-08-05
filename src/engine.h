// engine.h -- TensorRT engine wrapper.
#pragma once

#include <cuda_runtime.h>
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

    void*  inputBuffer() const { return inputBuf_; }
    int    inputW()      const { return inW_; }
    int    inputH()      const { return inH_; }
    bool   inputIsHalf() const { return inHalf_; }
    bool   hasNmsPlugin()const { return pluginNms_; }
    bool   ready()       const { return ready_; }

    const std::string& describe() const { return desc_; }

private:
    void  destroy();
    bool  decodeRaw(float confThresh, int maxDets, std::vector<RawDet>& out);
    bool  decodePlugin(float confThresh, int maxDets, std::vector<RawDet>& out);

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

    // pinned host staging for the small output copy
    void*  hostStage_ = nullptr;
    size_t hostStageBytes_ = 0;

    // raw-head geometry
    int    numAnchors_ = 0;
    int    numClasses_ = 0;

    std::string desc_;
};

} // namespace lc
