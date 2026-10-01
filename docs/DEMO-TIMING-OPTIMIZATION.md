# C# Demo 计时与展示优化（2026-10-01）

本次优化仅针对 Windows C# Demo 与 GPU 探测工具；不修改模型、原生计算图、精度或 ABI。部署包仍是 `0.5.0-dev.2` 技术预览，包修订标识 `timing-ui-opt1`。

## 实际修改

1. probe 在 Windows 控制台设置 UTF-8 输出，重定向仍使用 UTF-8；GPU 检查脚本使用代码页 65001。
2. CMake 构建 C# 示例时启用 `/optimize+`，与源码项目的 Release 优化一致。
3. 紧凑正向 BGR 一次 `Marshal.Copy`，行填充和负 stride 保留原逐行路径，避免重复边界调用。
4. JSON 和结果表格改为选中对应页签时更新，不再每次更新隐藏的原生控件。
5. 显示 OCR 调用、界面就绪、原生流水线、三阶段及流水线其他耗时；首轮单独标记。不改变原生 JSON 时间字段。

## 本机验证

Windows 10 x64，Ryzen 7 7735H，AMD Radeon(TM) 核显与 RTX 4060 Laptop。
使用包内 `sample.jpg`，500×500、Tiny、DET 上限 960、CLS 开启。每张 GPU 新建引擎，运行 13 次；前 3 次作为预热，后 10 次取中位数。

| GPU | 首轮 OCR 调用 | 预热 OCR 调用 | 预热原生调用 | 预热界面就绪 |
| --- | ---: | ---: | ---: | ---: |
| RTX 4060 Laptop | 132.76 ms | 50.09 ms | 48.67 ms | 82.07 ms |
| AMD Radeon(TM) 核显 | 174.44 ms | 87.69 ms | 85.85 ms | 119.50 ms |

这是 staged 包的本机观测，不是旧/新版严格交替 A/B，也不是 GPU 加速比例。界面就绪包括当前页签更新，不保证屏幕完成绘制。初次计划创建、JIT、功耗和驱动会影响结果。

- 10 项原生 CTest 全部通过。
- GUI 两张 GPU 各 13 次结果，文字、坐标、score、det_score、cls_label、cls_score 逐字段严格相等。
- 紧凑及带填充 BGR、鼠标框选映射、可填写 GPU 编号、JSON / 表格按需显示通过。
- C# 控制台两张 GPU 的 Tiny/Small/Medium 整图及仅识别通过。
- 原生 DLL SHA-256 保持 `0cfd9b5bbc192193b3208392eaee1cdb9e7012202e5f944522a504570ea81894`；保留已有原生正确性与性能资格，不把本次短测称为新的长期泄漏测试。

## 重现

```powershell
python tests/test_winforms.py --package <解压目录> --output build/reports/demo --device 1 --repeat 13
python tests/test_csharp_share.py --package <解压目录> --output build/reports/demo/console.json --devices 0 1 --require-manifest
```

设备编号以本机 probe 枚举结果为准。报告保留每次调用的原生流水线、原生调用、C# 调用、界面就绪耗时。

计时完整定义见 [C# 体验包说明](CSHARP-SHARE-PACKAGE.md#耗时口径与本次-demo-优化)。

English: This revision optimizes the managed demo and fixes console UTF-8. Native inference/model/precision/ABI are unchanged. Measurements are local staged-package observations, not a controlled old/new performance comparison. First-call setup and UI readiness are reported separately. Exact-archive deployment evidence is distributed next to the ZIP.
