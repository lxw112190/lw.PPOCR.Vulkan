# 大小图切换的 REC 缓存优化 / REC cache size switching

日期：2026-10-03。本优化纳入 v1.0.1，不修改 C ABI、HTTP、配置、日志或模型。
下面的性能数据与 DLL 哈希来自版本号更新前的优化构建，不等同于最终 v1.0.1 CI 附件验收。
不能覆盖已发布 v1.0 标签/附件；新版本发布前仍须复测最终 CI 包。

## 原因与改动

原默认 REC 批次按 `(slot, width)` 缓存，所有槽位共享 32 个条目的 LRU。
客户的小图有 16 个区域，大图有 85 个区域，按返回框计算大图有约 76 种缓存键。
大图不断淘汰小图计划，切回小图要重建张量布局、描述符、命令池并重新录制。
这部分主要进入 `total_ms - DET - CLS - REC`，不是 C# 时间显示错误。

- 默认串行 REC 路径改为 **8 个槽位独立 LRU，每个最多 16 个宽度，合计最多 128 条**。
- 某槽位的 upload/CTC 缓冲区更换，只使该槽位计划失效；共享 arena 更换时仍全部失效。
- 预算收紧导致槽位缩减时，先使被删除槽位的命令失效再释放缓冲区，不能留下旧引用。
- GPU 原图/crop 替换、超时后的 poisoned 状态、工作区预算和低预算单行回退保持原规则。
- 不增加网络 arena 副本，不调整 REC 宽度、DET960、模型、FP32 精度、文本或坐标排序。
- 非默认多 REC lane 研究路径未改，仍使用原 32 条 LRU。

128 是元数据/命令条数上限，不是 128 份张量工作区。命令、描述符及 CPU 元数据仍有实际
内存代价，且不包含在 `max_workspace_bytes` 的张量工作区预算中；不是“免费缓存”。
工作集超出每槽位 16 个宽度时仍会淘汰，初次/新尺寸调用也仍需准备执行计划。

## 本机对比

Windows 10 x64；NVIDIA GeForce RTX 4060 Laptop GPU、AMD Radeon(TM) Graphics。
两张客户原图预解码为 BGR：小图 500×500，大图 2448×3264。
前后 DLL 在独立 Python 进程运行，同一句柄，不含初始化、文件解码和 GUI 时间。
每次先预热、连续小图基线，再 30 轮大/小交替，最后连续小图；使用默认 GPU 路径、
FP32、DET960、CLS 开启。下表为交替阶段原生流水线 `total_ms` 中位数（ms）。

| GPU / 模型 | 小图：前 → 后 | 大图：前 → 后 |
| --- | ---: | ---: |
| RTX 4060 / Tiny | 32.21 → 20.05 | 151.47 → 86.44 |
| RTX 4060 / Small | 60.91 → 38.69 | 269.17 → 158.80 |
| RTX 4060 / Medium | 115.33 → 91.14 | 464.08 → 351.14 |
| AMD 集显 / Tiny | 71.76 → 57.63 | 283.10 → 208.13 |

前后所有调用的 items（文字、四点坐标、置信度、分类结果、排序）逐字节规范化 JSON
哈希一致；同一 DLL 混合尺寸重复调用也一致。小图预热后的基线未出现明显退化。
这是本机两图工作集结果，不承诺其他驱动、图像或 GPU 有相同收益。

### 内存取舍与短循环

30 轮测试中，结束时进程 RSS（MiB）如下；峰值和各轮样本见 JSON。

| GPU / 模型 | 修改前 RSS | 修改后 RSS | 增加 |
| --- | ---: | ---: | ---: |
| RTX 4060 / Tiny | 235.04 | 266.11 | 31.07 |
| RTX 4060 / Small | 288.95 | 333.05 | 44.10 |
| RTX 4060 / Medium | 490.58 | 535.76 | 45.18 |
| AMD 集显 / Tiny | 386.56 | 507.01 | 120.45 |

驱动命令缓存分配策略不同，集显此次 RSS 增量更大，不能套用独显的内存数据。
Tiny / RTX 4060 另外完成 100 轮（200 次交替调用），前后输出一致：小图 32.16 →
20.13 ms、大图 151.20 → 87.15 ms；修改后交替首轮至末轮 RSS 266.54 → 264.93 MiB。
未见该短循环持续增长，但 RSS 不等同于 VRAM，也不证明无泄漏；Linux sanitizer、
更多随机尺寸、目标机和最终包验收仍需 CI/维护者完成。

原始报告（仅保存 OCR 内容哈希，不保存客户 OCR 正文）：

- [Tiny / NVIDIA](reports/rec-cache-size-switch/tiny-nvidia.json)
- [Small / NVIDIA](reports/rec-cache-size-switch/small-nvidia.json)
- [Medium / NVIDIA](reports/rec-cache-size-switch/medium-nvidia.json)
- [Tiny / AMD](reports/rec-cache-size-switch/tiny-amd.json)
- [Tiny / NVIDIA 100 轮](reports/rec-cache-size-switch/tiny-nvidia-soak.json)

源基线 commit：`e0904d14f7798a048caf03087a6c51fb88daf9da`；
前 DLL SHA-256：`b3c4feb210edb68815c2a7dabd960de73efa7b6bccaa4179a6bcfff34911db55`；
后 DLL SHA-256：`1f1e1bcdb4e7de3b161bf5cf20617f97d0658ddf0d893b375af1f158e9fa8699`。
这是本地构建的证据，不能自动当作后续 CI 不同哈希部署包的验收记录。

## 自动回归与复测

`rec-cache-policy-host-unit` 无 GPU 即可检查槽位公平性、104 条热工作集、128 条硬上限及
IO/arena/删除槽位的失效规则；已纳入 CTest，日常 CI 与 sanitizer CTest 自动执行。
`lwvk_rec_batch_probe` 的默认路径增加真实 104/128 条计划、返回旧宽度复用、逐行输出一致
与淘汰测试，原有旋转、stride、异常恢复、20 次并发批次和低预算回退仍保留。
这是实体 GPU 工程探针，手动运行，不增加日常 CI 的软件 GPU 负担。

本次已完成 Windows Release 构建、30 项 CTest、NVIDIA 三模型与 AMD Tiny 的原生 REC
探针（包括 104/128 条真实计划）、安装目录 19 个导出/ABI 检查、Tiny 扩展 ONNX Runtime
CPU 参考对拍，以及 C# WinForms 整图/ROI 五轮与 HTTP 认证/异常/429/503 恢复测试。
上述应用测试运行于本地 staging 目录，并非未来 CI 的归档验收。Linux/ASan/UBSan
结果以推送后的 CI 为准，本机 Windows 不能代替该验证。

```powershell
$env:LWVK_GPU_TEXT_PREPROCESS="1"
.\build\local\Release\lwvk_rec_batch_probe.exe models/onnx/ppocrv6-tiny/rec.onnx 1
Remove-Item Env:LWVK_GPU_TEXT_PREPROCESS

python tests/benchmark_ocr_size_switch.py --before before.dll --after after.dll `
  --small-image small.jpg --large-image large.jpg `
  --model-root models/onnx/ppocrv6-tiny --device 1 --rounds 100 `
  --report build/reports/size-switch.json
```

按实际枚举编号选择 GPU，关闭诊断 profiling 后再比较耗时。首次使用未缓存尺寸仍可能慢；
不要通过隐式多跑一次、隐藏首轮或改计时口径来消除显示抖动。

English: default serial REC now uses independent 16-width LRUs for eight slots,
with selective IO invalidation and a shared arena. Exact OCR output is preserved
on this local two-image workload. Additional bounded command memory is a tradeoff,
especially on the AMD integrated driver. Finite caches can still evict shapes;
cold starts and CI/archive-specific qualification remain separate concerns.
