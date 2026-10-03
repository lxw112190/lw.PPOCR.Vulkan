# CLS 有界小批量提交优化

本轮保持三套官方来源模型、FP32 网络、DET 长边上限 960、方向分类与结果字段不变。优化的是 GPU 提交粒度，不是更换模型、降低精度或少识别文字。

## 实现与启用

之前每个文字区域分别上传 CLS 输入、提交命令并等待 fence。现在在 `LWVK_GPU_TEXT_PREPROCESS=1` 时，把最多 8 行的独立 CLS 命令放入一次 `vkQueueSubmit`，一次等待完成后读取各行概率；模型权重只保留一份。每行的数学运算和执行计划仍然独立，**不是网络 batch-N，也不保证 GPU 同时执行八行**。REC 暂时仍逐行执行。

本报告测量时通过环境变量同时开启 DET/CLS/REC GPU 前处理和 CLS 小批量；当时普通启动入口仍为 CPU 前处理、逐行 CLS，不宣称同样提速。当前 `Start-CSharp-Demo.bat` 已自动选择 GPU 前处理，旧实验脚本已移除；局部路径对照见 [默认策略](GPU-DEFAULT.md)。无需新增开关。GPU 前处理仍要求 `shaderFloat64`，网络始终 FP32。

内存和异常边界：

- 裁剪块最多 8 行、BGR 暂存合计最多 16 MiB；单行本身超过 16 MiB 时单独处理，仍受原有单行最大 800 万像素、累计最大 6400 万像素约束。不是整图所有裁剪同时保留。
- 最多缓存 8 个 CLS 槽位；所有槽位的 arena、上传及回读缓冲区与串行工作区**合计**受原 `max_workspace_bytes` 约束，不是八份独立预算。默认每个模型图 512 MiB，按需分配。
- 合批所需容量超预算时回退原串行路径；串行也装不下则明确拒绝，不绕过预算。后续小批次可以恢复批量。无请求在途时先销毁旧命令，再释放被引用的缓冲区。
- 上限计的是请求的工作缓冲区字节；权重、Vulkan 分配对齐/驱动内部资源、CPU 原图和进程其他内存不在此预算内，不能理解为整个进程或显存上限。合批用少量额外 CLS 缓冲区换取较少提交。
- 同一句柄仍加锁串行。提交/等待失败会使相关计划失效，必须重建句柄；30 秒 fence 等待超时**不是取消 GPU 工作**。本轮没有注入真实 GPU 挂起故障验证超时路径。

## RTX 4060 Laptop 实测

本机 Windows 10、设备编号 1。基准为上一版 `network-opt1`：

`39d4397307d8c1b19df22a6a312a1fd49e4ebd4d04cd1d445cf755f00d634929`

新 DLL：

`6f08b959a9a7fc78561faa003e1b1a37a834aaf434e5063834f0b51381d1fff3`

两边都开启 DET 和文字行 GPU 前处理；单一 GPU 测试进程运行，不把 C#/GUI、图片文件解码计入。时间包含预解码 BGR → 原生 OCR → JSON 复制/解析，不是纯 GPU kernel 时间。

### 100 张生成图

每图预热、交替新旧调用顺序，每条路径每模型记录 200 个样本；比较完整 `items`（文字、八个坐标、DET/REC/CLS 分数和类别）。

| 模型 | 旧均值 ms | 新均值 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny | 21.792 | 20.494 | 6.0% |
| Small | 36.569 | 34.256 | 6.3% |
| Medium | 83.675 | 81.648 | 2.4% |

三模型的全部 100 图结果逐项完全一致。这是相对上一版的一次增量改善，不是相对 CPU/DML 的比较，也不能把历次百分比直接相加。

### sample.jpg 预热后 30 轮交替中位数

| 模型 / 图片 | 旧完整调用 ms | 新完整调用 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny / 500×500 | 27.383 | 24.616 | 10.1% |
| Tiny / 1000×1000 | 46.863 | 42.441 | 9.4% |
| Small / 500×500 | 48.786 | 43.881 | 10.1% |
| Small / 1000×1000 | 75.597 | 69.134 | 8.5% |
| Medium / 500×500 | 107.474 | 102.319 | 4.8% |
| Medium / 1000×1000 | 156.264 | 149.120 | 4.6% |

样例均为 16 个区域。500 图 CLS 累计中位数分别从 7.196/8.875/9.603 ms 降至 5.424/5.623/5.695 ms。CLS 计时包含整批上传、提交、执行、等待与回读；计划创建仍记在总耗时里，首轮不能代替稳定吞吐。

普通 CPU 前处理路径同条件复测结果一致，六组中位数变化约 -1.1%～+0.1%（正值代表改善），不宣称提速。性能随图片文字行数、驱动、功耗与显卡变化。

## 可复现验证与报告

原始 JSON 和探针输出在 [reports/cls-batch-optimization](reports/cls-batch-optimization)：最终 DLL 哈希与报告绑定。功能测试不是长时间内存泄漏证明，也不是所有显卡兼容性承诺。

- `cls_batch_probe.cpp`：RTX 与 AMD 核显，1～8 行、填充 stride、分类概率逐位一致；非法输入清空结果、错误后恢复、20 次并发批量调用；933896 字节紧预算回退/恢复与串行高水位释放。设置 Khronos 验证层后探针未报告 VUID 错误。
- `ocr_batch_host_unit.cpp`：CPU-only，可在 CI 执行，覆盖 8 行分块、16 MiB 边界、超 16 MiB 单行、错误分类结果和累计像素拒绝/恢复。
- 三模型 100 图 + 10 组尺寸/旋转/空白变体、带填充步长、40 个检测尺寸计划、上传预算拒绝与恢复。
- REC-only 三模型各 24 组结果，长度查询/小输出缓冲、40 个宽度和每模型 20 次并发调用。
- 三模型独立 ONNX Runtime CPU 图参考（仅用于测试，未打包）、旋转/禁用分类/资源拒绝与恢复；RTX/AMD 各 27 组原始 FP32 概率输出逐位一致。
- CTest 13/13、176 个固定依赖/模型文件校验；HTTP 二进制/Base64、认证、429/503、日志隐私和恢复回归通过。
- 包内真实 C# 三模型整图/ROI、WinForms GPU 可编辑编号/鼠标框选/重复调用，以及普通 CPU 前处理启动路径通过。最终 ZIP 解压验证另存附件旁，不拿旧版本长测代替本轮测试。

```powershell
$env:LWVK_GPU_TEXT_PREPROCESS="1"
.\build\cls-batch\Release\lwvk_cls_batch_probe.exe models/onnx/ppocrv6-tiny/cls.onnx 1
$env:LWVK_GPU_DET_PREPROCESS="1"
python tests/benchmark_vulkan_pair.py --before build/vector-epilogue/Release/lw.PPOCR.Vulkan.dll --after build/cls-batch/Release/lw.PPOCR.Vulkan.dll --iterations 30 --report build/cls-batch/paired.json
```

设备编号须按目标机器枚举，不要照抄 1。CUDA、ONNX Runtime 和 DirectML 不进入本轮运行依赖；公共 C ABI 未增加导出。

English: With GPU text preprocessing enabled, up to eight independent CLS command buffers share one queue submission/fence, while retaining FP32 arithmetic and one resident weight buffer. Cropped BGR staging is bounded; combined CLS work buffers retain the existing per-graph cap, with sequential fallback and recovery under tight budgets. This is submission batching, not a batch-N model or guaranteed concurrent execution. REC remains sequential. Paired 100-image mean latency improves by 6.0%/6.3%/2.4% for Tiny/Small/Medium versus the previous network-opt1 build, with exact prediction-field equality. The normal CPU-preprocessing launcher remains unchanged in behavior and has no claimed speedup. Reports are not a leak proof, universal hardware guarantee or CPU/DML comparison.
