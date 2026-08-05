// preprocess.cu
#include "preprocess.cuh"

#include <cuda_fp16.h>

namespace lc {

template <typename T>
__global__ void k_letterbox(cudaTextureObject_t tex,
                            T* __restrict__ dst,
                            int dstW, int dstH,
                            int srcW, int srcH,
                            float invScale,
                            int padX, int padY,
                            float padValue)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= dstW || y >= dstH) return;

    const int plane = dstW * dstH;
    const int idx   = y * dstW + x;

    float r, g, b;

    const float sx = (x - padX + 0.5f) * invScale;
    const float sy = (y - padY + 0.5f) * invScale;

    if (sx < 0.0f || sy < 0.0f || sx >= (float)srcW || sy >= (float)srcH) {
        r = g = b = padValue;
    } else {
        // BGRA8 read as normalised float4: .x=B .y=G .z=R .w=A
        float4 p = tex2D<float4>(tex, sx, sy);
        r = p.z;
        g = p.y;
        b = p.x;
    }

    dst[0 * plane + idx] = (T)r;
    dst[1 * plane + idx] = (T)g;
    dst[2 * plane + idx] = (T)b;
}

cudaError_t launch_preprocess(cudaTextureObject_t tex,
                              void*  dst,
                              int    dstW, int dstH,
                              int    srcW, int srcH,
                              bool   fp16,
                              float  padValue,
                              cudaStream_t stream)
{
    LetterboxMeta m = letterbox_meta(dstW, dstH, srcW, srcH);
    const float invScale = 1.0f / m.scale;

    dim3 block(32, 8);
    dim3 grid((dstW + block.x - 1) / block.x,
              (dstH + block.y - 1) / block.y);

    if (fp16) {
        k_letterbox<__half><<<grid, block, 0, stream>>>(
            tex, (__half*)dst, dstW, dstH, srcW, srcH,
            invScale, m.padX, m.padY, padValue);
    } else {
        k_letterbox<float><<<grid, block, 0, stream>>>(
            tex, (float*)dst, dstW, dstH, srcW, srcH,
            invScale, m.padX, m.padY, padValue);
    }
    return cudaGetLastError();
}

} // namespace lc
