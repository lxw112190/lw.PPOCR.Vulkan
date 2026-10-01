# 协作矩阵实验：不能替代 DET 960 / DML 验收

2026-10-01，开发实验，**默认关闭，不进入已验证的 dev.2 部署包**。

两个目标分别验收：

1. DET 长边保持 960，三种模型正常运行、工作空间有界。默认 512 MiB/图按需增长，
   不是预先占满显存，也不包含权重/驱动内存。此项已有独立回归证据。
2. 同 RTX 4060、相同模型/字典/图片/预处理/CLS/DET 960 的完整 OCR，预热后
   median、P95 超过强化 DML 基准，文字/框/分数/稳定性同时通过。**此项仍未完成**。

提高工作空间上限不能计入第二项，也不能缩小 DET 或挑慢的动态 DML 会话来宣布胜出。
基准仍使用固定形状 DML 会话和原生贪心 CTC；详见 [主报告](WORKSPACE-PERFORMANCE.md)。

## 实验实现

采用共享内存中 FP16 的 A/B 矩阵、FP32 协作矩阵累加、FP32 输入/权重/输出缓冲区。
其余运算仍为 FP32，不是整图 FP16。M64/N128/K32，128 线程，4 个 subgroup32，
共享行加 8 个 half 元素 padding，并对固定循环显式 unroll。
目前只选择部分较大卷积；CLS 和词表投影保留 FP32。
共享 staging/subgroup 设计的来源集中记录在 [NOTICE](../NOTICE)；不能把上游的速度当成本项目的结果。

使用前严格枚举扩展、features、矩阵尺寸/元素类型、compute stages、共享内存及 subgroup32 能力。
要求 16×16×16 的 FP16 A/B + FP32 C/result；还要启用 Vulkan memory model、
完整 compute subgroups，并在管线指定 subgroup32。仅有扩展名称不是能力证明。
实验请求而设备不满足要求时明确失败，不静默转 CPU。
默认构建不编译该 SPIR-V、不启用可选设备功能，公共 C ABI/结构大小未改变。

参考 [KHR 协作矩阵规范](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html)
及 [GLSL 定义](https://github.com/KhronosGroup/GLSL/blob/main/extensions/khr/GLSL_KHR_cooperative_matrix.txt)。

## 本机门槛

RTX 4060 Laptop 与 AMD Radeon 集显均通过五组人工 GEMM/卷积：
非整块 M/N/K、通道尾部、stride=2、padding、bias/ReLU 和输出尾部保护区。
人工数据是 FP16 可精确表达的二进制分数；误差 0 **不代表真实模型没有精度损失**。
补齐 memory model / full subgroup 设置后，两设备 Khronos validation layer 无报告。
这是主机验证层检查，不是 GPU-assisted validation 或长跑/显存泄漏证明。

真实完整 OCR 测试暴露了更严格的问题：Medium 1000×1000 输入（DET 960×960）
文字、框和 CLS 标签相同，但部分 REC 分数差约 0.05，超过既有 0.002 门槛。
保持大词表投影 FP32 仍未消除；目前原因尚未定位，不能放宽容差绕过。
失败报告明确标注 `failed_or_interrupted`，保留前面已测案例及失败案例，不能作为完成的性能报告。
[失败门槛原始记录](reports/coop-experiment/accuracy-gate.json)仅运行一次计时样本，
用于复现精度拒绝和失败报告留存，不是 median/P95 性能证据。
默认 FP32 的七项 CTest 与[三模型工作空间回归](reports/coop-experiment/fp32-workspace.json)通过；
173 项锁定资产校验通过。新增实验不会替换已有包，也未改 DET 960 或公共 ABI。
最新默认 FP32 与独立 ORT CPU 的完整 OCR quick 对拍（320 示例、180°、空白、
长度/stride、资源拒绝后恢复、结果生命周期、同句柄并发）也通过：
[Tiny](reports/coop-experiment/fp32-full-tiny.json)、
[Small](reports/coop-experiment/fp32-full-small.json)、
[Medium](reports/coop-experiment/fp32-full-medium.json)。
此 quick 回归不是长跑，也不用于性能结论；DET 960 由上面的工作空间回归单独覆盖。

小分块、未启用完整 Vulkan 功能的早期实验数据已作废；不能用它们作性能结论。
已验证 dev.2 的默认 FP32 数据仍是对外口径，本实验不替换部署包，不宣称超过 DML。

## 可复现命令（工程调试，不是用户部署参数）

```powershell
cmake -S . -B build/coop-experiment -DLWVK_EXPERIMENTAL_COOP=ON -DLWVK_BUILD_CSHARP_EXAMPLE=OFF
cmake --build build/coop-experiment --config Release
$env:LWVK_EXPERIMENTAL_COOP = "1"
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
./build/coop-experiment/Release/lwvk_coop_probe.exe 1
./build/coop-experiment/Release/lwvk_coop_probe.exe 0
Remove-Item Env:VK_INSTANCE_LAYERS
```

设备编号须先枚举。环境变量要在进程启动前设置，不提供运行中切换精度的契约。
探针仅在 `BUILD_TESTING=ON` 的实验构建存在；validation layer 需要本地 Vulkan SDK。
测性能时关闭验证层。DML 参考工具的 SDK 配置见主报告，在实验构建目录中同样配置。

```powershell
python tests/benchmark_dml_ocr.py --library build/coop-experiment/Release/lw.PPOCR.Vulkan.dll --dml-library build/coop-experiment/Release/lwvk_dml_reference.dll --device 1 --dml-shape-cache --iterations 20 --report build/reports/coop-dml.json
Remove-Item Env:LWVK_EXPERIMENTAL_COOP
```

目前该命令在 Medium 大图精度门槛处应失败；不得用于发布验收。
下一步先定位概率/CTC 差异，再进行每算子 GPU timestamp profiling、
FP16 数据布局/常驻权重、矩阵分块选择和文字行提交合并。

English: this opt-in mixed-FP16 cooperative-matrix experiment is not a release
default. FP32 global buffers and accumulation remain. Capability and validation
checks pass on the two local GPUs, but real Medium OCR score accuracy currently
fails the unchanged gate. No DML performance success is claimed; DET-960 memory
correctness and matched end-to-end performance remain independent requirements.
