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
Setup on a fresh machine
```
setup.bat      installs the prerequisites   (asks for admin, may reboot)
build.bat      compiles                     (no admin)
```
`setup.bat` installs, via winget: Git, Visual Studio 2022 Build Tools with
the C++ toolset and Windows SDK, the CUDA Toolkit, and optionally Python
plus ultralytics for exporting engines. Budget 20–40 minutes and one reboot.
TensorRT is the one manual step. NVIDIA gates it behind a free developer
account and a click-through EULA, so no script can fetch it unattended. The
setup script opens the download page, then takes the path to the zip you
downloaded — extracts it, finds the right subfolder, and sets `TENSORRT_DIR`.
You can also paste in a folder you already extracted.
Get TensorRT 10.16 GA, Windows x64, CUDA 12.x, ZIP. Three parts of that
matter:
10.x, not 11.x. TensorRT 11 removed `BuilderFlag::kFP16`,
`BuilderFlag::kINT8`, `IInt8Calibrator`, and the `IPluginV2` family. That
breaks this build and the ultralytics export flags `half=True` and
`int8=True`. Revisit once ultralytics has migrated.
The CUDA variant must match your toolkit. TensorRT 10.16 ships both
`cuda-12.9` and `cuda-13.x` Windows zips. Run `nvcc --version`, then
download the one whose major version matches. Mixing them fails at link
time. `setup.bat` detects your toolkit and tells you which to grab.
ZIP, not the pip wheel. The wheel gives Python bindings only; the C++
headers and `.lib` files are in the ZIP.
The RTX 2080 Ti (Turing, sm_75) is supported by both TensorRT 10 and 11 and
by CUDA 13 — but CUDA 13 dropped Maxwell, Pascal, and Volta, so Turing is now
the minimum architecture rather than a comfortable middle. Fine today,
worth knowing for a project measured in months.
Note that TensorRT 10.16 moved the Windows `.dll` files from `lib\` to
`bin\`. `build.bat` checks both, so either layout works.
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
Engine
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
The bin folder
Everything the app keeps lives next to the exe:
```
loopcore.exe
bin/
  models/    every model you load, copied here on import
  config/    named profiles, plus last.cfg
```
Models are copied, not referenced. Loading a file puts a copy in
`bin/models` and loads that copy. The library keeps working after the original
is moved, renamed, or deleted, and because conversion writes its `.onnx` and
`.engine` beside the input, those land in the library too. Nothing is ever
overwritten — a same-named file gets a numeric suffix, since two models can
share a name and be different networks.
Profiles capture every setting on every tab plus the model in use. Save
them by name on the Misc tab. The current session is written to `last.cfg` on
exit and restored on start, so the app comes back exactly as you left it.
The config format is plain `key=value` text — readable, diffable, and
hand-editable. Unknown keys are ignored and missing ones keep their default,
so an older file still loads after new settings are added. Values are clamped
on load, so a corrupted file can't put the app in a state with no way back
through the UI.
The Arduino side
`firmware/loopcore_mouse/loopcore_mouse.ino` is the other half of the serial
output. Without it the packets arrive and nothing acts on them.
Flash it to any board with native USB HID — Leonardo, Micro, Pro Micro,
Teensy. An Uno or Nano cannot do this without reflashing its 16U2.
Turn on USB Host Shield in the Firmware card if a shield is fitted and a
physical mouse is plugged into it. That flag does two jobs: it brings up the
MAX3421E, which is what switches VBUS through to the downstream port, and it
builds the passthrough path. Without it a mouse on the shield gets no power
at all.
Passthrough reads the mouse in report protocol, not boot protocol. Boot
protocol is a fixed three-byte report — three buttons, X, Y — with no wheel
field and no room for buttons 4 and 5. It exists so a mouse works in a BIOS.
Output goes through HID-Project rather than the stock Mouse library for the
same reason: the stock one has three buttons and no wheel.
If a mouse reports an unusual layout, set `DUMP_REPORTS` to 1 in the sketch.
The board prints its raw HID reports back over the serial line and loopcore
logs them as `board: ...` entries.
Two things in the sketch exist for the same reason and should not be
loosened: serial is drained a bounded number of bytes per pass, and HID
reports are capped at 1 kHz. USB work must never be starved by serial work.
Draining the whole buffer in one go is what makes a pass-through mouse feel
like it is dragging.
loopcore can flash it for you: Settings, Firmware, Flash board. It installs
the `arduino:avr` core and the `Mouse` library first — `Mouse.h` is a
separate library rather than part of the core, which the compiler error does
not make obvious. That needs
`arduino-cli` on PATH or dropped into `bin\tools`; `setup.bat` offers to
install it. Uploading is delegated rather than reimplemented because a 32u4
exposes its bootloader on a different COM port for a few seconds after a
1200-baud touch, and arduino-cli already follows that correctly.
loopcore rate-limits its end to match. The control loop still runs at its own
rate; movement between packets is accumulated rather than dropped.
Panel
Loop — engine path, ROI anchor (screen centre or mouse), size, offsets,
confidence, box cap. Negative Y offset lifts the region toward the horizon.
Overlay — click-through, always-on-top boxes. Colour, thickness,
opacity, and a separate confidence floor for drawing so you can watch
marginal detections without letting them into the control path.
Text labels are drawn as tabs rather than glyphs: `UpdateLayeredWindow`
needs premultiplied alpha, and GDI text renders opaque black into that
format. Class is encoded as tab width, confidence as tab fill. Swapping this
for Direct2D would give real text and is a reasonable next change.
Debug — per-stage mean/p50/p90/p99/max, a 256-frame loop-time plot,
counters for arrived / processed / dropped / empty, and a timestamped log
with warnings and errors.
Read the p99, not the mean. A 2 ms mean with an 18 ms p99 drives worse
than a steady 4 ms.
Finding where latency comes from
The Debug tab carries the tools for this, and two of its rows are easy to
misread.
`inference` versus `gpu only`. The first is wall clock around enqueue and
synchronise, so it includes however long the thread waited to be scheduled.
The second is measured with CUDA events on the device. If they agree, the
model genuinely costs that and the fix is the engine: rebuild without
embedded NMS, or at a smaller input size. If `gpu only` is much smaller, the
GPU is fast and something is holding the thread up.
Idle frames are not timed. When nothing wants the output, the callback
returns immediately, and mixing those into the loop timing produced a graph
that swung between two unrelated populations.
Drift compares a recent median against a baseline taken once the run
settled. "It got slower after a few minutes" is hard to see in a percentile
covering the whole session, because the early frames hold it down.
Resources reads GPU clock, temperature, power and throttle reasons
through NVML, loaded from the driver at runtime. A card that drops from its
boost clock under sustained load is the usual explanation for a run that was
fine and then was not.
Copy timing report puts all of it on the clipboard, including the current
detections and the raw loop times. Take one while holding the activation key.
Rules the output path follows
Written down because every one of them was learned by breaking it.
Nothing on the control thread may block. Not a disk write, not a log
mutex shared with the UI, not a serial write on a synchronous handle. Serial
I/O is overlapped and skipped rather than waited on; anything the board says
is queued for the UI thread to log.
Timestamp frames when they arrive, not when they are finished with.
Stamping at the end of the pipeline reports every detection as brand new and
the prediction leads short by exactly the processing time.
Objects shared between threads are replaced under a lock. The output sink
is swapped by the UI thread and used by the control thread; without the lock,
changing input method frees it mid-send.
Timer periods are in whole milliseconds. `1000/rate` is integer division
and goes to zero above 1000 Hz, which silently turns a periodic timer into a
one-shot. Re-arm a one-shot with an exact 100 ns due time instead.
An overlapped read needs a buffer that outlives the call. A local array
plus an abandoned pending read is the driver writing into a dead stack frame.
cudaMemcpyAsync into pageable memory is not asynchronous. The driver
stages it and blocks. Destinations that matter are pinned.
Known limits
Overlay draws with GDI. Fine at 60 Hz, and off the hot path, but Direct2D
would be cleaner and would give real text.
CUDA graph capture isn't wired yet. Worth roughly 0.3–1 ms once the rest
is stable.
The engine's compute precision cannot be read back from a built engine;
TensorRT offers no API for it. The `io fp32` in the description is the
input binding only and says nothing about the layers. `gpu only` is the
honest guide.
Nothing is sent to the Arduino yet. The publish step is where a Kalman
filter and the serial writer go next.
Single monitor (index 0). Multi-monitor needs a picker in the Misc tab.
The icon is generated by `tools/make_icon.py` (needs Pillow). `build.bat`
compiles `res/loopcore.rc` if `rc.exe` is available and carries on without
an icon if it is not.
