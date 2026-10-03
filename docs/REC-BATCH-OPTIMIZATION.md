# REC 共享 arena、有界合并提交优化

本轮沿用上一版 CLS 小批量优化，继续减少文字识别（REC）的 GPU 提交/等待开销。模型、FP32 网络、DET 长边上限 960、阅读顺序、方向校正、CTC 解码和公共 C ABI 不变。

## 实现与启用

开启 `LWVK_GPU_TEXT_PREPROCESS=1` 时，最多 8 行 REC 命令一次提交、一次等待。每行保留独立原始 BGR 上传区和紧凑 CTC 输出，**共享一块中间张量 arena**；命令开头的显式屏障保护跨行 RAW/WAR/WAW 依赖，上一行读完后下一行才复用内存。计算仍逐行，不是 batch-N 模型，也不保证 GPU 并行执行八行。

这个内部 BGR-only 计划只录制所需路径，不分配 CPU 归一化输入上传或完整类别概率回读区；GPU 贪心标签/置信度回读后仍由 CPU 完成 CTC 去重、去空白和字典映射。单行 REC API 和默认 CPU 前处理路径保留原有执行方式。

本报告测量时通过环境变量同时启用 DET/CLS/REC GPU 前处理及两种合批，不新增开关；当时普通入口保持 CPU 前处理，不宣称本轮提速。当前 `Start-CSharp-Demo.bat` 已自动选择 GPU 路径，旧实验脚本已移除；局部路径对照见 [默认策略](GPU-DEFAULT.md)。GPU 前处理仍要求 `shaderFloat64`，网络保持 FP32。

资源边界：

- CPU 裁剪暂存仍最多 8 行、合计 16 MiB；大于该值的单行独立处理，仍受原单行/累计像素约束。关闭 CLS 时也可 REC 合批。
- REC 最多 8 份输入/CTC IO、一个共享 arena；slot/识别宽度的执行计划全局 LRU 最多 32 个，不能逐请求无限增长。形状/容量变更会先使引用旧缓冲区的命令失效，再释放和分配资源。
- 所有 REC 合批缓冲区与该图串行工作区的**合计请求字节**保持原 `max_workspace_bytes` 上限。默认每图 512 MiB，按需分配；不是为八行各给 512 MiB。
- 合批容量不足时回退逐行，后续小批次可恢复。原单行预算校验仍保留；单行也不满足预算时明确失败，不返回半份 OCR。
- 预算不包含权重、驱动内部资源/Vulkan 分配对齐、CPU 原图和进程其他内存，不代表整个进程或全部显存上限。取消了不使用的概率回读，并不等于所有图片的总内存一定下降。
- 同一句柄仍加锁串行。GPU 提交/等待失败后禁止复用相关计划，必须重建句柄；30 秒 fence 超时不是取消。未注入实际 GPU 挂起/设备丢失故障，不宣称该类故障已实测。

## RTX 4060 Laptop 性能

本机 Windows 10、设备编号 1；两边都开启 DET 和文字行 GPU 前处理。基准为 `cls-batch-opt1` DLL：

`6f08b959a9a7fc78561faa003e1b1a37a834aaf434e5063834f0b51381d1fff3`

本轮候选：

`a5e773d702492f909f7ec2dfd7dff2d99aeb3e4248c5c86936b0741f59c0b8c7`

以下为预解码 BGR → 原生 OCR → JSON 复制/解析的完整调用耗时，排除图片文件解码、初始化和 GUI。单一 GPU 测试任务运行；不是纯 GPU kernel 计时，也不是 CPU/DML 比较。

### 100 图逐图预热后的均值

每图预热一轮，两条路径交替顺序，每模型每条路径记录 200 个样本，完整 `items` 全字段逐项一致。

| 模型 | 上一版 ms | 本轮 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny | 20.985 | 20.297 | 3.3% |
| Small | 34.529 | 33.638 | 2.6% |
| Medium | 81.346 | 80.404 | 1.2% |

这是相对上一轮 CLS 合批的增量，不可将各轮百分比直接相加，也不是识别准确率的提高；本轮保持识别结果不变。

### 连续换图：不逐图预热

补充用同一 100 图按顺序连续调用，第一遍包含首次尺寸计划创建，第二遍反序；交替新旧调用顺序。初始化不计入。两遍全部预测字段一致。

| 模型 | 第一遍旧→新 ms | 降幅 | 第二遍旧→新 ms | 降幅 |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 25.578 → 23.999 | 6.2% | 24.447 → 23.000 | 5.9% |
| Small | 42.925 → 41.168 | 4.1% | 40.723 → 38.984 | 4.3% |
| Medium | 87.956 → 86.314 | 1.9% | 85.760 → 84.343 | 1.7% |

本次语料未发现连续换图导致整体退化。32 计划缓存仍可能被其他图片尺寸/文字行组合淘汰；不保证所有冷启动、所有流量都提速。

### sample.jpg：3 次预热、30 轮交替中位数

| 模型 / 图尺寸 | 上一版 ms | 本轮 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny / 500×500 | 24.668 | 23.348 | 5.3% |
| Tiny / 1000×1000 | 42.718 | 40.867 | 4.3% |
| Small / 500×500 | 43.962 | 42.242 | 3.9% |
| Small / 1000×1000 | 68.941 | 66.718 | 3.2% |
| Medium / 500×500 | 103.717 | 101.952 | 1.7% |
| Medium / 1000×1000 | 151.072 | 149.604 | 1.0% |

均为 16 个区域。500 图 REC 累计中位数 Tiny/Small/Medium 从 9.694/26.249/76.782 ms 降至 8.543/24.803/75.222 ms。该阶段包括批量上传、提交、网络、GPU 贪心 CTC、等待与回读；CPU 字典解码和计划准备仍计入总耗时，不把阶段计时调整宣传为网络本身加速。

## 验证与复现

原始数据见 [reports/rec-batch-optimization](reports/rec-batch-optimization)，报告绑定最终 DLL 哈希。内部 GPU 探针单独编译生产源文件，不安装或增加 ABI 导出。

- REC 探针：RTX 三模型与 AMD Tiny，1～8 行混合宽度、填充 stride、180° 校正、文本/分数严格一致；40 个宽度/最多 32 计划、非法输入清空和恢复、20 次并发批量调用、共享 arena 与紧预算回退/恢复。
- 显式启用 Khronos 同步验证（`VK_LAYER_VALIDATE_SYNC=1`，提交边界验证开启）。日志确认同步验证激活，上述探针无 VUID/SYNC-HAZARD 错误；不是 GPU-assisted shader 越界检测的声明。
- CPU-only 主机分块测试覆盖数量、16 MiB 边界、超大单行、CLS→REC 方向传递、关闭 CLS、错误结果数量和累计像素异常恢复，纳入 CTest。
- 三模型 100 图 + 10 组尺寸/旋转/空白变体、带填充 stride、40 个检测尺寸、上传预算拒绝与恢复。
- 三模型独立 ONNX Runtime CPU 图参考、关闭 CLS/旋转/输出长度/资源限制/并发恢复；三模型 REC-only API 的长度查询、小输出缓冲、24 组结果和每模型 20 次并发调用；RTX/AMD 各 27 组原始 FP32 输出逐位一致。
- CTest 13/13、176 个固定依赖/模型校验。默认 CPU 前处理路径完整预测一致，六组样例中位数变化约 -0.3%～+1.5%，不宣称这条路径提速。
- HTTP 二进制/Base64、认证、429/503、批量、日志隐私及异常恢复通过；完整 C# 暂存包在 RTX/AMD 上运行三模型整图/ROI，WinForms GPU 编号编辑、鼠标框选与重复调用通过，普通启动路径也通过。最终 ZIP 解压复验另存压缩包旁。

```powershell
$env:LWVK_GPU_TEXT_PREPROCESS="1"
.\build\rec-batch\Release\lwvk_rec_batch_probe.exe models/onnx/ppocrv6-tiny/rec.onnx 1
$env:LWVK_GPU_DET_PREPROCESS="1"
python tests/benchmark_ocr_stream.py --before build/cls-batch/Release/lw.PPOCR.Vulkan.dll --after build/rec-batch/Release/lw.PPOCR.Vulkan.dll --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/rec-batch/stream.json
```

设备编号以目标机器实际枚举为准。没有新增长时间 soak、故障注入或完整进程/显存测量，不拿以前包的长测代替本轮证据。

English: With GPU text preprocessing enabled, at most eight independent REC commands share one tensor arena and one submission/fence. Explicit cross-command barriers serialize arena reuse; each line has independent raw upload and compact CTC IO, with a global 32-plan LRU. BGR-only recording avoids unused normalized uploads and full-logit readback. It is not batch-N inference or guaranteed parallel execution. Existing workspace caps, sequential fallback, FP32, DET960 and prediction fields are retained. Paired warmed 100-image means improve by 3.3%/2.6%/1.2% for Tiny/Small/Medium over cls-batch-opt1; an un-warmed changing stream also improves in this local corpus. The normal CPU-preprocessing launcher has no claimed equivalent benefit. Results are not a universal performance guarantee or a memory-leak proof.
