# GPU DET 前处理实验

2026-10-01，Windows 10 / Ryzen 7 7735H / RTX 4060 Laptop GPU，版本仍为 `0.5.0-dev.2`。
这是默认关闭的原生优化实验，不是正式发布或“已经全面超过 DML”的声明。

## 本轮改了什么

默认 CPU 路径不变。设置 `LWVK_GPU_DET_PREPROCESS=1` 后，原图 BGR 上传至 DET 图的共享 upload 缓冲区，一个 compute shader 完成双线性缩放、BGR 三通道归一化和 NHWC 布局输出，直接写入 DET 输入所在的 arena。中间图像/张量不回读、不再经 CPU 转成 NCHW 后上传。DB、透视裁剪、CLS/REC 前处理和 CTC 字典映射这轮不迁移。

前处理沿用 CPU 的 half-pixel、边界 clamp 和 double 运算；使用 `precise` 避免融合乘加改变舍入，最后写入 FP32。**前处理用 FP64，不代表网络用 FP64**；所有网络仍用 FP32，不改变模型、DET 长边上限 960、分类开关或公共 C ABI/HTTP 字段。

实验额外要求所选 GPU 支持 `shaderFloat64`。不支持时初始化明确报错，不偷偷换成低精度或 CPU；普通路径仍仅要求原有 Vulkan 能力。编译嵌入这个 shader，不等于默认路径会创建/执行它。

## RTX 4060 稳态对照

同一进程加载独立基线/候选 DLL，每模型每尺寸各预热 3 次，再交替推理 30 次，奇数轮反转顺序。输入是预解码 BGR；wall 包括原生 OCR、JSON 复制和 Python 解析，不含加载模型、文件解码和 WinForms 绘制。没有同时运行另一项 GPU 工作负载；GPU/host 诊断计时关闭。

| 模型 | 原图 | CPU 前处理基线 wall 中位数 | GPU 前处理 wall 中位数 | 耗时下降 |
|---|---|---:|---:|---:|
| Tiny | 500×500 | 36.98 ms | 33.77 ms | 8.67% |
| Tiny | 1000×1000 | 63.28 ms | 54.23 ms | 14.30% |
| Small | 500×500 | 60.12 ms | 56.76 ms | 5.60% |
| Small | 1000×1000 | 92.91 ms | 83.02 ms | 10.64% |
| Medium | 500×500 | 124.48 ms | 121.37 ms | 2.50% |
| Medium | 1000×1000 | 180.08 ms | 168.85 ms | 6.24% |

所有调用的 `items`（文本、四点坐标、检测/识别分数、分类结果）严格相等，不只是文本条数相等。关闭实验后另做 30 次默认路径对照，结果一致，wall 变化约 -0.72%～+1.48%，没有稳定提速或明显退化。单轮结果会随时钟、调度和热状态变化；不要与历史报告不同轮次的绝对值直接拼接比较。

## 100 图与边界回归

使用 C 项目已有的 100 张合成图，每图每路径预热一次、测量两次，交替顺序。每模型每路径 200 个测量样本；完整预测对象严格相同，因此没有引入这套测试集上的准确率回退，**不等于 OCR 100% 正确**。

| 模型 | CPU 基线 wall 均值 | GPU 前处理 wall 均值 | 耗时下降 |
|---|---:|---:|---:|
| Tiny | 35.07 ms | 28.75 ms | 18.01% |
| Small | 46.94 ms | 41.27 ms | 12.07% |
| Medium | 98.21 ms | 92.43 ms | 5.89% |

验证还包括：

- NVIDIA 和 AMD Radeon(TM) Graphics 各 8 个真实 GPU 张量案例：单像素、上采样、下采样、非整齐尺寸、带行填充、2448×3264 原图及 960×960 输出；逐个 FP32 bit 与 CPU 参考相同，输出尾部哨兵未被改写。
- 三模型各 10 个整图案例及其 padded-stride 调用，包含旋转、空白、小图、宽图和高分辨率图片；短缓冲区、错误 stride 被拒绝，结果指针清零。
- 40 个 DET 尺寸触发 32 计划 LRU 淘汰；返回旧尺寸可恢复相同结果。
- 原图上传也计入每图共享工作区预算。2 MiB 预算下拒绝较大原图上传，随后小图仍可用。动态原图宽、高、stride 放在 upload 头里，不依赖旧计划的原图元数据。
- 12/12 主机/ABI 单元测试；三模型共享工作区增长、REC 的 40 尺寸/CTC、回读哨兵、低预算拒绝与恢复；Tiny 独立 ORT CPU 对照、50 次重复、并发/结果生命周期测试通过。
- NVIDIA 专项张量 probe 使用 Khronos validation layer 运行，未输出验证错误；不是整个服务的 GPU 内存检查证明。
- AMD Tiny 每组交替 10 次：500 图 73.94→70.08 ms，1000 图 127.75→115.98 ms；完整结果相同。本轮未测 AMD Small/Medium，不推广 NVIDIA 百分比到其他显卡。

本轮不宣称无内存泄漏、完整长期稳定性、Intel/Linux 实机兼容性或比 DML 更快。RSS、VRAM 和长测需在后续独立验证。

## 如何体验

C# 完整体验包中：

- `Start-CSharp-Demo.bat`：默认 CPU DET 前处理，显式关闭实验。
- `Start-CSharp-Demo-GPU-DET-Experiment.bat`：开启实验，选择实际 GPU 和模型后初始化。

必须关闭之前的 Demo，再从对应脚本新启动。MSVC 静态 CRT 会在加载时持有环境快照，不要在同一个已加载 DLL 的进程中改环境变量当作 A/B 切换。

对 HTTP/自己的程序，可以在 PowerShell 中先设环境变量，再启动新的进程：

```powershell
$env:LWVK_GPU_DET_PREPROCESS = '1'
.\lw-ppocr-vulkan-http-service.exe --config http-service.json
# 回到默认路径：关闭进程，删除环境变量后重新启动。
Remove-Item Env:\LWVK_GPU_DET_PREPROCESS
```

Linux 对应 `LWVK_GPU_DET_PREPROCESS=1 ./lw-ppocr-vulkan-http-service --config http-service.json`，仅为开关用法，本轮没有 Linux 实机测试。

注意计时口径：开启实验后 `det_ms` 包含原图打包/上传、GPU 缩放归一化、DET 网络、等待和概率图回读；默认路径中 CPU DET 前处理计入 `total_ms` 的其他部分。因此 **DET 数字稍增，而总耗时下降是正常的**。比较完整 pipeline/client wall，不能把两种模式的 `det_ms` 当作纯网络 GPU 时间相减。

## 复现与证据

```powershell
$env:LWVK_GPU_DET_PREPROCESS = '1'
.\build\gpu-det-preprocess\Release\lwvk_det_gpu_preprocess_probe.exe 1
python tests/benchmark_vulkan_pair.py --before build/pipeline-opt/Release/lw.PPOCR.Vulkan.det-cpu-baseline.dll --after build/gpu-det-preprocess/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 30 --report build/gpu-det-preprocess/pair.json
python tests/test_gpu_det_preprocess.py --before build/pipeline-opt/Release/lw.PPOCR.Vulkan.det-cpu-baseline.dll --after build/gpu-det-preprocess/Release/lw.PPOCR.Vulkan.dll --device 1 --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/gpu-det-preprocess/corpus.json
Remove-Item Env:\LWVK_GPU_DET_PREPROCESS
```

`--before` 必须是保留的旧 CPU 前处理 DLL；它不识别实验开关。基线 SHA-256：`b7f9d1251d01409f53339cc59e0f32438c482b24e080e41b81c7da8411ac5815`；候选：`c509c8162f863b6b5aa45e02a9fe02f99cb75cec8f66c05877348ff0a631fdd4`。

原始结果在 [reports/gpu-det-preprocess](reports/gpu-det-preprocess)，分别记录 RTX 对照、100 图、默认关闭、AMD Tiny、工作区与独立 ORT 参考，哈希绑定当前二进制。性能表使用最终 DLL，不是编译前中间版本。

English: This opt-in experiment fuses DET BGR resize, normalization and NHWC packing into one GPU dispatch, writing directly into the DET arena. FP64 preprocessing preserves the CPU reference's FP32 output bits; neural networks remain FP32 with DET cap 960. RTX 4060 paired tests show 2.5–14.3% lower sample latency and 5.9–18.0% lower mean latency on the 100-image corpus, with every prediction field unchanged. The experiment requires shaderFloat64, is off by default, and is not a claim of universal speedup, long-run leak freedom or DML superiority. Use the separate experimental WinForms launcher and compare full pipeline/client time rather than DET stage time alone.
