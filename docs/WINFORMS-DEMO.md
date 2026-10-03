# WinForms 测试程序 / WinForms tester

版本：1.0.0 正式版，C ABI/HTTP/config/JSONL v1 已正式冻结。历史验证记录仍对应当时版本和二进制；新附件须重新核对。

参考 `PP-OCRv5_Test_dml` 的操作布局，但只调用本项目 Vulkan C ABI；
不引入 DML、OpenCVSharp、ONNX Runtime 或 NuGet 包。支持 PP-OCRv6 Tiny/Small/Medium。

## 使用 / Usage

解压 Windows 包，运行根目录 `lw.PPOCR.Vulkan.WinFormsDemo.exe`。
默认加载 `test-images/sample.jpg`，模型目录为 `models/onnx/ppocrv6-tiny`，
均相对于程序目录，而不是当前工作目录。需要兼容 .NET Framework 4.0 的运行环境、x64 Windows、
Vulkan 1.1 驱动/loader。Windows 10 已本机验证，Win7 不在已验证范围。

- GPU：下拉选择 `[编号] 名称`，或直接填写数字编号。点击“刷新设备”重新枚举。
  编号不保证跨电脑/驱动不变，不做独显优先重排。无效编号报错，绝不静默换卡。
- 模型：下拉选择 Tiny/Small/Medium，会填入相对程序目录的 ONNX 模型包位置，也可浏览或手填。
- 初始化：选择设备、模型目录、DET 长边上限（32 的倍数，32..960）、检测/扩框/CLS 参数，
  点击“初始化 / 重载”。修改参数后必须重新初始化。
- 每模型工作区上限：0=默认 512 MiB，可显式设置至 1024 MiB；按需分配，完整 OCR 有最多三张模型图。
  DET 960 是默认正常路径。工作区不足会显示需要/预算字节数，不静默降低尺寸。
- 整图 OCR：DET → DB 框 → 透视裁剪 → 可选 CLS → REC → CTC。
  左侧红框与序号；右侧文字、JSON、结果列表。选中列表项高亮对应检测框。
- 框选仅识别：原图上左键拖动，再点击“框选仅识别”。仅 REC + CTC，不检测、不自动旋转。
  正向/反向拖动均可，边缘截断；显示缩放映射回原图，最小区域为 2×2 像素。
- 输出：复制文字或保存 UTF-8 JSON。整图结果使用原生规范四点字段，无重复 `box` 数组；
  仅识别结果含 ROI、文字、置信度和耗时，不伪造检测框。
- 关闭：推理在后台执行；关闭窗口会等待当前调用结束，不会强行销毁 GPU 资源。

English: select a GPU or type its numeric ID, initialize, then run full OCR or drag
a text ROI for REC-only recognition. Settings changes require reinitialization.
Assets are executable-directory-relative. The same pixels are used regardless of overlays.
Closing waits for the active native call; it does not cancel GPU work.

## 耗时与资源 / Timing and resources

底部端到端耗时包括 BGR 拷贝、预处理、CPU 后处理、原生调用及结果转换；
DET/CLS/REC 来自宿主原生图调用计时（含上传、提交、等待和读回），不是 Vulkan timestamp 的纯 GPU 计算时间；CLS/REC 是各文字区域累加。
初始化和首次推理会建立资源/执行计划，请勿当作稳态性能。
鼠标 ROI 的 REC 网络在首次使用时懒加载，可能额外占用一张模型图的资源；
换设备/重载会先释放旧引擎，减少瞬时资源峰值。

图片读取使用 System.Drawing：解码之后检查尺寸/像素上限，再复制为 24bpp BGR。
这不是面对恶意上传的受限解码 HTTP 服务。不得以桌面 Demo 的图片校验替代服务端安全设计。
显示叠加不改写原图；SafeHandle 管理原生引擎、网络和结果生命周期。

## 编译 / Build

CMake 默认在找到 .NET Framework C# 编译器时构建控制台和 WinForms 程序，安装到包根目录。
设置 `-DLWVK_BUILD_CSHARP_EXAMPLE=OFF` 可只构建原生程序，但官方 Windows 打包脚本要求两个 Demo 齐全。

Visual Studio 打开 `examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.sln`，选择 Release/x64。
界面由代码布局，不依赖 Designer。先构建原生库；工程默认读取项目 `build/Release` 的 DLL，
也支持已安装包根目录中的 DLL。其他构建目录可显式指定：

```powershell
MSBuild examples/winforms/lw.PPOCR.Vulkan.WinFormsDemo.csproj /p:Configuration=Release /p:Platform=x64 /p:NativeBuildDir=E:\path\to\native\Release
```

工程会复制 DLL、模型和示例图到 `bin/x64/Release`，不应提交 `bin/obj`。

## 本机验证 / Local validation

在与安装目录不同的工作目录启动包中的 Demo，使用应用自身的测试入口验证布局和实际鼠标事件映射。

```powershell
python tests/test_winforms.py --package dist/staging --output build/reports/winforms --host-only
python tests/test_winforms.py --package dist/staging --output build/reports/winforms --device 0
python tests/test_winforms.py --package dist/staging --output build/reports/winforms --device 1
```

- 主机模式：布局渲染、正反向框选、调整窗口后原图映射；GPU 选择/手填编号及无效编号拒绝（存在设备时）。不执行 GPU 推理。
- 本机 AMD Radeon(TM) Graphics、NVIDIA RTX 4060 Laptop：完整 OCR 均得到 16 个结果，
  标题及手动 ROI 均识别为“纯臻营养护发素”，原图坐标检查通过。
- 测试输出包含 JSON 与应用自身 DrawToBitmap 截图；这不是人工操作 UI 全覆盖，
  也不是长期压力/泄漏证明。物理 GPU 结果不可推导成 CI 软件 Vulkan 或其他显卡均通过。

本次报告见 [AMD](reports/winforms-device0.json)、[NVIDIA](reports/winforms-device1.json)、
[主机界面](reports/winforms-host.json)。Windows CI 新增主机模式，尚未远程运行验证。

当前 dev.2 的主机验证为 7 项 CTest、172 个固定依赖/模型文件校验；HTTP 服务和网页已提供，
详见 [HTTP 服务](HTTP-SERVICE.md)。历史界面报告不代表最新二进制已经覆盖全部显卡，
当前测试与性能边界见 [工作区/性能报告](WORKSPACE-PERFORMANCE.md)。
本项目不使用 OpenCV，通用 OpenCV 服务预检中的 OpenCV 版本约束不适用。
SBOM 属于后续正式发布门槛，当前为技术验证包，不宣称已完成安全/许可证完整审计。
