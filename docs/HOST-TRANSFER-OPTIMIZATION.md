# 第四轮优化：回读内存、持久映射与 DET 预处理

测试日期：2026-10-01。候选仍为 `0.5.0-dev.2`，不是新正式发布包。

## 本轮结果

在原来的 100 张带标准答案图片上，默认 FP32 完整 OCR 平均耗时进一步下降；三模型的 **100/100 张完整输出对象与优化前完全一致**，包括文字、坐标、DET/REC/CLS 分数和标签。不缩小 DET 960、不更换模型、不调整阈值，也不启用 FP16 或协作矩阵。

| 模型 | 优化前平均 ms | 本轮平均 ms | 平均耗时降低 | 本轮 P50 ms | 本轮 P95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tiny | 84.69 | 51.02 | 39.76% | 49.81 | 72.50 |
| Small | 81.22 | 58.55 | 27.91% | 58.47 | 76.74 |
| Medium | 122.10 | 106.27 | 12.97% | 106.96 | 139.17 |

这次默认路径在 6 个强化 DML 对照场景中均取得更低的完整 OCR 中位耗时；但 Medium 大图仅领先约 3%，不表示所有图片、所有显卡都已超过 DML。

## 为什么先优化数据传输

此前算子 profiler 显示，Tiny DET-960 的 dispatch GPU 时间合计约 7.55 ms，Small 约 15.9 ms，但完整图调用明显更慢。诊断计时不是端到端性能结论，它提示需要检查上传、回读和主机处理，而不只是继续改卷积 shader。

原来上传和回读都选择第一个兼容的 host-visible/coherent 类型。本机 RTX 4060 的类型 2 为 `HOST_VISIBLE | HOST_COHERENT`，类型 3 额外具有 `HOST_CACHED`。两者都能让 CPU 访问，却不具有相同的 CPU 读取性能。`HOST_COHERENT` 与 `HOST_CACHED` 是不同属性，不能把“无需手动 cache 同步”理解为“CPU 读取已经缓存”。参见 [Vulkan 内存属性](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryPropertyFlagBits.html)和 [Vulkan 内存章节](https://docs.vulkan.org/spec/latest/chapters/memory.html)。

本轮实现三项改动：

1. **按用途选择内存**：设备 arena 优先 device-local；上传优先 coherent；概率图/CTC 回读优先 cached，再考虑 coherent。始终检查 `memoryTypeBits`，不存在可选属性时保留兼容类型回退，而不是要求每张显卡都具有某一种内存布局。
2. **持久映射 host 缓冲区**：创建后映射一次，销毁时解除映射，避免每次上传/回读重复 map/unmap。非 coherent 内存仍执行 flush/invalidate；保留 fence、host 可见性和工作区替换规则。持久映射不意味着可以同时无同步地读写 GPU 使用中的内存，参见 [VMA 的持久映射说明](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html)。
3. **DET 横向坐标预计算**：半像素双线性缩放的横向坐标、边界和权重对每一行相同，提前计算一次；仍保留原 MIT 实现的 double 运算顺序和 FP32 输出。没有换成固定点或近似插值。24 组不同尺寸/stride 张量逐字节 `memcmp` 一致，另有 5 组非法输入拒绝测试。

仅 cached 回读的代表图交替实验已经看到收益，例如 Tiny img-001 完整调用约 62.37→47.21 ms，DET 图调用约 24.66→8.13 ms；该小样本用于定位原因，不替代上表的完整 100 图测试。三项改动的最终收益是合并测量，不能把总降幅全部归于某一项。

## 100 图测试条件与准确率

同一台 Ryzen 7 7735H / RTX 4060 Laptop / Windows 10 主机、同一 manifest、同一模型和字典 SHA-256，参数与[原三项目测试](THREE-PROJECT-100.md)相同。每模型独立进程，完整预热 100 图，再测 3×100 次；本轮新增 1200 次完整调用，其中 900 次计时调用。测量时没有并行的编译或其他 GPU 测试。

计时包含原生完整 OCR 与结果转为 Python 列表，排除图片解码、模型初始化、GT 评分和 HTTP。GPU profiler、validation、混合精度和 M64 实验全部关闭。前后 corpus 测试发生在不同时间窗口，不是同一窗口交替测量，笔记本温度、频率和调度可能影响小幅差异。

| 模型 | 整行准确率（优化前后相同） | 端到端字符准确率（优化前后相同） |
| --- | ---: | ---: |
| Tiny | 370/614，60.26% | 95.12% |
| Small | 486/614，79.15% | 97.72% |
| Medium | 497/614，80.94% | 98.80% |

评分沿用原来的 NFC/保留大小写空白标点、轴对齐 IoU≥0.30 一对一匹配、缺失/额外字符计入编辑数的规则。置信度不是准确率。合成语料不代表所有实拍场景。

**Small 原先相对 C 项目的额外未匹配行（img-048.jpg，51 字符）仍然存在**。本轮解决性能问题，没有掩盖或修复该正确性差异。原报告的 C=97.96%、Vulkan=97.72% 字符指标仍成立。

## 内存

单位 MiB。RAM 取 Windows 进程生命周期 peak working set；GPU 为按 PID 采样的 WDDM 专用内存归属值。

| 模型 | RAM 峰值：前→后 | GPU 专用归属峰值：前→后 |
| --- | ---: | ---: |
| Tiny | 280.30→277.04 | 133.65→133.65 |
| Small | 346.99→342.59 | 235.22→235.22 |
| Medium | 618.68→618.40 | 625.75→625.75 |

没有观察到这次优化引入明显的峰值增长。原始报告另保留 private bytes、共享 GPU 内存和每轮结束值：例如 Tiny 三轮结束 private bytes 为约 382.27/382.44/382.39 MiB。采样可能遗漏短峰值，RAM 包含 Python/输入/驱动等；WDDM 归属值不是物理显存使用总量。三轮趋势和 1200 次调用均不构成无泄漏证明；之前旧部署包的长测不能直接移用于当前候选。

## 强化 DML 对照：匹配原生主机流程

此处是本项目的 **测试专用 ORT DirectML 1.24.4 对拍 DLL**，不是原 DML 项目的实际部署 DLL，不能混为同一次三项目测试。两者使用同一份编译后的 DET/CLS/REC 预处理、DB/crop/JSON 主机代码、同一预解码 BGR，并确认 Vulkan device 1 与 DXGI device 1 都是 RTX 4060。

DML 开启完整图优化、顺序执行、CPU arena/memory pattern 和每图最多 32 个固定尺寸 LRU 会话；固定尺寸会话可能复制权重，因此不宣称两者 VRAM 预算等价。每场景预热 3 次，再交替测 30 次/后端，仅同时执行一个 GPU 工作负载。计时包含完整原生 OCR 和 C ABI JSON 复制，排除 Python JSON 解析、文件解码和 HTTP。

| 模型 / 输入 | Vulkan 中位 ms | DML 中位 ms | Vulkan P95 ms | DML P95 ms |
| --- | ---: | ---: | ---: | ---: |
| Tiny / 500×500 | 48.45 | 60.26 | 49.95 | 62.25 |
| Tiny / 1000×1000 | 87.80 | 100.74 | 94.71 | 109.45 |
| Small / 500×500 | 71.69 | 97.74 | 79.47 | 114.59 |
| Small / 1000×1000 | 110.37 | 129.31 | 123.97 | 142.34 |
| Medium / 500×500 | 138.22 | 150.10 | 140.59 | 152.79 |
| Medium / 1000×1000 | 199.34 | 205.60 | 201.70 | 214.87 |

输入是仓库公开示例及其双线性放大版本，实际 DET 尺寸分别为 512 和 960。6 组均为 16 条文字，文本/CLS 标签一致，最大坐标差为 0，最大分数绝对差约 1.073×10⁻⁶；不是用调低精度门槛掩盖输出差异。此结果只覆盖这 6 个场景。

## 本轮验收

- Release 构建完成；CTest 10/10 通过，新增内存策略 10 种选择/回退情况、DET 24 张量字节一致测试。
- RTX 4060 三模型 DET 960×960 / 960×736 / 736×960、识别 40 种宽度超过 32-plan LRU、概率与 CTC 交替、工作区增长后回到小尺寸、回读 canary 均通过。
- Medium 显式 128 MiB 预算正确拒绝大图，随后小图仍可复用；默认每图 512 MiB 不变。
- 用户提供的 2448×3264 私有大图：Medium / DET 960 连续 5 次均为 85 条，文字重复一致。报告只保留图片 SHA-256、尺寸、条数和耗时，不收录图片或其 OCR 文本。
- RTX 4060 三模型和 AMD 核显 Medium 均通过 ORT CPU 完整 OCR quick 对拍：公开原图、180°、空白、stride/长度、资源上限与拒绝后恢复、结果生命周期、同句柄 4 线程调用。
- 上述工作区/ORT 实机功能测试启用 `VK_LAYER_KHRONOS_validation`，退出码均为 0，工具输出中未出现 validation error/VUID。功能测试的层开销不用于性能表。
- 前后全部输出对象的审计脚本测试 3/3 通过，包含“文字相同但分数变了也拒绝”的防放宽检查。

这不是新的 1000～5000 次稳定性验收，也未在其他操作系统/GPU 上验证本轮收益。C ABI、配置和版本号未变化。

## 产物与复现

新候选：`build/transfer-optimized/Release/lw.PPOCR.Vulkan.dll`。

SHA-256：`0cfd9b5bbc192193b3208392eaee1cdb9e7012202e5f944522a504570ea81894`。

优化前 corpus DLL SHA-256：`a973523b657b2a3e5e0a306c5f56fc496a8bf0e31550776903d86fb5762ee17e`。已验证的旧 zip 未覆盖，C 项目和原 DML 项目源码未修改。

完整输出/每图耗时/GT 明细在 [Tiny](reports/host-transfer/tiny-vulkan.json)、[Small](reports/host-transfer/small-vulkan.json)、[Medium](reports/host-transfer/medium-vulkan.json)。审计、DLL/来源/模型/输入哈希见 [qualification.json](reports/host-transfer/qualification.json)；[强化 DML 原始对照](reports/host-transfer/transfer-optimized-matched-dml.json)与[工作区回归](reports/host-transfer/transfer-optimized-workspace-nvidia.json)单独保留。CPU 对拍的 4 个报告也在同一目录，记录其调用证据，不假定该旧测试格式独立携带 DLL 哈希。

从项目根目录复测每一模型（下面以 Tiny 为例，其他两种改 `--variant`）：

```powershell
python tests/benchmark_three_projects.py --worker --backend vulkan --variant tiny --vk-library build/transfer-optimized/Release/lw.PPOCR.Vulkan.dll --output build/reports/transfer-optimized-100/tiny-vulkan.json
python tests/benchmark_dml_ocr.py --library build/transfer-optimized/Release/lw.PPOCR.Vulkan.dll --dml-library build/transfer-optimized/Release/lwvk_dml_reference.dll --device 1 --dml-shape-cache --iterations 30 --report build/reports/transfer-optimized-matched-dml.json
python scripts/report_host_transfer.py --results build/reports/transfer-optimized-100 --library build/transfer-optimized/Release/lw.PPOCR.Vulkan.dll --dml build/reports/transfer-optimized-matched-dml.json --workspace build/reports/transfer-optimized-workspace-nvidia.json --output build/reports/transfer-audit
```

运行前关闭其他 GPU/CPU 密集工作，移除 validation/profiler 环境变量并保持实验选项关闭；worker 默认使用原 corpus 和三轮测量。显卡编号必须以本机探测结果为准。DML helper 是可选测试构建目标，需要对应 NuGet 开发依赖，不是客户的 Vulkan 部署依赖。

下一步仍应按 profiler 做小通道卷积、CLS/REC 多次提交与 CPU crop 的成本优化，并单独定位 Small img-048 的 GT 差异。协作矩阵/混合精度继续独立验收，不直接替换本轮通过的 FP32 路径。

English summary: cached CPU readback, persistent host mappings and bit-identical DET preprocessing reduce mean latency on the same 100-image corpus by 39.76% / 27.91% / 12.97% for Tiny / Small / Medium. All prediction objects and ground-truth metrics remain unchanged. Six matched-host DML cases favor Vulkan, but this is not a universal performance claim, release qualification or proof of no leaks.
