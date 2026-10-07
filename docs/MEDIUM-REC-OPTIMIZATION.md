# Medium REC 批量 FP32 投影候选（2026-10-07）

结论：大通道 REC 投影是 Medium 的主要热点。本轮增加**可选的逐层执行与合并投影**，固定 sample 整图耗时降低约 12%～17%，100 张变尺寸流热身轮降低约 4%～5%。仍未超过既有 TensorRT，不能将固定图收益推广到所有图片。

它是 **默认 OFF 的研究构建**，不是新的正式版本或 Release 附件。保留普通构建的串行路径；源码、模型、FP32、DET960 和公共接口不变。本机研究 Demo 位于 `build/medium-next/experimental-staging`，请选择 Medium 和 GPU 1；普通包不会自动启用。新增构建信息字段与打包门禁拒绝将研究 ON 构建当正式附件发布。

## 实现

- 针对 REC 图中 `[768,1536,1,1]` 的大通道 projection 架构选择候选，不凭文件夹名猜测模型。仅 NVIDIA、GPU REC 前处理开启、非 profiling、至少两行且预算允许时启用；Tiny/Small、AMD、单行和紧预算保留原路径。
- 一个 arena 分配最多四个独立激活切片，同组按网络层执行。它**增加激活内存**，不是零成本并行；四个切片合计仍受原工作区预算约束，不足时减少到三、二或一条。
- 兼容的 pointwise 层将 2～4 条文字行的矩阵行虚拟拼接，一次 dispatch 完成 FP32 GEMM；只共享权重，不复制中间图像、不混合文字行的卷积或注意力。M32/N128/K32，128 线程，20 KiB shared memory。
- K 的顺序、FP32 累加、点积后的 Bias、ReLU/GELU/SiLU/HardSwish epilogue 保持一致。N/K 必须能被四整除；检查共享内存、工作组、descriptor range 和地址范围，否则使用各行原 dispatch。
- 相邻层统一屏障保护 RAW/WAR/WAW；8 条文字行按最多 4 条一组，下一组在前组完成后复用切片。CPU 引擎互斥锁仍保留，**不是 HTTP 多请求并行**。
- 原有每槽 16 / 总计 128 的尺寸计划 LRU 不扩大，另加最多 **8 个组合命令缓存**，按 Plan 地址与录制 revision 校验。缓冲区换绑、计划淘汰后旧命令不会被再次提交；GPU 超时仍污染句柄，不将超时视作取消。

## 成对完整 OCR

Windows 10 x64 / Ryzen 7 7735H / RTX 4060 Laptop / 驱动 596.36。预解码 BGR，预热三次，新旧交替，每版每图 40 次，候选优先初始化；不含模型初始化/GUI/文件解码，包含原生 OCR、结果复制和 Python JSON 解析。未锁频/功耗，没有后台持续推理。

| 图片 | 旧版整图中位数 ms | 候选中位数 ms | 耗时降低 | REC 旧→新 ms |
| --- | ---: | ---: | ---: | ---: |
| sample 500×500 | 88.31 | 72.92 | 17.43% | 65.85 → 50.80 |
| sample 1000×1000 | 124.06 | 109.05 | 12.10% | 63.48 → 48.72 |

所有结果对象逐字段严格相等。Tiny/Small 不走新批量路径，但本轮固定图测得约 0.1%～0.8% 耗时增加，不删除这些结果，不宣称三模型均提速。[三模型原始 A/B](reports/medium-rec/pair.json)。

## 100 张变尺寸流

lw.PPOCR.C 的 100 张生成图片；每模型三遍，逐图交替新旧，第二遍反向，不做每图单独预热；包含首次尺寸计划录制。全部字段相同。增加反向引擎初始化控制，检查 GPU 分配顺序对测量的影响。

| Medium | 首遍旧→新平均 ms | 热身遍一 | 热身遍二 |
| --- | ---: | ---: | ---: |
| 首轮 | 75.59 → 74.76 | 71.74 → 68.47 | 71.88 → 69.03 |
| 反向初始化控制 | 76.35 → 73.10 | 74.75 → 71.21 | 75.77 → 72.59 |

Medium 热身遍耗时降低 **3.97%～4.72%**。首轮 Tiny/Small 热身遍反而慢约 2.8%～4.5%；反向初始化后为约 0.3%～0.7%。说明分配顺序/未控制环境会影响小差异，不能仅凭一轮给出普遍速度保证；也不能断言某一个原因已被证明。组合命令在变尺寸流中有更多录制/缓存周转，收益远小于固定图。

因此本轮候选保留为 **opt-in**，不替换稳定默认路径。原始数据：[首轮](reports/medium-rec/stream-first.json)、[反向控制](reports/medium-rec/stream-reverse.json)。首轮候选哈希为 `628423e57ebb914b3abbeff2406b2f00df98f78baeb8b701af332934a466a79c`；随后补充能力/整数边界检查，反向控制使用下面的最终候选，不改计算内核。

## 稳定性、边界与正确性

- NVIDIA：99 个合并投影探针，2/3/4 条不等长行、N/K 尾部、Bias 和全部激活 flag，与旧 wide 内核输出逐位相等；输入和片段间保护区未改写。AMD 通过 33 个小型探针。两卡均开启 Vulkan validation 与 synchronization validation。
- Medium 批量探针在两卡通过：1..8 行、旋转/非紧 stride、非法输入恢复、40 个宽度与 104→128 LRU 上限、组合缓存八项淘汰、紧预算、20 次并发批次调用；RTX 有效四片段，AMD 原单片段回退。
- 三模型 DET/CLS/REC 独立网络 27 个形状与旧版概率逐位相等：[回归](reports/medium-rec/regression.json)。这个单网络检查本身不触发跨行合并，合并内核另由上述探针验证。
- Medium 整图与 ORT CPU / NumPy / CTC 参考一致，包含方向、空图、异常、资源上限、结果生命周期和并发检查：[参考](reports/medium-rec/reference-medium.json)。几何参考仍共享生产几何库，不冒充完全独立几何实现。
- 32 项 CTest、9 项供应链测试、181 个固定资产和 5 份冻结 v1 合同通过。
- Medium WinForms 真 GPU 整图、ROI、5 次重复、可填 GPU、延迟加载 JSON/表格、异工作目录通过：[Demo](reports/medium-rec/winforms-medium.json)。首轮包含创建执行计划/管线，约 632 ms 原生调用，不能当作预热性能；后续原生调用约 69～86 ms。HTTP 原有 Tiny 功能、认证、异常恢复、429/503 和日志检查通过：[HTTP](reports/medium-rec/http.json)，不把它说成 Medium HTTP 压测。
- Linux 软件 Vulkan CI 增加合并内核 quick 探针；**本轮未运行远程 CI、Linux 实体 GPU 或 Linux sanitizer**，不继承 Windows 的验收结果。

### Medium 单句柄长测

同一引擎处理 5 张 sample 变形 + 100 张生成图，包含首次、预热、连续、大小图切换、**1000 次混合图 soak** 和五次 2 秒空闲调用，共 1400 次完整 OCR。0 错误、重复结果哈希相同、所有计时有限。

- soak：中位数 **70.35 ms**，P95 **93.18 ms**，最大 **177.08 ms**；大小图切换 P95 **112.59 ms**。
- 预热→关闭前：Private **+7.59 MiB**，RSS **-113.12 MiB**，线程 -4、句柄 -3；100 次 checkpoint 后资源没有持续单调增长。总可见 RAM 约 15.24 GiB，期间可用约 2.7～2.9 GiB。
- 关闭引擎后 Private 下降约 667 MiB；监控包含 Python、105 张预解码图片和驱动，不是纯 DLL 内存。单次资源平稳不是无泄漏证明。
- 两秒空闲后中位数仍 **271.33 ms**，最大 **333.67 ms**。本项优化不解决空闲降频、首次调用或全部尾延迟问题。

[完整长测/资源 checkpoint](reports/medium-rec/soak-medium.json)。

## 当前 TRT 差距

同 ONNX/字典，独立进程串行执行；每组 100 图预热，再测三遍 300 次。TRT 仍是既有 **FP16-enabled plans / FP32 I/O / 4 个 REC predictor / batch4**；Vulkan 为 FP32，新方案单队列。两者宽度对齐、批处理和前后处理不同，不能视作相同数学路径的纯后端对拍。

| 模型 | Vulkan 平均 / P95 ms | TRT 平均 / P95 ms |
| --- | ---: | ---: |
| Tiny | 20.70 / 25.97 | 15.15 / 18.99 |
| Small | 32.09 / 41.54 | 23.19 / 29.78 |
| Medium | 68.59 / 88.91 | 34.38 / 47.04 |

**Medium 仍约慢两倍**。其进程生存期峰值工作集 Vulkan/TRT 为 610.9/706.4 MiB，进程专用 GPU 内存采样峰值 723.6/1555.6 MiB，整卡平均 GPU 利用率 67.5%/48.5%；利用率不是 shader occupancy。新候选比上一轮 Vulkan 的 653.9 MiB 专用 GPU 峰值多约 70 MiB，但这是不同时间的观测，不是严格配对内存差分。

本生成集端到端 CER 为 Vulkan **1.20%**、TRT **2.35%**，各 614 行 GT；不代表真实业务准确率，也不以降低精度换提速。250 ms 采样可能漏峰值，进程归属内存、整卡利用率不能相加。[完整三模型复测、预测与资源采样](reports/medium-rec/trt.json)。

## 撤回的尝试

- wide K64：短 REC 宽 32/48/96 局部改善约 4%，但宽 320/960 慢约 13%/14%，已撤回。[数据](reports/medium-rec/rejected-k64.json)。
- wide K16：各宽度慢约 7%～11%，已撤回。[数据](reports/medium-rec/rejected-k16.json)。
- 每行/层 secondary command：额外命令执行开销造成固定图和变尺寸流退化，已删除。当前直接录制合并主命令，不使用 secondary command。[数据](reports/medium-rec/rejected-secondary.json)。

## 身份与复现

基线为上一轮 HardSwish 开发 DLL，**不是原始 v1.0.1 Release 附件**：`6a1d2adcc38f50f35bfda50e0c05ba7e64980900ac0dc36ae6afe32756e56ff9`。

本轮最终研究 DLL：`a0fdc10647ec091cbcab987bfc8f9baaab62fec68110cdbdb132defcd6c7c8fd`。版本字符串保留 1.0.1，BUILD-INFO 标记 `experimental_rec_layer_major=ON`。没有提交、推送、打标签或替换线上附件。

```powershell
cmake -S . -B build/medium-research -G "Visual Studio 17 2022" -A x64 -DLWVK_EXPERIMENTAL_REC_LAYER_MAJOR=ON -DLWVK_EXPERIMENTAL_REC_LANES=OFF -DLWVK_EXPERIMENTAL_SHORT_REC=OFF -DLWVK_EXPERIMENTAL_COOP=OFF
cmake --build build/medium-research --config Release --parallel 4
ctest --test-dir build/medium-research -C Release --output-on-failure
.\build\medium-research\Release\lwvk_pointwise_batch_probe.exe 1
$env:LWVK_GPU_TEXT_PREPROCESS='1'
.\build\medium-research\Release\lwvk_rec_batch_probe.exe models/onnx/ppocrv6-medium/rec.onnx 1
Remove-Item Env:LWVK_GPU_TEXT_PREPROCESS
cmake --install build/medium-research --config Release --prefix build/medium-research/staging
python tests/test_winforms.py --package build/medium-research/staging --output build/medium-research/demo --device 1 --repeat 5 --model medium
```

设备编号先用 probe 核对。普通发布必须 OFF；正式 packager 会拒绝研究 ON 包。后续优先增加真实 Medium 宽度/文档样本与多驱动复测，再决定默认启用，不把本机固定图优势当作普遍结论。
