# 0.5 本机性能与验证报告

2026-09-30，Windows 10 x64，AMD Ryzen 7 7735H；
NVIDIA RTX 4060 Laptop GPU（device 1，driver 596.36）和 AMD Radeon(TM) Graphics（device 0）。
当前版本 `0.5.0-dev.1`，FP32，无 FP16、协作矩阵或 CPU 自动回退。

## 可复现的性能口径

原生 C ABI 测试使用随包 500×500 测试图，先解码成连续 BGR；启用 CLS，DET 最长边上限 960，
REC 自适应宽度上限 960。每套模型预热 3 次，测量 10 次；不含 HTTP、Base64 或图片文件解码，
包含原生预处理、DB/裁剪、推理及文字组装。测试时没有同时运行其他 OCR 测试。
GPU 表中列的是宿主调用墙钟耗时，不是三个 GPU 阶段相加。

| NVIDIA FP32 完整 OCR | 中位数 | 平均数 |
| --- | ---: | ---: |
| 优化前 0.4 Tiny（5 次测量） | 398.66 ms | 见原始样本 |
| 当前 Tiny | 62.78 ms | 62.49 ms |
| 当前 Small | 165.44 ms | 181.02 ms |
| 当前 Medium | 427.65 ms | 428.32 ms |

Tiny 中位数约改善 6.35 倍；这是单图、单机开发测试，不是跨显卡通用倍数或精度排名。
Small/Medium 不能与 Tiny 当成同等工作量。Medium 当前端到端明显高于 GPU 阶段之和，
预算内计划缓存的淘汰/重建、CPU 几何及调度开销仍需进一步 profiling，不能把差值全部归因于某一项。
首次推理还包括创建计划与驱动编译，WinForms 第一次点击比预热数据慢是正常的。

实测文件包含 DLL SHA-256、图片 SHA-256、逐次采样和模型路径：
[优化前 Tiny](reports/onnx-050/perf-before.json)、
[当前 Tiny](reports/onnx-050/perf-final-tiny-nvidia.json)、
[Small](reports/onnx-050/perf-final-small-nvidia.json)、
[Medium](reports/onnx-050/perf-final-medium-nvidia.json)。

```powershell
python tests/benchmark_native.py --library dist/staging/lw.PPOCR.Vulkan.dll --models dist/staging/models/onnx/ppocrv6-tiny --device 1 --iterations 10 --report build/reports/perf.json
```

## 与 lw.PPOCR.C CPU 的比较

本机已有 `lw.PPOCR.C/build-performance-vs/Release/lw-ocr-benchmark.exe`，AVX2、4 个文字行 worker，
相同 Tiny 模型、500×500 图片、CLS 和最大识别宽度 960，预热 3 次/测量 10 次：
完整 OCR **平均 104.21ms**，对比当前 Vulkan **平均 62.49ms**。
JPEG 解码像素与 CPU 的 PPM 输入逐像素一致；两边都不计文件解码。
原始结果：[CPU](reports/onnx-050/perf-050-cpu-tiny-4.json)。
CPU 可执行文件 SHA-256：`316b1fe95c2ae615285cde51f93adc66080c7c7c302a463f5f73046ab0c8e563`。

**不能据此声称 Vulkan 已全面超过 CPU。** CPU 项目已有更快历史报告：Tiny 4 worker 约 54ms；
本次使用现有二进制，没有重建 CPU 最新优化分支，电源/温度/后台负载也未做严格实验室控制。
当前 Vulkan 约 62ms 尚未明显超过那个最好 CPU 结果。AMD 核显仅作正确性验证，未作速度领先承诺。
下一轮重点是 REC 多行分组/批处理、缓存命中与 CPU 调度，以及经过能力探测的 FP16/协作矩阵路径。

## 正确性与异常处理

- 三套模型直接 ONNX 的 DET/CLS/REC 已与 ONNX Runtime CPU 对拍；运行时不依赖 ORT。
- NVIDIA：完整 OCR 原图、180°、宽图、小图、空白图；REC 宽度 32/64/96/320/640/960。
- AMD：三套完整 OCR 的 320×320 原图、180°与空白 quick 套件。
- 检查概率、CTC 文字/置信度、缓冲区长度/stride、资源上限、错误后恢复、结果生命周期、同句柄串行并发。
- 完整流程对拍共享已复用的 C 几何库，预处理/模型/CTC 独立；不是独立 DB 算法证明。
- 6 项主机 CTest 通过；导入九个模型，拒绝截断文件，并测试 2000 个短随机 protobuf。
- 170 个固定依赖/模型资产 SHA-256 校验通过；官方来源及 Apache-2.0 归属已保留。
- 最终安装目录通过 NVIDIA HTTP 和 WinForms smoke；AMD 也通过两套测试，非程序目录启动可正确定位资源。

原始对拍文件集中在 [reports/onnx-050](reports/onnx-050)；文件名区分模型与设备。
早期完整套件与最终 quick 回归分别留档；最终 50 次回归和性能文件可核对最终 DLL SHA-256。
CI 已增加三套 ONNX 的软件 Vulkan 对拍，尚未在远程执行，不能当作 Linux 实体 GPU 已验证。

## 长测与内存告警

NVIDIA Tiny，五种尺寸 250×250/500×500/1000×1000/900×450/450×900 循环，
每 25 次额外做标题仅识别；结果与各尺寸预热结果逐次比较。

第一次只预热一轮五尺寸：1000 次完整 OCR 都一致，但 RSS 从 462.49MiB 增到 649.04MiB，
净增 186.55MiB，**未通过** 96MiB 告警线。原始程序在写报告前断言，现已改为先保存报告再失败；
这次已从控制台保留 [失败样本](reports/onnx-050/http-stress-first-1000-memory-warning.json)。

保持原来的 96MiB 告警线，另开新进程，追加 200 次相同变尺寸 OCR 预热，再测 1000 次：
1000 次 OCR + 主循环 40 次仅识别全部一致，约 201.56 秒；HTTP 往返中位 203ms，P95 329ms。
RSS 从 626.13MiB 增到 657.60MiB，净增约 31.47MiB；采样峰值约 708.67MiB，后续回落。
线程从 31 降到 26，句柄从 472 降到 469，没有持续增加；
完整数据及单轮预热 RSS 也保留在 [追加预热长测](reports/onnx-050/http-stress-warm-1000.json)。

两个测试不是同一预热口径，不能隐去首轮告警，也不能把后一次通过说成已修复泄漏。
这些现象与有上限的多尺寸缓存/驱动高水位相符，但尚未用 GPU 分配追踪或 sanitizer 排除泄漏。
长测之后仅补充导入器算子输入数量及 Identity-only 图的拒绝保护，不改变数值/调度路径；
最终包再做三模型 quick、HTTP 与 50 次变尺寸回归，报告中保留最终 DLL/EXE SHA-256。
最后 50 次（另有 100 次明确记录的预热）也通过相同 RSS 告警线：
[最终二进制回归](reports/onnx-050/http-final-50.json)。

English: the local warm FP32 Tiny median improved from 398.66ms to 62.78ms.
Small and Medium work but still have substantial end-to-end overhead. The existing CPU binary measured
104.21ms mean, while previous CPU reports were around 54ms; no universal GPU-over-CPU claim is made.
The first 1000-request run exceeded the RSS warning threshold. After explicitly recorded additional
warmup, a second 1000-request run passed the same threshold; neither establishes freedom from leaks.
This is a development preview with bounded native ONNX parsing, independent model reference tests,
and explicit hardware/test limitations, not proof of zero leaks or production support.
