loopcore
Event-driven screen capture → TensorRT detection loop, with a control panel
and a debug tab. This is the C++ core from phase 3 of the plan: no polling,
no host round-trip, and per-stage timing on everything.
What the loop actually does
```
compositor presents a frame
        │
        ▼  FrameArrived  ← an event, not a poll. Fires the instant the
   (WinRT thread pool)      frame exists, so age is as low as Windows allows.
        │
        ├─ CopySubresourceRegion   crop the ROI on the GPU, no host copy
        ├─ cudaGraphicsMapResources  the same texture, now a CUDA array
        ├─ one CUDA kernel         resize + letterbox + BGRA→RGB + /255 + NCHW
        ├─ TensorRT enqueueV3      fp16 or int8
        ├─ EfficientNMS_TRT        NMS on the GPU
        └─ ~200 bytes D→H          the box list, and nothing else
```
The frame crosses PCIe exactly once, in the direction that costs almost
nothing. The overlay and the panel run on a separate thread at 60 Hz and
never touch this path.
Newest frame wins. If the pipeline is still busy when the next frame
arrives, that frame is dropped and counted, not queued. A queue would trade
latency for throughput; here latency is the product. The Debug tab shows the
drop rate — a non-zero number means the loop is slower than the sim's present
rate, which is the signal to shrink the ROI or quantise harder.
