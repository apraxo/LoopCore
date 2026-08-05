// preprocess.cuh -- one-pass GPU preprocessing.
#pragma once

#include <cuda_runtime.h>

namespace lc {

// Letterbox-resize a BGRA8 texture into a normalised NCHW tensor.
//
//   tex     : texture object over the CUDA-mapped D3D11 ROI texture
//   dst     : TensorRT input buffer (device)
//   dstW/H  : engine input width/height
//   srcW/H  : ROI width/height
//   fp16    : true if the engine input is __half
//
// One kernel does: bilinear resize, aspect-preserving pad, BGRA->RGB
// channel swap, /255 normalise, and HWC->CHW transpose. Nothing else
// touches the frame between capture and TensorRT.
cudaError_t launch_preprocess(cudaTextureObject_t tex,
                              void*  dst,
                              int    dstW, int dstH,
                              int    srcW, int srcH,
                              bool   fp16,
                              float  padValue,
                              cudaStream_t stream);

// Scale/pad actually used, so boxes can be mapped back to ROI pixels.
struct LetterboxMeta {
    float scale;
    int   padX, padY;
};

inline LetterboxMeta letterbox_meta(int dstW, int dstH, int srcW, int srcH) {
    float r  = fminf((float)dstW / srcW, (float)dstH / srcH);
    int   nw = (int)(srcW * r + 0.5f);
    int   nh = (int)(srcH * r + 0.5f);
    return { r, (dstW - nw) / 2, (dstH - nh) / 2 };
}

} // namespace lc
