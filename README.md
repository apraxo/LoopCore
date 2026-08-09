# loopcore

Event-driven screen capture → TensorRT detection loop, with a control panel
and a debug tab. This is the C++ core from phase 3 of the plan: no polling,
no host round-trip, and per-stage timing on everything.

## What the loop actually does

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

**Newest frame wins.** If the pipeline is still busy when the next frame
arrives, that frame is dropped and counted, not queued. A queue would trade
latency for throughput; here latency is the product. The Debug tab shows the
drop rate — a non-zero number means the loop is slower than the sim's present
rate, which is the signal to shrink the ROI or quantise harder.

## Setup on a fresh machine

```
setup.bat      installs the prerequisites   (asks for admin, may reboot)
build.bat      compiles                     (no admin)
```

`setup.bat` installs, via winget: Git, Visual Studio 2022 Build Tools with
the C++ toolset and Windows SDK, the CUDA Toolkit, and optionally Python
plus ultralytics for exporting engines (recommended). Budget 20–40 minutes and one reboot.

**TensorRT is the one manual step.** NVIDIA gates it behind a free developer
account and a click-through EULA, so no script can fetch it unattended. The
setup script opens the download page, then takes the path to the zip you
downloaded — extracts it, finds the right subfolder, and sets `TENSORRT_DIR`.
You can also paste in a folder you already extracted.

It also checks for an NVIDIA driver first and stops early if there isn't one,
since CUDA and TensorRT both need it.

If a prerequisite is missing when you run `build.bat`, it names the missing
piece and offers to launch `setup.bat` rather than failing with a compiler
error.

Already have the toolchain? Just run `build.bat`. It finds MSVC through
vswhere, CUDA through `CUDA_PATH`, TensorRT through `TENSORRT_DIR`, clones
Dear ImGui on first run, compiles for sm_75 with forward PTX, and copies the
TensorRT and CUDA runtime DLLs next to the exe.

`build.bat debug` for symbols, `build.bat clean` to start over.

## Engine

```bat
yolo export model=best.pt format=engine half=True nms=True imgsz=[384,640] device=0
```

`nms=True` embeds `EfficientNMS_TRT` so NMS runs on the GPU and only the
final boxes come back. Without it the code falls back to a CPU decode, which
works but costs a few ms — the Debug log says so when it happens.

Set `imgsz` to the aspect ratio of the ROI you actually want. A 640×384 ROI
into a 640×640 engine spends about 40% of its compute on grey padding. The
Loop tab warns when the two disagree.

Engines are tied to one GPU model and one TensorRT version. Export on the
machine that runs this.

## Panel

**Loop** — engine path, ROI anchor (screen centre or mouse), size, offsets,
confidence, box cap. Negative Y offset lifts the region toward the horizon.

**Overlay** — click-through, always-on-top boxes. Colour, thickness,
opacity, and a separate confidence floor for drawing so you can watch
marginal detections without letting them into the control path.

Text labels are drawn as tabs rather than glyphs: `UpdateLayeredWindow`
needs premultiplied alpha, and GDI text renders opaque black into that
format. Class is encoded as tab width, confidence as tab fill. Swapping this
for Direct2D would give real text and is a reasonable next change.

**Debug** — per-stage mean/p50/p90/p99/max, a 256-frame loop-time plot,
counters for arrived / processed / dropped / empty, and a timestamped log
with warnings and errors.

Read the **p99**, not the mean. A 2 ms mean with an 18 ms p99 drives worse
than a steady 4 ms.

## Known limits

- Overlay draws with GDI. Fine at 60 Hz, and off the hot path, but Direct2D
  would be cleaner and would give real text.
- CUDA graph capture isn't wired yet. Worth roughly 0.3–1 ms once the rest
  is stable.
- Nothing is sent to the Arduino yet. The publish step is where a Kalman
  filter and the serial writer go next.
- Single monitor (index 0). Multi-monitor needs a picker in the Loop tab.
