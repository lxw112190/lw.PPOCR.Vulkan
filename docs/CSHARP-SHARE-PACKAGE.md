# lw.PPOCR.Vulkan C# 完整体验包

作者：天天代码码天天；QQ：819069052。
群名称：天天代码码天天
群号码：264292622

这是 Windows x64 技术预览体验包，包含本轮已验证的 FP32 优化 DLL，方便直接转发给其他人测试。不是 1.0 正式版，也不承诺所有显卡、所有图片性能一致。

## 三步开始

1. **完整解压 ZIP** 到本地文件夹，不要在压缩软件里直接运行 EXE，不要只复制一个 EXE。
2. 双击 `Start-CSharp-Demo.bat`，或直接双击 `lw.PPOCR.Vulkan.WinFormsDemo.exe`。
3. 选择 GPU 和 Tiny/Small/Medium 模型，点击“初始化”，再点击“整图 OCR”。已默认载入包内测试图片；也可以打开自己的图片。在图片上用鼠标框选文字区域后点击“框选仅识别”。

双显卡电脑建议手动选 NVIDIA/AMD 独显；**设备编号以目标电脑实际枚举为准**，不要照抄开发机编号 1。切换 GPU、模型或参数后需要重新初始化。默认 DET 长边上限 960、每张模型图工作区预算 512 MiB，按需分配；Medium 更占内存。

新增 GPU DET 前处理实验：关闭已有 Demo，双击 `Start-CSharp-Demo-GPU-DET-Experiment.bat`，再选 GPU/模型初始化。普通 `Start-CSharp-Demo.bat` 保持默认 CPU 前处理；直接运行 EXE 会继承当前进程环境。实验额外要求 `shaderFloat64`，前处理用 FP64、网络仍用 FP32；不支持时明确报错。结果和计时口径见包内 `docs/GPU-DET-PREPROCESS-EXPERIMENT.md`。

本轮继续增加 CLS/REC 前处理实验。关闭已有 Demo，使用 `Start-CSharp-Demo-GPU-Preprocess-Experiment.bat` 可同时开启 DET＋CLS＋REC GPU 前处理（含 REC 180° 采样）。相对上一版 DET-only 实验，RTX 4060 的 100 图均值再下降 Tiny/Small/Medium 约 14.6%/10.3%/4.8%，完整结果一致。报告见包内 `docs/GPU-TEXT-PREPROCESS-EXPERIMENT.md`。

| 启动脚本 | DET 前处理 | CLS/REC 前处理 |
| --- | --- | --- |
| `Start-CSharp-Demo.bat` | CPU | CPU |
| `Start-CSharp-Demo-GPU-DET-Experiment.bat` | GPU | CPU |
| `Start-CSharp-Demo-GPU-Preprocess-Experiment.bat` | GPU | GPU |

实验开关应在新进程加载 DLL 前设置；脚本使用局部环境，不修改系统配置。两种实验都需要 `shaderFloat64`，不支持时可关闭 Demo 后用普通启动脚本重新启动。默认算法流程示意见 README；开启实验后，缩放/归一化和 REC 180° 校正转到 GPU，裁剪/DB/字典映射仍在 CPU。

最新 `network-opt1` 包继续优化 FP32 1×1 卷积（向量写回、小空间 kernel 与安全的 SiLU 融合），按形状自动启用。普通启动和 GPU 前处理实验都受益，不需要新增开关。相对上一轮 GPU 前处理包，RTX 4060 的 100 图均值再下降约 2.1%/3.0%/3.3%；模型、FP32 和 DET960 不变，详情见包内 `docs/POINTWISE-OPTIMIZATION.md`。

## 对方电脑需要什么

- Windows 10/11 **64 位**；本机实测 Windows 10，Windows 11 是目标平台，尚未独立实机验证。
- .NET Framework 4.0 或更高版本；Demo 以 .NET Framework 4.0/x64 编译，不需要 .NET 8/9、Visual Studio、Python、Vulkan SDK 或 CUDA。
- 支持 Vulkan 1.1 及本项目 compute 能力要求的显卡，且已安装匹配的 NVIDIA/AMD/Intel 厂商驱动。当前实体测试覆盖 AMD Radeon(TM) 核显与 NVIDIA RTX 4060 Laptop；Intel 等其他硬件需要对方测试。

**包内 DLL 完整，不等于可以免装显卡驱动。** `vulkan-1.dll` 是通用 loader，需要通过目标电脑安装的驱动/ICD 使用实际 GPU；不能复制开发机 NVIDIA/AMD 驱动 DLL 来替代驱动安装，也不会静默切换到 CPU。

本包已附带官方 LunarG **1.4.350.0 x64** `vulkan-1.dll`，应用根目录即可加载；附带其原始 MIT/Apache-2.0 声明与官方 Runtime 安装器。该便携测试方式避免 loader 缺失，但应用私有 loader 不会随系统驱动自动更新。长期部署建议使用系统 driver/runtime；有新驱动/兼容问题时，先关闭 Demo，**把包根目录的 `vulkan-1.dll` 移到包外备份**，再使用已安装的系统 loader。不要覆盖 `C:\Windows\System32`。

Khronos 更推荐分发 Runtime 安装器，避免长期绑定应用私有 loader，详见 [官方 loader 分发说明](https://vulkan.lunarg.com/doc/view/1.4.350.0/windows/LoaderApplicationInterface.html#bundling-the-loader-with-an-application)。`prerequisites/VulkanRT-X64-1.4.350.0-Installer.exe` 是可选官方安装器，本包和启动脚本**不会自动执行安装或修改系统**；需要时由使用者主动运行、按界面完成。它不包含显卡厂商驱动。

## 包里有什么

- `lw.PPOCR.Vulkan.WinFormsDemo.exe` / `.exe.config`：图形测试程序。
- `lw.PPOCR.Vulkan.CSharpDemo.exe`：C# 控制台对接示例。
- `lw.PPOCR.Vulkan.dll`：OCR 原生库，静态 MSVC runtime；无需额外 VC++ Redistributable。
- `vulkan-1.dll`：官方 x64 Vulkan loader；其传递依赖为 Windows 系统 DLL，不复制系统库。
- `models/onnx/ppocrv6-tiny|small|medium`：三套 DET/CLS/REC 官方来源 ONNX、字典和 catalog。
- `test-images/sample.jpg`：默认 OCR 验证图片；不包含客户的私有证件图片。
- `Check-GPU.bat` / `lw-ppocr-vulkan-probe.exe`：枚举 GPU、检查能力。
- `examples/winforms`：可打开的 C# `.sln` / `.csproj` 与完整源码；`examples/csharp/Program.cs` 为控制台示例源码，`sdk/include` 含 C ABI 头文件。
- `docs`、`licenses`、`NOTICE`、`PACKAGE-INFO.json`、`sbom/csharp-demo.cdx.json`、`FILES.sha256`：使用说明、许可证、来源/依赖及逐文件校验。
- `prerequisites`：可选 Runtime 安装器及原始许可证。

仅保留实际 C# Demo 运行依赖，**不混入测试专用的 `onnxruntime.dll`、`DirectML.dll`、DML 对拍器、validation layer 或 CUDA DLL**。本包不包含 HTTP 服务；HTTP 服务仍用单独的完整部署包。

## 控制台验证

在解压目录打开 PowerShell，先查看目标设备：

```powershell
.\lw-ppocr-vulkan-probe.exe
.\lw.PPOCR.Vulkan.CSharpDemo.exe --ocr models/onnx/ppocrv6-tiny test-images/sample.jpg 0
.\lw.PPOCR.Vulkan.CSharpDemo.exe --recognize models/onnx/ppocrv6-tiny/rec.onnx test-images/sample.jpg 0 20 28 292 46
```

把最后的设备编号 `0` 改为探测出来的目标 GPU 编号。示例图第一行及上述框选区域期望文字为 `纯臻营养护发素`；OCR 不是零错误保证。模型切换为 `ppocrv6-small` 或 `ppocrv6-medium` 即可。路径相对解压目录，整个包可以移动。

## 修改 C# 示例

直接运行不需要开发环境。需要改界面时打开 `examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.sln`，使用支持 .NET Framework 4.0 项目的 Visual Studio/MSBuild，选择 **x64**，安装相应目标框架开发包。项目会从分享包根目录复制 OCR DLL、可用的 loader、模型与测试图到输出目录。

原生 C++源码在开发中的 `lw.PPOCR.Vulkan` 项目，不含在此 C#体验包里；C ABI 和调用示例见 `sdk/include/lw_ppocr_vulkan.h`、`examples/winforms/NativeOcr.cs` 和 `docs/OCR-API.md`。

## 耗时口径与本次 Demo 优化

- **OCR 调用**：后台 C# `Run`，包含 BGR 转换、原生 OCR、结果复制和 JSON 解析；不含图片文件解码、界面更新。
- **界面就绪**：从点击操作开始，包含图片快照、后台调度、OCR 调用和当前页签更新；不保证屏幕绘制已完成。文件读取和模型初始化另计。
- **流水线**：原生 JSON 的 `timing.total_ms`，包含等待句柄锁、CPU 预处理、检测后处理、裁剪、分类和识别等；结束于结果 JSON 最终序列化之前。
- **DET / CLS / REC**：上传、提交、GPU 执行与等待、回读；CLS/REC 累计所有文字区域，不是纯 GPU 时间。
- **其他**：流水线减去三阶段合计，包含 CPU 前后处理、执行计划准备等；不是全部 C# / UI 开销。

开启 GPU 前处理后，DET/CLS/REC 阶段还包含相应 GPU 缩放、填充和归一化；REC 也包含采样时的 180° 校正。因此部分耗时会从“其他”移入阶段计时，不能只比较阶段标签推断纯网络加速。优化收益按同条件的完整 OCR 调用耗时比较。

“首轮调用”可能包含首次尺寸的 GPU 执行计划创建；“重复调用”只表示该引擎已经调用过，不保证新图片的所有尺寸都已预热。比较速度时，同 GPU、模型、参数和图片先运行三次，再测多次，不用首轮耗时代表稳定吞吐。

此前 C# 构建启用 `/optimize+`；紧凑 BGR 内存一次复制，带行填充或负 stride 保留逐行路径；隐藏的 JSON / 表格页签按需更新，避免每次识别都更新不可见控件。最新 `pipeline-opt2` 包进一步优化原生 CLS/REC 缩放和透视裁剪，DLL 已更新；模型、精度、GPU shaders 和公共 API 不变。实际提速证据见 `docs/PIPELINE-OPTIMIZATION.md`，不将计时显示调整宣传为 GPU 推理加速。

探测程序已在 Windows 控制台设置 UTF-8 输出；`Check-GPU.bat` 同时设置代码页 65001。重定向输出仍为 UTF-8。

English: OCR-call time includes managed pixel conversion, native inference and result parsing. UI-ready time additionally includes scheduling and active-tab updates; it does not guarantee completed screen painting. Native pipeline and DET/CLS/REC timings retain their existing definitions. Hidden JSON/grid tabs are updated on demand, tightly packed BGR is copied in one operation, and C# builds use optimization. No native precision/model changes are made.

## 常见问题

- **无法加载 DLL/BadImageFormat**：完整解压，确认 Windows/x64；不要从另一个包混入 x86 DLL，也不要单独复制 EXE。
- **没有 GPU/初始化失败**：先运行 `Check-GPU.bat`，安装或更新显卡厂商驱动；虚拟机/远程桌面的显卡可见性取决于环境。没有可用 Vulkan 设备时本包不能完成 GPU OCR。
- **模型找不到**：保留原目录结构；GUI 模型路径可改，但应指向含 det.onnx、cls.onnx、rec.onnx、dictionary.txt 的目录。
- **显存/工作区不足**：先选 Tiny、关闭其他 GPU 程序，核对实际预算；不要盲目把检测长边调到很大。默认每模型图 512 MiB 是上限，不是启动预留量，权重/驱动/CPU 图像内存另计。
- **不知道选哪个编号**：编号不固定，查看 GPU 名称；把 probe 输出、操作系统、显卡驱动版本、模型、参数和错误文字发给作者。

## 校验与验证范围

ZIP 旁边 `.sha256` 用于校验压缩包；包内 `FILES.sha256` 是解压后文件清单，检测传输损坏，不是数字签名。LunarG loader/安装器保留官方数字签名；本项目 Demo/原生库未作代码签名，可能遇到下载来源提示。

历史 `pipeline-opt2` 核心 DLL SHA-256 为 `91bf808abe1fdb26afb0ce692b54d2b64286d20a2f54d3aa97c6590ab251cfee`，其性能与局限见 `docs/PIPELINE-OPTIMIZATION.md`。上一版 `gpu-det-experiment1` DLL 为 `c509c8162f863b6b5aa45e02a9fe02f99cb75cec8f66c05877348ff0a631fdd4`。本轮 `gpu-text-experiment1` DLL 为 `b87cbb9559586053cd46e5a9bb1ef28b8d5f9df56cf8b5e219c3e20dd33482fa`，保留前述优化，新增默认关闭的 CLS/REC GPU 前处理和直接分段上传。`PACKAGE-INFO.json` 和 `validation/native-qualification.json` 标明实际包版本与 DLL 哈希；最终解压包的 C# GUI/整图/框选证据单独保留，不把旧包长测当作本包新长测结果。

English: Extract the entire ZIP, launch `Start-CSharp-Demo.bat`, select your actual GPU/model, initialize, then run OCR. All application-native DLLs and three models are included. A compatible installed GPU vendor driver and .NET Framework are still required. No CUDA, Python, Vulkan SDK or Visual Studio is required to run. This is a preview, not a universal compatibility/performance guarantee.

`network-opt1` 原生 DLL SHA-256：`39d4397307d8c1b19df22a6a312a1fd49e4ebd4d04cd1d445cf755f00d634929`。上段 `gpu-text-experiment1` 哈希属于上一轮构建，不是本包；以本包 `PACKAGE-INFO.json` 和文件校验清单为准。新包有独立回归与 ZIP 验证，不沿用旧包长测作为本轮稳定性结论。
