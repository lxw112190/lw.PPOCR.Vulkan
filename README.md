# lw.PPOCR.Vulkan

[English](README_EN.md)

C++17 实现的 PP-OCR Vulkan GPU 推理项目，提供原生 C ABI、HTTP/Web 服务与 C# WinForms 示例。
不依赖 OpenCV DNN、ONNX Runtime 或 CUDA **运行时**。

新增 [REC 1/2/4 路实验报告](docs/REC-LANES-EXPERIMENT.md)：同一批 100 张图、三模型、正反两轮共 12,000 次调用，预测字段完全一致，但 2/4 路在 RTX 4060 上未超过已验证单路，且增加专用 GPU 内存。**不接入默认、不替换部署包**；研究代码仅通过默认 OFF 的 `LWVK_EXPERIMENTAL_REC_LANES` 构建选项启用。

作者：天天代码码天天<br>
QQ：819069052<br>
群名称：天天代码码天天<br>
群号码：264292622

## 当前阶段：0.6.0-dev.2 / v1.0 测试加固

新增 [ASan/UBSan 与异常恢复门禁](docs/SANITIZERS.md)：统一插桩全部原生目标，独立 Linux CI 校验 ASan/UBSan/LeakSanitizer 确实启用，测试安装后的 C ABI 与 HTTP 服务；增加 2000 次图像异常输入与恢复、200 次 HTTP 异常输入，以及 11 个旋转/宽高比/灰度/低对比度正确性场景。后者仍是同一张公开样图的派生集，不是多来源真实图片测试集。Linux sanitizer 结果须以推送后的 CI 为准；本机 Windows 普通构建不能证明无泄漏。

[dev.2 本机验收](docs/LOCAL-VALIDATION-062.md)：23 项主机测试、双显卡三模型共 66 个场景组合、双卡图像精确探针、原生 ABI 与 HTTP smoke，以及 209 次异常 HTTP 请求后的真实 REC 恢复通过；附原始 JSON 报告。没有复用旧版本长测作为新二进制的无泄漏证明。

新增可自动验收的 v1 **候选**契约：C ABI 的 19 个导出/结构布局基线、HTTP OpenAPI、配置与响应 JSON Schema、访问日志 JSONL Schema，以及契约哈希检查。配置执行 Schema/原生程序对照测试；HTTP 实测响应和日志严格校验。CI 包含 `schemas/` 与逐文件 `PACKAGE-MANIFEST.json`，压缩后再次验证附件及所有文件的 SHA-256。候选契约不等于正式冻结或 LTS；详细路线和未完成门槛见 [v1.0 发布验收](docs/RELEASE-GATES.md)。
本机 [0.6 验收记录](docs/LOCAL-VALIDATION-060.md)：21 项主机测试、111 项配置矩阵、AMD/NVIDIA 三模型对拍，以及 NVIDIA 三模型各 1000 次混合尺寸 HTTP 回归通过；RSS/线程/句柄未见无界增长，不据此宣称无泄漏或远程 CI 已通过。

已将 [共享原图与 GPU 前处理/透视裁剪](docs/GPU-DEFAULT.md) 接入**原生默认路径**，C#、Python 与 HTTP 无需额外开启：支持 `shaderFloat64` 的所选 GPU 自动启用，三网络共享 device，原图一次上传，裁剪结果不回读再上传；不支持时保留 CPU 图像处理，网络仍在 GPU。网络 FP32、DET960、预算和公共接口不变。可用 `Start-CSharp-Demo-CPU-Preprocess.bat` 显式关闭作兼容/性能对照。性能依据见 [历史实验报告](docs/GPU-CROP-EXPERIMENT.md)，当前开关规则见默认策略文档。

新增 [三模型 TensorRT 实际项目对比](docs/VULKAN-TENSORRT-100.md)：RTX 4060 上 100 图五轮，TensorRT 四 REC 实例为 15.97/23.12/34.08 ms，默认 Vulkan 为 23.09/35.75/75.67 ms，**当前 TensorRT 更快**；Vulkan 主对比资源开销较低。报告同时提供单实例 TensorRT、RAM/专用与共享显存、进程与整卡 GPU 利用率和 GT 指标，明确 FP16-enabled TensorRT 与 FP32 Vulkan 的流水线差异，不宣称纯后端全面胜负。

此前 [Medium REC 宽 tile 优化](docs/REC-WIDE-OPTIMIZATION.md)：复用输入 tile，扩大 FP32 1×1 投影的输出通道块。相对 `det-vector-opt1`，RTX 4060 的 100 图 Medium 完整调用均值再降 **3.6%**，样例 REC 阶段降约 **6.8%**，完整字段一致。仅符合条件的 NVIDIA REC 自动采用，Tiny/Small 不宣称加速；Small 首遍换图和 AMD 完整调用有退化数据，详见历史报告。FP32、DET960、预算和接口不变，不新增开关。

本轮新增 [DET FP32 向量访存优化](docs/DET-VECTOR-OPTIMIZATION.md)：延续 GPU 前后处理、缓冲区复用和减少搬运的思路，四个 NHWC 通道共用地址计算与向量加载/写回。相对 REC 合批版，RTX 4060 的 100 图 Medium 完整调用均值再降 **4.8%**，连续换图降 **3.9%/4.2%**，完整预测一致；Tiny/Small 基本持平。仅 DET 自动启用，实验中退化的 REC 分派未保留。FP32、DET960、预算和公共接口不变，不新增开关；见报告中的文章借鉴、测试口径与局限。

当前是 **Tiny/Small/Medium Vulkan 完整 OCR 技术验证版**，同时支持完整 OCR 和裁剪文字区域仅识别，尚不是正式生产版：

- Vulkan 设备枚举、明确的设备编号与能力信息；
- C++ 原生运行时 + 实验性 C ABI；
- 复用上游通用、注意力/LayerNorm shader，构建时派生 FP32 版本并嵌入 SPIR-V；
- PP-OCRv6 Tiny DET 的完整 242 节点图在 Vulkan 上执行；
- 动态高度/宽度，NHWC 中间张量、按生命周期复用的工作区；
- 权重一次上传到 GPU 常驻；最多 32 个 LRU 尺寸计划共用一组工作区，arena/IO 合计按预算约束；
- C++ 原生 ONNX protobuf 解析、图规范化，直接加载官方 PP-OCRv6 Tiny/Small/Medium；
- FP32 分块 GEMM、NHWC 连续读取及预排布权重、寄存器分块 1×1 卷积、单消费者仿射/ReLU 融合、融合 LayerNorm/Attention、GPU 贪心 CTC；
- ONNX Runtime CPU 独立对拍、异常输入及重复变尺寸测试。
- Tiny CLS 方向分类、Tiny REC 概率输出与 CPU CTC 文字解码；
- 带字节长度/stride 校验的 BGR 图片仅识别接口、自适应宽度、C#/Python 图片示例。
- DB 检测框、阅读顺序、透视裁剪、竖长区域转横向、可选 CLS/180° 校正和 REC 串联；
- 完整 OCR C ABI、独立结果句柄、坐标/文字/置信度/分阶段耗时，以及裁剪像素上限。
- C# WinForms 测试程序：GPU 下拉选择/手填编号、整图 OCR、鼠标框选仅识别、检测框与耗时展示。
- 单引擎 HTTP 服务与 Web 页面：二进制/Base64、完整/仅识别/批量、API Key、日志、过载保护和服务脚本。

**尚未实现**：CPU 自动回退、
协作矩阵、FP16 性能档。接口尚未冻结；性能需按同图、同模型、同参数和预热条件比较。

ONNX 来源、模型切换与解析边界见 [模型说明](docs/ONNX-MODELS.md)。
本机优化前后、CPU 对照与验证范围见 [0.5 性能与验证报告](docs/LOCAL-PERFORMANCE-050.md)。
100 张带标准答案的生成图片、Tiny/Small/Medium、实际 C/原 DML/Vulkan 三项目的
速度、RAM/GPU 内存和正确率比较见 [三项目 100 图测试报告](docs/THREE-PROJECT-100.md)。
这与使用统一预处理的 DML 对拍器报告不同；原项目批处理/REC 宽度策略有差异，不能只看后端名称作性能归因。
最新[三模型主机流水线优化报告](docs/PIPELINE-OPTIMIZATION.md)：同一 100 图平均耗时 Tiny/Small/Medium 为 41.04/50.53/98.98 ms，完整预测对象与上一轮一致。性能数据对应本机测试条件，不代表所有显卡、所有图片。

新增[GPU DET 前处理实验](docs/GPU-DET-PREPROCESS-EXPERIMENT.md)：将缩放、归一化与输入布局转换合并到 GPU，网络仍为 FP32、DET 上限仍为 960。RTX 4060 上 100 图完整耗时均值下降约 Tiny 18.0%、Small 12.1%、Medium 5.9%，完整结果一致。实验阶段默认关闭（现已接入自动默认），通过 `LWVK_GPU_DET_PREPROCESS=1` 或独立 Demo 实验启动脚本开启；额外要求 `shaderFloat64`，不支持时明确报错。

继续优化：[GPU CLS/REC 前处理与上传实验](docs/GPU-TEXT-PREPROCESS-EXPERIMENT.md)。相对上一版 DET 实验，同一 100 图均值再下降 Tiny **14.6%**、Small **10.3%**、Medium **4.8%**，完整结果逐项一致。新增 `LWVK_GPU_TEXT_PREPROCESS=1`，融合文字行缩放、灰色填充、归一化与 REC 180° 校正，去掉原图上传的临时复制；这两项当时默认关闭（当前默认策略见上文）、额外要求 `shaderFloat64`。完整 C# 包用 `Start-CSharp-Demo-GPU-Preprocess-Experiment.bat` 同时开启，当时普通启动脚本保持 CPU 前处理。

此前[回读内存与预处理优化报告](docs/HOST-TRANSFER-OPTIMIZATION.md)保留为历史对照。

最新[FP32 1×1 卷积优化](docs/POINTWISE-OPTIMIZATION.md)：连续向量写回、小空间卷积专用 kernel 与安全的 SiLU 融合，按形状自动启用，不新增实验开关。相对上一轮，两边开启 GPU 前处理时，100 图均值再降 Tiny/Small/Medium **2.1%/3.0%/3.3%**；RTX/AMD 三模型 27 种原始概率输出逐位一致。当时的默认 CPU 前处理路径也通过回归；保持 FP32、DET960，不以降精度或缩小图片换取速度。

继续[CLS 有界小批量提交优化](docs/CLS-BATCH-OPTIMIZATION.md)：开启文字行 GPU 前处理后，最多 8 行共享一次提交/等待，REC 仍逐行执行；裁剪暂存有界、合计工作区保持原预算，紧预算自动回退串行。相对上一版，RTX 4060 的 100 图均值再降 Tiny/Small/Medium **6.0%/6.3%/2.4%**，完整结果一致。使用现有 `Start-CSharp-Demo-GPU-Preprocess-Experiment.bat` 即可，不新增开关；普通 CPU 前处理启动路径不宣称相同提速。

新增[REC 共享 arena 合并提交优化](docs/REC-BATCH-OPTIMIZATION.md)：最多 8 行共享一块中间张量区、一次提交/等待，保留独立输入和紧凑 CTC 输出，不分配未使用的完整概率回读。相对上一轮 CLS 合批，100 图逐图预热均值再降 Tiny/Small/Medium **3.3%/2.6%/1.2%**；不逐图预热的连续换图首遍下降 **6.2%/4.1%/1.9%**，预测字段一致。仍通过现有 GPU 前处理实验脚本开启，该历史 CPU 前处理基线不宣称相同提速；REC 计算仍逐行、共享工作区受原预算约束。

## 项目原理：C++ 如何使用 Vulkan 推理 OCR

本项目不是将 OCR 请求转发给其他推理框架，而是在 C++ 中加载受支持的 ONNX 模型，
将模型算子组织成 Vulkan Compute 计算任务。Vulkan 是 GPU 计算接口，不是 OCR 模型，也不会自动执行 ONNX。
模型仍决定识别能力；本项目负责模型解析、算子执行、图片前后处理和对外接口。

```text
图片 / 相机 BGR 像素
  → GPU：原图一次上传，DET 缩放、归一化
  → GPU：DET 检测网络，输出文字概率图
  → CPU：DB 后处理、检测框排序、透视系数
  → GPU：透视裁剪、竖长转横向，可选 CLS 前处理与方向分类
  → CPU：方向判断；GPU：REC 旋转采样、缩放、归一化、网络与贪心标签
  → CPU：CTC 去重/去空白、字典映射、文字与坐标结果组装
  → C ABI → C# WinForms / Python / HTTP 与 Web
```

上图为支持 `shaderFloat64` 的设备默认路径；能力不足或显式关闭时保留 CPU 图像处理，不改变 GPU 网络推理。三项 `LWVK_GPU_*_PREPROCESS` 默认 `auto`，`0` 关闭、`1` 强制要求；裁剪需要另外两项均启用。详见 [默认规则](docs/GPU-DEFAULT.md)。CLS/REC 可有界合并最多 8 行提交，但不是 8 行网络同时执行。

原图只上传一次，三网络共享 device，CPU 只传框坐标和方向元数据。为保留原来的插值与 BGR8 舍入语义，仍分为“裁剪成 BGR8”和“网络缩放/归一化”两个 GPU 步骤，不直接合并成一次插值。DB 轮廓、框扩展、排序、方向判断和 CTC/UTF-8/JSON 仍在 CPU。独立仅识别接口不需要原图共享路径。

- **模型加载**：原生 ONNX protobuf 解析器读取图和权重，规范化、转换受支持的算子，并进行经过验证的融合。它仅支持随项目固定的 PP-OCRv6 Tiny/Small/Medium，不是通用 ONNX 推理引擎。
- **GPU 执行**：构建时把计算 shader 编译成 SPIR-V 并嵌入库；运行时创建计算管线、提交命令并通过 fence 等待结果。默认使用 FP32，执行卷积、矩阵运算、注意力等网络计算，不依赖 CUDA。
- **CPU/GPU 分工**：网络推理在 GPU，几何处理和部分解码在 CPU。因此“GPU OCR”不等于整条流水线都在 GPU，也不代表 CPU 开销可以忽略。
- **复用与内存**：权重初始化上传后常驻 GPU；按张量生命周期复用工作区，最多缓存 32 个尺寸计划，共享 arena/输入输出缓冲区。工作区预算不是整个进程或显卡总内存上限，权重、驱动和 CPU 缓冲区另计。
- **仅识别**：客户已有裁剪文字行，或在 Demo 中框选时，只执行 REC 与 CTC，不重新运行 DET/CLS。需要方向校正时应先由调用方处理。
- **设备与部署**：显式选择 Vulkan 设备，同句柄串行调用；不同显卡是否可用取决于实际能力和驱动验证，不承诺所有显卡。无可用设备或 GPU 故障会明确报错，不静默回退 CPU。运行包无需 Python/Vulkan SDK，但仍需要匹配的 Vulkan Loader 和显卡驱动。

## Windows 本地构建

开发机安装 Visual Studio 2022 C++、CMake >=3.20、Python >=3.9、Vulkan SDK
（本地开发使用 1.4.350.0，含 `glslc`）。Python 仅用于生成 shader 和开发测试。
运行 CTest 的 Python 需要 NumPy，可执行 `python -m pip install numpy`。
客户使用编译好的库不需要 Python/Vulkan SDK，但需要支持 Vulkan 1.1 的显卡驱动/loader。

```powershell
cmake -S . -B build/local -G "Visual Studio 17 2022" -A x64
cmake --build build/local --config Release --parallel 4
ctest --test-dir build/local -C Release --output-on-failure
.\build\local\Release\lw-ppocr-vulkan-probe.exe
```

设备编号采用 Vulkan 枚举顺序，不会在内部悄悄把 0 改成“第一张独显”。先枚举，再指定。
软件 Vulkan 的 device_type 为 4，只能验证兼容性，不能证明硬件加速。

## Linux 源码构建

```bash
sudo apt-get install cmake ninja-build g++ python3 python3-numpy libvulkan-dev glslc spirv-tools
cmake -S . -B build/local -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/local --parallel 4
ctest --test-dir build/local --output-on-failure
./build/local/lw-ppocr-vulkan-probe
```

Ubuntu 构建必须安装 `libvulkan-dev`：仅有运行包 `libvulkan1` 或 SDK 的头文件/`glslc`
不代表链接库可用，可能导致 CMake 报 `missing: Vulkan_LIBRARY`。客户运行编译好的包不需要开发包。

Linux 构建/运行需要验证，Windows 结果不能当作 Linux 已通过。国产 Linux/ARM64/macOS
暂不作正式支持承诺；后续按 GPU 驱动、系统基线逐一验证。

## WinForms 快速测试

Windows 包解压后运行 `lw.PPOCR.Vulkan.WinFormsDemo.exe`，不需要 Python 或 Vulkan SDK。
需要 .NET Framework 4.0 或更高的兼容版本、x64 Windows 和支持 Vulkan 1.1 的显卡驱动。
本机验证为 Windows 10；使用 .NET 4.0 **不代表原生 Vulkan 库已支持 Win7**。

1. 顶部 GPU 下拉列表显示编号与名称，也可以直接填写 `0`、`1` 等编号。编号来自实际 Vulkan 枚举顺序；错误编号明确拒绝，不自动换卡或回退 CPU。
2. 点击“初始化 / 重载”。模型和默认测试图片按程序目录定位，与启动时的工作目录无关。
3. 点击“整图 OCR”，左侧绘制原图坐标检测框，右侧显示文字、JSON、置信度；底部显示端到端与 GPU 分阶段耗时。
4. 图片上按住鼠标左键拖动框选，再点击“框选仅识别”：仅运行 REC + CTC，不再次检测，也不执行 CLS。拖动方向不限，缩放显示会映射到原始像素。
5. 修改 GPU、模型目录或阈值等参数后，需要重新初始化。可选择自己的图片、复制文字或保存 JSON。

模型下拉可选择随包的 PP-OCRv6 Tiny/Small/Medium 原生 ONNX；仍支持旧 Tiny 转换格式，不支持任意 ONNX。绘制与框选只改变显示层，
不污染送入推理的原始像素。首次推理包含计划建立开销，不应当作稳态性能。
完整说明和本机验证见 [WINFORMS-DEMO.md](docs/WINFORMS-DEMO.md)。
源码项目：[examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.sln](examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.sln)。

<a href="docs/assets/winforms-demo.png"><img src="docs/assets/winforms-demo.png" alt="C# WinForms Demo：GPU 选择、检测框、识别文字与耗时" width="960"></a>

本轮 `gpu-default1` 完整体验包默认模式实测界面：RTX 4060 Laptop GPU / Tiny 模型，自动启用共享原图、GPU 裁剪和 DET/CLS/REC 前处理，展示整图检测与鼠标框选。截图中的耗时仅为该次重复调用示例，不代表所有设备或图片的性能，也不是 Medium 的基准结果。点击图片查看原图。

### Demo 中的耗时如何理解

| 显示项 | 计时范围 |
| --- | --- |
| OCR 调用 | C# 图片转 BGR、原生 OCR 调用、结果 JSON 复制、UTF-8 解码与 C# JSON 解析；不含模型初始化或此前的图片文件加载 |
| 界面就绪 | 从点击开始后的处理入口，到图片克隆、后台任务、OCR 调用及界面结果数据更新完成；不是显示器实际完成绘制的时间 |
| 流水线 | 原生 OCR 内部，从同句柄锁等待开始，经前处理、推理、后处理到结果对象组装；不含最终 JSON 序列化及跨 ABI 结果复制 |
| DET / CLS / REC | 对应网络各次执行耗时的累计值，包含输入上传、命令提交、GPU 等待与结果回读；不是纯 GPU shader 执行时间，不包含执行计划准备 |
| 其他 | `max(0, 流水线 − DET − CLS − REC)`，是差额汇总，不是独立测量的一道工序 |

“其他”主要包括 CPU 缩放/归一化/张量排列、DB 与轮廓/框扩展/阅读排序、文字行透视裁剪与旋转、
CLS/REC 输入预处理、回读标签后的 CTC 去重/去空白/字典映射、结果组装，以及执行计划准备、工作区分配和锁等待。
因此不应要求流水线等于 `DET + CLS + REC`；上传、GPU 等待和回读已经算在网络耗时里，不会再次计入“其他”。

GPU 裁剪实验中，“其他”还包含 GPU 裁剪的命令准备、提交与等待，以及 CPU 的框映射/方向判断；它不再等同于纯 CPU 时间。原图上传耗时计入 DET，CLS/REC 的网络缩放仍计入相应阶段，执行计划准备仍属于流水线开销。
C# 图片转 BGR、JSON 复制/解析和界面更新也不属于这个“其他”。

首轮或新尺寸可能建立计划、扩大工作区，不能当作预热后的稳态速度。耗时按一位小数显示，手工相加可能有舍入差异。
比较性能时应固定图片、模型、设备、参数和计时口径，分别记录初始化、首轮与重复调用。

## HTTP / Web 快速使用

修改 `http-service.json` 的 `device_index` 为设备探测程序实际编号，Windows 运行 `run-http-service.bat`，
Linux 运行 `./run-http-service.sh`，访问 `http://127.0.0.1:8787/`。
网页加载测试图或选择图片，显示检测框、识别文字、JSON 和耗时；仅识别请上传已裁剪文字行。

`api_key` 非空时，接口必须带 `X-API-Key` 请求头，网页也填写相同值；空值关闭认证。
`LWVK_API_KEY` 可覆盖配置，日志/启动输出不显示密钥。默认监听本机，不要无认证暴露公网。
程序启动显示作者、QQ、配置参数与实际 GPU。模型、网页和启动/服务管理脚本均随包提供。

详细接口、队列/解码/批量限制、超时原理、日志和 Windows/Linux 服务安装见 [HTTP-SERVICE.md](docs/HTTP-SERVICE.md)。
两卡接口测试与 1000 次长测见 [LOCAL-HTTP-REPORT.md](docs/LOCAL-HTTP-REPORT.md)。

## 运行真实模型对拍

```powershell
python -m pip install -r requirements-dev.txt
python tests/test_det_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 0 --iterations 100 --report build/reports/device0.json
python tests/test_det_reference.py --library build/local/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 100 --report build/reports/device1.json
```

测试与运行时使用同一份输入 FP32 张量，隔离图片解码/缩放差异。对照模型固定 SHA-256。
测试比较概率图绝对误差、0.2 阈值图 IoU、反复运行的一致性，并记录 RSS 趋势。
独立对拍依赖使用 Python >=3.10（CI 为 3.12）。本机结果见 [LOCAL-DET-REPORT.md](docs/LOCAL-DET-REPORT.md)。
CLS/REC 的两卡对拍、CTC、并发及重复测试见 [LOCAL-TEXT-REPORT.md](docs/LOCAL-TEXT-REPORT.md)。
完整 OCR 两卡测试见 [LOCAL-OCR-REPORT.md](docs/LOCAL-OCR-REPORT.md)。完整流程使用独立 ORT CPU
模型、NumPy 预处理/CTC，但共享经过复用的 C 几何代码；不能称为独立 DB 算法对拍。
RSS 不是无泄漏证明；仍需要 validation layer、ASan/UBSan 以及长时间实体机测试。

`models/ppocrv6-tiny/det.json` 和 `weights.bin` 是内部 v0 格式，随开发可调整。
这是保留兼容的旧 Tiny 转换格式；新默认路径直接使用 `models/onnx/ppocrv6-tiny` 中的 ONNX 与字典，不需要转换。旧格式重新生成：

```powershell
python scripts/export_det.py --input models/ppocrv6-tiny/det.onnx --output models/ppocrv6-tiny
python scripts/export_text_models.py --input models/ppocrv6-tiny/cls/source.onnx --output models/ppocrv6-tiny/cls --task cls
python scripts/export_text_models.py --input models/ppocrv6-tiny/rec/source.onnx --output models/ppocrv6-tiny/rec --task rec --dictionary models/ppocrv6-tiny/rec/dictionary.txt
```

## API 与资源约束

见 `include/lw_ppocr_vulkan.h`、`examples/python/lwvk.py`。

| 能力 | 输入 | 输出 |
| --- | --- | --- |
| DET | FP32 NCHW `[1,3,H,W]`，H/W 为 32 的倍数且在 32..960 | 检测概率图，尚不输出检测框 |
| CLS | FP32 BGR `[-1,1]`，`[1,3,80,160]` | `[1,2]` 方向概率；0=正常、1=180° |
| REC | FP32 BGR `[-1,1]`，`[1,3,48,W]`，W 为 8 的倍数且在 32..960 | `[T,C]` 概率或 UTF-8 文字/置信度；Tiny C=6906，Small/Medium C=18710 |
| 仅识别 BGR | 已裁剪的一行文字，BGR8 + 宽高 + 正 stride + 缓冲区字节数 | 原生预处理 + GPU REC/贪心 CTC + CPU 字符拼接 |
| 完整 OCR BGR | 整张 BGR8 图像 + 宽高 + 正 stride + 缓冲区字节数 | 检测框、文字、置信度、CLS 与耗时 JSON |

所有接口带缓冲区长度，错误不会跨 C ABI 抛异常。`lwvk_network_shape` 返回输出行数/类别数，
可能建立执行计划但不会提交推理；输出长度单位为 float 元素数。
`lwvk_recognize_tensor` / `lwvk_recognize_bgr` 的文字长度单位为 UTF-8 字节，包含末尾 NUL。
文字缓冲区不足返回 `LWVK_BUFFER_TOO_SMALL`，不返回截断文字。

## 整张图片：完整 OCR

Python 图片示例需要 `python -m pip install numpy pillow`（仅示例依赖，DLL 不需要）。

```powershell
python examples/python/ocr_image.py --library build/local/Release/lw.PPOCR.Vulkan.dll --models models/onnx/ppocrv6-tiny --image test-images/sample.jpg --device 1 --draw build/ocr-boxes.png
```

不需要方向分类时增加 `--no-cls`。结果 `items` 只输出一份四点坐标 `x1/y1` 到 `x4/y4`，
以及 `text`、`score`、`det_score`、`cls_label`、`cls_score`；不重复输出 `box`。
坐标在原图坐标系，顺序为左上、右上、右下、左下。未启用 CLS 时 label=-1、score=0。
这是文本行 OCR，不是表格/版面/PDF 解析；识别仍可能出现错字。

C/C# 调用先使用 `lwvk_ocr_config_default`，然后 `lwvk_ocr_create`、`lwvk_ocr_run_bgr`。
结果句柄独立于引擎：查询 JSON 大小、复制到调用方缓冲区，再 `lwvk_ocr_result_destroy`；
查询/复制不重复推理。不得在运行中销毁引擎，也不得在复制中销毁结果。
详细默认参数、资源边界和耗时口径见 [OCR-API.md](docs/OCR-API.md)。

## 已裁剪文字区域：直接识别

客户已完成裁剪时使用 `lwvk_recognize_bgr`，**不会再次检测，也不会自动方向分类**。
原始图像不是 JPEG 字节流；必须是 BGR8 像素。仅支持正 stride，至少需要
`(height-1)*stride + width*3` 字节。源图限制 4000 万像素。
`rec_width=0` 自动选取 32..960 的 8 倍数；保留比例、右侧填充归一化的 128 灰度。
极长文字可能因 960 上限被压缩，建议调用方合理分行。GPU 耗时不包含图片解码、CPU 预处理或最终字符拼接。

下面从示例图片**手动指定**标题 ROI（不是自动检测框），调用原生 BGR 识别路径：

```powershell
python examples/python/recognize_image.py --library build/local/Release/lw.PPOCR.Vulkan.dll --model models/onnx/ppocrv6-tiny/rec.onnx --image test-images/sample.jpg --roi 20 28 292 46 --device 1
```

本机示例输出：`纯臻营养护发素`。客户自己的裁剪图片省略 `--roi` 即可。
原生 C ABI 和 C# 也能直接传相机得到的 BGR 缓冲区，不需要先编码 JPEG/Base64。

同句柄调用串行；销毁句柄前必须等待所有调用结束。不同句柄各自拥有设备和工作区。
默认工作区上限为**每张模型图 512 MiB**，完整 OCR 最多 3 张图；这是按需分配的上限，不是启动时预留 512 MiB。
该预算包括共享 arena、输入 staging、概率及 CTC 读回缓冲区；模型权重、CPU 图像/几何和 driver 分配另计。
WinForms 可设置每模型工作区上限（MiB，0=默认）；DET 长边默认保持 960，不以缩小检测输入规避内存或伪造提速。
详见 [工作区修复、DML 基准与后续优化](docs/WORKSPACE-PERFORMANCE.md)，其中耗时属于当时的历史基线。
新增[算子计时工具与第二轮 FP32 优化报告](docs/GPU-PROFILING.md)，诊断默认关闭；大图仍需继续优化。
第三轮[安全 GELU 融合与完整 OCR 对拍](docs/GELU-FUSION.md)记录此前 Medium 大图仍慢于 DML 的结果；第四轮[回读/主机优化](docs/HOST-TRANSFER-OPTIMIZATION.md)继续改善默认 FP32 路径，不缩小 DET 960 或放宽精度门槛。
第五轮[三模型主机流水线优化](docs/PIPELINE-OPTIMIZATION.md)优化 CLS/REC 缩放和透视裁剪；RTX 4060 同图交替测试 Tiny 耗时降低 20～24%、Small 约 16%、Medium 约 9～10%，100 图完整预测对象不变，其他显卡不保证同等收益。
新增[协作矩阵工程实验](docs/COOPERATIVE-MATRIX-EXPERIMENT.md)，默认关闭；Medium 大图分数门槛未过，不能替换已验证部署包。
显式 Vulkan 失败直接报错，不静默改用 CPU。等待 GPU fence 的 30 秒保护不是 GPU 任务取消；
设备丢失/超时后不能继续复用该计划，应结束处理并重建服务/句柄。

## 复用与许可证

项目采用 Apache-2.0；第三方来源、固定提交和修改说明集中见 [NOTICE](NOTICE)，完整许可证保留在 `licenses`。
nlohmann/json 为 MIT，模型保留 PaddleOCR 来源说明与 Apache-2.0 许可证。
复用上游不代表上游为本项目背书，也不能照搬其精度/性能结论。

## CI 与技术验证包

Windows CI 固定并校验官方 Vulkan SDK 1.4.350.0 下载，缓存安装器下载；构建、检查 C ABI、
测试安装目录、WinForms 布局/框选映射，并生成 zip + SHA-256。WinForms CI 主机测试不执行 GPU OCR，
无实体 GPU 时明确跳过硬件测试，不把跳过算作通过。
Linux CI 缓存校验后的 SDK，使用 lavapipe 软件 Vulkan 对拍，生成 tar.gz + SHA-256。
软件 Vulkan 对拍包含 DET、CLS、REC、CTC、BGR stride/长度及同句柄并发检查。
完整 OCR 软件对拍显式使用 CPU 图像前处理，网络仍由 Vulkan 执行；GPU DET/CLS/REC 前处理与透视裁剪另用独立着色器探针检查精确位值、stride、补边和恢复。这样避免共享 runner 上 8 行软件 REC 合批超过单次 30 秒 fence 等待；不是关闭网络对拍或修改硬件 GPU 默认行为。
Medium 在软件 Vulkan 上连单个网络提交也可能超过 30 秒，因此 Linux CI 显式设置 `LWVK_SOFTWARE_GRAPH_TIMEOUT_MS=180000`（单次网络 fence 等待 180 秒）。此工程选项只对 Vulkan `device_type=4` 的 CPU 软件设备生效，允许范围为 30000～300000 毫秒；未设置仍为 30 秒，独显、集显及虚拟 GPU 始终保持 30 秒。打包不会将 CI 环境变量写入客户配置。它不是整个 OCR 请求的截止时间，更不代表超时会取消已提交任务；模型、精度比较和失败后的 poisoned 处理均保持不变。
并增加完整 OCR、DB/crop 单元测试、裁剪资源上限和独立结果生命周期测试。
本地检查不等于远程 CI 成功；当前修改仍须推送后确认 Actions 结果。

本地打包（确认测试通过后）：

```powershell
cmake --install build/local --config Release --prefix dist/staging
python tests/test_api.py --library dist/staging/lw.PPOCR.Vulkan.dll
python scripts/package.py --staging dist/staging --output dist --platform windows-x64
```

包内含完整 OCR 库、HTTP 服务与 Web、设备探测程序、WinForms 程序、DET/CLS/REC 模型、字典、示例、测试图片和文档。
Windows 下载包解压后，可直接用下面的 C# 控制台程序验证，不需要 Python 或 Vulkan SDK。
Windows 使用静态 MSVC 运行库；已检查 DLL 的直接依赖仅有 `vulkan-1.dll` 和 `KERNEL32.dll`。
另有[可转发的 C# 完整体验包](docs/CSHARP-SHARE-PACKAGE.md)，带三模型、官方 x64 loader、可选 Runtime 安装器和 C# 源码；仍需目标电脑安装匹配显卡驱动。
最新 [Demo 计时与展示优化说明](docs/DEMO-TIMING-OPTIMIZATION.md)区分 OCR 调用、原生流水线与界面就绪时间，记录 UTF-8 修复、按需展示和本机重复调用验证。
Vulkan loader/显卡驱动应由显卡厂商安装，不能仅复制开发机的驱动文件。
构建机找到 .NET Framework C# 编译器时，包内同时提供 x64 控制台示例：

```powershell
.\lw-ppocr-vulkan-probe.exe
.\lw.PPOCR.Vulkan.CSharpDemo.exe models/ppocrv6-tiny/det.json 1
.\lw.PPOCR.Vulkan.CSharpDemo.exe --recognize models/ppocrv6-tiny/rec/model.json test-images/sample.jpg 1 20 28 292 46
.\lw.PPOCR.Vulkan.CSharpDemo.exe --ocr models/ppocrv6-tiny test-images/sample.jpg 1
```

请把 `1` 改成探测程序输出的设备编号；上述是控制台示例，图形界面运行 `lw.PPOCR.Vulkan.WinFormsDemo.exe`。
识别示例中的 ROI 由调用方给出；`Bitmap` 按行复制，避免填充/负 stride 穿过 ABI。
使用例子：

```powershell
python examples/python/detect_image.py --library build/local/Release/lw.PPOCR.Vulkan.dll --model models/ppocrv6-tiny/det.json --image test-images/sample.jpg --device 1 --output build/det-map.png
```

兼容证据见 [COMPATIBILITY.md](docs/COMPATIBILITY.md)，开发边界见 [ROADMAP.md](docs/ROADMAP.md)。

下一阶段：验收本轮 ASan/UBSan CI、继续增加实际公开正确性样本 → SBOM/供应链和兼容性审计 → 最终包长测/实体机部署验收 → RC → v1.0。正式发布不以超过 DML/TensorRT 或支持所有平台为前提，也不把软件 Vulkan 测试当作硬件 GPU 验收。

开发目录、格式化与关键设计说明见 [开发指南](docs/DEVELOPMENT.md)。

## 捐赠支持

如果项目对你有帮助，欢迎自愿扫码支持开源维护。感谢你的使用、反馈和支持！

<a href="www/sponsor.jpg"><img src="www/sponsor.jpg" alt="微信捐赠二维码" width="240"></a>
