# FP32 HardSwish 融合与 TRT 差距复测（2026-10-07）

结论：保留有界的 `Conv → HardSwish` 融合，减少独立激活的读写、dispatch 和同步；**仍未超过现有 TensorRT 版**。这是 v1.0.1 发布后的开发快照，不覆盖正式附件，也不改变版本、模型、FP32、DET960 或冻结的 v1 接口。

## 保留的实现

- 深度卷积支持标量和 vec4 epilogue；普通卷积仅接受 group=1、1×1、输入通道为正且能被 4 整除、stride=1、零 padding。
- 只融合常量权重、单消费者、相邻的 HardSwish，且 FP32 alpha **严格等于 1/6**、beta **严格等于 1/2**。非标准参数、已有激活、共享/可观察的中间输出保留原路径。
- 原始点积、累加次序与点积后的 Bias 不变，不改权重；激活直接作用于原 FP32 结果。内部 flag=256，不新增 C ABI/客户配置字段。
- 全部 pointwise 调度家族均支持该 epilogue，包括 small-M、tile64、wide、词表尾部和默认关闭的短行研究 kernel。生成脚本若找不到插入位置会立即失败，避免漏执行激活。
- 参考 graph/kernel 模式不做新融合；带该激活的节点不调度未验收的协作矩阵实验。

普通构建默认采用这项融合。未改变 GPU 电源设置，也没有后台循环保温或降低检测尺寸。

## 500/1000 sample 交替 A/B

Windows 10 x64 / Ryzen 7 7735H / RTX 4060 Laptop GPU。预解码 BGR，各版预热 3 次；候选优先初始化，新旧交替、每版每尺寸 40 次。计入完整原生 OCR、结果复制与 Python JSON 解析，不含 GUI、文件解码或模型初始化。完整结果对象严格相等。

| 模型 | 图片 | 旧版中位数 ms | 新版中位数 ms | 耗时降低 |
| --- | --- | ---: | ---: | ---: |
| Tiny | 500×500 | 19.69 | 19.07 | 3.13% |
| Tiny | 1000×1000 | 31.06 | 30.22 | 2.71% |
| Small | 500×500 | 36.38 | 35.76 | 1.70% |
| Small | 1000×1000 | 53.68 | 53.01 | 1.25% |
| Medium | 500×500 | 87.41 | 87.41 | -0.01%（持平） |
| Medium | 1000×1000 | 123.47 | 122.65 | 0.66% |

CLS 阶段中位数减少约 0.40～0.52 ms（约 7.5%～9.4%）；Medium 的 REC 远比 CLS 重，因此整图收益有限。不能把阶段提速当成所有模型整图提速 9%。原始数据：[pair-final.json](reports/hardswish/pair-final.json)。

## 100 张变尺寸流

使用 lw.PPOCR.C 的公开生成集 `lw-generated-ocr`；每模型 3 轮，新旧交替，第二轮反向，共 1800 次 OCR。首轮含执行计划首次使用，后两轮为热身轮。每图的文字、坐标、分类与分数严格相等。

| 模型 | 首轮旧→新平均 ms | 热身轮 1 旧→新平均 ms | 热身轮 2 旧→新平均 ms |
| --- | ---: | ---: | ---: |
| Tiny | 22.22 → 21.41 | 19.48 → 19.15 | 19.34 → 18.85 |
| Small | 36.48 → 36.10 | 32.77 → 32.33 | 32.45 → 32.14 |
| Medium | 75.06 → 74.63 | 72.45 → 71.91 | 72.66 → 72.26 |

热身轮降低约 Tiny 1.67%～2.52%、Small 0.98%～1.34%、Medium 0.56%～0.75%。这是单台笔记本的小幅收益，功耗/时钟未锁定，不是其他设备的速度保证。原始数据：[stream.json](reports/hardswish/stream.json)。

## 本轮试过但撤回的方案

1. 将 Bias Add 融合从既有安全 pointwise 范围扩展到所有 group=1 卷积：严格输出回归通过，但固定 sample 的变化约 -0.3%～1.3%，未建立稳定整图收益，已撤回扩展，保留原门槛。[A/B](reports/hardswish/rejected-general-bias-pair.json)。
2. M16/N128/K32 wide REC tile：短 REC 局部更快，但 Medium 宽 320/960 分别慢约 5.21%/16.15%，变尺寸热身轮慢约 6.3%～6.5%。[REC](reports/hardswish/rejected-wide16-rec.json)、[整图流](reports/hardswish/rejected-wide16-stream.json)。
3. 256 线程、M32/N128/K32 wide tile：宽 320/960 慢约 2.76%/8.23%，变尺寸热身轮慢约 4.0%～5.6%。[REC](reports/hardswish/rejected-wide256-rec.json)、[整图流](reports/hardswish/rejected-wide256-stream.json)。

两种 wide 原型在 NVIDIA/AMD 探针中输出逐位一致，但正确不等于快。候选调度、生成代码、未使用的 shader 已清理，原宽 REC 内核不变；报告保留，避免只公布微基准收益。128/256 线程或更小 tile 不能直接推导出整图性能。

## 当前版本与现有 TensorRT 的复测

使用既有 `PP-OCRv5_Test_trt` DLL 和缓存计划，REC batch=4、predictors=4；三模型 ONNX/字典哈希相同。Vulkan FP32；TRT 使用已有 FP16-enabled plans，FP32 I/O，可能有 FP32 回退。两者 REC 宽度对齐、批处理/并行、CPU 图像处理流程不同，因此不是完全同数学路径的纯后端比较。完整边界见 [历史对比说明](VULKAN-TENSORRT-100.md)。

每组独立进程、串行执行；先预热 100 图，再计时 3 轮，共 300 次/组。250 ms 监控涵盖进程资源；未锁频/功耗，GPU 利用率是连续流观测，不是 shader occupancy。正确率仅为 614 行合成 GT 的评估，不代表真实业务场景。

| 模型 | 后端 | 平均 ms | P95 ms | 进程峰值工作集 MiB | 进程专用显存峰值 MiB | 整卡平均利用率 | 端到端 CER |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tiny | TRT | 17.32 | 24.73 | 600.6 | 539.5 | 24.7% | 4.97% |
| Tiny | Vulkan | 21.85 | 28.12 | 276.6 | 149.7 | 34.8% | 4.88% |
| Small | TRT | 28.17 | 38.41 | 670.2 | 1045.6 | 32.1% | 2.38% |
| Small | Vulkan | 34.91 | 46.86 | 327.1 | 263.2 | 39.5% | 2.28% |
| Medium | TRT | 37.54 | 50.76 | 707.5 | 1555.6 | 41.6% | 2.35% |
| Medium | Vulkan | 74.54 | 99.26 | 614.5 | 653.9 | 65.6% | 1.20% |

Vulkan 相对 TRT 耗时约为 1.26× / 1.24× / 1.99×，**尚未反超**；本机资源开销较低，不能据此外推其他机器。计数器可能漏短暂峰值，不同列不能相加当成总内存。模型与所有原始结果、采样、设备/脚本哈希见 [复测矩阵](reports/hardswish/trt-confirm.json)。

本轮较早的深度卷积融合原型也做了完整复测，Small TRT 出现很大长尾（平均 41.42、P95 89.49 ms）。当时观察到 GPU 84℃，但没有温度/频率因果证据，不能断言全部由温度造成。该轮 Vulkan Small 更快，**不拿这一轮异常长尾宣称反超**，也不把它与最终版拼表；[初轮原始矩阵](reports/hardswish/trt-first-noisy.json)完整保留。

## 正确性与部署验收

- 32 项 CTest、93 项优化器门槛、181 个固定资产和 5 份冻结 v1 合同通过；未改公共接口或模型。
- NVIDIA、AMD 各完成 27 个 DET/CLS/REC 输出逐位对照；完整结果字段严格相等，不等于真实业务准确率 100%。[NVIDIA](reports/hardswish/regression-final.json)、[AMD](reports/hardswish/regression-amd.json)。
- 两卡均在 validation/synchronization validation 下通过 96 组 pointwise HardSwish 逐位对比、36 个深度卷积 HardSwish/bias/奇数通道/stride/padding/tail 检查。标量与向量输出逐位相等，CPU 最大绝对误差分别约 4.77e-7、2.98e-8。
- 三模型独立 ORT CPU/NumPy/CTC 参考、资源上限/异常恢复/结果生命周期/并发检查通过。[Tiny](reports/hardswish/ort-tiny.json)、[Small](reports/hardswish/ort-small.json)、[Medium](reports/hardswish/ort-medium.json)。几何参考复用生产 C 几何库及其独立 host golden tests，不冒充完全独立几何实现。
- 独立 staging 的 C# WinForms 真 GPU、整图、鼠标框选、5 次调用、可填 GPU 与异工作目录通过；HTTP 二进制/Base64、认证、日志隐私、429/503、异常输入与恢复通过。[WinForms](reports/hardswish/winforms-device1.json)、[HTTP](reports/hardswish/http-smoke.json)。
- Linux 软件 Vulkan CI 已增加两类融合探针；本轮没有执行 Linux 构建或远程 CI，不能宣称它们已经通过。

### 单句柄长测：功能通过，但保留延迟异常

每模型同一引擎处理 5 张 sample 变形与 100 张生成图：首次/预热/连续/切换/1000 次混合图 soak/间隔调用，共 1400 次完整 OCR。全部结果哈希稳定，无错误或非有限计时。

| 模型 | soak 中位数 / P95 / 最大值 ms | 预热后 Private 变化 MiB | 线程 / 句柄变化 |
| --- | ---: | ---: | ---: |
| Tiny | 22.00 / 30.83 / 45.86 | +0.47 | -1 / -2 |
| Small | 33.78 / 44.62 / 68.50 | +1.55 | -4 / -5 |
| Medium（首轮） | 85.65 / 166.91 / 953.05 | +3.31 | -5 / -5 |

原始报告：[Tiny](reports/hardswish/soak-tiny.json)、[Small](reports/hardswish/soak-small.json)、[Medium 首轮](reports/hardswish/soak-medium.json)。进程内存包含 Python、105 张预解码图片与驱动；线程/句柄不增长及 RSS 平稳不是无泄漏证明。

**Medium 首轮不能表述为延迟稳定**：在尺寸切换与首轮混合图中观察到接近 1 秒长尾，DET/CLS/REC 均有增大。随后只读检查宿主机（约 15.24 GiB 可见 RAM）仅剩约 750 MiB 可用物理内存，后一次约 660 MiB；没有关闭其他用户程序。资源压力是疑点，不是已证实原因。两秒空闲后调用中位数仍约 150/176/328 ms，这项融合不解决空闲尾延迟。

随后旧/新各补跑相同的 Medium 单句柄 1400 次调用，其中各 1000 次 soak：

| 控制 | soak 中位数 / P95 / 最大值 ms | 大小图切换 P95 ms | Private 增量 MiB |
| --- | ---: | ---: | ---: |
| 旧版 | 81.94 / 109.44 / 172.48 | 131.15 | +1.59 |
| 新版 | 81.96 / 108.65 / 158.73 | 141.03 | +1.55 |

本轮共完成 **5000 次 soak、7000 次上述分场景完整 OCR**，功能/重复结果检查均通过。控制中 soak 延迟基本持平，未复现接近 1 秒尖峰；尺寸切换 P95 新版仍比旧版高约 10 ms。因此不宣称这项融合解决 Medium 尾延迟，也不将首轮异常删除。[旧版控制](reports/hardswish/soak-medium-control-before.json)、[新版控制](reports/hardswish/soak-medium-control-after.json)。

为后续定位，长测工具新增只读的宿主总内存/可用内存/占用比例 checkpoint，观察失败保留 null 和错误原因，不伪装成 0。不改变 OCR 错误门禁、不清理系统进程、不隐式限频。本页已有长测在新增观测字段前完成，不向历史报告补造该数据。低内存与尖峰的因果仍需干净环境和同步观测确认。

## 构建身份与复现

对照为上一轮默认 OFF 短 REC 的本地开发 DLL，不是 v1.0.1 原始 Release 附件：
`c717cab0465813e325cb9baedd66252a8096b5194699fdf7639041e3e8879232`。

本轮最终 DLL：
`6a1d2adcc38f50f35bfda50e0c05ba7e64980900ac0dc36ae6afe32756e56ff9`。

```powershell
python tests/test_pointwise_regression.py --before BEFORE.dll --after AFTER.dll --device 1 --report build/check.json
python tests/benchmark_vulkan_pair.py --before BEFORE.dll --after AFTER.dll --device 1 --iterations 40 --reverse-initialization --report build/pair.json
python tests/benchmark_ocr_stream.py --before BEFORE.dll --after AFTER.dll --device 1 --passes 3 --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/stream.json
python tests/benchmark_vulkan_trt.py --vk-library AFTER.dll --output build/new-trt-report --repeats 3
```

测试时不要同时编译、运行另一个 Demo 或推理进程；TRT 对照依赖本地其他项目的 DLL/计划，不随 Vulkan 部署包分发。下一步重点是测量真正批量 REC、权重复用与提交开销，再决定是否单独研究可选混合精度；不会为追求反超直接改变默认 FP32 或放宽正确性门槛。

English: The default development build fuses only canonical, single-consumer pointwise/depthwise HardSwish after unchanged FP32 accumulation and bias. Warmed changing-stream gains are modest (roughly 0.6–2.5%). Two wide-REC kernel variants regressed and were removed. Current three-model tests remain slower than existing FP16-enabled, four-predictor TRT plans; differing pipelines and uncontrolled laptop clocks prevent universal claims.
