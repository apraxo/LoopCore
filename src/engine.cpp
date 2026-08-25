// engine.cpp
#include "engine.h"

#include <NvInfer.h>
#include <NvInferPlugin.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <functional>
#include <cmath>
#include <sstream>

#include "store.h"

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

const char* dtypeName(DataType t) {
    switch (t) {
    case DataType::kFLOAT: return "fp32";
    case DataType::kHALF:  return "fp16";
    case DataType::kINT32: return "int32";
    case DataType::kINT8:  return "int8";
#if NV_TENSORRT_MAJOR >= 9
    case DataType::kINT64: return "int64";
    case DataType::kBOOL:  return "bool";
#endif
    default: return "?";
    }
}

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
    if (nmsStage_) { cudaFreeHost(nmsStage_); nmsStage_ = nullptr; nmsStageBytes_ = 0; }
    if (evStart_) { cudaEventDestroy((cudaEvent_t)evStart_); evStart_ = nullptr; }
    if (evStop_)  { cudaEventDestroy((cudaEvent_t)evStop_);  evStop_  = nullptr; }
    if (evPre_)   { cudaEventDestroy((cudaEvent_t)evPre_);   evPre_   = nullptr; }
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

    // Anything half-built is torn down on the way out.
    //
    // destroy() at the top clears whatever the previous load left, but the
    // failure paths below return having already stored a runtime, an engine,
    // a context and some device buffers in the members -- and nothing freed
    // them. A model that fails to load leaked all of it, and repeated
    // attempts at a bad file leaked it repeatedly.
    //
    // A guard object is used rather than adding a call before each return,
    // because there are a dozen of them and the next one added would be
    // forgotten.
    //
    // It holds a callable rather than a pointer to Engine: a local class may
    // reach a private member of the enclosing class, but that is a corner of
    // the language worth not depending on when a lambda says the same thing
    // plainly.
    struct FailGuard {
        std::function<void()> cleanup;
        bool ok = false;
        ~FailGuard() { if (!ok && cleanup) cleanup(); }
    } guard{[this] { destroy(); }};

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
    engineBytes_ = blob.size();

    // Checked before deserializing, not after failing to.
    //
    // TensorRT cannot be asked what a plan was built for, so it reports the
    // mismatch as an error code after the attempt. The sidecar written at
    // build time can say the same thing in terms of the two machines
    // involved, which is what someone moving a folder between PCs needs.
    {
        const store::EngineMeta meta = store::loadEngineMeta(path);
        if (meta.valid) {
            cudaDeviceProp prop{};
            if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
                // A portable plan is meant to run elsewhere, so the check
                // becomes "is this card new enough" rather than "is it the
                // same card". Ampere is compute 8.0.
                const bool mismatch = (meta.smMajor != prop.major ||
                                       meta.smMinor != prop.minor);
                if (meta.portable && mismatch) {
                    if (prop.major < 8) {
                        char m[320];
                        snprintf(m, sizeof(m),
                                 "This is a portable engine, which needs "
                                 "Ampere or newer. This machine is compute "
                                 "%d.%d. Portability does not extend back "
                                 "this far; convert the source here instead.",
                                 prop.major, prop.minor);
                        log.error(m);
                        return false;
                    }
                    log.info("Portable engine, built elsewhere. It will run, "
                             "but a native build would be faster.");
                } else if (mismatch) {
                    char m[400];
                    snprintf(m, sizeof(m),
                             "This engine was built on a %s (compute %d.%d) and "
                             "this machine has a %s (compute %d.%d). A plan is "
                             "tied to the card it was built on, and cannot "
                             "be retargeted -- TensorRT chooses its kernels "
                             "by timing them on the device in front of it. "
                             "Convert the source here instead; the .onnx or "
                             ".pt is all that needs copying between machines.",
                             meta.gpuName.c_str(), meta.smMajor, meta.smMinor,
                             prop.name, prop.major, prop.minor);
                    log.error(m);
                    return false;
                }
            }
            if (meta.trtMajor != NV_TENSORRT_MAJOR) {
                char m[300];
                snprintf(m, sizeof(m),
                         "This engine was built with TensorRT %d.%d and this "
                         "build uses %d.%d. Plans do not carry across major "
                         "versions; convert the source again.",
                         meta.trtMajor, meta.trtMinor,
                         NV_TENSORRT_MAJOR, NV_TENSORRT_MINOR);
                log.error(m);
                return false;
            }
        }
    }
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

    log.info("Engine tensors for " + path + ":");

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

    // ---- describe every tensor, for the log and the UI -------------------
    report_.clear();
    int nIn = 0, nOut = 0;
    for (auto& bnd : bindings_) {
        (bnd.isInput ? nIn : nOut)++;
        std::ostringstream ss;
        ss << (bnd.isInput ? "in  " : "out ") << bnd.name << "  [";
        for (size_t k = 0; k < bnd.dims.size(); ++k)
            ss << (k ? "," : "") << bnd.dims[k];
        ss << "]  " << dtypeName((DataType)bnd.dtype);
        report_.push_back(ss.str());
        log.info("  " + ss.str());
    }

    if (nIn != 1) {
        log.error("Expected exactly one input tensor, found " + std::to_string(nIn));
        return false;
    }

    for (auto& bnd : bindings_) {
        if (!bnd.isInput) continue;
        if (bnd.dims.size() != 4) { log.error("Input must be NCHW (4 dims)."); return false; }
        inputBuf_ = bnd.dev;
        inH_ = bnd.dims[2];
        inW_ = bnd.dims[3];
        inHalf_ = (DataType)bnd.dtype == DataType::kHALF;
    }

    chooseLayout(log);
    if (layout_ == LayoutAuto) {
        log.error("Could not work out how this engine's output is arranged. "
                  "Pick a layout by hand on the Model tab, and send the tensor "
                  "list above.");
        return false;
    }
    pluginNms_ = (layout_ == LayoutPluginNms || layout_ == LayoutEnd2End);

    // pinned staging sized to the largest output
    size_t maxOut = 0;
    for (auto& bnd : bindings_) if (!bnd.isInput) maxOut = std::max(maxOut, bnd.bytes);
    hostStageBytes_ = maxOut;
    if (cudaHostAlloc(&hostStage_, hostStageBytes_, cudaHostAllocDefault) != cudaSuccess) {
        log.error("cudaHostAlloc failed for the output staging buffer.");
        return false;
    }

    std::ostringstream ss;
    // This is the dtype of the input binding, not the precision the network
    // computes in. TensorRT keeps the input fp32 for an fp16 engine as a
    // matter of course, so calling the whole engine fp32 was misleading.
    ss << inW_ << "x" << inH_ << "  io " << (inHalf_ ? "fp16" : "fp32")
       << "  " << LayoutName(layout_);
    desc_ = ss.str();
    guard.ok = true;
    log.info("Engine ready: " + desc_);
    log.info("Note: the io precision above says nothing about the layer "
             "precision inside. TensorRT offers no way to read that back "
             "from a built engine; the gpu only timing is the honest guide.");

    // Device-side timing, so GPU cost can be told apart from waiting.
    {
        cudaEvent_t a = nullptr, b = nullptr;
        if (cudaEventCreate(&a) == cudaSuccess &&
            cudaEventCreate(&b) == cudaSuccess) {
            evStart_ = a;
            evStop_  = b;
        }
    }

    seenMask_ = 0;
    ready_ = true;
    return true;
}

Engine::SmokeResult Engine::smokeTest(cudaStream_t stream, Log& log) {
    SmokeResult r;
    if (!ready_) { r.note = "no engine loaded"; return r; }

    const Binding* in = nullptr;
    const Binding* out = nullptr;
    for (auto& b : bindings_) {
        if (b.isInput) in = &b;
        else if (!out || b.bytes > out->bytes) out = &b;
    }
    if (!in || !out) { r.note = "could not identify the bindings"; return r; }

    // Mid-grey rather than zeros. An all-zero input can legitimately produce
    // an all-zero output, which would make the check unable to tell a broken
    // engine from a working one.
    std::vector<float> fill(in->bytes / sizeof(float), 0.5f);
    if (inHalf_) {
        // Half input: fill with the fp16 bit pattern for 0.5 instead.
        std::vector<uint16_t> h(in->bytes / sizeof(uint16_t), 0x3800);
        cudaMemcpyAsync(in->dev, h.data(), in->bytes, cudaMemcpyHostToDevice, stream);
    } else {
        cudaMemcpyAsync(in->dev, fill.data(), in->bytes, cudaMemcpyHostToDevice, stream);
    }

    const int64_t t0 = now_ns();
    // Same accessor the real inference path uses: the context is held as a
    // void* so this header need not pull in the TensorRT one.
    auto* ctx = (IExecutionContext*)context_;
    if (!ctx) { r.note = "no execution context"; return r; }
#if NV_TENSORRT_MAJOR >= 10
    if (!ctx->enqueueV3(stream)) { r.note = "enqueue failed"; return r; }
#else
    // Built the same way as the real path: a fixed-size array would be wrong
    // for an engine with more bindings than the guess allowed for.
    std::vector<void*> ptrs;
    ptrs.reserve(bindings_.size());
    for (auto& b : bindings_) ptrs.push_back(b.dev);
    if (!ctx->enqueueV2(ptrs.data(), stream, nullptr)) {
        r.note = "enqueue failed";
        return r;
    }
#endif

    // Read as floats because every layout this supports emits float output.
    // Sized from the byte count so a partial trailing element cannot be
    // walked past.
    std::vector<float> back(out->bytes / sizeof(float));
    if (back.empty()) { r.note = "output binding is empty"; return r; }
    cudaMemcpyAsync(back.data(), out->dev, out->bytes, cudaMemcpyDeviceToHost, stream);
    const cudaError_t ce = cudaStreamSynchronize(stream);
    r.ms = (now_ns() - t0) / 1e6;

    if (ce != cudaSuccess) {
        r.note = std::string("CUDA error: ") + cudaGetErrorString(ce);
        return r;
    }
    r.ran = true;

    r.finite = true;
    r.minVal = back.empty() ? 0.0f : back[0];
    r.maxVal = r.minVal;
    for (float v : back) {
        if (std::isnan(v) || std::isinf(v)) { r.finite = false; break; }
        r.minVal = std::min(r.minVal, v);
        r.maxVal = std::max(r.maxVal, v);
        if (v != 0.0f) r.nonZero = true;
    }

    // An end-to-end engine is allowed to output nothing.
    //
    // The test feeds a flat grey image. A raw-head model always answers with
    // anchor values whatever it sees, so all zeros there means something is
    // wrong. A model with NMS built in answers with a list of detections --
    // and the correct list for a blank grey image is empty, which is an
    // all-zero tensor.
    //
    // So the same output means "broken" for one layout and "working exactly
    // as it should" for the other. Reporting a good end-to-end build as
    // failed sent the user looking at a source model that was fine.
    // Both NMS-applied layouts, not just the single-tensor one.
    const bool endToEnd = (layout_ == LayoutEnd2End ||
                           layout_ == LayoutPluginNms);

    if (!r.finite)              r.note = "output contains NaN or infinity";
    else if (!r.nonZero && endToEnd)
        r.note = "no detections in a blank test image, which is correct for "
                 "an engine with NMS built in";
    else if (!r.nonZero)        r.note = "output is entirely zero";
    else                        r.note = "output looks plausible";

    char b[220];
    snprintf(b, sizeof(b),
             "Smoke test: %s. One inference in %.2f ms, output range %.4f to "
             "%.4f.", r.note.c_str(), r.ms, r.minVal, r.maxVal);
    // An empty result from an NMS engine is a pass, not a failure.
    r.ok = r.ran && r.finite && (r.nonZero || endToEnd);
    if (r.ok) log.info(b);
    else      log.error(b);
    return r;
}

Engine::BenchResult Engine::benchmark(cudaStream_t stream, int iterations,
                                      Log& log) {
    BenchResult r;
    if (!ready_ || !context_) {
        log.warn("No engine loaded, so there is nothing to benchmark.");
        return r;
    }

    const int n = std::clamp(iterations, 5, 500);
    auto* ctx = (IExecutionContext*)context_;

    // Mid-grey, the same input the smoke test uses. The content does not
    // change the work a convolution does, and a fixed input keeps runs
    // comparable.
    const Binding* in = nullptr;
    for (auto& b : bindings_) if (b.isInput) in = &b;
    if (!in) return r;

    std::vector<float> fill(in->bytes / sizeof(float), 0.5f);
    cudaMemcpyAsync(in->dev, fill.data(), in->bytes, cudaMemcpyHostToDevice,
                    stream);
    cudaStreamSynchronize(stream);

    // A few untimed runs first. The first inference after a load pays for
    // kernel loading and the clock ramp, and including that would report a
    // number nobody ever experiences.
    for (int i = 0; i < 5; ++i) {
        ctx->enqueueV3(stream);
        cudaStreamSynchronize(stream);
    }

    std::vector<double> ms;
    ms.reserve(n);
    for (int i = 0; i < n; ++i) {
        const int64_t t0 = now_ns();
        if (!ctx->enqueueV3(stream)) break;
        if (cudaStreamSynchronize(stream) != cudaSuccess) break;
        ms.push_back((now_ns() - t0) / 1e6);
    }
    if (ms.empty()) return r;

    std::sort(ms.begin(), ms.end());
    r.ran = true;
    r.iterations = (int)ms.size();
    r.medianMs = ms[ms.size() / 2];
    r.bestMs = ms.front();
    r.worstMs = ms.back();

    char b[256];
    snprintf(b, sizeof(b),
             "Benchmark: %d runs, median %.2f ms (best %.2f, worst %.2f). "
             "This is the model alone; compare it with the gpu figure while "
             "playing to see what sharing the card costs.",
             r.iterations, r.medianMs, r.bestMs, r.worstMs);
    log.info(b);
    return r;
}

Engine::Complexity Engine::complexity(double inferMs) const {
    Complexity c;
    if (!ready_) return c;

    c.engineBytes = engineBytes_;
    c.anchors     = numAnchors_;
    c.classes     = numClasses_;
    c.megapixels  = (double)inW_ * inH_ / 1e6;

    // A YOLO-family detector produces roughly one anchor per 4x4, 8x8, 16x16
    // and 32x32 cell, so the anchor count and the input size together imply
    // the head's shape. The weight footprint then stands in for depth. This
    // is an order-of-magnitude figure, not a measurement: the honest number
    // is the inference time next to it, which is why both are shown.
    const double mb = c.engineBytes / (1024.0 * 1024.0);
    c.gflopsEst = c.megapixels * 8.0 * std::sqrt(std::max(1.0, mb));

    // Tiers by serialised size, which tracks parameter count closely enough
    // for a label once precision is accounted for.
    const double effective = inHalf_ ? mb * 2.0 : mb;   // fp16 stores half
    if      (effective < 14.0)  c.tier = "nano";
    else if (effective < 40.0)  c.tier = "small";
    else if (effective < 100.0) c.tier = "medium";
    else if (effective < 200.0) c.tier = "large";
    else                        c.tier = "extra large";

    if (inferMs > 0.0) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "%.1f ms per inference, so %.0f frames a second is the "
                 "ceiling from the model alone.",
                 inferMs, 1000.0 / inferMs);
        c.note = buf;
    } else {
        c.note = "Run it to see what it costs.";
    }
    return c;
}

// ------------------------------------------------------------ layout

const char* LayoutName(int layout) {
    switch (layout) {
    case LayoutPluginNms: return "EfficientNMS plugin";
    case LayoutEnd2End:   return "end-to-end (N,6)";
    case LayoutRawCHW:    return "raw head (4+nc, anchors)";
    case LayoutRawHWC:    return "raw head (anchors, 4+nc)";
    default:              return "auto";
    }
}

void Engine::setLayoutOverride(int layout, Log& log) {
    override_ = layout;
    if (!engine_) return;
    chooseLayout(log);
    std::ostringstream ss;
    ss << inW_ << "x" << inH_ << "  " << (inHalf_ ? "fp16" : "fp32")
       << "  " << LayoutName(layout_);
    desc_ = ss.str();
}

void Engine::chooseLayout(Log& log) {
    if (override_ != LayoutAuto) {
        layout_ = override_;
        log.info(std::string("Output layout forced to ") + LayoutName(layout_) + ".");
        return;
    }

    int nOut = 0;
    const Binding* single = nullptr;
    for (auto& b : bindings_) {
        if (b.isInput) continue;
        ++nOut;
        single = &b;
    }

    if (nOut == 4) {
        layout_ = LayoutPluginNms;
        log.info("Output layout: EfficientNMS plugin (four tensors).");
        return;
    }

    if (nOut == 1 && single && single->dims.size() == 3) {
        const int d1 = single->dims[1], d2 = single->dims[2];

        // Ultralytics exported with nms=True produces one end-to-end tensor
        // of (1, N, 6). A raw head is (1, 4+nc, anchors) or its transpose,
        // where the anchor count is in the thousands. That difference in
        // magnitude is what separates them.
        if (d2 == 6 && d1 <= 1000) {
            layout_ = LayoutEnd2End;
            numAnchors_ = d1;
            numClasses_ = 0;
            log.info("Output layout: end-to-end, up to " + std::to_string(d1) +
                     " boxes of [x1 y1 x2 y2 score class]. NMS already applied.");
            return;
        }
        if (d1 < d2) {
            layout_ = LayoutRawCHW;
            numClasses_ = d1 - 4;
            numAnchors_ = d2;
        } else {
            layout_ = LayoutRawHWC;
            numClasses_ = d2 - 4;
            numAnchors_ = d1;
        }
        if (numClasses_ <= 0) { layout_ = LayoutAuto; return; }
        log.info("Output layout: " + std::string(LayoutName(layout_)) + ", " +
                 std::to_string(numClasses_) + " classes over " +
                 std::to_string(numAnchors_) + " anchors.");
        log.warn("This engine needs NMS, which runs on the CPU here. "
                 "Re-exporting with nms=True moves it into the engine -- but "
                 "measure before keeping it. The ONNX NMS operator often "
                 "forces neighbouring layers back to FP32, and on a card "
                 "whose speed comes from fp16 tensor cores that can cost more "
                 "than the copy it saves. Compare the gpu figure both ways.");
        return;
    }

    layout_ = LayoutAuto;
}

EngineDiag Engine::diag() const {
    std::lock_guard<std::mutex> lk(diagMutex_);
    return diag_;
}

// ------------------------------------------------------------- inference

void Engine::markFrameStart(cudaStream_t stream) {
    if (!evPre_) {
        cudaEvent_t e = nullptr;
        if (cudaEventCreate(&e) != cudaSuccess) return;
        evPre_ = e;
    }
    cudaEventRecord((cudaEvent_t)evPre_, stream);
}

bool Engine::infer(cudaStream_t stream, float confThresh, int maxDets,
                   std::vector<RawDet>& out, Log& log)
{
    out.clear();
    if (!ready_) return false;
    auto* ctx = (IExecutionContext*)context_;

    if (evStart_) cudaEventRecord((cudaEvent_t)evStart_, stream);

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

    if (evStop_) cudaEventRecord((cudaEvent_t)evStop_, stream);

    bool ok = false;
    switch (layout_) {
    case LayoutPluginNms: ok = decodePluginNms(stream, confThresh, maxDets, out, log); break;
    case LayoutEnd2End:   ok = decodeEnd2End(stream, confThresh, maxDets, out, log); break;
    case LayoutRawCHW:    ok = decodeRaw(stream, true,  confThresh, maxDets, out, log); break;
    case LayoutRawHWC:    ok = decodeRaw(stream, false, confThresh, maxDets, out, log); break;
    default: return false;
    }

    for (const RawDet& d : out)
        if (d.cls >= 0 && d.cls < 32) seenMask_ |= (1u << d.cls);

    float gpuMs = 0.0f;
    if (evStart_ && evStop_) {
        // The decode already synchronised the stream, so the events are
        // ready and this does not add a wait of its own.
        // Everything ahead of the model, if the frame marked its start.
        if (evPre_ && evStart_) {
            float pre = 0.0f;
            if (cudaEventElapsedTime(&pre, (cudaEvent_t)evPre_,
                                     (cudaEvent_t)evStart_) == cudaSuccess)
                diag_.preMs = pre;
        }
        cudaEventElapsedTime(&gpuMs, (cudaEvent_t)evStart_, (cudaEvent_t)evStop_);
    }

    {
        std::lock_guard<std::mutex> lk(diagMutex_);
        diag_.gpuMs = gpuMs;
        diag_.kept = (int)out.size();
        diag_.minBoxPx = 0.0f;
        diag_.maxBoxPx = 0.0f;
        for (const RawDet& d : out) {
            const float w = d.x2 - d.x1, h = d.y2 - d.y1;
            const float m = std::max(w, h);
            if (diag_.maxBoxPx == 0.0f || m > diag_.maxBoxPx) diag_.maxBoxPx = m;
            if (diag_.minBoxPx == 0.0f || m < diag_.minBoxPx) diag_.minBoxPx = m;
        }
    }
    return ok;
}

bool Engine::decodePluginNms(cudaStream_t stream, float confThresh, int maxDets,
                             std::vector<RawDet>& out, Log& log)
{
    const Binding *bn = nullptr, *bb = nullptr, *bs = nullptr, *bc = nullptr;
    for (auto& b : bindings_) {
        if (b.isInput) continue;
        size_t elems = 1; for (int d : b.dims) elems *= d;
        if (elems == 1)                                  bn = &b;
        else if (b.dims.size() == 3 && b.dims[2] == 4)   bb = &b;
        else if ((DataType)b.dtype == DataType::kFLOAT)  bs = &b;
        else                                             bc = &b;
    }
    if (!bn || !bb || !bs || !bc) {
        log.error("Could not identify the four EfficientNMS outputs.");
        return false;
    }

    // One batch of copies and one synchronise.
    //
    // Reading the count first, synchronising, then reading the boxes and
    // synchronising again cost two full pipeline drains per frame when the
    // sizes are known in advance. The plugin's outputs are fixed capacity,
    // so everything can be fetched at once and the count used afterwards to
    // decide how much of it to look at.
    //
    // The destinations are pinned as well. cudaMemcpyAsync into ordinary
    // pageable memory is not actually asynchronous -- the driver stages it
    // through its own buffer and blocks -- so the "async" copies here were
    // synchronous ones with extra steps.
    const size_t capBoxes   = bb->bytes;
    const size_t capScores  = bs->bytes;
    const size_t capClasses = bc->bytes;
    const size_t need = sizeof(int32_t) + capBoxes + capScores + capClasses;
    if (nmsStageBytes_ < need) {
        if (nmsStage_) cudaFreeHost(nmsStage_);
        nmsStage_ = nullptr;
        if (cudaHostAlloc(&nmsStage_, need, cudaHostAllocDefault) != cudaSuccess) {
            log.error("Could not allocate pinned staging memory for decoding.");
            nmsStageBytes_ = 0;
            return false;
        }
        nmsStageBytes_ = need;
    }

    uint8_t* base = (uint8_t*)nmsStage_;
    int32_t* pn        = (int32_t*)base;
    float*   pBoxes    = (float*)(base + sizeof(int32_t));
    float*   pScores   = (float*)((uint8_t*)pBoxes + capBoxes);
    int32_t* pClasses  = (int32_t*)((uint8_t*)pScores + capScores);

    // All four copies and the wait are checked together.
    //
    // The count comes back in the first of them, and it is used to index the
    // other three. If any copy fails silently, the count is read from a stale
    // buffer and used to walk arrays that were never filled -- so a failure
    // here is not just wrong detections, it is a read past whatever the last
    // frame left behind.
    //
    // Checked as one condition because the recovery is the same for all of
    // them: report nothing this frame.
    const bool copiesOk =
        cudaMemcpyAsync(pn,       bn->dev, sizeof(int32_t), cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
        cudaMemcpyAsync(pBoxes,   bb->dev, capBoxes,        cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
        cudaMemcpyAsync(pScores,  bs->dev, capScores,       cudaMemcpyDeviceToHost, stream) == cudaSuccess &&
        cudaMemcpyAsync(pClasses, bc->dev, capClasses,      cudaMemcpyDeviceToHost, stream) == cudaSuccess;

    if (!copiesOk || cudaStreamSynchronize(stream) != cudaSuccess) {
        log.error(std::string("Reading the model output failed: ") +
                  cudaGetErrorString(cudaGetLastError()));
        return false;
    }

    int32_t n = *pn;
    {
        std::lock_guard<std::mutex> lk(diagMutex_);
        diag_.rawCandidates = n;
        diag_.maxScore = 0.0f;
    }
    n = std::min<int32_t>(n, (int32_t)maxDets);
    n = std::min<int32_t>(n, (int32_t)(capScores / sizeof(float)));
    if (n <= 0) return true;

    const float*   boxes   = pBoxes;
    const float*   scores  = pScores;
    const int32_t* classes = pClasses;

    float best = 0.0f;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        best = std::max(best, scores[i]);
        if (scores[i] < confThresh) continue;
        out.push_back({boxes[i*4+0], boxes[i*4+1], boxes[i*4+2], boxes[i*4+3],
                       scores[i], (int)classes[i]});
    }
    {
        std::lock_guard<std::mutex> lk(diagMutex_);
        diag_.maxScore = best;
    }
    return true;
}

bool Engine::decodeEnd2End(cudaStream_t stream, float confThresh, int maxDets,
                           std::vector<RawDet>& out, Log& log)
{
    const Binding* ob = nullptr;
    for (auto& b : bindings_) if (!b.isInput) ob = &b;
    if (!ob) return false;

    // The copy and the wait are checked.
    //
    // Left unchecked, a failure here leaves hostStage_ holding the previous
    // frame's output -- and the decode below reads it as though it were
    // current. That does not look like an error, it looks like the model
    // detecting something that is no longer on screen, which is the hardest
    // kind of fault to trace back to its cause.
    //
    // Returning false means "no detections this frame", which is the honest
    // answer when the data could not be fetched.
    if (cudaMemcpyAsync(hostStage_, ob->dev, ob->bytes,
                        cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess) {
        log.error(std::string("Reading the model output failed: ") +
                  cudaGetErrorString(cudaGetLastError()));
        return false;
    }

    const float* p = (const float*)hostStage_;
    const int rows = ob->dims[1];

    int raw = 0;
    float best = 0.0f;
    out.reserve(16);

    for (int i = 0; i < rows && (int)out.size() < maxDets; ++i) {
        const float* r = p + (size_t)i * 6;
        const float score = r[4];
        // Rows are sorted by score and padded with zeros, so the first
        // worthless row means the rest are too.
        if (score <= 0.0f) break;
        ++raw;
        best = std::max(best, score);
        if (score < confThresh) continue;
        out.push_back({r[0], r[1], r[2], r[3], score, (int)r[5]});
    }

    std::lock_guard<std::mutex> lk(diagMutex_);
    diag_.rawCandidates = raw;
    diag_.maxScore = best;
    return true;
}

bool Engine::decodeRaw(cudaStream_t stream, bool chw, float confThresh,
                       int maxDets, std::vector<RawDet>& out, Log& log)
{
    const Binding* ob = nullptr;
    for (auto& b : bindings_) if (!b.isInput && b.dims.size() == 3) ob = &b;
    if (!ob) return false;

    // On our stream, and then wait for it. A plain cudaMemcpy runs on the
    // default stream, and a non-blocking stream does not synchronise with
    // that one -- so this was reading the output buffer before the network
    // had written it. Fast, and wrong.
    // The copy and the wait are checked.
    //
    // Left unchecked, a failure here leaves hostStage_ holding the previous
    // frame's output -- and the decode below reads it as though it were
    // current. That does not look like an error, it looks like the model
    // detecting something that is no longer on screen, which is the hardest
    // kind of fault to trace back to its cause.
    //
    // Returning false means "no detections this frame", which is the honest
    // answer when the data could not be fetched.
    if (cudaMemcpyAsync(hostStage_, ob->dev, ob->bytes,
                        cudaMemcpyDeviceToHost, stream) != cudaSuccess ||
        cudaStreamSynchronize(stream) != cudaSuccess) {
        log.error(std::string("Reading the model output failed: ") +
                  cudaGetErrorString(cudaGetLastError()));
        return false;
    }

    const float* p = (const float*)hostStage_;
    const int A = numAnchors_, C = numClasses_;

    cand_.clear();
    if (cand_.capacity() < 256) cand_.reserve(256);
    float best = 0.0f;
    int raw = 0;

    // The score scan is hoisted out of the general case for a single class,
    // which is what a one-class detector -- the common case here -- always
    // is.
    //
    // In the channel-first layout every class occupies a contiguous run of
    // anchors, so one class means the scores are one sequential array. The
    // general loop reads them through a branch on `chw` and a multiply per
    // anchor, and the compiler cannot hoist either because both depend on
    // values it cannot see are constant. Eight thousand four hundred
    // iterations of that, two hundred and fifty times a second, is worth
    // avoiding.
    const float* scoreRow = (chw && C == 1) ? (p + (size_t)4 * A) : nullptr;

    for (int a = 0; a < A; ++a) {
        int   bestC = -1;
        float bestS = 0.0f;
        if (scoreRow) {
            bestS = scoreRow[a];
            bestC = 0;
        } else {
            for (int c = 0; c < C; ++c) {
                const float s = chw ? p[(size_t)(4 + c) * A + a]
                                    : p[(size_t)a * (4 + C) + 4 + c];
                if (s > bestS) { bestS = s; bestC = c; }
            }
        }
        best = std::max(best, bestS);
        if (bestS > 0.01f) ++raw;
        if (bestC < 0 || bestS < confThresh) continue;

        const float cx = chw ? p[(size_t)0 * A + a] : p[(size_t)a * (4 + C) + 0];
        const float cy = chw ? p[(size_t)1 * A + a] : p[(size_t)a * (4 + C) + 1];
        const float w  = chw ? p[(size_t)2 * A + a] : p[(size_t)a * (4 + C) + 2];
        const float h  = chw ? p[(size_t)3 * A + a] : p[(size_t)a * (4 + C) + 3];
        cand_.push_back({cx - w*0.5f, cy - h*0.5f,
                         cx + w*0.5f, cy + h*0.5f, bestS, bestC});
    }

    // Only the strongest few need ordering.
    //
    // Suppression walks the list in score order and stops once enough
    // survivors are found, so sorting the whole candidate list is work whose
    // result is mostly discarded. Partial sorting orders exactly the part
    // that gets looked at.
    const size_t want = std::min(cand_.size(), (size_t)std::max(1, maxDets) * 4);
    std::partial_sort(cand_.begin(), cand_.begin() + want, cand_.end(),
                      [](const RawDet& a, const RawDet& b) {
                          return a.score > b.score;
                      });

    // uint8_t rather than vector<bool>: the latter is a bit array whose
    // element access is a shift and a mask, which is slower than a byte for
    // a buffer this small and cannot be reused without reallocating.
    dead_.assign(want, 0);
    for (size_t i = 0; i < want && (int)out.size() < maxDets; ++i) {
        if (dead_[i]) continue;
        out.push_back(cand_[i]);
        for (size_t j = i + 1; j < want; ++j)
            if (!dead_[j] && cand_[j].cls == cand_[i].cls &&
                iou(cand_[i], cand_[j]) > 0.45f)
                dead_[j] = true;
    }

    std::lock_guard<std::mutex> lk(diagMutex_);
    diag_.rawCandidates = raw;
    diag_.maxScore = best;
    return true;
}

} // namespace lc
