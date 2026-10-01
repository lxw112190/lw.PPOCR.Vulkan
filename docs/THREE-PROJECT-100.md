# 100 张生成图片：C、原 DML、Vulkan 三项目比较

测试日期：2026-10-01。本报告比较 Tiny、Small、Medium 三组相同来源模型的实际完整 OCR 流程；不是 HTTP 吞吐测试，也不是相同张量/算子的微基准。

## 结论

- Tiny：C CPU 57.96 ms/张，原 DML 87.99，Vulkan 84.69；Vulkan/C 相对速度 0.684×。Vulkan 与原 DML 的平均耗时差距 3.75% 较小，应结合逐图和重复结果，不只看平均值。
- Small：Vulkan 81.22 ms/张，对原 DML 为 1.477×、对 C 为 2.440×；逐图三轮平均快于原 DML 100/100 张。
- Medium：Vulkan 122.10 ms/张，对原 DML 为 1.307×、对 C 为 6.816×；逐图三轮平均快于原 DML 90/100 张，不能从平均值推断所有输入都胜出。
- **Small 字符准确率 C=97.96%、Vulkan=97.72%**。Vulkan Small 有 1 条 C 已匹配、Vulkan 未匹配的 GT 行，共 51 字符（img-048.jpg）。两者总编辑数 C=411、Vulkan=459；不能用整行准确率或调整评分规则掩盖字符指标上的差异。
- 不同批量/会话/权重复用策略影响内存，RAM 和 GPU 专用/共享归属值必须分别比较，不能只归因于 GPU API。
- 3600 次完整 OCR 调用执行完毕；2700 次计时调用失败 0 次，后两轮文字/坐标变化 0 次。轮次结束内存见后文，不把三轮趋势作为无泄漏证明。
- 下一步根据逐图延迟和 GT 差异定位瓶颈；此次不改变推理代码，不据此宣称已达到所有输入全面超过 DML 的目标。

## 测试条件

- 本机：AMD Ryzen 7 7735H（8 核 16 线程），Windows 10 x64；GPU 为 NVIDIA GeForce RTX 4060 Laptop GPU。
- 本机可见物理内存约 15.24 GiB；NVIDIA 驱动 Windows 版本 `32.0.15.9636`。GPU/CPU 峰值占用来自本次进程测量，不采用显卡规格标称值。
- C 项目为 `lw.PPOCR.C` 的新编译 Release DLL，CPU 内部文字行 worker=8；保留项目默认 AVX2、Conv3x3 FMA、固定点缩放、自适应线程预算、持久文字行池和并行裁剪设置。
- DML 为 `lw.OnnxRuntime.PPOCRSharp_dml` 的新编译原始 Release DLL，不使用 Vulkan 项目的 DML 对拍器代替推理。ORT DirectML 1.23.0、DirectML 1.15.4、OpenCV 4.8.1；REC batch=8、predictor=4、CLS batch=1。
- Vulkan 使用已完成准确性验证的 FP32/GELU 优化 DLL，版本 0.5.0-dev.2；不启用协作矩阵、FP16、M64 实验、GPU profiler 或 validation layer。此次不是对实验性最新混合精度代码作发布验证。
- Vulkan device=1、DML DXGI device=1，控制器核验 vendor/device ID 一致。每组独立进程，逐组运行，不同时占用 CPU/GPU；轮换项目的测试先后顺序。
- 三套：DET 长边 960、阈值 0.3/0.6、unclip=1.5、dilation=false、启用 CLS、CLS 阈值 0.9，使用同一解码 BGR 输入与同一模型/字典 SHA-256。
- **仍有流程差异**：C/Vulkan REC 最大宽度 960；原 DML REC 按 320～1280 桶批量执行。缩放、裁剪/排序、预处理浮点与固定点路径、内部并行策略亦来自各自项目。因此结果代表现有项目的实际使用表现，不能全部归因于 CPU、DML、Vulkan 后端本身。
- 语料：`lw.PPOCR.C/build-local-data/lw-generated-ocr`，100 张生成 JPEG，614 条带文字和位置的标准答案；含 0°/180°、不同尺寸、中文/英文/数字/混合文本。没有使用另一组不带标准答案的样本作为正确率依据。
- 每组完整预热 100 张一次，再顺序执行 3 轮，每轮 100 张；共 9×400=3600 次 OCR 调用，计时样本应为每组 300 次。仅保留当前一张解码图，不预加载 100 张到内存。
- 计时包含完整 OCR 原生接口及转换到 Python 结果列表，排除 JPEG 解码、模型初始化、标准答案评分和 HTTP；三种返回格式的包装开销也包含在内。原 DML 的原生 console 日志未关闭，其成本在实际调用时间内。
- 无人工指定功耗/频率锁定，无法排除笔记本温度和调度影响；单机合成语料结果不构成所有 GPU、实拍图片或生产环境的性能/正确率保证。

语料 manifest SHA-256：`c51474cb3761515c8c9b07c0afb1846303aba1b2304ef9a87043c9d8c282159d`。

## 速度

单位 ms/张。平均值、P50、P95 均来自三轮成功调用；初始化单独记录，不混入推理平均值。

| 模型 | 项目 | 平均 | P50 | P95 | 初始化 ms | 成功 / 计时调用 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Tiny | C / CPU | 57.96 | 58.79 | 76.50 | 44.9 | 300 / 300 |
| Tiny | 原 DML / GPU | 87.99 | 86.42 | 120.12 | 2105.7 | 300 / 300 |
| Tiny | Vulkan / GPU | 84.69 | 82.72 | 123.05 | 866.8 | 300 / 300 |
| Small | C / CPU | 198.22 | 199.27 | 250.42 | 156.6 | 300 / 300 |
| Small | 原 DML / GPU | 120.00 | 118.50 | 144.59 | 1299.5 | 300 / 300 |
| Small | Vulkan / GPU | 81.22 | 83.41 | 106.36 | 958.4 | 300 / 300 |
| Medium | C / CPU | 832.21 | 842.44 | 1033.55 | 607.6 | 300 / 300 |
| Medium | 原 DML / GPU | 159.64 | 157.68 | 197.74 | 2572.4 | 300 / 300 |
| Medium | Vulkan / GPU | 122.10 | 126.20 | 158.37 | 2847.4 | 300 / 300 |

Vulkan 相对速度 = 对照项目平均耗时 / Vulkan 平均耗时；大于 1× 表示 Vulkan 更快。

| 模型 | 相对 C | 相对原 DML |
| --- | ---: | ---: |
| Tiny | 0.684× | 1.039× |
| Small | 2.440× | 1.477× |
| Medium | 6.816× | 1.307× |

### 逐图速度差异

每张图片取三轮平均再比较；以下统计不能替代总体平均，也不承诺所有图片都更快。

| 模型 | Vulkan 快于 C 的图片数 / 100 | Vulkan 快于原 DML 的图片数 / 100 |
| --- | ---: | ---: |
| Tiny | 0 | 59 |
| Small | 100 | 100 |
| Medium | 100 | 90 |

## 内存

单位 MiB（1 MiB=2²⁰ 字节）。RAM 工作集峰值由 Windows 进程生命周期峰值计数器取得；Private/GPU 每 250 ms 采样，可能漏掉短暂尖峰。峰值包括加载、预热和测量阶段，不只统计测量结束时。

RAM 包含统一 Python/NumPy/Pillow 测试壳、输入/输出、运行时及驱动；GPU 为 WDDM 对该 PID 的专用/共享内存归属值，跨该进程的适配器求和，并非显卡全局实际 VRAM。**这些列不能相加作为总物理占用**。CPU 无 GPU counter 实例显示“—”，不是测得 0。

| 模型 | 项目 | RAM 工作集峰值 | Private 峰值 | GPU 专用峰值 | GPU 共享峰值 |
| --- | --- | ---: | ---: | ---: | ---: |
| Tiny | C / CPU | 222.9 | 403.6 | — | — |
| Tiny | 原 DML / GPU | 545.4 | 1104.1 | 581.7 | 165.1 |
| Tiny | Vulkan / GPU | 280.3 | 411.2 | 133.7 | 69.9 |
| Small | C / CPU | 363.9 | 719.7 | — | — |
| Small | 原 DML / GPU | 714.7 | 1786.9 | 1109.2 | 229.8 |
| Small | Vulkan / GPU | 347.0 | 538.0 | 235.2 | 87.3 |
| Medium | C / CPU | 879.0 | 1601.0 | — | — |
| Medium | 原 DML / GPU | 817.9 | 2457.6 | 1703.8 | 233.2 |
| Medium | Vulkan / GPU | 618.7 | 929.1 | 625.7 | 167.0 |

### 预热后 RAM 变化

列出完整预热后及每轮结束时的 Private bytes，辅助判断缓存/输出留存的变化。不同尺寸计划、allocator/驱动缓存以及第一轮保留评分结果都会影响占用；三轮趋势和峰值不能证明没有泄漏。

| 模型 | 项目 | 预热结束 | 第 1 轮结束 | 第 2 轮结束 | 第 3 轮结束 |
| --- | --- | ---: | ---: | ---: | ---: |
| Tiny | C / CPU | 335.6 | 342.9 | 343.1 | 343.2 |
| Tiny | 原 DML / GPU | 1032.4 | 1084.2 | 1076.9 | 1078.5 |
| Tiny | Vulkan / GPU | 379.0 | 387.1 | 387.5 | 388.6 |
| Small | C / CPU | 594.1 | 594.7 | 597.4 | 596.1 |
| Small | 原 DML / GPU | 1654.9 | 1665.2 | 1668.4 | 1668.4 |
| Small | Vulkan / GPU | 502.0 | 511.1 | 512.1 | 512.8 |
| Medium | C / CPU | 1199.5 | 1217.6 | 1218.6 | 1218.8 |
| Medium | 原 DML / GPU | 2343.7 | 2353.5 | 2354.5 | 2355.2 |
| Medium | Vulkan / GPU | 905.8 | 914.1 | 913.2 | 913.4 |

## 正确率

只按第一轮 100 张独立图片评分，不能把三轮相同图片算成 300 张独立语料。按轴对齐位置 IoU≥0.30 贪心一对一匹配；NFC/CRLF 规范化，保留空格、大小写和标点。

- 整行准确率：文本完全相同的匹配行 / 全部 614 条 GT 行，漏检也记错误。
- 字符 CER：匹配行 Levenshtein 编辑数 + 漏检行全部字符（删除）+ 未匹配输出全部字符（插入），除以全部 GT 字符数。字符准确率 = max(0, 1−CER)，不是平均置信度。
- 检测召回：位置匹配行 / 614；严格的单行匹配可能对合并/拆分行计为漏检或多余行，三套使用同一规则。

| 模型 | 项目 | 完全正确行 / 614 | 整行准确率 | 字符准确率 | CER | 检测召回 | 输出行数 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tiny | C / CPU | 362 / 614 | 58.96% | 95.06% | 4.94% | 99.51% | 618 |
| Tiny | 原 DML / GPU | 339 / 614 | 55.21% | 95.06% | 4.94% | 99.84% | 622 |
| Tiny | Vulkan / GPU | 370 / 614 | 60.26% | 95.12% | 4.88% | 99.51% | 618 |
| Small | C / CPU | 483 / 614 | 78.66% | 97.96% | 2.04% | 99.67% | 612 |
| Small | 原 DML / GPU | 463 / 614 | 75.41% | 97.60% | 2.40% | 99.84% | 613 |
| Small | Vulkan / GPU | 486 / 614 | 79.15% | 97.72% | 2.28% | 99.51% | 611 |
| Medium | C / CPU | 495 / 614 | 80.62% | 98.76% | 1.24% | 100.00% | 614 |
| Medium | 原 DML / GPU | 472 / 614 | 76.87% | 97.67% | 2.33% | 100.00% | 614 |
| Medium | Vulkan / GPU | 497 / 614 | 80.94% | 98.80% | 1.20% | 100.00% | 614 |

### 方向分类相关分组

| 模型 | 项目 | 0° 完全正确 / GT | 180° 完全正确 / GT |
| --- | --- | ---: | ---: |
| Tiny | C / CPU | 252 / 436 | 110 / 178 |
| Tiny | 原 DML / GPU | 237 / 436 | 102 / 178 |
| Tiny | Vulkan / GPU | 258 / 436 | 112 / 178 |
| Small | C / CPU | 335 / 436 | 148 / 178 |
| Small | 原 DML / GPU | 320 / 436 | 143 / 178 |
| Small | Vulkan / GPU | 335 / 436 | 151 / 178 |
| Medium | C / CPU | 344 / 436 | 151 / 178 |
| Medium | 原 DML / GPU | 327 / 436 | 145 / 178 |
| Medium | Vulkan / GPU | 345 / 436 | 152 / 178 |

### 三项目输出内容比较

这里的一致率仅在两项目都匹配到 GT 的行上计算，**不是正确率**。A/B 独有正确行数则在全部 614 条 GT 上统计。

| 模型 | A / B | 两者匹配行 | 文字完全相同行 | A 独有正确 | B 独有正确 |
| --- | --- | ---: | ---: | ---: | ---: |
| Tiny | C / CPU / 原 DML / GPU | 611 | 426 | 44 | 21 |
| Tiny | C / CPU / Vulkan / GPU | 611 | 554 | 3 | 11 |
| Tiny | 原 DML / GPU / Vulkan / GPU | 611 | 420 | 17 | 48 |
| Small | C / CPU / 原 DML / GPU | 612 | 538 | 34 | 14 |
| Small | C / CPU / Vulkan / GPU | 611 | 594 | 2 | 5 |
| Small | 原 DML / GPU / Vulkan / GPU | 611 | 539 | 13 | 36 |
| Medium | C / CPU / 原 DML / GPU | 614 | 562 | 29 | 6 |
| Medium | C / CPU / Vulkan / GPU | 614 | 600 | 1 | 3 |
| Medium | 原 DML / GPU / Vulkan / GPU | 614 | 558 | 6 | 31 |

## 重复稳定性与范围

| 模型 | 项目 | 计时调用失败 | 后两轮文字/坐标变化 | GPU counter 错误 |
| --- | --- | ---: | ---: | --- |
| Tiny | C / CPU | 0 | 0 | 无 |
| Tiny | 原 DML / GPU | 0 | 0 | 无 |
| Tiny | Vulkan / GPU | 0 | 0 | 无 |
| Small | C / CPU | 0 | 0 | 无 |
| Small | 原 DML / GPU | 0 | 0 | 无 |
| Small | Vulkan / GPU | 0 | 0 | 无 |
| Medium | C / CPU | 0 | 0 | 无 |
| Medium | 原 DML / GPU | 0 | 0 | 无 |
| Medium | Vulkan / GPU | 0 | 0 | 无 |

坐标重复检查使用小数点后 3 位，文字要求相同；不把置信度的微小浮点差异算作文字错误。这是 3600 次交替尺寸完整 OCR 的观察，不是 ASan/UBSan、validation-layer 长测、无泄漏证明或跨平台支持验证。

## 原始证据与复现

- [matrix.json](reports/three-project-100/matrix.json)：九组逐图片结果、三次耗时、评分明细、配置、DLL/模型/语料/脚本 SHA-256、内存统计及轮次结束占用。发布到文档的副本仅删去完整 250 ms 内存采样序列，原文件保留在本机 build/reports。
- [content-comparison.json](reports/three-project-100/content-comparison.json)：每条 GT 的三套输出，可查找长行、180°、标点/空格和漏检差异。
- [build-provenance.json](reports/three-project-100/build-provenance.json)：C 项目的实际 CMake 开关和 Tiny/Small/Medium 模型转换工具 metadata，不把不支持的通用入口偷偷改成任意模型转换。
- 本机完整采样和原生日志：`build/reports/three-project-100/`；图片及测试日志不进入部署包。

```powershell
python -m pip install -r requirements-dev.txt
python tests/test_benchmark_three_projects.py
python tests/test_report_three_projects.py
python tests/benchmark_three_projects.py --output build/reports/three-project-100
python scripts/report_three_projects.py --input build/reports/three-project-100 --output docs/reports/three-project-100
```

复现前必须准备相邻 C/DML 源码、100 张带 metadata.json 的原语料、新编译 Release DLL、同哈希 ONNX/字典以及 C 项目工具转换出的 `.lwm`。脚本默认路径和配置见 `tests/benchmark_three_projects.py`，所有 DLL 路径、GPU 编号及内部线程参数可显式传入。C Small/Medium 必须使用项目内各自的稳定转换工具，不能绕过 Tiny 通用转换入口的模型限制。

C 默认构建：运行时共享库 Release，`LW_RUNTIME_ONLY=OFF`、`LW_BUILD_HTTP_DEMO=OFF`、`LW_BUILD_CSHARP_DEMOS=OFF`、`BUILD_TESTING=OFF`。DML 直接构建原始 vcxproj（Release/x64），运行时依赖隔离在新输出目录。Vulkan 使用 `build/gelu-optimized/Release` 已验证快照；具体 DLL 字节以报告 SHA-256 为准，不能把当前正在开发的协作矩阵实验代码自动当作同一已测产物。
