# 完整 OCR 本机验证报告 / Full OCR local report

日期：2026-09-30；版本：0.3.0-dev.1。Windows 10 x64，MSVC 19.40，
Vulkan SDK 1.4.350.0；FP32 正确性基线，不是性能优化版。

## 测试与结果

AMD Radeon(TM) Graphics、NVIDIA RTX 4060 Laptop GPU 均通过：

- 整图 DET → DB/阅读顺序 → 透视裁剪 → CLS/旋转 → REC/CTC。
- 原图 500x500、180° 倒置、640x400、320x320、96x64 空白，共 5 种输入。
- 非空测试图均得到 16 个区域，空白图得到 0 个；标题“纯臻营养护发素”正确。
- CPU 参考文字、CLS label 一致；分数差 <=0.003、原图坐标差 <=1.1 像素。
- 每张卡分别完成 100 次及后续 300 次完整 OCR 重复/尺寸切换。
- 同句柄 4 个线程、8 次调用串行安全；不足字节数、错误 stride、超尺寸明确拒绝。
- JSON 查询/不足缓冲区不截断、不重复推理；结果可以在引擎销毁后继续读取。
- 禁用 CLS 和单区域裁剪像素限制/异常后恢复通过。
- 三个 CTest：几何 golden unit、CTC/预处理 unit、C ABI 输入契约通过。
- C# 完整 OCR 示例实测 16 项、标题框约 (22,32)..(308,75)。修复 GDI+
  图片 DPI 引发的意外缩放，改为显式源/目标像素矩形。

原始记录：
[AMD 100](reports/full-amd-100.json)、[NVIDIA 100](reports/full-nvidia-100.json)、
[AMD 300](reports/full-amd-300.json)、[NVIDIA 300](reports/full-nvidia-300.json)。
额外累计裁剪像素限制和最终 staging 包使用最新测试脚本复测。
两卡 staging 小图/倒置/空白、累计裁剪限制及恢复通过；清理开发环境 PATH 后，
包内设备探测器和 C# 完整 OCR 实测通过。DLL 直接依赖仅 `vulkan-1.dll`、
`KERNEL32.dll`；实验 C ABI 19 个导出。没有把测试专用几何 DLL 或原始 ONNX 放入部署包。

参考流程使用独立 ONNX Runtime CPU 模型、NumPy 双精度半像素预处理和 CTC，
但 DB/裁剪/阅读顺序共享本项目复用的 C 代码；不是独立几何算法对拍。
几何单元覆盖缩放规则、空白/矩形概率图、NaN、容量不足、裁剪采样、竖框旋转和排序。
这不是广泛文本正确率评测；小图及低质量文字仍可能错字，CPU/GPU 一致不等于文字全对。

测试设置 `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`、
`VK_LAYER_VALIDATE_SYNC=1`；终端未报告 validation 错误。
没有据此宣称 GPU-assisted validation、ASan/UBSan 或真实设备丢失注入已经完成。

## 内存观察：300 次

| 设备 | 循环首个 RSS | 循环最后 RSS | 采样最大 RSS | 引擎销毁后 RSS |
| --- | ---: | ---: | ---: | ---: |
| AMD | 384.86 MiB | 374.69 MiB | 401.44 MiB | 302.90 MiB |
| NVIDIA | 329.94 MiB | 350.41 MiB | 380.94 MiB | 283.65 MiB |

引擎创建前两个进程均约 163 MiB。后半段 RSS 有上下波动，没有在这 300 次范围内
观察到按请求持续线性上升；销毁引擎后下降，但没有回到初始值。
这些值包括 Python、ORT、Vulkan loader/driver、validation layer 与分配器缓存，
**不是 VRAM 统计，也不是“零内存泄漏”的证明**。仍需原生长跑、分配器分析、
ASan/UBSan、1000..5000 次及更多驱动验证。两卡测试有重叠，不使用其耗时作性能宣传。

代码层边界：每图只保留一个 GPU plan；DB 图最大 960x960；裁剪逐行释放，
默认单区域 400 万、累计 3200 万像素；调用方需要主动销毁结果句柄。
每模型图 256 MiB workspace 上限不等于整个进程 RAM/VRAM 上限。

## 复现

```powershell
ctest --test-dir build -C Release --output-on-failure
$env:PYTHONDONTWRITEBYTECODE='1'
$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'
$env:VK_LAYER_VALIDATE_SYNC='1'
python tests/test_ocr_reference.py --library build/Release/lw.PPOCR.Vulkan.dll --geometry build/Release/lwvk_geometry_reference.dll --device 1 --iterations 300 --report build/reports/full-nvidia-300.json
python tests/test_ocr_reference.py --library build/Release/lw.PPOCR.Vulkan.dll --geometry build/Release/lwvk_geometry_reference.dll --device 0 --iterations 300 --report build/reports/full-amd-300.json
```

Windows/Linux CI 已增加完整流程和 staging 测试定义，尚未推送运行。
本机 Windows 结果不能作为 Linux、Intel、ARM64、macOS 或所有显卡的兼容承诺。
