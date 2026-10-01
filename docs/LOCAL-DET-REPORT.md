# 本机 Tiny DET Vulkan 验证报告

日期：2026-09-30。版本：`0.1.0-dev.1`。Windows 10 x64，MSVC 19.40，Vulkan SDK 1.4.350.0。
阶段：**检测概率图技术验证，不是完整 OCR 性能报告，也不是 v1.0 质量认证**。

这是 0.1 阶段历史记录；0.2 阶段已实现 CLS/REC 与 CTC，新增验证见
[LOCAL-TEXT-REPORT.md](LOCAL-TEXT-REPORT.md)。下方数据/当时边界保持原样。

## 复用范围

9 个通用 shader 由固定的上游快照派生为 FP32 基线（来源见 [NOTICE](../NOTICE)）；完整 Tiny DET 242 节点
由新 C++ Vulkan 调度层执行。未启用上游优化 GEMM、协作矩阵、FP16 或算子融合。

## 独立正确性对拍

参考：固定 SHA-256 的 Tiny DET ONNX、ONNX Runtime 1.21.0 CPU。
使用同一张示例图分别缩放至 6 种尺寸，CPU/GPU 输入张量字节一致。

| H×W | RTX 4060 Laptop 最大绝对误差 | AMD 核显最大绝对误差 | 两卡 0.2 阈值图 IoU |
| --- | ---: | ---: | ---: |
| 32×32 | 1.120e-7 | 1.120e-7 | 1.0 |
| 64×96 | 9.596e-6 | 1.231e-5 | 1.0 |
| 96×64 | 9.626e-6 | 8.017e-6 | 1.0 |
| 192×320 | 1.514e-5 | 1.341e-5 | 1.0 |
| 640×640 | 4.005e-5 | 3.889e-5 | 1.0 |
| 960×640 | 1.179e-4 | 1.192e-4 | 1.0 |

这不是“所有图片完全一致”的声明，仅说明当前样例/尺寸组合通过。
需要继续扩展多图正确性集、检测框和最终文字的回归。

## 重复运行与同步校验

两卡各执行 **1000 次**短循环，每两次切换一次尺寸；包含同尺寸计划复用和重新规划。
所有概率图对拍通过。启用 Khronos validation layer 和同步校验，没有出现 Vulkan
资源/同步错误。NVIDIA 测试使用旧环境开关，校验层仅提示设置已弃用；AMD 测试使用
`VK_LAYER_VALIDATE_SYNC=1`，后续复测统一使用新开关。

| Device | Repeat wall time | First sampled RSS | Last sampled RSS | Max sampled RSS |
| --- | ---: | ---: | ---: | ---: |
| RTX 4060 Laptop | 25.33 s | 270.61 MiB | 284.86 MiB | 310.59 MiB |
| AMD Radeon(TM) Graphics | 47.12 s | 342.82 MiB | 304.01 MiB | 345.65 MiB |

RSS 包含 Python、ORT CPU 参考、Vulkan loader/driver 和校验层，不等于原生库或显存占用。
没有观察到本轮持续单调增长，但不能据此证明无泄漏；1000 次短循环也不是数小时长期测试。
当前代码只保留一个尺寸计划，工作区按张量生命周期复用，图的命令/描述符池随计划销毁。

测试运行期间的单次内部耗时仅包括输入 staging、GPU 提交/等待、输出回读；不包含
计划构建、图片解码/缩放、DB 后处理、CLS/REC 或 HTTP。不同尺寸混合的 P50 不适合
对外宣传“每张 OCR 耗时”。

## 异常与集成

- C ABI null、结构大小、缓冲区长度、无效尺寸、NaN 输入拒绝；随后合法请求恢复。
- 模型版本、相对路径逃逸、权重大小、常量越界、拓扑、未知算子、坏 JSON、权重校验失败拒绝。
- 4 KiB 工作区限制测试触发明确错误，新的正常句柄可恢复执行。
- C# x64 控制台示例已编译并运行；可枚举两卡并调用原生 DET。
- 示例图生成了可视化概率图，尚不输出文本或检测框。
- 最终安装目录再次通过两卡各 6 种尺寸、20 次循环及异常模型测试，启用新同步校验开关。
- DLL 导出 7 个预期 C ABI 符号；直接依赖仅 `vulkan-1.dll` 和 `KERNEL32.dll`。
- Windows 预览包附 SHA-256，打包脚本拒绝 `__pycache__`/`.pyc`/`.pyo`。

## 原始记录与复现

原始记录在 `docs/reports/`，是当前开发快照的结果；生产发布前须对最终 CI 包重新测试。

```powershell
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
python tests/test_det_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 1000 --report build/reports/nvidia.json
python tests/test_det_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 0 --iterations 1000 --report build/reports/amd.json
python tests/test_invalid_models.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 1
```

后续仍需 CLS/REC、DB/CTC、并发/销毁边界、GPU assisted validation、ASan/UBSan、
真实多图长跑和 Linux/Intel 实体 GPU 验证。
