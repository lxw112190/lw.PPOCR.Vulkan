# Compatibility / 兼容性

| Combination | Current evidence |
| --- | --- |
| Windows 10 x64, MSVC 19.40, SDK 1.4.350.0 | Local build and installed-package tests |
| WinForms, .NET Framework 4.0 target, Windows 10 x64 | Installed-package full OCR/ROI on AMD and NVIDIA; editable GPU and mouse mapping tests |
| HTTP, Windows 10 x64, AMD/NVIDIA | Binary/Base64/full/REC/batch/invalid/429/503/recovery locally passed; NVIDIA 1000 varied full OCR requests |
| Windows SCM / Linux systemd Vulkan GPU | Scripts/host implemented; target-machine service-account GPU access verification pending |
| AMD Radeon(TM) Graphics, driver 25.10.30.02, Vulkan 1.4.315 | Local Tiny DET/CLS/REC FP32 + full OCR reference, 100 varied full OCR repeats |
| NVIDIA RTX 4060 Laptop GPU, driver 596.36, Vulkan 1.4.329 | Local Tiny DET/CLS/REC FP32 + full OCR reference, 100 varied full OCR repeats |
| PP-OCRv6 Tiny/Small/Medium direct ONNX, AMD/NVIDIA | Local full OCR/rotation/blank/recovery against ORT CPU passed; NVIDIA varied sizes, AMD quick 320×320 suite |
| Windows 11 x64 | Target, not independently tested yet |
| Linux x64, Ubuntu 22.04, software Vulkan/lavapipe | CI definition prepared, remote result pending |
| Intel GPU / Linux physical GPU | Not yet tested in this port |
| ARM64 / UOS / openEuler / macOS / Win7 | Outside the first official binary support claim |

Runtime baseline: Vulkan >=1.1, compute queue, >=256 invocations per workgroup,
>=128-byte push constants, sufficient storage buffer and memory limits.
The current FP32 path does not require FP16 or cooperative-matrix extensions.
Future fast paths will have additional per-device capability requirements.

Windows binaries use static MSVC runtime. Regular packages use the customer's
system Vulkan loader/ICD. The [C# sharing preview](CSHARP-SHARE-PACKAGE.md) includes
an official x64 loader and optional runtime installer; GPU vendor drivers still
must be installed on the target. App-local loader is not recommended as a
permanently pinned production runtime. Linux previews require
system Vulkan loader and matching vendor ICD; initial CI Ubuntu 22.04 build
is not a UOS 20-compatible binary promise.

No portable software package can manufacture a missing vendor GPU driver.
