# Roadmap / 开发边界

## 0.1.0-dev.1: Tiny DET 技术验证

Implemented: portable FP32 shaders, device probe,
complete Tiny DET graph, C ABI, Python/C# examples, malformed inputs/models,
CPU reference tests, Windows/Linux CI definitions and preview packaging.
Historical initial stage: Windows physical devices were tested locally; CI was not yet run at that point.

## 0.2.0-dev.1: CLS/REC and recognition-only

Implemented: complete pinned Tiny CLS/REC graphs on FP32 Vulkan, offline BN and
MatMul lowering, native CTC, UTF-8 length-safe text API, BGR buffer/stride checks,
aspect-preserving adaptive REC preprocessing, C#/Python image examples.
Two physical GPUs passed per-model CPU comparison, same-handle concurrent calls,
and 1000 CLS + 1000 REC repeated invocations each. This is not full OCR yet.

## 0.3.0-dev.1: complete OCR

Implemented: reviewed lw.PPOCR.C DET preprocessing, DB, reading order and
perspective crop (MIT), connected optional CLS/rotation and REC. Raw BGR has explicit
length/stride checks; sequential crop and cumulative pixel limits bound memory/work.
Independent result handles return canonical coordinates once, confidence and timing.
Python and C# full-image examples. Full OCR reference uses independent ORT CPU graphs
and NumPy preprocessing/CTC, sharing reviewed C geometry; host golden tests cover basics.

## 0.3.0-dev.2: WinForms tester

Implemented: .NET Framework 4.0 x64 UI, editable GPU enumeration/index validation,
full OCR overlays, mouse ROI recognition-only, text/JSON/confidence and timing.
Worker-thread inference and safe shutdown waiting; app-directory model/sample paths.
Installed-package full OCR/ROI smoke passed on local AMD and NVIDIA devices.
Windows CI adds host-only layout/mapping tests without a hardware inference claim.

## 0.4.0-dev.1: HTTP/Web

Implemented: single serial Vulkan engine, binary/Base64 full OCR/REC-only/batch,
bounded transport queue, wait timeout, allocation-budgeted JPEG/PNG/BMP decoding,
sequential batches and cumulative budgets, API Key, request IDs, spdlog rotating
runtime/JSONL logs, web overlays/timing, Windows SCM and Linux systemd scripts.
Local AMD/NVIDIA API/invalid-input/429/503/recovery tests passed. A 1000-request
varied-size NVIDIA full OCR run completed; RSS alone does not establish leak freedom.

## 0.5.0-dev.1: native ONNX / three model sizes / first performance pass

Implemented: bounded native ONNX reader (provenance retained in NOTICE),
official Tiny/Small/Medium assets and matching dictionaries, shared CLS,
fused LayerNorm/attention, tiled FP32 kernels, GPU greedy CTC, resident weights
and byte-bounded LRU plans. WinForms model selection and HTTP config paths updated.
No generic arbitrary-model ONNX support, FP16 or cooperative matrices claimed.

## 0.6.0-dev.1: machine-checked v1 candidate contracts

Implemented: OpenAPI + config/HTTP response/access-log JSON Schema, canonical hashes,
19-symbol C ABI candidate baseline and native/Python x64 layouts, config/native
process equivalence cases, live response/log validation, packaged schemas and
per-file archive integrity checks. RELEASE_VERSION is the single version source.
No production freeze, LTS or full security audit is claimed.

## Next: validation and hardening

1. Wider correctness corpus, driver/device matrix and 1000..5000 full OCR stress.
2. Linux physical GPU and service-account deployment checks; wider WinForms usage.
3. GPU validation layers, sanitizer and fault recovery/shutdown tests.
4. Supply-chain inventory/SBOM, license and vulnerability review, upgrade/rollback,
   final artifact qualification, RC and explicit support policy. See [release gates](RELEASE-GATES.md).

## Optional optimization / release gates

- Optional research, not a v1.0 prerequisite: port cooperative-matrix GEMM and FP16 variants with feature gates;
  do not assume subgroup size, shared memory or matrix shapes across vendors.
- Keep correctness-first FP32 mode available; track CER/boxes for FP16 separately.
- A bounded 32-plan metadata LRU and shared arena/IO are implemented; retain resource-cap and changing-width regression tests.
- Independent Intel/AMD/NVIDIA tests; OS-specific loader/package and service tests.
- ASan/UBSan, GPU-assisted/synchronization validation, concurrent calls, shutdown,
  fault recovery, 1000..5000 varied real-image requests; do not infer leak freedom from RSS alone.
- Frozen C ABI / HTTP / config / model contracts, SBOM and dependency audits before 1.0.

不承诺未经验证的 Win7、国产 Linux、ARM64、macOS 或“所有 Vulkan 显卡”。
不把软件 Vulkan CI 对拍说成真实 GPU 性能测试。
