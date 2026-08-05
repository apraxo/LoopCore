// engine.cpp
#include "engine.h"

#include <NvInfer.h>
#include <NvInferPlugin.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace nvinfer1;

namespace lc {

namespace {

class TrtLogger : public ILogger {
public:
    Log* sink = nullptr;
    void log(Severity s, const char* msg) noexcept override {
        if (!sink) return;
        if (s == Severity::kERROR || s == Severity::kINTERNAL_ERROR)
            sink->error(std::string("TensorRT: ") + msg);
        else if (s == Severity::kWARNING)
            sink->warn(std::string("TensorRT: ") + msg);
    }
};
TrtLogger gLogger;

size_t dtypeSize(DataType t) {
    switch (t) {
        case DataType::kFLOAT: return 4;
        case DataType::kHALF:  return 2;
        case DataType::kINT32: return 4;
        case DataType::kINT8:  return 1;
#if NV_TENSORRT_MAJOR >= 9
        case DataType::kINT64: return 8;
        case DataType::kBOOL:  return 1;
#endif
        default: return 4;
    }
}

float iou(const RawDet& a, const RawDet& b) {
    float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return ua > 0.0f ? inter / ua : 0.0f;
}

} // namespace

Engine::~Engine() { destroy(); }

void Engine::destroy() {
    for (auto& b : bindings_)
        if (b.dev) cudaFree(b.dev);
    bindings_.clear();
    if (hostStage_) { cudaFreeHost(hostStage_); hostStage_ = nullptr; }
    auto ctx = (IExecutionContext*)context_;
    auto eng = (ICudaEngine*)engine_;
    auto rt  = (IRuntime*)runtime_;
#if NV_TENSORRT_MAJOR >= 8
    delete ctx; delete eng; delete rt;
#else
    if (ctx) ctx->destroy();
    if (eng) eng->destroy();
    if (rt)  rt->destroy();
#endif
    context_ = engine_ = runtime_ = nullptr;
    ready_ = false;
}

bool Engine::load(const std::string& path, Log& log) {
    destroy();
    gLogger.sink = &log;

    // EfficientNMS_TRT lives in nvinfer_plugin; without this the
    // deserialize below fails with "cannot find plugin creator".
    static bool pluginsReady = false;
    if (!pluginsReady) {
        pluginsReady = initLibNvInferPlugins(&gLogger, "");
        if (!pluginsReady) log.warn("initLibNvInferPlugins returned false.");
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        log.error("Engine file not found: " + path +
                  " -- export one with: yolo export model=best.pt format=engine "
                  "half=True nms=True device=0");
        return false;
    }
    std::vector<char> blob((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    if (blob.empty()) { log.error("Engine file is empty: " + path); return false; }

    auto* rt = createInferRuntime(gLogger);
    if (!rt) { log.error("createInferRuntime failed."); return false; }
    runtime_ = rt;

    auto* eng = rt->deserializeCudaEngine(blob.data(), blob.size());
    if (!eng) {
        log.error("Could not deserialize the engine. Engines are tied to one "
                  "GPU model and one TensorRT version -- rebuild it on this machine.");
        return false;
    }
    engine_ = eng;

    auto* ctx = eng->createExecutionContext();
    if (!ctx) { log.error("createExecutionContext failed."); return false; }
    context_ = ctx;

    // ---- enumerate bindings -------------------------------------------
#if NV_TENSORRT_MAJOR >= 10
    int nio = eng->getNbIOTensors();
    for (int i = 0; i < nio; ++i) {
        const char* nm = eng->getIOTensorName(i);
        Binding b;
        b.name    = nm;
        b.isInput = eng->getTensorIOMode(nm) == TensorIOMode::kINPUT;
        b.dtype   = (int)eng->getTensorDataType(nm);
        Dims d    = ctx->getTensorShape(nm);
        size_t elems = 1;
        for (int k = 0; k < d.nbDims; ++k) { b.dims.push_back((int)d.d[k]); elems *= d.d[k]; }
        b.bytes = elems * dtypeSize(eng->getTensorDataType(nm));
        if (cudaMalloc(&b.dev, b.bytes) != cudaSuccess) {
            log.error("cudaMalloc failed for tensor " + b.name);
            return false;
        }
        ctx->setTensorAddress(nm, b.dev);
        bindings_.push_back(std::move(b));
    }
#else
    int nb = eng->getNbBindings();
    for (int i = 0; i < nb; ++i) {
        Binding b;
        b.name    = eng->getBindingName(i);
        b.isInput = eng->bindingIsInput(i);
        b.dtype   = (int)eng->getBindingDataType(i);
        Dims d    = ctx->getBindingDimensions(i);
        size_t elems = 1;
        for (int k = 0; k < d.nbDims; ++k) { b.dims.push_back((int)d.d[k]); elems *= d.d[k]; }
        b.bytes = elems * dtypeSize(eng->getBindingDataType(i));
        if (cudaMalloc(&b.dev, b.bytes) != cudaSuccess) {
            log.error("cudaMalloc failed for binding " + b.name);
            return false;
        }
        bindings_.push_back(std::move(b));
    }
#endif

    // ---- interpret ------------------------------------------------------
    int nIn = 0, nOut = 0;
    for (auto& b : bindings_) (b.isInput ? nIn : nOut)++;
    if (nIn != 1) { log.error("Expected exactly 1 input tensor, found " + std::to_string(nIn)); return false; }

    for (auto& b : bindings_) {
        if (!b.isInput) continue;
        if (b.dims.size() != 4) { log.error("Input must be NCHW (4 dims)."); return false; }
        inputBuf_ = b.dev;
        inH_ = b.dims[2];
        inW_ = b.dims[3];
        inHalf_ = (DataType)b.dtype == DataType::kHALF;
    }

    pluginNms_ = (nOut == 4);
    if (!pluginNms_) {
        // raw head: (1, 4+nc, anchors)
        for (auto& b : bindings_) {
            if (b.isInput) continue;
            if (b.dims.size() == 3) {
                numClasses_ = b.dims[1] - 4;
                numAnchors_ = b.dims[2];
            }
        }
        if (numClasses_ <= 0 || numAnchors_ <= 0) {
            log.error("Unrecognised output shape. Re-export with nms=True for the "
                      "EfficientNMS plugin, which is faster and simpler.");
            return false;
        }
        log.warn("Engine has a raw detection head. Decode runs on the CPU. "
                 "Re-export with nms=True to move NMS onto the GPU.");
    }

    // pinned staging sized to the largest output
    size_t maxOut = 0;
    for (auto& b : bindings_) if (!b.isInput) maxOut = std::max(maxOut, b.bytes);
    hostStageBytes_ = maxOut;
    if (cudaHostAlloc(&hostStage_, hostStageBytes_, cudaHostAllocDefault) != cudaSuccess) {
        log.error("cudaHostAlloc failed for the output staging buffer.");
        return false;
    }

    std::ostringstream ss;
    ss << inW_ << "x" << inH_ << "  " << (inHalf_ ? "fp16" : "fp32")
       << "  outputs=" << nOut << (pluginNms_ ? "  EfficientNMS" : "  raw head");
    desc_ = ss.str();
    log.info("Engine loaded: " + desc_);

    ready_ = true;
    return true;
}

bool Engine::infer(cudaStream_t stream, float confThresh, int maxDets,
                   std::vector<RawDet>& out, Log& log)
{
    out.clear();
    if (!ready_) return false;
    auto* ctx = (IExecutionContext*)context_;

#if NV_TENSORRT_MAJOR >= 10
    if (!ctx->enqueueV3(stream)) { log.error("enqueueV3 failed."); return false; }
#else
    std::vector<void*> ptrs;
    ptrs.reserve(bindings_.size());
    for (auto& b : bindings_) ptrs.push_back(b.dev);
    if (!ctx->enqueueV2(ptrs.data(), stream, nullptr)) {
        log.error("enqueueV2 failed."); return false;
    }
#endif

    if (pluginNms_) {
        // num_dets(int32) / boxes(float) / scores(float) / classes(int32)
        const Binding *bn = nullptr, *bb = nullptr, *bs = nullptr, *bc = nullptr;
        for (auto& b : bindings_) {
            if (b.isInput) continue;
            size_t elems = 1; for (int d : b.dims) elems *= d;
            if (elems == 1)                        bn = &b;
            else if (b.dims.size() == 3)           bb = &b;   // 1,N,4
            else if ((DataType)b.dtype == DataType::kFLOAT) bs = &b;
            else                                   bc = &b;
        }
        if (!bn || !bb || !bs || !bc) {
            log.error("Could not identify the four EfficientNMS outputs.");
            return false;
        }

        int32_t n = 0;
        cudaMemcpyAsync(&n, bn->dev, sizeof(int32_t), cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);
        n = std::min<int32_t>(n, (int32_t)maxDets);
        if (n <= 0) return true;

        std::vector<float>   boxes(n * 4);
        std::vector<float>   scores(n);
        std::vector<int32_t> classes(n);
        cudaMemcpyAsync(boxes.data(),   bb->dev, n * 4 * sizeof(float),   cudaMemcpyDeviceToHost, stream);
        cudaMemcpyAsync(scores.data(),  bs->dev, n * sizeof(float),       cudaMemcpyDeviceToHost, stream);
        cudaMemcpyAsync(classes.data(), bc->dev, n * sizeof(int32_t),     cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);

        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            if (scores[i] < confThresh) continue;
            out.push_back({boxes[i*4+0], boxes[i*4+1], boxes[i*4+2], boxes[i*4+3],
                           scores[i], (int)classes[i]});
        }
        return true;
    }

    return decodeRaw(confThresh, maxDets, out) ? true : (cudaStreamSynchronize(stream), true);
}

bool Engine::decodeRaw(float confThresh, int maxDets, std::vector<RawDet>& out) {
    const Binding* ob = nullptr;
    for (auto& b : bindings_) if (!b.isInput && b.dims.size() == 3) ob = &b;
    if (!ob) return false;

    cudaMemcpy(hostStage_, ob->dev, ob->bytes, cudaMemcpyDeviceToHost);
    const float* p = (const float*)hostStage_;
    const int C = 4 + numClasses_, A = numAnchors_;

    std::vector<RawDet> cand;
    cand.reserve(256);
    for (int a = 0; a < A; ++a) {
        int   best = -1;
        float bs   = confThresh;
        for (int c = 0; c < numClasses_; ++c) {
            float s = p[(4 + c) * A + a];
            if (s > bs) { bs = s; best = c; }
        }
        if (best < 0) continue;
        float cx = p[0 * A + a], cy = p[1 * A + a];
        float w  = p[2 * A + a], h  = p[3 * A + a];
        cand.push_back({cx - w*0.5f, cy - h*0.5f, cx + w*0.5f, cy + h*0.5f, bs, best});
    }

    std::sort(cand.begin(), cand.end(),
              [](const RawDet& a, const RawDet& b) { return a.score > b.score; });

    std::vector<bool> dead(cand.size(), false);
    for (size_t i = 0; i < cand.size() && (int)out.size() < maxDets; ++i) {
        if (dead[i]) continue;
        out.push_back(cand[i]);
        for (size_t j = i + 1; j < cand.size(); ++j)
            if (!dead[j] && cand[j].cls == cand[i].cls && iou(cand[i], cand[j]) > 0.45f)
                dead[j] = true;
    }
    return true;
}

bool Engine::decodePlugin(float, int, std::vector<RawDet>&) { return false; }

} // namespace lc
