# FP32 1×1 卷积与输出融合优化

日期：2026-10-01；版本：0.5.0-dev.2；本机 Windows 实测，不代表 CI 或全部显卡验证。

本轮开始优化 GPU 网络内部，不修改模型、FP32 精度、DET 960 上限或公共 C ABI。有效改动按形状自动选择，不需要新的实验开关；上一轮 DET/CLS/REC GPU 前处理仍为独立、默认关闭的实验。

## 100 图配对结果

基线为上一轮 `gpu-text-experiment1`，两边都开启 `LWVK_GPU_DET_PREPROCESS=1` 和 `LWVK_GPU_TEXT_PREPROCESS=1`。设备：RTX 4060 Laptop GPU；100 张 C 项目生成图片；每图预热一次、每条路径测两次，同图交替执行。统计包括原生调用、结果复制和 Python JSON 解析，不含文件解码、初始化和 GUI。

| 模型 | 上一轮均值 ms | 本轮均值 ms | 耗时下降 |
| --- | ---: | ---: | ---: |
| Tiny | 22.301 | 21.835 | 2.1% |
| Small | 37.332 | 36.207 | 3.0% |
| Medium | 86.728 | 83.838 | 3.3% |

三模型各 100 图的完整 `items` 字段逐项一致（文字、分数、坐标、分类信息）。这是结果回归，不是新的零错误或正确率保证。详见 [原始 100 图报告](reports/pointwise-optimization/corpus-final.json)。不同轮次的绝对时间和百分比不能直接拼接推导累计提升。

### sample.jpg 与放大图

每种形状各预热三次，然后交替运行 30 次；完整调用中位数，单位 ms：

| 模型 | 图尺寸 | 上一轮 | 本轮 | 耗时下降 |
| --- | --- | ---: | ---: | ---: |
| Tiny | 500×500 | 28.076 | 27.410 | 2.4% |
| Tiny | 1000×1000 | 46.724 | 46.287 | 0.9% |
| Small | 500×500 | 50.475 | 48.345 | 4.2% |
| Small | 1000×1000 | 76.407 | 74.406 | 2.6% |
| Medium | 500×500 | 113.502 | 107.618 | 5.2% |
| Medium | 1000×1000 | 163.042 | 157.053 | 3.7% |

六组完整结果相同，详见 [30 次配对数据](reports/pointwise-optimization/pair-final.json)。默认 CPU 前处理路径也做了同样的 30 次对照，结果一致，完整耗时下降约 1.3%～4.5%；见 [默认路径对照](reports/pointwise-optimization/default-pair-final.json)。不能将上述网络优化等同于 GPU 前处理实验。

## 三项保留的改动

1. **连续向量写回**：已有 M32/N64/K32 1×1 卷积保持原来的共享内存分块和 K 顺序累加，最后直接写出四个相邻 NHWC 通道，而不是转置寄存器块、逐个标量散写。输出通道不满足四对齐时仍用原 kernel；实验性的 M64 tile 不变。
2. **小 M 专用 kernel**：空间位置数 ≤4、输入输出通道均 ≥64 的 1×1 卷积，每个 lane 计算一个有效输出。避免 M=1 时仍计算四行寄存器块的三行无效工作；顺序累加 K，不采用会改变舍入顺序的并行归约。
3. **Conv→SiLU 输出融合**：仅融合 group-one、单消费者、紧邻且没有其他激活冲突的节点，保持 `v * (1 / (1 + exp(-v)))`。图输出、共享分支、depthwise、其他激活不融合。Medium REC 诊断中五个独立 SiLU 减为三个，命中两处融合；不宣传所有激活均已融合。

新增自有 shader 位于 `src/shaders`；固定第三方源码和模型哈希未修改。融合与其他 FP32 分派（包括参考 dense 路径）都具备相应输出计算；mixed-precision cooperative 路径不接收尚未支持的 SiLU epilogue。

## 未保留的尝试

试过将 K staging 从 32 增至 64，减少 barrier，但共享内存由 12 KiB 增至 24 KiB。本机 Medium 500/1000 图耗时略增，且没有稳定收益。因此该分派、生成与构建接入已移除，不增加一个无收益的部署开关。试验数据保存在报告目录的 `rejected-k64.json`，只用于记录负结果。

## 验证范围

- CTest 12/12；图优化单测新增图输出/共享分支/激活冲突/错误 arity 等保护条件。
- RTX 与 AMD kernel 探针各 45 组通过，覆盖 bias、ReLU/GELU/SiLU、通道和空间尾部及输出 canary。最大绝对误差相对 CPU 探针约 `4.77e-7`。
- RTX kernel 探针在 Khronos validation layer 下运行未报告错误；此范围仅为探针，不是所有图的 GPU memory validation。
- `test_pointwise_regression.py`：RTX、AMD 各三模型 DET/CLS/REC 共 27 种输入形状，每形状重复三次，与上一轮 DLL 的原始 FP32 概率逐位相同；REC CTC 文字/分数也相同。见 [RTX](reports/pointwise-optimization/raw-fp32-final.json)、[AMD](reports/pointwise-optimization/amd-raw-fp32.json)。AMD 本轮没有做端到端性能测量。
- 独立 ONNX Runtime CPU：三模型 CLS/REC 概率、CTC 与 BGR 路径各 20 次回归；三模型完整 OCR 各 5 次，以及旋转、宽图、空白、资源上限、结果生命周期和并发测试通过。
- 三模型 DET960、40 种 REC 尺寸 LRU、共享工作区增长、回读 canary、预算不足后恢复通过。
- REC BGR：三模型各 24 组精确对照、长度查询/小缓冲区、非法输入、40 个宽度缓存、20 次并发调用、上传预算和恢复通过。
- HTTP：二进制/Base64、整图/仅识别/批量、API Key、异常/过大输入、429/503、日志隐私及异常后恢复通过。
- 新 C# 包使用最终 DLL；另做移除 SDK/PATH、包外工作目录的三模型整图/框选与 WinForms 实测。最终 ZIP 解压后再核验全部文件 SHA-256 和实际 loader 来源，验证文件单独提供。
- 176 个固定依赖/模型文件校验、仓库预检和 C++ 格式检查通过。本轮未运行远程 CI、未提交或推送。

基线 DLL SHA-256：`b87cbb9559586053cd46e5a9bb1ef28b8d5f9df56cf8b5e219c3e20dd33482fa`。

本轮 DLL SHA-256：`39d4397307d8c1b19df22a6a312a1fd49e4ebd4d04cd1d445cf755f00d634929`。

报告均绑定当时的 DLL，不自动适用于未来构建。算子 profiling 的时间戳会扰动执行，只用于定位热点，不把其和当作端到端时间。本轮没有新做长稳测试、完整 RSS/VRAM 测量或泄漏证明，也没有新比较 DML，不宣称普遍优于 DML。

## 下一步

本轮增益小于前处理迁移，但来自 GPU 网络内部且默认路径也能获益。后续优先研究文字行 CLS/REC 的批量化与提交/等待开销；同时保持累计裁剪/上传预算和同句柄串行保护。更大的 tile、混合精度或批量不能仅凭理论推导启用，必须通过三模型输出与端到端门禁。
