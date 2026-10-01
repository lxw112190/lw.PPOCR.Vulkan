# Roadmap / 开发边界

## 0.1.0-dev.1: Tiny DET 技术验证

Implemented: portable FP32 shaders, device probe,
complete Tiny DET graph, C ABI, Python/C# examples, malformed inputs/models,
CPU reference tests, Windows/Linux CI definitions and preview packaging.
Windows physical devices are tested locally; CI definitions have not been run remotely yet.

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

## Next: validation and hardening

1. Wider correctness corpus, driver/device matrix and 1000..5000 full OCR stress.
2. Linux physical GPU and service-account deployment checks; wider WinForms usage.
3. GPU validation layers, sanitizer and fault recovery/shutdown tests.
4. Only if justified: explicit CPU reference/fallback, `cpu/vulkan/auto` and reasons.

## Optimization / release gates

- Port cooperative-matrix GEMM and FP16 variants together with feature gates;
  do not assume subgroup size, shared memory or matrix shapes across vendors.
- Keep correctness-first FP32 mode available; track CER/boxes for FP16 separately.
- Bounded shape-plan LRU only after evidence justifies replacing the one-plan policy.
- Independent Intel/AMD/NVIDIA tests; OS-specific loader/package and service tests.
- ASan/UBSan, GPU-assisted/synchronization validation, concurrent calls, shutdown,
  fault recovery, 1000..5000 varied real-image requests; do not infer leak freedom from RSS alone.
- Frozen C ABI / HTTP / config / model contracts, SBOM and dependency audits before 1.0.

不承诺未经验证的 Win7、国产 Linux、ARM64、macOS 或“所有 Vulkan 显卡”。
不把软件 Vulkan CI 对拍说成真实 GPU 性能测试。
