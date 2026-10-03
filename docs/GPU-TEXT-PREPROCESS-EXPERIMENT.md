# GPU CLS/REC 前处理与上传优化实验

日期：2026-10-01；版本：0.5.0-dev.2；状态：默认关闭的本地验证实验。

本轮在上一版 GPU DET 前处理基础上，将文字行 CLS/REC 缩放、归一化和输入布局转换合并到 GPU，REC 的 180° 校正也合入采样。模型、网络 FP32 精度、DET 长边上限 960、REC 宽度规则和公共 C ABI 未改变。

## 100 图实测结果

设备为 NVIDIA GeForce RTX 4060 Laptop GPU。基线是**上一版已开启 GPU DET 前处理、CLS/REC 前处理仍在 CPU**的 DLL，不是最早的未优化版本，也不是 C 项目或 DML 项目。

| 模型 | 上一版均值 ms | 本轮均值 ms | 耗时下降 |
| --- | ---: | ---: | ---: |
| Tiny | 26.008 | 22.200 | 14.6% |
| Small | 42.327 | 37.952 | 10.3% |
| Medium | 90.333 | 85.998 | 4.8% |

使用 `lw.PPOCR.C/build-local-data/lw-generated-ocr` 的 100 张生成图片，预解码为 BGR。同图交替运行两个 DLL，每个模型每条路径测 200 次（每图两次），预热另计。测量从 Python 调用开始，包含原生 OCR、结果复制和 JSON 解析，不包含文件解码、初始化或界面绘制。

所有图片完整 `items` 字段逐项相等：文字、置信度、坐标和分类结果均不变。这是相对于上一版的结果一致性，不是新的零错误或绝对正确率保证。不同轮次运行环境存在差异，不能把历史百分比直接叠加或用不同轮次绝对耗时推导收益。

原始数据：[100 图对照](reports/gpu-text-preprocess/rtx4060-corpus-final.json)。

### 同图重复 30 次

每种模型、尺寸先各预热三次，再交替测量 30 次；以下是完整调用耗时中位数，单位 ms。500 为原始 sample.jpg，1000 为同图双线性放大。

| 模型 | 图片尺寸 | 上一版 | 本轮 | 耗时下降 |
| --- | --- | ---: | ---: | ---: |
| Tiny | 500×500 | 34.002 | 27.774 | 18.3% |
| Tiny | 1000×1000 | 53.547 | 47.141 | 12.0% |
| Small | 500×500 | 57.769 | 50.236 | 13.0% |
| Small | 1000×1000 | 82.978 | 76.337 | 8.0% |
| Medium | 500×500 | 121.129 | 113.046 | 6.7% |
| Medium | 1000×1000 | 168.846 | 161.349 | 4.4% |

六组完整结果一致，数据见 [RTX 对照](reports/gpu-text-preprocess/rtx4060-pair-final.json)。AMD Radeon(TM) 核显仅测 Tiny，10 次中位数：500 图 70.950→66.028 ms，1000 图 118.886→111.342 ms，结果一致；未在本轮验证 AMD Small/Medium，详见 [AMD Tiny 对照](reports/gpu-text-preprocess/amd-tiny-pair.json)。

## 改动原理

1. **CLS**：将 CPU 原有的左侧窗口采样、160×80 缩放、归一化和 NHWC 输出融合为一个 compute shader，直接写入网络 arena。
2. **REC**：融合高度 48 的缩放、有效宽度计算、128 灰色右侧填充、归一化和 NHWC 输出。保持宽度最小 32、最大 960、8 对齐，不把图片拉伸到灰色填充区。
3. **180° 翻转**：CLS 判断后，GPU REC 直接反向寻址源像素，不再先在 CPU 旋转文字行。CPU REC 路径仍保留原行为。
4. **上传**：头部与原始 BGR 直接写入持久映射的上传缓冲区，补齐最后一个 uint；去掉组装整张源图临时 vector 的分配和额外复制。非一致性内存统一 flush。
5. **执行/回读**：前处理和网络同一次图命令执行，REC 使用原有 GPU 贪心标签选择，只回读标签/分数对。公共原始浮点张量接口继续保留。
6. **内存约束**：原图上传计入共享工作区预算；过大输入先拒绝，错误后仍可用小图片继续调用。shader 地址和缓冲区大小在提交前校验。

这不是整条 OCR 流水线 GPU 化：DB 后处理、框排序、透视裁剪、CTC 去重和字典映射仍在 CPU。CLS 与 REC 各自上传文字行，本轮没有做共享一次上传、GPU 裁剪或多行批量网络。

## 开启与回退

本报告测量时两个开关均默认为关闭；当前默认策略已改为自动选择，见 [GPU 默认策略](GPU-DEFAULT.md)。复现本报告的局部模式应关闭 GPU 裁剪，并在**启动新进程、加载 DLL 前**设置；不能依赖在已加载 DLL 的进程中修改环境变量后重新初始化来切换。

```powershell
$env:LWVK_GPU_DET_PREPROCESS = '1'
$env:LWVK_GPU_TEXT_PREPROCESS = '1'
$env:LWVK_GPU_CROP_PREPROCESS = '0'
.\lw.PPOCR.Vulkan.WinFormsDemo.exe
```

旧 GPU 实验启动脚本已移除。当前普通入口自动选择 GPU 路径，CPU 诊断入口关闭三项优化。复现历史局部模式时，可使用以下环境变量组合，并直接运行 EXE：

| DET / TEXT / CROP | DET 前处理 | CLS/REC 前处理 |
| --- | --- | --- |
| `0 / 0 / 0` | CPU | CPU |
| `1 / 0 / 0` | GPU | CPU |
| `1 / 1 / 0` | GPU | GPU |

先关闭已有 Demo，再直接启动 EXE（不要调用会重置变量的默认启动脚本），选目标 GPU、模型并初始化。直接启动 EXE 会继承环境变量。HTTP 服务也可在启动前使用这两个环境变量，服务安装/配置 Schema 未新增字段。

实验额外要求 GPU 的 `shaderFloat64`；初始化时显式开启此能力，不支持时明确报错，不静默回退。为保持 CPU 前处理参考的运算顺序，前处理插值使用 FP64，网络、存储和其他算子仍为 FP32。实验不能与 `LWVK_GPU_PROFILE=1` 同时启用；性能测试也应关闭 `LWVK_HOST_PROFILE`。包内仍需要已安装的 GPU 厂商驱动。

## 验证与兼容性

- GPU 前处理探针：RTX/AMD 的 16 组 CLS/REC 输出与 CPU 参考逐个 FP32 位相等，覆盖灰色填充、极端比例、单像素、宽度封顶、stride 填充和 180° 翻转；输出尾部 canary 与上传边界检查通过。
- RTX 探针在 Khronos validation layer 下运行未报告错误；这只覆盖该探针，不声称整套 OCR 都经过 GPU validation。
- 三模型完整 OCR：100 图、变形图片、带填充 stride、异常输入、40 个尺寸 LRU 和上传预算不足后恢复通过。
- REC-only C ABI：三模型各 24 组精确对照；长度查询、过小输出缓冲区、非法 stride/宽度、40 种宽度缓存、20 次并发调用和预算不足后恢复通过。见 [REC 接口报告](reports/gpu-text-preprocess/rec-bgr-final.json)。
- 独立 ONNX Runtime CPU 对拍：三模型 CLS/REC 浮点接口与 BGR CTC 测试、Tiny 50 次完整 OCR 及旋转/空白/超长图等回归通过。见目录内 `text-reference-*.json` 和 `ort-reference-final.json`。
- 默认关闭路径：三模型、两尺寸各 30 次，完整结果一致；耗时降幅指标 `wall_reduction_percent` 约 -1.1%～+0.2%（负值表示稍慢），属于本轮噪声范围，不宣称默认路径加速。见 [默认路径对照](reports/gpu-text-preprocess/default-off-pair-final.json)。
- CTest 12/12、176 个固定依赖/模型文件校验、C++ 格式检查通过；公共头文件没有改动。
- HTTP 回归通过：二进制/Base64/批量、API Key、错误输入、429/503、日志隐私与关闭日志、异常恢复。
- 最终 ZIP 解压后，另做去掉 SDK/PATH、包外工作目录的 C# 三模型整图/框选测试，以及真实 GPU WinForms 测试；产物旁边单独提供验证 JSON，不把旧包证据套用到新包。

本轮基线 DLL SHA-256：`c509c8162f863b6b5aa45e02a9fe02f99cb75cec8f66c05877348ff0a631fdd4`。

本轮候选 DLL SHA-256：`b87cbb9559586053cd46e5a9bb1ef28b8d5f9df56cf8b5e219c3e20dd33482fa`。

## 边界与下一步

本轮没有重新做 1000～5000 次长稳测试，也没有测完整 RSS/VRAM 或证明无泄漏；短回归、预算拒绝和 canary 通过不等于长期稳定性结论。本轮不重新比较 C 项目/DML，不声称已超越所有 DML 场景。

Medium 的收益较小，因为 REC 网络执行已占更大比例。下一轮应继续基于算子/同步测量选择卷积、GEMM 或提交批量化的接入点，不能只靠减少“其他耗时”推断 GPU 网络已经加速。
