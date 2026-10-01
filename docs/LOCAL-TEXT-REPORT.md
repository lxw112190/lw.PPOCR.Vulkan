# 本机 CLS/REC 与仅识别验证

日期：2026-09-30；版本：`0.2.0-dev.1`；Windows 10 x64、MSVC 19.40、Vulkan SDK 1.4.350.0。
这是 GPU 核心开发验证，不是完整 OCR 性能报告、长期稳定性认证或正式发布。

## 实现与对拍范围

- CLS：固定 80×160，测试标题区域正向及旋转 180°；两卡标签均与 CPU 一致。
- REC：高度 48，宽度 32/64/96/320/640/960；BGR FP32 输入 CPU/GPU 字节一致。
- ONNX Runtime 1.21.0 CPU 使用原始固定 SHA-256 ONNX，独立于内部降级图。
- Vulkan 执行全部数值图；CPU 只做 BGR 预处理和 CTC，不调用 ORT 进行实际服务推理。
- 复用上游新增 AveragePool/Softmax Shader，MatMul 降为已有 1×1 Conv；BN 降为逐通道 scale/bias。

| 设备 | CLS 最大绝对误差 | REC 最大绝对误差 | CTC 与 CPU |
| --- | ---: | ---: | --- |
| NVIDIA RTX 4060 Laptop | 1.2733e-11 | 4.7207e-5 | 当前裁剪样例完全相同 |
| AMD Radeon(TM) Graphics | 1.7280e-11 | 4.7267e-5 | 当前裁剪样例完全相同 |

小宽度的长文字被强制压缩时可能无法识别；对拍通过只表示 GPU 与 CPU 结果一致，
不是这些裁剪/宽度组合的 OCR 准确率认证。还需要独立多图文本回归集。

## 原始 BGR 与 C# 接入

标题 ROI 为调用方指定 `(20,28,292,46)`，不是 DET 自动输出。
原生半像素双线性预处理与独立 NumPy 实现对照，经 CPU/GPU REC 后均为：

```text
纯臻营养护发素
score ≈ 0.9992589
```

测试覆盖 padded stride、末行不含额外 padding、字节长度差 1 拒绝、stride 过小拒绝、
文字容量不足、NaN 输入、错误后正常请求恢复。
C# 示例按 Bitmap 行复制到紧密 BGR 数组，避免原始 Bitmap padding/负 stride 的误用。
CTC 主机单元测试覆盖 blank、相邻重复、blank 分隔重复、空标签、概率并列和 NaN。
最终安装目录的两卡 DET/CLS/REC 回归、异常字典与任务类型检查均通过。
清空开发工具 PATH 后，包内设备探测和 C# ROI 识别均能运行；C# 输出同一标题文字。

## 重复、并发与内存观察

两卡各执行 **1000 次 CLS + 1000 次 REC**，REC 每两次切换一次宽度；同句柄额外执行
4 线程、16 次并发调用。所有输出对拍通过，Khronos validation layer +
`VK_LAYER_VALIDATE_SYNC=1` 未出现资源/同步错误。
两卡测试同时运行，不使用这些耗时对外宣传硬件性能。

| 设备/阶段 | 首次阶段采样 RSS | 最后阶段采样 RSS | 阶段最高 RSS |
| --- | ---: | ---: | ---: |
| NVIDIA / CLS | 164.74 MiB | 164.94 MiB | 164.94 MiB |
| NVIDIA / REC | 208.04 MiB | 208.07 MiB | 211.75 MiB |
| AMD / CLS | 135.04 MiB | 134.95 MiB | 135.27 MiB |
| AMD / REC | 183.17 MiB | 182.04 MiB | 186.68 MiB |

RSS 包含 Python、CPU 参考模型、Vulkan loader/driver 和校验层，不等于 DLL 或显存占用。
阶段之间加载不同模型会改变基线；不能把 CLS 首次到 REC 最后的差值称为泄漏。
本轮未发现阶段内持续单调增长，但短循环不能证明无泄漏；主机 sanitizers、显存观察和
数小时多图测试仍未完成。

## 复现

```powershell
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
python tests/test_text_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 1000 --report build/reports/text-nvidia-1000.json
python tests/test_text_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 0 --iterations 1000 --report build/reports/text-amd-1000.json
```

原始开发记录位于 `docs/reports/text-*-1000.json`。最终安装目录另行运行回归，
未来正式发布仍须对实际 CI 下载包验证。下一步是 DB、透视裁剪和完整 OCR 串联；
HTTP/Web、WinForms、自动 CPU 回退、Intel/Linux 实体 GPU 验证尚未完成。

复用的通用 OpenCV 发布预检检查了 workflow 脚本、忽略规则和依赖锁。
其中“查找 OpenCV/锁定 OpenCV 5/拒绝旧 OpenCV”三项对本项目不适用：本项目刻意不依赖
OpenCV，未为通过该通用检查而添加它。Vulkan 项目的门禁为 SDK 校验、27 项资产校验、
原生测试、独立模型对拍及安装目录测试；不宣称通用 OpenCV 预检全部通过。
