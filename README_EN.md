# lw.PPOCR.Vulkan

[中文](README.md)

A cross-platform C++17 PP-OCR Vulkan GPU inference project with a stable C ABI, HTTP/Web service and C# WinForms examples.
No OpenCV DNN, ONNX Runtime or CUDA **runtime** dependency. Full source is available under Apache-2.0.

Author: 天天代码码天天<br>
QQ: 819069052<br>
QQ group name: 天天代码码天天<br>
QQ group number: 264292622

## v1.0.0 stable release

v1.0.0 freezes C ABI v1, HTTP API v1, configuration Schema v1 and JSONL access-log Schema v1.
Subsequent 1.x maintenance preserves existing exports, structure layouts, fields and semantics; breaking changes require new versioned interfaces. Stable does not mean unlimited platform support, perpetual LTS, or a guarantee of zero vulnerabilities/leaks.

- Pinned PP-OCRv6 **Tiny / Small / Medium** ONNX models, loaded directly by the native C++ reader.
- Default **Vulkan FP32** full OCR (DET → optional CLS → REC) and recognition-only for pre-cropped text lines.
- Length/stride-checked BGR8 C ABI for camera pixels; independent result handles with text, four-point coordinates, confidence and timings.
- C# WinForms with editable GPU selection, three models, full-image OCR, mouse-selected recognition, overlays and timings; C#/Python integration examples.
- HTTP/Web with binary/Base64, recognition batches, API Key, bounded queue/wait and image budgets, rotating runtime/access logs.
- Windows SCM / Linux systemd install/uninstall/start/stop/restart scripts, with models, dictionaries, pages, sample and examples included.
- Capability-aware GPU DET/CLS/REC preprocessing and perspective crops share the original image. Without `shaderFloat64`, image processing stays CPU-side; **networks still use Vulkan**.
- Pinned assets, 19 C exports/layout checks, OpenAPI/Schemas, ASan/UBSan/LSan gates, SBOM, build metadata and archive SHA-256.

Download deployment packages and checksums from [GitHub Releases](https://github.com/lxw112190/lw.PPOCR.Vulkan/releases):
`lw.PPOCR.Vulkan-v1.0.0-windows-x64-full-ocr.zip` and
`lw.PPOCR.Vulkan-v1.0.0-linux-x64-full-ocr.tar.gz`, each with a matching `.sha256`.
CI artifacts support build/qualification; published Release attachments are the official downloads.

Tag CI uploads the two deployment archives and matching `.sha256` files only after both
Windows/Linux build/package jobs succeed. Missing Releases are created as **drafts** for
maintainer qualification and publication. Branch pushes/PRs and diagnostic artifacts are
not published. Matching existing assets are skipped; conflicting assets are never replaced.
Use the manual `release_tag` input to backfill an existing version; see [CI release operations](docs/CI.md#release-附件自动上传).

### Supported scope and prerequisites

Build/package targets are **Windows x64 / Linux x64**. Windows native services target Windows 10/11; recorded physical GPU tests cover Windows 10 with AMD integrated graphics and NVIDIA RTX 4060 Laptop GPU. Linux CI uses Ubuntu 22.04; software Vulkan references do not qualify Linux hardware GPUs. See the [compatibility matrix](docs/COMPATIBILITY.md).

Install a matching Vulkan loader/vendor driver. Devices need Vulkan 1.1 plus actual model/resource capabilities.
Prebuilt packages do not need Python or the Vulkan SDK; WinForms also needs a compatible .NET Framework 4.0+ installation.
Win7/8, x86, ARM64, domestic Linux distributions and macOS are not promised, nor inherited from other projects.
Missing compatible Vulkan devices fail explicitly: **there is no automatic CPU OCR fallback**.

Cooperative-matrix/mixed-FP16 and multi-REC-lane research options remain OFF by default, outside the stable inference mode.
This is not a generic ONNX engine, PDF/layout parser or table-structure recognizer.

### Validation, performance and maintenance

See [release qualification and compatibility policy](docs/RELEASE-GATES.md), [CI layers](docs/CI.md)
and [maintainer-reported acceptance](docs/MAINTAINER-ACCEPTANCE.md). Host builds, software Vulkan,
physical GPUs and service accounts are separate evidence levels. Qualify the final attachment for every release;
CI rebuilds this version/documentation update, rather than renaming historical binaries to v1.0.

Detailed optimization evidence remains in [GPU defaults](docs/GPU-DEFAULT.md),
[100-image three-project comparison](docs/THREE-PROJECT-100.md), [TensorRT comparison](docs/VULKAN-TENSORRT-100.md)
and [REC-lane experiment](docs/REC-LANES-EXPERIMENT.md). No universal DML/TensorRT superiority is claimed;
multiple lanes lacked reliable gains and are not the default. Historical reports qualify their own versions/hashes/conditions only.

[Supply chain and SBOM](docs/SUPPLY-CHAIN.md) covers 181 pinned assets and 16 components, not security certification.
[Sanitizer/recovery gates](docs/SANITIZERS.md) retain leak/error failures; flat RSS is not leak proof.
Further work expands real correctness samples, driver qualification and performance without reducing FP32 precision,
shrinking DET960 or relaxing correctness gates to claim acceleration.

## How it works

The C++ runtime loads supported ONNX graphs and implements their operations with
Vulkan Compute; it does not forward OCR to another inference framework. Vulkan is
a GPU API, not an OCR model or an automatic ONNX executor. Models determine recognition
capability; this project supplies graph execution, image processing and integration APIs.

```text
Image / camera BGR pixels
  → GPU: one original upload, DET resize and normalization
  → GPU: DET network and text probability map
  → CPU: DB postprocessing, reading order and perspective coefficients
  → GPU: perspective crops, tall-region rotation, optional CLS preprocessing/network
  → CPU: orientation decision; GPU: REC flip/resize/padding/normalization/network/greedy labels
  → CPU: CTC repeat/blank removal, dictionary mapping and result assembly
  → C ABI → C# WinForms / Python / HTTP and Web
```

This is the default on devices with `shaderFloat64`. All three `LWVK_GPU_*_PREPROCESS` options default to `auto`; `0` disables a stage and `1` strictly requires it. Automatic crop requires both DET and text preprocessing. Unsupported devices retain CPU image processing, not CPU inference fallback. The legacy preprocessing launcher sets all three to `0`. Two GPU steps (BGR8 crop, then network resize) preserve intermediate rounding. DB, sorting, orientation decisions, CTC and UTF-8/JSON remain CPU-side. Bounded command grouping is not parallel batch-N. “Other” includes GPU crop submission/wait; original upload is in DET. GPU profiling automatically selects legacy preprocessing, while forced preprocessing conflicts are rejected.

- **Loading:** a bounded native ONNX protobuf reader normalizes/lowers supported operations and applies validated fusions. It supports the pinned PP-OCRv6 Tiny/Small/Medium assets, not arbitrary ONNX models.
- **Execution:** compute shaders are compiled to SPIR-V and embedded at build time. Runtime pipelines execute convolution, matrix and attention operations in default FP32, with command submission and fence synchronization. No CUDA runtime is required.
- **CPU/GPU split:** neural networks execute on the GPU; geometry and part of decoding remain on the CPU. GPU OCR is not an entirely GPU-resident pipeline.
- **Reuse:** weights remain GPU-resident after initialization. Ordinary graphs cache up to 32 shape plans; default REC batches retain up to 16 widths per slot across eight slots (128 plans maximum), sharing one arena and per-slot IO. IO growth invalidates only its slot; arena replacement invalidates all commands. This reduces image-size switching latency at the cost of additional bounded CPU/driver command memory. Workspace limits do not cap total process/VRAM usage; weights, driver and CPU buffers are additional. See [size-switch measurements and limitations](docs/REC-CACHE-SIZE-SWITCH.md).
- **Recognition-only:** pre-cropped lines and Demo selections run REC/CTC without DET or CLS. The caller handles orientation correction when needed.
- **Devices:** selection is explicit and calls on one handle are serialized. Availability depends on capabilities and tested drivers, not a promise for every GPU. Missing devices/GPU failures are reported without silent CPU fallback. Deployment needs a matching Vulkan loader/vendor driver, but not Python or the Vulkan SDK.

## Build

Windows: Visual Studio 2022 C++, CMake >=3.20, Python >=3.9, Vulkan SDK with
`glslc` (local development SDK: 1.4.350.0).

```powershell
cmake -S . -B build/local -G "Visual Studio 17 2022" -A x64
cmake --build build/local --config Release --parallel 4
ctest --test-dir build/local -C Release --output-on-failure
.\build\local\Release\lw-ppocr-vulkan-probe.exe
```

Linux build prerequisites: CMake, Ninja, C++17 compiler, Python/numpy,
`libvulkan-dev`, `glslc` and optionally `spirv-tools`.
Configure with `-G Ninja -DCMAKE_BUILD_TYPE=Release`, then build and run CTest.
On Ubuntu, install `libvulkan-dev`, not just the runtime package `libvulkan1`.
SDK headers and `glslc` alone do not satisfy the loader link-library requirement;
otherwise CMake can report `missing: Vulkan_LIBRARY`. Compiled-package users do not need this development package.
Linux hardware GPUs and additional ARM64/domestic distributions/macOS require separate qualification;
Windows tests and Linux software Vulkan do not establish their compatibility.

Daily `build.yml` builds/packages Windows x64 and Linux x64 with equivalent
host ABI/config/unit and staged-package checks; neither job performs real OCR
inference. Windows additionally tests the WinForms host UI, while Linux checks
ELF dependencies and service-unit scripts. Hardware absence is explicit (SKIP).
The original full software Vulkan suite now lives in the manual-only
`linux-software-validation.yml`: run it on the release ref before publication.
It retains DET/CLS/REC, all three models, shader probes and real HTTP inference,
not a hardware acceleration benchmark. See [CI layers](docs/CI.md). Full OCR reference tests explicitly use
CPU image preprocessing while networks still execute through Vulkan. Independent
shader probes cover GPU DET/CLS/REC preprocessing and perspective crops (exact
bits, stride, padding and recovery), avoiding an eight-line software REC batch
exceeding a single 30-second fence budget on shared runners. Hardware defaults
are unchanged. Medium single graphs can also exceed 30 seconds on software
Vulkan, so the full software suite explicitly sets `LWVK_SOFTWARE_GRAPH_TIMEOUT_MS=180000`.
This bounded engineering option applies only to CPU software devices (Vulkan
device type 4), accepts 30000..300000 milliseconds, and defaults to 30000 when
unset. Hardware/virtual GPUs always retain 30 seconds. Packaging does not persist
the CI environment into customer configuration. This is a per-submission network
fence wait, not an OCR request deadline or task cancellation; comparisons and
poisoned-plan cleanup are unchanged. SDK downloads are pinned and SHA-256
verified. Local checks do not establish remote CI success; confirm Actions after pushing.

Compiled packages do not need Python or the Vulkan SDK. They need a matching
Vulkan >=1.1 driver/loader. Device IDs follow Vulkan enumeration order; no
implicit discrete-first remapping. Device type 4 means software Vulkan, not hardware GPU.

## WinForms quick start

Extract the Windows package and run `lw.PPOCR.Vulkan.WinFormsDemo.exe`.
Requires x64 Windows, .NET Framework 4.0+ and a Vulkan >=1.1 driver/loader;
no Python, Vulkan SDK or NuGet dependencies are needed to run it.
Tested locally on Windows 10. The .NET target does **not** establish Win7 native compatibility.

1. Select a GPU from the editable dropdown, or type its numeric ID (`0`, `1`, etc.).
   IDs follow actual Vulkan enumeration; invalid IDs are rejected, never silently remapped.
2. Initialize/reload the engine. Model/sample paths are resolved against the executable directory.
3. Run full OCR to see boxes, text, JSON, confidence and wall/GPU-stage timings.
4. Drag a text region in either direction and run recognition-only: REC + CTC, no DET/CLS.
   Zoomed coordinates are mapped back to source pixels; overlays never modify inference pixels.
5. Reinitialize after changing the GPU/model/thresholds. Load your own image, copy text or save JSON.

Select bundled PP-OCRv6 Tiny/Small/Medium ONNX models; the old Tiny converted format remains supported, not arbitrary ONNX. First-run plan creation
is included in wall time; it is not a steady-state benchmark.
See [usage and validation](docs/WINFORMS-DEMO.md) and
[Visual Studio solution](examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.sln).
Windows CI performs application-owned layout/ROI smoke tests, not GPU inference.

<a href="docs/assets/winforms-demo.png"><img src="docs/assets/winforms-demo.png" alt="C# WinForms Demo with GPU selection, detection boxes, OCR text and timings" width="960"></a>

Actual `gpu-default1` sharing bundle on RTX 4060 Laptop GPU with Tiny, shared-original GPU cropping and DET/CLS/REC preprocessing enabled, showing full-image detection and mouse-selected recognition. Timings illustrate this repeated invocation only, not all devices/images or the Medium benchmark. Click to view the full-size screenshot.

### Understanding Demo timings

| Display | Measurement boundary |
| --- | --- |
| OCR call | C# Bitmap-to-BGR conversion, native OCR, JSON copy, UTF-8 decoding and C# JSON parsing; excludes model initialization and prior image-file loading |
| UI ready | Image cloning, background scheduling, OCR call and UI result-data updates from the run handler; not completion of actual screen painting |
| Pipeline | Native OCR from handle-lock waiting through preprocessing, inference, postprocessing and result-object assembly; excludes final JSON serialization and ABI result copying |
| DET / CLS / REC | Accumulated network execution durations including upload, submission, GPU waiting and readback; not pure GPU shader time, and excluding execution-plan preparation |
| Other | `max(0, pipeline − DET − CLS − REC)`: a residual, not an independently timed operation |

Other includes CPU resizing/normalization/layout conversion, DB contours/box expansion/reading
order, perspective crops and rotation, CLS/REC preprocessing, CPU CTC repeat/blank removal,
dictionary mapping and result assembly, plus plan preparation, workspace allocation and lock waiting.
Pipeline time therefore need not equal DET + CLS + REC. Upload/wait/readback are already counted
in network timings, not counted again in Other. C# pixel conversion, JSON copying/parsing and
UI updates are outside Other. With GPU cropping, Other also includes crop submission/wait
and CPU box/orientation work; it is not pure CPU time. Original upload is counted in DET.

First calls/new shapes may prepare plans or grow workspace; do not treat them as warmed-up
performance. One-decimal displays can introduce rounding differences. Compare the same image,
model, device, parameters and timing boundaries; report initialization, first and repeated calls separately.

## HTTP / Web quick start

Run the probe, set `device_index` in `http-service.json`, then launch
`run-http-service.bat` (Windows) or `./run-http-service.sh` (Linux).
Open `http://127.0.0.1:8787/`, load the sample or your image and run OCR.
The page draws canonical boxes and shows text/JSON plus wall and GPU-stage timings.
Recognition-only expects an already-cropped text line, with no DET/CLS.

POST `/api/ocr` or `/api/recognize` accepts JPEG/PNG/BMP binary bodies
(`application/octet-stream`) or JSON `{"image_base64":"..."}`.
REC-only also accepts `{"images_base64":["...","..."]}`, processed sequentially.
Nonempty `api_key` requires the `X-API-Key` header; the web page sends that header
without URL/local-storage persistence. `LWVK_API_KEY` overrides config (empty disables).
Keys, image bodies and OCR text are not logged. Default binding is localhost;
use authentication, firewall and HTTPS reverse proxy before external deployment.

One engine serializes decode/inference; workers and transport queue are bounded.
Queue overflow returns best-effort 429 (an unparsed transport rejection has no request ID).
Waiting for the engine times out with 503; it does not cancel GPU work.
Runtime GPU faults poison the engine, requiring a restart rather than silent fallback.
Decoder allocations and single/batch pixel budgets limit image memory.
`logging_enabled` and `access_log_enabled` control rotating runtime/JSONL access logs.
Windows SCM and Linux systemd install/start/stop/restart/uninstall scripts are bundled;
target-machine service-account/GPU access still needs separate verification.

See [HTTP service details](docs/HTTP-SERVICE.md) and [local HTTP test report](docs/LOCAL-HTTP-REPORT.md).

## Independent GPU correctness test

```powershell
python -m pip install -r requirements-dev.txt
python tests/test_det_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 0 --iterations 100 --report build/reports/device0.json
```

Tests use identical FP32 inputs for CPU/GPU, multiple image sizes, probability-map
error and threshold-map IoU, invalid inputs/recovery and repeated shape changes.
ONNX Runtime is an optional test dependency, not a native runtime dependency.
RSS observations alone do not prove leak freedom. GPU validation layers,
host sanitizers and extended physical-device tests remain required.

Input: normalized FP32 NCHW `[1,3,H,W]`, H/W multiples of 32 in 32..960.
Output: FP32 `[H,W]` probability map, not text or boxes.
Generic graph API additionally supports CLS `[1,3,80,160]` -> `[1,2]` and REC
`[1,3,48,W]` -> `[T,C]`, C=6906 for Tiny or 18710 for Small/Medium; W=32..960 and a multiple of 8. CLS/REC use BGR [-1,1].
`lwvk_recognize_tensor` returns native UTF-8 text and mean emitted-label confidence.
Text buffer sizes include the trailing NUL; insufficient capacity returns 5 without truncation.
See `include/lw_ppocr_vulkan.h` and `examples/python/lwvk.py`.
Calls on the same handle are serialized; never destroy a handle during an active call.
Default graph workspace limit: 512 MiB for the shared arena and IO buffers, excluding weights and driver allocations.
This is an on-demand ceiling, not an upfront reservation. WinForms exposes a per-graph MiB limit (0=default).
DET keeps its default longest-side limit of 960; reducing image size is not a performance improvement claim.
See [workspace fixes, DML baseline and remaining optimization](docs/WORKSPACE-PERFORMANCE.md).
See [operator profiling and the second FP32 optimization round](docs/GPU-PROFILING.md): diagnostics default off; large-image DML gaps remain.
The [third FP32 round](docs/GELU-FUSION.md) adds guarded GELU fusion and matched full-OCR measurements. Medium large-image latency improved, but it still trails DML; DET-960 and accuracy gates remain unchanged.
These are historical baselines. The [fourth FP32 round](docs/HOST-TRANSFER-OPTIMIZATION.md)
improves cached readback, persistent mappings and CPU preprocessing; it passes six
matched-host DML comparisons, but does not establish superiority on every input/device.
See the [opt-in cooperative-matrix experiment](docs/COOPERATIVE-MATRIX-EXPERIMENT.md); it does not replace the tested FP32 deployment package.
Explicit Vulkan failures do not silently fall back. The 30-second fence timeout
is not cancellation; a failed plan cannot be reused.

The default models are now direct ONNX under `models/onnx/ppocrv6-tiny`, with matching dictionaries. Legacy Tiny internal v0 assets remain compatible:
`models/ppocrv6-tiny/det.json` + `weights.bin`. The exporter checks the exact model SHA-256.

## Attribution

Apache-2.0; see `LICENSE` and [NOTICE](NOTICE) for third-party sources, pinned revisions and modifications. Complete dependency licenses are retained in `licenses`.
nlohmann/json is MIT. Model assets retain upstream attribution and licensing.
Port correctness and performance must be measured independently.

See [compatibility evidence](docs/COMPATIBILITY.md), [DET report](docs/LOCAL-DET-REPORT.md),
[CLS/REC report](docs/LOCAL-TEXT-REPORT.md)
and [full OCR report](docs/LOCAL-OCR-REPORT.md), plus [roadmap](docs/ROADMAP.md).
Full OCR tests use independent ORT graphs/NumPy preprocessing/CTC but share C geometry;
they are NOT independent DB algorithm comparisons. Python tests use Python >=3.10 (CI: 3.12).

To build a deployment archive after qualification (use a fresh staging directory):

```powershell
cmake --build build/local --config Release --parallel 4
cmake --install build/local --config Release --prefix dist/staging
python tests/test_api.py --library dist/staging/lw.PPOCR.Vulkan.dll
python scripts/package.py --staging dist/staging --output dist --platform windows-x64
```

The package contains the library, probe, DET/CLS/REC assets, dictionary, examples, sample image and
documentation, HTTP/Web, service scripts and a SHA-256 sidecar.
Windows uses the static MSVC runtime; inspected direct DLL dependencies are
`vulkan-1.dll` and `KERNEL32.dll`. Install the GPU vendor's Vulkan driver/loader;
do not copy development-machine driver files as a deployment substitute.
If the build machine has the .NET Framework C# compiler, the package also includes
an x64 console interop demo alongside the WinForms tester:

```powershell
.\lw-ppocr-vulkan-probe.exe
.\lw.PPOCR.Vulkan.CSharpDemo.exe models/ppocrv6-tiny/det.json 1
.\lw.PPOCR.Vulkan.CSharpDemo.exe --recognize models/ppocrv6-tiny/rec/model.json test-images/sample.jpg 1 20 28 292 46
.\lw.PPOCR.Vulkan.CSharpDemo.exe --ocr models/ppocrv6-tiny test-images/sample.jpg 1
```

Replace `1` with the device index printed by the probe.

## Recognition-only from a cropped text region

`lwvk_recognize_bgr` accepts BGR8 pixels with a positive byte stride and explicit
buffer length >= `(height-1)*stride+width*3`. Source images are capped at 40M pixels.
It performs bilinear resize, aspect-preserving right padding (normalized byte 128),
GPU REC/greedy CTC and CPU character assembly. It does **not** run DET or automatically rotate using CLS.
`rec_width=0` chooses an adaptive multiple of 8 in 32..960; very long lines may be compressed.
Reported GPU time excludes image decoding/preprocessing/CTC.

```powershell
python examples/python/recognize_image.py --library build/local/Release/lw.PPOCR.Vulkan.dll --model models/ppocrv6-tiny/rec/model.json --image test-images/sample.jpg --roi 20 28 292 46 --device 1
```

The ROI here is supplied manually, not automatically detected. Local output:
`纯臻营养护发素`. Omit `--roi` when the customer image is already cropped.
Camera pixel buffers can call the same native API without JPEG/Base64 encoding.
See the [Demo timing and presentation revision](docs/DEMO-TIMING-OPTIMIZATION.md) for UTF-8 fixes, on-demand detail tabs, and separate OCR-call, native-pipeline and UI-ready measurements. Native models and precision remain unchanged.

The latest [host-pipeline optimization](docs/PIPELINE-OPTIMIZATION.md) reduces redundant CLS/REC resize and crop work without changing FP32, models or shaders. Paired RTX 4060 tests improve Tiny by 20–24%, Small by ~16% and Medium by 9–10%; all 100-image predictions remain exactly equal. Other GPUs may not gain equally.

The [complete C# sharing package](docs/CSHARP-SHARE-PACKAGE.md) bundles three
models, an official x64 Vulkan loader, an optional Runtime installer and C#
source. A compatible installed GPU vendor driver is still required.

See the [development guide](docs/DEVELOPMENT.md) for first-party formatting and ownership/synchronization boundaries.

## Full-image OCR

The optional Python image example needs `python -m pip install numpy pillow`;
the compiled native library/C# console demo does not need Python or the Vulkan SDK.

```powershell
python examples/python/ocr_image.py --library build/local/Release/lw.PPOCR.Vulkan.dll --models models/ppocrv6-tiny --image test-images/sample.jpg --device 1 --draw build/ocr-boxes.png
```

Add `--no-cls` to disable classification. JSON `items` contains `x1/y1` through
`x4/y4` (top-left, top-right, bottom-right, bottom-left) in source coordinates,
`text`, `score`, `det_score`, `cls_label` and `cls_score`, without a duplicate box array.
Disabled CLS returns label -1, score 0. This is line OCR, not PDF/layout/table analysis;
recognition errors remain possible. Graph workspace defaults to 512 MiB PER GRAPH,
including shared staging/IO; up to three graphs plus weights/driver/CPU geometry memory.
Initialize config with `lwvk_ocr_config_default`, then create/run the engine.
The independently owned result survives engine destruction; query/copy JSON does
not rerun inference. Destroy results explicitly. Never destroy engines/results
while another call uses them. See [OCR API contract](docs/OCR-API.md).

## Support the project

If this project helps you, optional donations support open-source maintenance.
Thank you for using the project, reporting issues and contributing!

<a href="www/sponsor.jpg"><img src="www/sponsor.jpg" alt="WeChat donation QR code" width="240"></a>
