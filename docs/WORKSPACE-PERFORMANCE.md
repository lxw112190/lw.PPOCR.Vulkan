# DET 960 工作区修复与 DML 性能目标

2026-10-01，开发快照 `0.5.0-dev.2`。这两项独立验收：

1. 默认 DET 长边 960，Tiny/Small/Medium 正常使用，不能靠缩小图片绕过错误。
2. 同机、同 GPU、同模型、同预处理/输入/输出口径，性能超过 DML；目前未达成。

## 工作区不是显存总量

旧版 256 MiB 默认值对 Medium 960 的部分形状不够，且旧分配器不能拆分/合并空闲块，
缓存计划各自占用 arena 和 IO。不是显卡显存不足，也不是图片内容不受支持。

现在生命周期分配器拆分、合并空闲区间；每图最多 32 个 LRU 计划共享一套 arena、输入和概率/CTC 读回缓冲区。
增长时先释放缓存命令引用，再替换缓冲区，懒惰回绑。预算内按需增长；高水位合计超预算时调整容量，不无界累加。
默认上限改为 **512 MiB/图**，完整 OCR 有 DET/REC/可选 CLS 最多三图；模型权重、CPU 图像和驱动额外内存另计。
0 表示默认，并不在初始化时分配三份 512 MiB。WinForms 新增 MiB 输入框，HTTP 启动输出有效字节上限。

在 RTX 4060 上，三模型 DET 的 736×960、960×736、960×960 均通过。
你提供的 2448×3264 本地图片，Medium 的 DET 长边保持 960，连续五次得到 85 个区域；图片及识别正文未加入仓库。
另测 REC 40 种宽度超过 LRU 容量、大小交替、概率/CTC 交替、输出保护区、低预算拒绝后恢复。
详见 [工作区回归](reports/workspace-dev2/workspace.json)。这是功能/资源边界测试，不是 VRAM 测量或无泄漏证明。

## 本机完整 OCR 改善

Windows 10 x64，Ryzen 7 7735H，RTX 4060 Laptop GPU，driver 596.36。
同一随包 500×500 图片，预解码连续 BGR，CLS 开启，DET 长边上限 960，REC 自适应宽度。
旧版预热三次/测量十次；新计算内核预热三次/测量二十次；宿主端到端墙钟，不含 HTTP/文件解码。

| 模型 | 旧版中位数 | 共享工作区后 | 分块/融合后 |
| --- | ---: | ---: | ---: |
| Tiny | 62.78 ms | 61.83 ms | 59.04 ms |
| Small | 165.44 ms | 98.88 ms | 88.12 ms |
| Medium | 427.65 ms | 211.51 ms | 185.15 ms |

这是单机开发数据，不是全平台倍数保证。首轮新内核原始报告仍记录当时的 dev.1 版本号及 DLL 哈希，
dev.2 正式开发快照只追加修订元数据/说明；必须按报告内 DLL 哈希区分二进制，不能混用旧包。
[Tiny](reports/workspace-dev2/perf-tiny.json)、[Small](reports/workspace-dev2/perf-small.json)、[Medium](reports/workspace-dev2/perf-medium.json)。

当前计算改动：常驻预排布权重；单消费者 Conv→通道 Mul→Add 的权重/偏置折叠与 ReLU 融合；
M32/N64/K32 的共享内存+4×4 寄存器分块；通用卷积 K 顺序改为 kernel-position/channel，连续读 NHWC。
共享常量复制而非错误原地修改；分支/图输出阻止不安全融合。仍为 FP32，没有协作矩阵或 Tensor Core 快路径。

## DML 对照：目标尚未达到

测试工具只在显式配置本地 ORT DirectML SDK 时构建，**不安装、不打包，不成为 Vulkan 运行依赖**。
ORT 1.24.4，Microsoft.AI.DirectML 1.15.4；匹配 DXGI/Vulkan 设备名称和 vendor/device ID，不能假定编号相同。
两者均使用相同 FP32 张量，输出完整概率到 CPU，预分配输出缓冲区；预热三次、交替测量二十次，记录中位数/P95和全部样本。
这是图级测试，排除图片/DB/裁剪/CTC，不能作为完整 OCR 的最终验收。DML 可以将 shape 等节点放在 CPU；
FP32 输入输出也不保证 DML 内部实现的数学指令与我们的 FP32 shader 完全相同。

| Medium 图 | Vulkan 中位数 | DML 中位数 |
| --- | ---: | ---: |
| DET 512×512 | 26.31 ms | 16.03 ms |
| DET 960×736 | 73.78 ms | 35.94 ms |
| CLS 80×160 | 0.50 ms | 1.56 ms |
| REC 48×320 | 19.98 ms | 6.54 ms |
| REC 48×960 | 56.35 ms | 11.87 ms |

[完整三模型 DML 对照及二进制哈希](reports/workspace-dev2/dml-graphs.json)。
CLS 较快不代表整套 OCR 已超过 DML，REC 仍是关键差距。

```powershell
cmake -S . -B build -DLWVK_DML_REFERENCE_ROOT=C:/path/to/microsoft.ml.onnxruntime.directml/1.24.4 -DLWVK_DIRECTML_REFERENCE_ROOT=C:/path/to/microsoft.ai.directml/1.15.4
cmake --build build --config Release --target lwvk_dml_reference
python tests/benchmark_dml_graphs.py --library build/Release/lw.PPOCR.Vulkan.dll --dml-library build/Release/lwvk_dml_reference.dll --device 1 --iterations 20 --report build/reports/dml.json
python tests/test_workspace.py --library build/Release/lw.PPOCR.Vulkan.dll --device 1 --report build/reports/workspace.json
```

`--image` 可追加本地私有图片，报告不保留 OCR 正文。设备编号请先枚举。

## 完整 OCR 的强化 DML 基准

dev.2 新增 `tests/benchmark_dml_ocr.py`；两端编译同一份 `ocr_host.cpp`，
共用 DET/CLS/REC 预处理、DB、裁剪、排序、资源限制及 JSON 组装。
Vulkan 保留 GPU CTC，DML 使用原 lw.PPOCR.Inference 的原生 `std::max_element` 贪心解码方式，
校验胜出概率；没有用逐概率的额外检查刻意放慢 DML。

按 [ORT 官方性能建议](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html#performance-tuning)，
用 `AddFreeDimensionOverrideByName` 固定批次/尺寸；DML 各图最多 32 个 LRU 形状会话。
缓存的 DML 会话会复制部分权重/资源，不能认为两端 VRAM 预算相同，也未测 VRAM。
这比只用一个动态 DML 会话的对照更强；不能挑较慢的动态基准宣布胜出。

同 RTX 4060、相同模型/字典、相同预解码 BGR，CLS 开，DET 长边限制 960，
预热 3 次、交替测量 20 次。500 原图检测尺寸 512×512（32 对齐），
1000 图片由随包示例双线性放大，检测为 960×960。包含完整原生 OCR 和 C ABI 结果复制，
不含图片文件解码、HTTP、Python JSON 解析；DML 内部数学精度未建立同一性。

| 模型/输入 | Vulkan median | DML median | Vulkan P95 | DML P95 |
| --- | ---: | ---: | ---: | ---: |
| Tiny 500×500 | 59.66 ms | 63.16 ms | 63.58 ms | 66.89 ms |
| Tiny 1000×1000 | 113.98 ms | 99.07 ms | 127.86 ms | 109.73 ms |
| Small 500×500 | 89.14 ms | 99.80 ms | 99.20 ms | 105.15 ms |
| Small 1000×1000 | 156.34 ms | 136.38 ms | 173.62 ms | 155.62 ms |
| Medium 500×500 | 187.74 ms | 153.01 ms | 189.99 ms | 161.11 ms |
| Medium 1000×1000 | 288.86 ms | 213.82 ms | 293.05 ms | 221.79 ms |

六组均得到 16 个区域，文字与 CLS 标签一致，最大坐标差为 0，最大分数差不超过 1.32e-6。
原始样本、DML 推理/CPU CTC 分解、双方 DLL/模型/宿主源码 SHA-256 见
[完整强化对照](reports/workspace-dev2/dml-full-cached.json)。报告不保存识别文字。
结论：Tiny/Small 的小图有优势，但大图和 Medium 未胜出，**优化目标未完成**。
工程仍是 FP32 分块计算，不等于已经发挥 4060 的 Tensor Core 性能。

```powershell
python tests/benchmark_dml_ocr.py --library build/Release/lw.PPOCR.Vulkan.dll --dml-library build/Release/lwvk_dml_reference.dll --device 1 --dml-shape-cache --iterations 20 --report build/reports/dml-full.json
```

不带 `--dml-shape-cache` 可以分析普通动态形状会话，但必须披露该设置。
该测试适配器不安装到部署包，正式运行不依赖 ORT/DirectML。

## 本地部署包回归

该 dev.2 部署包的原生 DLL SHA-256 为 `b2f2c49a8353acd333489e21b74642829bdd58cee59d9c3d3714c3f7832052c8`。
7 项 CTest、172 项资产校验、独立 MSBuild（复制齐九个 ONNX 模型）通过。
AMD/4060 WinForms 的整图、ROI、手填/选择设备及坐标映射通过；AMD 的三模型识别与完整 OCR quick 对拍通过。
这不是其他驱动/显卡的保证，也没有在本轮重新跑 sanitizer 或 Vulkan validation layers。

从实际安装目录运行 HTTP 默认官方 Tiny ONNX：Base64/二进制、批量、认证、异常尺寸、
日志隐私、等待超时 503、队列有界及释放后恢复通过。429 传输拒绝尚未解析请求，
关闭时未读 TCP 数据可能导致连接重置；测试读“accept 后即时拒绝”以稳定验证既有 best-effort 行为，
不把连接中断当 OCR 成功。

预热额外 200 次 OCR 后，再运行 1000 次 OCR + 41 次仅识别，交替 250×250、500×500、
1000×1000、900×450、450×900；结果一致，正常停止。RSS 从 271,585,280 到 273,313,792 字节，
净增约 1.65 MiB，期间不是持续单调增长；线程/句柄未无界增加。
这是单次本机 RSS 趋势证据，**不是无内存/显存泄漏证明**。
[长跑报告](reports/workspace-dev2/stress-1000.json)、[HTTP](reports/workspace-dev2/http.json)、
[AMD 界面](reports/workspace-dev2/winforms-device0.json)、[4060 界面](reports/workspace-dev2/winforms-device1.json)。

通用 OpenCV 服务预检提示缺少 OpenCV 版本约束：本项目没有 OpenCV 依赖，因此不适用；
预检提示缺少 SBOM 属实，仍是后续正式发布门槛。本包为本地开发预览，不宣称正式发布/安全审计完成。

## 后续优化与验收

已完成[默认关闭的 GPU 算子计时与第二轮 FP32 优化](GPU-PROFILING.md)。
最新 Medium 大图 median 为 Vulkan 241.03 ms / DML 215.63 ms，仍未胜出；
不能把诊断时间或小图近似持平当作完成目标。已验证 dev.2 部署包未覆盖。

已新增[独立协作矩阵实验](COOPERATIVE-MATRIX-EXPERIMENT.md)，能力枚举与人工矩阵测试通过，
但真实 Medium 大图分数误差尚未过门槛。实验默认关闭，既有 dev.2 包保持不变，
不能将该实验视为已超过 DML。

1. 已建立共用原生宿主流程的端到端 DML 基准；继续扩充真实多行/透视图片并测 CPU/VRAM。
2. Vulkan timestamp 与 CPU 分阶段 profiling，分别定位卷积、矩阵、传输、计划和逐行等待。
3. 文字行按宽度分组，合并提交/等待，复用输入/结果，避免每行一次完整同步。
4. 引入带能力检测的 FP16/协作矩阵快路径，保留 FP32；不能只在 4060 编过就声称 AMD/Intel 均支持。
5. 同模型/同图片/同 DET 960，预热后比较端到端 median/P95、显存/内存和正确性。
   提速不得以丢文字、改字典、缩图或降低检测质量换取；误差、框、文字及长跑均须复测。

English: DET-960 memory correctness and outperforming DML are separate gates.
dev.2 implements split/coalesced lifetime allocation, shared bounded graph workspace,
safe affine/ReLU folding and coalesced FP32 tiled convolution/GEMM. Medium's local
full-OCR median improved, but DET/REC graph tests still trail DML. The goal remains
open: matched native end-to-end tests now include fixed-shape DML sessions and
show remaining large-image/Medium gaps. Next steps are submission batching and
capability-gated FP16/cooperative-matrix optimization with accuracy/stability gates.
