# Medium REC FP32 宽输出 tile 优化

2026-10-02，在 `det-vector-opt1` 之上继续优化 REC 大通道 1×1 投影。模型、FP32、DET 长边上限 960、工作区预算及公共 C ABI 均不变。本轮不实现 GPU 透视裁剪或 GPU DB 后处理。

## 实现

新增 `src/shaders/conv_pointwise_wide.comp`：将原 M32/N64/K32 tile 扩为 **M32/N128/K32**，128 个线程，每线程累计 8 行 × 4 个相邻输出通道。输入与权重使用连续 `vec4` 访问，同一输入 tile 服务两倍输出通道，减少重复读取。工作组 shared memory 为 **20 KiB**（输入 4 KiB、权重 16 KiB），不是额外 20 KiB 的全局 arena；不新建图像拷贝或中间 im2col 张量。

保持旧 kernel 的 K 遍历、FP32 累加、偏置及激活顺序；M/N/K 尾部有边界保护，所有线程先完成共享内存屏障，再退出。ReLU/GELU/SiLU 和无偏置路径均参与测试。

自动分派条件：REC、NVIDIA vendor ID、shared memory 至少 20 KiB，且为原先可向量化的无 padding、stride=1 的 1×1 卷积，M≥32、N/K≥512、N/K 为 4 的倍数。设备/任务策略在录制命令前判断一次。其他形状、DET、CLS、AMD/Intel/软件设备保留旧分派；reference/禁用 tiled 及显式 tile64 诊断选项保持优先级。大词表投影（18710 通道）不走新 kernel。

**设备门控是实测后的调优策略，不是所有 NVIDIA GPU 的提速保证。** 本轮 NVIDIA 实机仅覆盖 RTX 4060 Laptop。没有新增运行开关；普通 CPU 前处理和 GPU 前处理实验在符合形状时都采用它。

## 基准与口径

Windows 10，NVIDIA GeForce RTX 4060 Laptop GPU（本机设备 1），三套固定模型，CLS 开启。两边均开启 `LWVK_GPU_DET_PREPROCESS=1`、`LWVK_GPU_TEXT_PREPROCESS=1`，网络仍为 FP32。串行执行 GPU 测试；测量预解码 BGR → 原生完整 OCR → JSON 复制/解析，不含文件解码、初始化或 GUI。完整预测字段逐项一致，不代表识别准确率提高，也不是与 DML 的对比。

基准 DLL SHA-256：`05201449ff0705ecd3c70e7dbfbeb7740e300e6be6bbc1597cb8ba3ae8d13c0d`

最终 DLL SHA-256：`056b9b567a9b40b69407591a0ebc66b284ccd26835b4e289c0d05c4255393a6d`

### 100 图逐图预热

每图一轮预热，新旧交替调用，每模型每条路径 200 个测量样本。

| 模型 | 旧版均值 ms | 本轮均值 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny | 22.960 | 23.091 | -0.6% |
| Small | 33.680 | 33.704 | -0.1% |
| Medium | 78.434 | 75.585 | 3.6% |

Tiny/Small 本轮不宣称加速；不把不到 1% 的波动当作收益。

### 连续换图，不逐图预热

100 图首遍正序、第二遍反序，新旧仍交替调用；包含首次尺寸计划建立。

| 模型 | 首遍旧→新 ms | 第二遍旧→新 ms |
| --- | ---: | ---: |
| Tiny | 24.770 → 24.746 | 24.049 → 24.025 |
| Small | 40.744 → 41.400 | 38.724 → 38.717 |
| Medium | 89.295 → 85.407 | 86.665 → 83.717 |

Medium 降幅约 **4.4%/3.4%**；Small 首遍慢 **1.6%**，第二遍基本持平。不能宣传为三模型、所有冷启动均加速。

### sample.jpg，30 轮配对中位数

三次预热，每次交替新旧顺序，16 个区域。

| 模型 / 尺寸 | 旧版 ms | 本轮 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny / 500×500 | 23.748 | 23.645 | 0.4% |
| Tiny / 1000×1000 | 40.661 | 40.555 | 0.3% |
| Small / 500×500 | 42.409 | 42.246 | 0.4% |
| Small / 1000×1000 | 66.771 | 66.988 | -0.3% |
| Medium / 500×500 | 99.901 | 94.942 | 5.0% |
| Medium / 1000×1000 | 141.920 | 136.685 | 3.7% |

Medium REC 累计阶段分别为 74.824→69.717、72.897→67.942 ms，约降 **6.8%**。阶段含上传/提交/等待/回读，不是纯 shader 时间。DET/CLS 不在本轮改动范围。

**普通启动（两项 GPU 前处理关闭）**另测 Medium 30 轮：500 图 119.719→115.265 ms（3.7%），1000 图 177.031→171.784 ms（3.0%）。不能套用实验路径的百分比。

## AMD 取舍与尚未解释的偏差

初始全设备宽 tile 候选（DLL `7961acdb0e05a7ed69750e1b23a97ea6a19ae331f4c8c07dccea1ef161f716b5`）在 AMD 核显 Medium 样例慢约 **6.6%**，没有发布该分派，记录单独保存为 `rejected-amd-wide.json`。

最终版本已让 AMD 保留旧 kernel；所有已有 SPIR-V 与基准逐文件哈希一致，算子记录另行核对实际分派。但完整配对调用仍有偏差：15 轮旧→新 637.670→664.439 ms（慢 4.2%）；候选优先初始化的 20 轮为 623.969→648.308 ms（慢 3.9%）。交换初始化顺序并未消除它，因此**不认定原因是初始化顺序，也不承诺 AMD 无退化**。普通 CPU 前处理的逆序初始化样例另慢约 2.0%。驱动/内存放置等只是待排查方向，尚无因果证据。本轮部署包主要供 RTX 4060 验证，AMD 用户可保留上一版对照。

## 回归验证

- RTX/AMD 各 27 组三模型原始概率输出逐位一致；REC 文字/分数一致。
- `lwvk_wide_pointwise_probe` 两卡各 72 组，覆盖 M/N/K 尾部、ReLU/GELU/SiLU、偏置，输出和尾部保护逐位一致；CPU 标量参考最大绝对误差约 4.8e-7。
- 显式开启 Khronos 同步验证，两卡上述探针及 RTX Medium REC 合批通过，日志确认 `SYNCHRONIZATION_VALIDATION` 生效，无 VUID/SYNC-HAZARD；不等于 GPU-assisted 越界检测。
- 同步验证下的 Medium 全图算子诊断覆盖 DET 两尺寸、REC 两宽度及 CLS；RTX REC 每条普通/紧凑 CTC 图均选中 19 个宽 tile 投影，DET/CLS 为零，AMD 全部为零。profiling 开/关结果摘要一致，不能用被 timestamp 扰动的时长作完整调用收益。
- 三模型独立 ONNX Runtime CPU 完整 OCR 参考、默认 CPU 前处理、100 图完整字段、10 组变化输入、填充 stride、40 个尺寸/LRU/紧预算恢复通过。
- REC-only 每模型 24 组输入、40 宽度、20 次并发及异常恢复通过。原 Medium 报错大图 2448×3264 按 DET960 重复五次成功，85 个区域；仅记录图片哈希/尺寸，不记录客户文字。
- CTest 13/13，176 项固定资产、格式和仓库预检通过；公共头文件未改，导出仍为 19 个。
- HTTP 二进制/Base64、完整/仅识别/批量、API Key、429/503、日志隐私及异常恢复通过。
- 完整 C# 包继续验证自带 loader、两卡三模型整图/ROI、GPU 选择/手填编号、WinForms 框选和五次重复；最终 ZIP 的解压验证保留在 ZIP 旁 `.validation.json`。

原始报告见 [reports/rec-wide-optimization](reports/rec-wide-optimization)。算子 timestamp 会扰动执行，只用于定位热点与证明分派，不用它替代上述完整调用数据。本轮未新增 1000/5000 次 soak、设备丢失注入或跨系统实机测试；不构成无泄漏和全显卡兼容证明。

## 文章经验与下一步

延续前后处理加速文章中的思路：融合数据处理、减少搬运、复用缓冲区、按端到端计时验收。已有实验路径把 DET/CLS/REC 缩放与归一化放到 GPU，REC 只回读紧凑贪心标签。本轮进一步复用 GPU 内部输入 tile，不直接移植 CUDA/TensorRT 代码。

DB 轮廓、框排序、透视裁剪仍在 CPU；CPU 决定方向，REC GPU 前处理可以直接反向采样。下一步可评估 GPU 透视裁剪与文字前处理融合，但三张图当前各自拥有 Vulkan context/device，须先设计共享资源/生命周期，不能把它当作一个 shader 即完成的优化。GPU DB 也应先做热点占比与结果对拍，避免搬运成本抵消收益。

## 复现

```powershell
$env:LWVK_GPU_DET_PREPROCESS="1"
$env:LWVK_GPU_TEXT_PREPROCESS="1"
python tests/benchmark_vulkan_pair.py --before build/conv-vector/Release/lw.PPOCR.Vulkan.dll --after build/rec-wide-v2/Release/lw.PPOCR.Vulkan.dll --iterations 30 --report build/rec-wide-v2/sample-final.json
python tests/benchmark_ocr_stream.py --before build/conv-vector/Release/lw.PPOCR.Vulkan.dll --after build/rec-wide-v2/Release/lw.PPOCR.Vulkan.dll --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/rec-wide-v2/stream-final.json
.\build\rec-wide-v2\Release\lwvk_wide_pointwise_probe.exe 1
```

测试探针不进入客户包，不增加运行依赖或 ABI。
