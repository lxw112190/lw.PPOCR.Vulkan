# 共享原图与 GPU 透视裁剪实验

> 历史实验记录：以下“默认关闭”指该实验候选发布时的行为。随后已接入能力感知的原生默认路径，当前规则见 [GPU-DEFAULT.md](GPU-DEFAULT.md)。原始 DLL 哈希、性能数字与测试条件保持不变。

2026-10-02，本地 Windows x64 / RTX 4060 Laptop 与 AMD Radeon(TM) Graphics。版本仍为 `0.5.0-dev.2`，不是正式生产版。默认路径不变；新增第三个独立实验开关。

## 本轮做了什么

同一整图 OCR 句柄的 DET、CLS、REC 共享一个 Vulkan context/device。原图只上传一次，检测后 CPU 计算框和透视映射系数，GPU 从常驻原图生成文字行。裁剪结果保留在 GPU，CLS/REC 只上传少量方向元数据，不把裁剪像素下载回 CPU 再上传。

| 步骤 | 本实验执行位置 | 保留在 CPU 的部分 |
| --- | --- | --- |
| 图片解码 / 相机 BGR 输入 | 解码仍为 CPU；BGR 原图一次上传 | 文件/HTTP 解码及输入检查 |
| DET 缩放、归一化、布局、检测网络 | GPU | 尺寸规划 |
| DB 二值化、轮廓、框扩展、排序 | CPU，未迁移 | 全部 DB 后处理 |
| 透视裁剪、竖长区域转横向 | GPU | 框坐标与映射系数 |
| CLS 缩放、归一化、分类 | GPU | 根据分数决定方向 |
| REC 180° 采样、缩放、补边、归一化、网络和贪心标签 | GPU | 自适应宽度规划 |
| CTC 去重/去空白、字典、UTF-8、JSON | CPU，未迁移 | 结果组装 |

仍保留两个 GPU 图像步骤：“透视插值 → BGR8 舍入”与“网络缩放 → FP32 输入”。直接合成一次插值会改变 CPU 参考的中间量化语义，本轮没有用这种方式换取速度。图像插值使用 FP64，网络仍为 FP32；未修改模型、DET 长边 960 或公共 C ABI。

仅识别接口不执行 DET/CLS，也不需要共享原图裁剪。Demo 的鼠标框选仍由 C# 获取 ROI，然后调用仅识别接口；这轮新裁剪路径主要优化整图 OCR。

## 开启与回退

完整 C# 体验包：关闭已有 Demo，使用 `Start-CSharp-Demo-GPU-Crop-Experiment.bat`，选择实际 GPU 编号和模型后初始化。

HTTP / Python / 自己的宿主程序应在新进程加载 DLL 前设置：

```powershell
$env:LWVK_GPU_DET_PREPROCESS="1"
$env:LWVK_GPU_TEXT_PREPROCESS="1"
$env:LWVK_GPU_CROP_PREPROCESS="1"
```

缺少前两项时明确拒绝；额外要求 `shaderFloat64`。无此能力或 GPU 故障不静默回退 CPU。关闭 profiling 后再做性能测试。

普通 `Start-CSharp-Demo.bat` 明确关闭三项实验；原有 DET-only / GPU-preprocess 脚本也明确关闭新的 crop 开关。直接双击 EXE 会继承环境。环境不是热切换配置，Windows 静态 CRT 也可能保留 DLL 加载时的环境快照；应重新启动进程，不只重新初始化句柄。

## 资源与同步边界

- 原图 staging/device 缓冲区、最多八个 GPU 裁剪槽与映射元数据有界，按需分配。小裁剪槽以 64 KiB 为增量保留容量，超预算时回到本次必要容量。
- 原有每图工作区预算不放宽：共享 staging/source/crop/metadata 的逻辑字节数保守地从每张图预算中扣除，再给网络 arena/IO 分配。物理上共享缓冲区只存在一份，并非复制三份。权重、驱动与实际内存分配对齐另计；这不是 RSS/VRAM 总上限。
- 原图仅复制当前图片有效字节，不搬运历史大图容量；上传复用命令池/fence。共享缓冲区扩容时只使引用它的命令失效，保留无关尺寸计划和网络工作区。
- 图像/工作区资源超限后释放共享图像高水位，让后续小图可以恢复；不把资源拒绝标为设备损坏。
- 一个 OCR 句柄整体串行，保护共享 queue、pipeline 缓存及借用的裁剪槽；不同句柄各有自己的 context。八行合批不等于 batch-N 并行网络。
- 显式屏障连接上传、GPU 裁剪与网络读取。等待 GPU 的 30 秒保护不是任务取消；提交/等待失败后拒绝复用，销毁前等待设备空闲。未做设备丢失注入验证。

## 性能：相对上一版 rec-wide-opt1

基线 DLL：`056b9b567a9b40b69407591a0ebc66b284ccd26835b4e289c0d05c4255393a6d`。

本轮 DLL：`518073024b6d39cc18b6357b3475c0015ae3a6bea732b214e4753efc1daeafa6`。

两边均开启已有 DET/CLS/REC GPU 前处理；只有新 DLL 执行 crop 开关。单一 GPU 工作负载，FP32、DET960、CLS 开启，旧/新交替顺序。完整调用计时包含 BGR 输入后的原生 OCR、JSON 复制和 Python 解析，不含图片解码、模型初始化和 GUI。

100 张生成图片逐图预热一次、各测两次的完整均值：

| 模型 | 旧版 ms | 新版 ms | 耗时下降 |
| --- | ---: | ---: | ---: |
| Tiny | 19.674 | 17.718 | 9.9% |
| Small | 33.560 | 31.499 | 6.1% |
| Medium | 73.097 | 71.048 | 2.8% |

固定样例预热三次、交替测 30 次的中位数：

| 模型 | 500×500 旧 → 新 ms | 下降 | 1000×1000 旧 → 新 ms | 下降 |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 23.536 → 20.824 | 11.5% | 41.240 → 31.991 | 22.4% |
| Small | 42.612 → 39.481 | 7.3% | 67.218 → 56.801 | 15.5% |
| Medium | 95.730 → 92.631 | 3.2% | 137.522 → 128.107 | 6.8% |

Medium 的主要成本仍是 REC 网络，本轮没有宣称三模型网络本身大幅加速；收益主要来自裁剪和搬运链路。“其他耗时”现在还包含 GPU 裁剪的准备、提交/等待，不是纯 CPU 时间。原图上传计入 DET，CLS/REC 网络前处理仍计入相应阶段。

不逐图预热的连续 100 图换图：首遍 Tiny/Small/Medium 为 24.650→24.086、41.252→40.974、81.375→81.381 ms（下降 2.3%/0.7%/约 0%）；反向遍历第二遍为 23.413→21.978、39.003→37.772、80.369→79.645 ms（6.1%/3.2%/0.9%）。**Medium 首轮基本持平，不能宣称所有换图场景都加速。**

AMD 核显另做 500 图样例预热三次、交替 15 次：Tiny 63.582→58.864 ms（7.4%）、Small 181.849→175.426 ms（3.5%）、Medium 642.329→638.735 ms（0.6%，不视为明显收益）。只覆盖该样例，不替代 AMD 大图/连续流测试。

普通启动三项关闭另测三模型各 15 次：结果一致，耗时下降指标约 -1.2%/+0.9%/-0.4%，不宣称默认路径提速，也不隐藏 Tiny 的小幅变慢。原 GPU-preprocess 入口关闭 crop 后各测 10 次，指标约 -0.5%/+0.5%/0%。这些数据保存在报告目录，不应套用新实验的百分比。

最初候选在共享容量变化时清空全部网络缓存，首轮换图分别慢约 55%/27%/23%；已删除该策略，保留反例报告，不能把固定样例加速当作所有输入都快的证据。另一处历史大图容量全量复制也已改为仅复制本次有效字节。

## 验证与局限

三模型 100 图及十组变化输入（含 2448×3264、旋转、空白）、填充 stride、短缓冲区拒绝、40 尺寸/LRU、原图预算不足后恢复、网络预算受共享高水位挤压后恢复、关闭 CLS、独立句柄隔离均通过，完整预测字段与上一版一致。

独立 ONNX Runtime CPU 完整 OCR 参考覆盖三模型，使用独立 NumPy 前处理/CTC；DB/crop 几何参考沿用已有 C 实现及 golden 单元测试，不声称完全独立几何算法。原 Medium 报错的私有 2448×3264 图按 DET960 重复五次，85 区域，报告只保存哈希/尺寸，不记录客户文字。

RTX/AMD 像素探针各 111 组逐字节一致，覆盖竖长旋转、投影四边形、边界夹取、非整齐 stride、单像素、八槽、大图转小图的尾部保护及异常恢复。两卡探针与三模型整图、RTX 紧预算恢复显式开启 Khronos 同步验证，日志确认生效且无 VUID/SYNC-HAZARD；不等于 GPU-assisted 越界检测。CTest 13/13、176 项固定资产和 C++ 格式检查通过。HTTP 二进制/Base64、整图/仅识别/批量、API Key、429/503、日志隐私及恢复通过。

最终资格与原始报告见 [reports/gpu-crop-experiment](reports/gpu-crop-experiment)。保持公共头文件及 19 个导出，不新增部署依赖。普通路径、同步验证、HTTP 和完整 C# 包各自验证，不能借用旧包结果代替新包。

完整 C# 包使用自带官方 Vulkan loader，去掉 SDK/开发 PATH、从包外目录运行，两卡三模型的整图与 ROI 通过；WinForms 验证 GPU 选择/手填编号、框选坐标映射、重复调用与按需 JSON/网格展示。最终 ZIP 解压后的逐文件校验和验证报告另放 ZIP 旁，安装的显卡驱动与 .NET Framework 仍是必要条件，不是干净虚拟机验证。

本轮不是长期 soak、无泄漏证明、跨系统或全显卡兼容认证；未重新做 C/DML 三项目速度/正确率对照，也没有新的 DML 超越结论。预览包不作为 1.0 冻结承诺。下一轮优先测真正的 REC 网络热点，DB/CTC/字典仍按收益与维护成本决定是否迁移。

## 复现

```powershell
python tests/test_gpu_crop_contract.py --library build/gpu-crop/Release/lw.PPOCR.Vulkan.dll --report build/gpu-crop/crop-contract-final.json
python tests/benchmark_vulkan_pair.py --before build/rec-wide-v2/Release/lw.PPOCR.Vulkan.dll --after build/gpu-crop/Release/lw.PPOCR.Vulkan.dll --iterations 30 --report build/gpu-crop/sample-qualified.json
python tests/benchmark_ocr_stream.py --before build/rec-wide-v2/Release/lw.PPOCR.Vulkan.dll --after build/gpu-crop/Release/lw.PPOCR.Vulkan.dll --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/gpu-crop/stream-qualified.json
```

先设置上述三项开关，再执行命令；设备编号按本机实际选择，测试串行运行。像素探针和独立 CPU 参考不进入客户运行依赖。
