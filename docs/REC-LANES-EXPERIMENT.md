# REC 多路 / 多队列实验：本轮不接入默认

结论：在本机 RTX 4060 Laptop 上，增加到 2、4 路 REC 没有超过已验证的单路版本。三个模型的输出完全一致，但平均耗时、P95 和专用 GPU 内存均没有形成值得发布的改善。因此保留正常构建的原始串行实现，研究代码仅通过 `LWVK_EXPERIMENTAL_REC_LANES=ON` 编译启用；默认 OFF，现有部署包未替换。

这不是多个完整 OCR 引擎，也不是多个 HTTP 请求并行：仅在一张图片内部，将最多 8 行 REC 分配给同一设备、同一队列族的多个 Vulkan 队列。网络仍为 FP32；DET、CLS、DB、输出排序与接口不变。

## 方法与证据

- 时间：2026-10-02；Windows 10、Ryzen 7 7735H、RTX 4060 Laptop，驱动 596.36，Vulkan SDK 1.4.350.0。
- 使用 C 项目的原有 100 张生成图片、614 行 GT。清单 SHA-256：`c51474cb3761515c8c9b07c0afb1846303aba1b2304ef9a87043c9d8c282159d`。
- Tiny / Small / Medium，DET 长边 960、CLS 开启、默认 GPU 前处理与裁剪、FP32、REC 宽度 32..960 / 对齐 8，不改变阈值和模型。
- 四组：旧版已验证单路 DLL、实验版 1 路控制、实验版 2 路、实验版 4 路。每组使用独立进程；GPU 工作负载串行，不能同时跑另一个 OCR 基准。
- 第一轮每组预热整个数据集一次，再测 5 遍；第二轮反转四组顺序，预热一次，再测 3 遍。每遍交替正向/反向图片顺序，覆盖变宽计划缓存周转。
- 每个模型/模式有 800 个有效计时样本；共 12,000 次调用，其中 9,600 次计时。它们分散在 24 个进程，**不是单句柄 12,000 次长稳测试或无泄漏证明**。
- 耗时包括原生完整 OCR、JSON 返回与 Python 解析；不包括图片解码、模型初始化、GT 评分、HTTP 或 GUI。
- 所有预测对象逐字段比较：文字、四点坐标、检测/识别/分类分数、分类标签、结果顺序；不仅比较文字，也不对坐标取整。所有重复与两轮、四种模式之间完全一致，0 失败。
- [紧凑原始报告](reports/rec-lanes/measurement.json)保留预测参考、各图片的全部计时、预测摘要、模型/DLL 哈希、两轮分组、RAM/GPU 内存与利用率摘要，以及原始矩阵哈希。完整矩阵本地位于 `build/rec-lanes/full-forward/`、`build/rec-lanes/full-reverse/`。

## 合并两轮的完整 OCR 耗时

单位 ms；均值按全部 800 个样本计算，P95 从合并后的样本重新计算，并非平均两个 P95。

| 模型 | 旧版单路均值 / P95 | 实验 1 路均值 / P95 | 实验 2 路均值 / P95 | 实验 4 路均值 / P95 |
|---|---:|---:|---:|---:|
| Tiny | 23.20 / 29.77 | 24.40 / 31.52 | 23.91 / 30.69 | 24.24 / 31.13 |
| Small | 36.01 / 46.42 | 37.00 / 47.99 | 37.88 / 48.75 | 38.00 / 48.90 |
| Medium | 78.35 / 104.04 | 79.61 / 104.99 | 86.34 / 116.10 | 86.21 / 114.10 |

实验代码自身也有单路控制开销，不能只把 Tiny 2 路相对实验 1 路的约 2% 改善当作正式优化：与旧版相比，它仍慢约 3.1%。Small 2/4 路慢约 5.2%/5.5%；Medium 慢约 10.2%/10.0%。未锁定功耗、温度和频率，不能把所有微小差异归因于某一个函数；两种测试顺序都不支持替换旧版。

同样的 GT 评估下，字符准确率保持 Tiny 95.12%、Small 97.72%、Medium 98.80%。这些只是本生成数据集的指标，不是普遍准确率承诺。

## 内存

以下为两轮中观测到的最大峰值，单位 MiB。工作集是 OS 进程生存期峰值；Private、GPU 专用/共享内存以 250 ms 采样，可能漏掉短峰值。RAM 含 Python/NumPy/Pillow、输入、结果、运行时和驱动。WDDM GPU 内存是每 PID 的归属计数，不是整卡物理 VRAM 用量。

| 模型 / 模式 | 工作集 | Private | GPU 专用 | GPU 共享 |
|---|---:|---:|---:|---:|
| Tiny / 旧版单路 | 236.9 | 369.6 | 131.6 | 53.4 |
| Tiny / 实验 1 路 | 237.7 | 374.1 | 131.6 | 53.4 |
| Tiny / 2 路 | 243.1 | 385.1 | 139.5 | 57.5 |
| Tiny / 4 路 | 247.6 | 412.2 | 155.2 | 61.7 |
| Small / 旧版单路 | 283.7 | 482.0 | 227.2 | 59.4 |
| Small / 实验 1 路 | 294.6 | 473.7 | 227.2 | 59.4 |
| Small / 2 路 | 300.0 | 499.0 | 245.9 | 63.5 |
| Small / 4 路 | 313.1 | 546.4 | 283.3 | 67.7 |
| Medium / 旧版单路 | 611.1 | 871.9 | 615.9 | 157.9 |
| Medium / 实验 1 路 | 611.5 | 871.4 | 615.9 | 130.3 |
| Medium / 2 路 | 615.6 | 896.5 | 634.6 | 65.5 |
| Medium / 4 路 | 624.5 | 940.1 | 672.1 | 166.2 |

专用 GPU 内存随路数上升；共享内存由 WDDM/驱动和内存驻留情况影响，不能用 Medium 某组更低的共享峰值推导整体内存更省。报告还记录每 PID 最忙引擎利用率及 NVML 整卡利用率，不能将其当作 shader occupancy；GPU 百分比更高不等于延迟更低。两轮采样未记录 PDH/NVML 错误。

## 实现和安全边界

研究实现位于 `tests/experiments/rec_batch_lanes.cpp`，正常构建继续使用未改动的 `src/rec_batch.cpp`。

1. 共享一个 REC 图的只读权重、设备与 GPU 裁剪结果；每路拥有独立 arena，每个文字行拥有独立输入/CTC 输出、计划描述符、命令和 fence。
2. 各路内部仍逐行串行复用 arena。轮询分配文字行；多队列是否真正重叠执行由设备/驱动决定，不能承诺路数倍加速。
3. 队列 0 的先前权重上传、裁剪写入通过二进制 semaphore 释放；其他队列在 compute 阶段等待并获取可见性。**CPU 等待 fence 本身不能替代跨队列设备内存依赖**。
4. 主机调度保留引擎互斥锁，所有 lane fence 完成后才回读/回收/复用。没有删除队列和缓存保护，也没有跨请求访问同一 arena。
5. 各路 arena、IO、高水位保留容量都计入 REC 的同一个工作区预算；共享原图/裁剪预算继续扣除。默认每图 512 MiB，不是全部 OCR 或整个进程仅 512 MiB。预算不足先减少有效路数，仍不足则使用原有逐行回退，不扩张成 4 倍预算。
6. 非法 lane 值、计算队列数不足、GPU 文字前处理关闭或同时开启 GPU profiling 均拒绝相应实验配置。提交失败/等待超时会使计划 poisoned，禁止复用；30 秒等待超时不代表 GPU 任务取消。
7. 32 个 slot/width 计划的 LRU 上限不变；尾批次不无条件清空所有完整批次计划，只有绑定的 arena 改变才额外重录。

本机使用实际加载的 Khronos validation layer + synchronization validation 检查了 NVIDIA 三模型 × 1/2/4 路的原生 REC 和完整 GPU 裁剪 OCR，以及 AMD Tiny × 2/4 路原生 REC。覆盖 1..8 行、方向旋转、带 padding 的 stride、非法输入恢复、40 个宽度、32 计划上限、同句柄并发调用、累计预算降路、单行回退和恢复，未出现 VUID 或同步 hazard。AMD 全流程三模型、其他 GPU/驱动和 HTTP 多请求吞吐不在此结论中。

## 复现（PowerShell）

不要改默认 CI 或覆盖现有部署包。新建独立构建目录：

```powershell
$env:VULKAN_SDK = 'E:/VulkanSDK/1.4.350.0'
cmake -S . -B build/rec-lanes-optin -G 'Visual Studio 17 2022' -A x64 -DLWVK_EXPERIMENTAL_REC_LANES=ON
cmake --build build/rec-lanes-optin --config Release --parallel 4
ctest --test-dir build/rec-lanes-optin -C Release --output-on-failure

python tests/test_rec_lanes.py --report build/rec-lanes-optin/functional/nvidia.json
python tests/test_rec_lanes.py --device 0 --models tiny --lanes 2 4 --report build/rec-lanes-optin/functional/amd.json

python tests/benchmark_rec_lanes.py --output build/rec-lanes-optin/forward --repeats 5
python tests/benchmark_rec_lanes.py --output build/rec-lanes-optin/reverse --repeats 3 --reverse
python scripts/report_rec_lanes.py --runs build/rec-lanes-optin/forward/matrix.json build/rec-lanes-optin/reverse/matrix.json --output build/rec-lanes-optin/measurement.json
```

GPU 编号须按本机枚举结果调整。资源基准目前要求 Windows WDDM 与 RTX 4060；上述 AMD 测试仅使用原生探针。数据集路径可通过 `--dataset` 指定，旧版 DLL 通过 `--before` 指定。计时脚本清除 profiling/validation 环境标记，每种模式单独设置 lane 数并启动新进程，避免静态 CRT 环境快照问题。各测试必须串行运行，输出目录需全新。

手动使用实验 DLL 时，在启动新进程、加载 DLL 前设置 `$env:LWVK_REC_LANES='2'`（允许 1/2/4，未设置为 1）。**普通构建没有这个实验能力，仅设置环境变量不会启用多路**；原 ABI、版本号与现有包保持不变。

测时原型 DLL SHA-256：`74c467cc8a3816b4f65a04b648bc6e1cc6c2a93ff564e60712652e6bbd7e0491`；旧版控制：`4e95bc3c32aa449480761f615097912a8a223429f7115a42fa3b2867474b2c06`。隔离为 OFF-by-default 后重新编译的实验/正常构建是另外两个二进制，重新做功能验证；不能将原型性能样本冒充这两个文件的独立测时结果，见同目录 qualification 报告。

最终构建补充验证：

- 实验版 `9ecfb42c7749520f09754acf21e248c9d60c7b239e88acc35176a45895198486`：[NVIDIA 18 项](reports/rec-lanes/qualification-optin-nvidia.json)、[AMD 2 项](reports/rec-lanes/qualification-optin-amd.json)实际验证层检查全部通过。
- 默认版 `a69106876a7614b392f1e44ce0209b47bc0cb3965b0b0be633305dcfb76538a9`：[100 图三模型正反两遍旧/新逐字段相等检查](reports/rec-lanes/default-equality.json)通过。第二遍 Tiny / Small / Medium 平均耗时变化约 -0.41% / +0.09% / +0.07%，视为本机基本持平，**不作为新的加速宣传**。首遍含计划首次使用与分配顺序影响，原始值也保留。
- 两个构建的 14 项 CTest host/ABI 测试均通过。未改变公共 C ABI、HTTP API、模型或版本号，也未宣称远程 CI 或 Linux 多队列已验证。

## 下一步

这次测量说明“复制执行通道”不是当前机器上的有效捷径。更值得单独实验的是 REC 相近宽度的真正 batch-N 执行、降低每层调度/访存开销，以及有精度回归门禁的混合精度内核；都需要重新量化收益与维护成本。多个完整 OCR 实例属于 HTTP 多请求吞吐实验，不能用本次单图测试证明其好坏。
