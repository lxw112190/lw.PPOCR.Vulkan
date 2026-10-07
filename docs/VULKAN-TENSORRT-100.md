# 三模型：默认 Vulkan 与 PP-OCRv5_Test_trt 比较

测试日期：2026-10-02；RTX 4060 Laptop GPU / Windows 10 x64。

本页保留该日期原始数据，不代表最新开发 DLL。2026-10-07 的 HardSwish 融合后复测见 [最新报告](HARDSWISH-OPTIMIZATION.md)；三模型仍未超过已有 TRT plans。

## 结论

目前 **TensorRT 更快，Vulkan 尚未超过这个 TensorRT 项目**。主对比使用 Demo 的 REC batch=4、predictor=4；三模型 TensorRT 相对 Vulkan 的速度约为 **1.45× / 1.55× / 2.22×**。

Vulkan 主对比的专用显存峰值较低，约为 132/227/616 MiB；四实例 TensorRT 为 540/1046/1556 MiB。但实例数、FP16/FP32、不同流水线和工作区策略都会影响数据，不能把差异全部归因于后端。因此另外测了 TensorRT 单实例，详见后文。

较高 GPU 利用率不等于更快：Vulkan 的整卡平均利用率更高，却用更长时间完成同一批 OCR。这里的利用率是驱动计数器观察的忙碌比例，不是 shader occupancy、Tensor Core 利用率或浮点效率。

## 测试条件与边界

- CPU：AMD Ryzen 7 7735H，16 个逻辑线程；GPU：NVIDIA GeForce RTX 4060 Laptop GPU，驱动 596.36；Windows 10.0.19045，Python 3.12.7。
- 核验设备名一致：Vulkan device=1，CUDA device=0，NVML index=0；本机仅一个 CUDA 设备，不照搬该编号到其他电脑。
- 使用 `lw.PPOCR.C/build-local-data/lw-generated-ocr` 的 100 张合成 JPEG，614 条 GT；元数据 SHA-256 为 `c51474cb3761515c8c9b07c0afb1846303aba1b2304ef9a87043c9d8c282159d`。不是私有实拍图。
- 每个模型/配置独立进程串行测试，无并行测试争用；先完整预热 100 图一次，再测五轮（500 次），每隔一轮反向换图。主对比六组，加上三组单实例补测，共 **5400 次完整 OCR 调用，其中 4500 次计时**。
- 调用相同 BGR 输入；DET 长边 960、阈值 0.3/0.6、unclip=1.5、dilation=false、CLS 开、CLS 阈值 0.9。不是 HTTP 吞吐或 GUI 响应测试。
- 速度计时包括实际项目的完整原生 OCR 和 Python 结果 JSON 转换；不含文件解码、模型加载、GT 评分、HTTP 或界面。启用相同 250ms 资源监控；不锁定频率/功耗，无法排除温度和调度差异。
- GPU 利用率统计覆盖持续测量流，包含调用间的 JPEG 解码/JSON 等间隙；GT 评分延后到资源采样结束。不能将这列解释成纯网络执行期间的利用率。

### 实际测试的是哪些程序

Vulkan：`build/gpu-default/Release/lw.PPOCR.Vulkan.dll`，版本 `0.5.0-dev.2`，SHA-256 `4e95bc3c32aa449480761f615097912a8a223429f7115a42fa3b2867474b2c06`。三项前处理环境变量均未设置，在该 GPU 上自动使用共享原图/GPU 裁剪/DET+CLS+REC 前处理；网络 FP32，每图工作区上限 512 MiB。未启用协作矩阵、GPU profiler 或 validation layer。

TensorRT：**直接调用 `PP-OCRv5_Test_trt/OCRV5Test/bin/x64/Release/lw.TensorRT.PPOCRSharp.dll`**，没有使用另一个推理实现代替。该 DLL SHA-256 `0a5da97fa4b54d9b094c69fb117c5c46abfb4878da8821169da44d18e23fa0b6`，TensorRT 10.16.1.11，项目自带 CUDA 12 runtime / OpenCV 4.8.1。

Tiny 用 Demo 现有引擎；Demo 目录缺少 Small/Medium，引擎复用本地 `TensorRT-10.16.1.11.Windows.amd64.cuda-12.9/TensorRT-10.16.1.11/bin/inference_trt` 缓存。六份 DET/REC ONNX、共享 CLS ONNX 和字典均与 Vulkan 固定资产逐个 SHA-256 核对一致。引擎及运行时依赖哈希记录在原始报告中；缓存引擎没有完整构建参数/签名来源证明，本次没有重新构建引擎。

**并非相同数学执行路径**：TensorRT 是已有 FP16-enabled plan（FP32 I/O，允许部分 FP32 回退），不能从文件名断言每层都为 FP16；Vulkan 网络全部 FP32。TensorRT REC 以 320 为最小宽度、32 对齐、最大 1280，真实批量推理及多 REC 引擎；Vulkan 为 32～960、8 对齐、有界合并提交但网络逐行，单句柄串行。原项目的 OpenCV 图像处理、CPU CTC、排序和日志保留。这是现有项目部署表现，不是剥离所有差异后的纯后端竞赛。

## 速度

单位 ms/张，主对比每组 500 个成功调用；初始化单独统计。

| 模型 | 项目 | 平均 | P50 | P95 | 初始化 |
| --- | --- | ---: | ---: | ---: | ---: |
| Tiny | TensorRT，REC 4 实例 | 15.97 | 15.63 | 21.58 | 351.1 |
| Tiny | Vulkan，默认 | 23.09 | 23.71 | 29.48 | 243.0 |
| Small | TensorRT，REC 4 实例 | 23.12 | 22.63 | 30.39 | 582.8 |
| Small | Vulkan，默认 | 35.75 | 36.09 | 45.81 | 340.4 |
| Medium | TensorRT，REC 4 实例 | 34.08 | 33.61 | 46.57 | 920.8 |
| Medium | Vulkan，默认 | 75.67 | 76.68 | 99.48 | 714.2 |

不把已有 TensorRT 引擎构建耗时计入初始化；Vulkan 的首次尺寸计划准备位于预热中。此处五轮配对参数同图，但各组是串行进程，而不是同一进程逐调用交替。

## 内存与显存

单位 MiB。生命周期工作集峰值取 Windows 进程计数；Private 与 GPU 峰值每 250ms 采样，可能漏掉短暂尖峰，覆盖加载/预热/测量。RAM 包含同一 Python/NumPy/Pillow 壳、当前输入、结果、运行时和驱动。

| 模型 | 项目 | 工作集峰值 | Private 峰值 | GPU 专用峰值 | GPU 共享峰值 |
| --- | --- | ---: | ---: | ---: | ---: |
| Tiny | TensorRT，4 实例 | 599.6 | 1360.1 | 539.5 | 120.0 |
| Tiny | Vulkan | 237.0 | 368.7 | 131.6 | 53.4 |
| Small | TensorRT，4 实例 | 669.7 | 1956.4 | 1045.6 | 174.0 |
| Small | Vulkan | 294.4 | 474.7 | 227.2 | 59.4 |
| Medium | TensorRT，4 实例 | 706.4 | 2595.3 | 1555.6 | 178.0 |
| Medium | Vulkan | 612.6 | 870.5 | 615.9 | 61.4 |

GPU 为 WDDM 对 PID 的专用/共享归属值，跨该进程的适配器求和，不是整卡 VRAM 使用总量。Private 是进程私有提交量，工作集是驻留内存；**各列不能相加当作总物理内存占用**。工作区上限也不是整个进程/显卡的内存上限。

## GPU 利用率

| 模型 | 项目 | 进程最忙引擎平均 / 峰值 % | NVML 整卡平均 / 峰值 % | 有效测量样本 |
| --- | --- | ---: | ---: | ---: |
| Tiny | TensorRT，4 实例 | 24.4 / 35.8 | 27.9 / 46 | 59 |
| Tiny | Vulkan | 35.2 / 42.3 | 35.2 / 41 | 75 |
| Small | TensorRT，4 实例 | 27.1 / 34.3 | 34.7 / 46 | 75 |
| Small | Vulkan | 48.4 / 59.2 | 49.0 / 59 | 101 |
| Medium | TensorRT，4 实例 | 42.5 / 52.3 | 48.3 / 63 | 99 |
| Medium | Vulkan | 68.5 / 86.2 | 67.5 / 87 | 183 |

- 进程列取该 PID 在每个采样区间最忙的 GPU engine，不把 Cuda/Compute/Copy 多引擎百分比相加。TensorRT 主要在 `Cuda`，Vulkan 主要在 `Compute_1`。
- NVML 列是 NVIDIA 整卡，可包含其他程序活动，驱动内部平均窗口不一定等于采样周期；不是精确每张图的 GPU 占用。
- 平均值是有效样本的算术均值，不是 shader 工作量积分；采样间隔会受监控开销和系统调度影响。
- 原始数据还记录 `board_memory_utilization_percent`，这是显存活动比例，**不是显存容量已用比例**，所以不把它作为“占了多少显存”。
- 计数器缺失保留 null，而不当成 0；正式九组有效，未出现 PDH/NVML 采样错误。首次速率计数尚未形成区间时不参与平均。

## 补测：TensorRT 单 REC 实例

仍是同模型、CLS 开、REC batch=4 和 500 次计时，只将 predictor 从 4 改为 1。

| 模型 | 平均 ms | 工作集峰值 MiB | Private 峰值 MiB | GPU 专用峰值 MiB | GPU 共享峰值 MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tiny | 19.62 | 564.9 | 983.6 | 277.5 | 104.0 |
| Small | 28.66 | 583.5 | 1258.7 | 421.5 | 132.0 |
| Medium | 41.79 | 595.1 | 1531.4 | 691.5 | 134.0 |

单实例降低了 TensorRT 资源开销，但比四实例慢，仍快于本轮 Vulkan。Medium 单实例工作集 595.1 MiB，略低于 Vulkan 的 612.6 MiB，说明“Vulkan 所有内存指标都更低”并不成立；Private 和专用显存仍较低。Vulkan 对单实例 TensorRT 的专用显存优势只约 11%（Medium），不是四实例对比中的约 60%。不同 REC 分组/宽度会影响文本预测，单实例不要求和四实例逐项相同。

单实例整卡 GPU 平均/峰值为 Tiny 28.9/52%、Small 34.4/43%、Medium 49.0/61%；进程最忙引擎平均为 22.8/24.1/40.2%。原始报告保留全部采样与轮次内存。

## 正确性与重复检查

虽然用户重点是速度与资源，本轮也用已有 GT 评分，避免把少识别或空结果当作性能优势。NFC 规范化、轴对齐框贪心一对一 IoU≥0.30；字符指标包含未匹配 GT 的删除和多余预测的插入，保留空白、标点与大小写。

| 模型 | TensorRT 4 实例字符准确率 | TensorRT 1 实例字符准确率 | Vulkan 字符准确率 |
| --- | ---: | ---: | ---: |
| Tiny | 95.03% | 94.93% | 95.12% |
| Small | 97.62% | 97.49% | 97.72% |
| Medium | 97.65% | 97.64% | 98.80% |

这些仅是该合成语料的端到端字符指标，不是通用准确率，也不能把差异全部归因于 FP16。五轮同组文字/坐标（坐标按 0.001 精度比较）无变化，请求失败 0；**5400 次分散在九个独立进程，不是一个进程 5400 次长测，更不是无泄漏证明**。

## 后续优化方向（本次未修改推理代码）

可以明确的观察：Medium 的差距最大；Vulkan GPU 忙碌时间比例较高，但完成请求更慢。建议下一轮在保持当前准确性门槛的前提下对 DET/REC 网络做算子级 profiling，分析卷积/GEMM/Attention 的耗时、访存和实际 REC 批处理收益。FP16/tensor-core 路径只能作为独立精度验证候选，不能直接替换已验证 FP32 默认。利用率计数器本身不足以证明某一算子是瓶颈，本轮也没有对所有潜在原因做因果定位。

## 原始证据与复现

[主对比矩阵与全部原始结果/采样](reports/vulkan-trt-100/matrix.json)；[Tiny 单实例](reports/vulkan-trt-100/single-tiny-trt.json)、[Small 单实例](reports/vulkan-trt-100/single-small-trt.json)、[Medium 单实例](reports/vulkan-trt-100/single-medium-trt.json)。主矩阵包含硬件、模型/引擎/依赖/测试脚本哈希、逐图文字/坐标/GT、500 次延迟和资源采样。不会将 TRT DLL/引擎打入 Vulkan 客户部署包。

在仓库目录执行（Windows、本机驱动与上述本地对照目录齐全，输出应使用新目录）：

```powershell
python tests/test_benchmark_three_projects.py
python tests/test_gpu_activity.py
python tests/benchmark_vulkan_trt.py --output build/trt-comparison/new-full --repeats 5
foreach ($variant in @('tiny','small','medium')) {
    python tests/benchmark_vulkan_trt.py --worker --backend trt --variant $variant `
        --predictors 1 --batch 4 --repeats 5 `
        --output "build/trt-comparison/new-single/$variant-trt.json"
}
```

完整跑完可能随温度/频率/驱动变化，不应期待每个数字完全复现。缓存 TensorRT 引擎与 GPU/软件版本相关；跨机器使用应重新构建并记录配置。本轮没有重新进行 CI、纯 FP32 TensorRT 对照或其他显卡测试。
