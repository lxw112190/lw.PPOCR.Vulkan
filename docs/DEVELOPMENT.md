# 开发指南 / Development guide

核心是 C++17/Vulkan；第三方参考快照、官方模型与许可证单独保留。不要把技术预览当作 ABI 冻结或生产稳定版。

## 目录与关键边界

| 目录/文件 | 职责 |
|---|---|
| `include/lw_ppocr_vulkan.h`、`src/api.cpp` | 实验性 C ABI；固定宽度类型、缓冲区长度、UTF-8 和异常转状态码 |
| `src/ocr_pipeline.cpp`、`src/ocr_host.cpp` | 完整 OCR、同句柄串行、DB/裁剪/CLS/REC、累计像素限制 |
| `src/onnx_import.cpp` | 有消息与资源预算的 ONNX 解析，只支持已固定的模型 |
| `src/graph.cpp`、`src/workspace_planner.hpp` | 图执行、张量生命周期、32 项尺寸计划 LRU、共享 arena/IO |
| `src/vulkan_context.cpp` | 设备能力、Vulkan 资源 RAII、映射/flush/invalidate 与 GPU 同步 |
| `apps/http` | 配置、认证、请求队列、解码预算、引擎等待超时、日志与平台服务入口 |
| `examples`、`www` | C#/Python 集成示例、WinForms、HTTP 调用页 |
| `third_party`、`licenses`、`NOTICE` | 固定来源快照与必要归属声明，不参与统一格式化 |
| `docs/reports` | 历史实验的原始证据；哈希对应当时构建，不能冒充新源码的验收结果 |

同句柄复用工作区必须串行；GPU fence 超时不等于取消 GPU 任务。HTTP 等待引擎超时只放弃排队请求。
`timing.total_ms` 包括 CPU 前后处理与锁等待；DET/CLS/REC 图执行时间含上传/等待/回读，不能称作纯 GPU 时间。
工作区预算不含常驻权重、驱动及 CPU 内存；模型目录、字典和参数必须与测试条件一致。

## 格式与注释

使用 clang-format **17** 和根目录 `.clang-format`：4 空格缩进、120 列，不重排 include。
仅处理自有 `src/apps/include/tests` 中的 `.c/.cpp/.h/.hpp`，保留第三方原始字节。

```powershell
python scripts/format_cpp.py --write
python scripts/format_cpp.py --check
```

脚本在 PATH 查找工具，Windows 还会查找 VS 2022 的 LLVM；可用 `--clang-format` 指定路径。
注释说明设计原因、所有权、资源/同步边界和精度约束；不要逐句复述代码。
源码采用 UTF-8/LF；MSVC 的所有原生目标统一启用 `/utf-8`。

## 本地验证

```powershell
python scripts/repository_preflight.py
python scripts/verify_assets.py
cmake -S . -B build/local -G "Visual Studio 17 2022" -A x64
cmake --build build/local --config Release --parallel 4
ctest --test-dir build/local -C Release --output-on-failure
```

GPU 正确性、HTTP 与部署包测试必须额外执行，不能把 host 单元测试或软件 Vulkan 当作实体 GPU 性能验证。
每次改模型、算子、数值顺序或部署二进制，都需要新的回归/资格记录。不得为了通过校验自动刷新所有依赖哈希。
这次文档/格式整理不会覆盖此前已验证的 `pipeline-opt2` 分享 ZIP；包内旧群信息随该历史包保留，新包应重新生成并验证。

English: Format first-party C/C++ with clang-format 17 only. Keep vendored snapshots unchanged, explain ownership/synchronization/resource bounds, and rerun native plus GPU/deployment tests after code changes. Historical report hashes apply to their original binaries, not automatically to today's source.

## FP32 kernel 回归

FP32 kernel 优化新增 `tests/test_pointwise_regression.py --before <旧DLL> --after <新DLL> --device <GPU编号> --report <JSON>`，对三模型的 DET/CLS/REC 原始概率逐位对照；与独立 ORT 参考测试互补。GPU 算子、端到端和部署包验证均不能省略。自有 pointwise shader 位于 `src/shaders`，构建派生时注入统一 GELU/SiLU epilogue，保持固定 `third_party` 资产原始字节。

## Linux CI：Vulkan 头文件已找到但链接库缺失

若 CMake 输出 `found suitable version "1.4.350"`，同时报 `missing: Vulkan_LIBRARY`，
版本来自头文件，并不代表 Vulkan Loader 的链接库已找到。`glslc` 存在也只证明着色器编译工具可用。
Ubuntu 的 `libvulkan1` 提供运行时库；构建需要 `libvulkan-dev` 提供的 `libvulkan.so` 链接文件。

工作流安装 `libvulkan-dev`（自动带入运行依赖）和 `pkg-config`，保留固定 SDK 用于头文件与着色器工具。
配置前通过 `pkg-config --exists vulkan` 和 `test -r "$(pkg-config --variable=libdir vulkan)/libvulkan.so"`
检查开发链接文件。不要手工创建软链接，也不要通过修改模型或锁文件掩盖工具链问题。
SDK 缓存不包含 runner 的 apt 安装状态，因此系统依赖安装在缓存命中时也必须执行。

这是构建依赖修复，不是 GPU 驱动验证；客户运行部署包仍需要可用的 Vulkan Loader/驱动。

## Linux 软件 Vulkan CI：REC batch fence 超时

`submit/wait REC batch ... VkResult=2` 是 `VK_TIMEOUT`，不是识别内容不一致，也不能仅凭这条信息断定死锁。常规 REC 合批一次提交最多 8 行，整批共用 30 秒 fence 等待；lavapipe 使用 CPU 执行着色器，共享 runner 上整批可能超时。多路研究构建默认 OFF，CI 也显式设置 `-DLWVK_EXPERIMENTAL_REC_LANES=OFF`，因此不要误将该错误归因于多队列实验。

软件 CI 的完整参考、安装目录、三模型和 HTTP 测试显式设置三个 `LWVK_GPU_*_PREPROCESS=0`：使用 CPU 图像前处理、逐行网络提交，**网络仍走 Vulkan**。另一个步骤仅覆盖环境为 1，通过 DET 8 例、CLS/REC 16 例和 crop 111 例轻量探针校验着色器精确结果、padding/stride、越界 canary 和错误恢复；保留 validation layer 与同步检查。不能省略这些覆盖，也不能把 CPU 前处理软件对拍当作完整硬件 GPU 默认流水线的验证。

硬件 GPU 默认路径、模型、阈值、30 秒等待及 poisoned 生命周期处理不改；超时不是取消，禁止失败后复用仍可能在 GPU 执行的计划。完整 OCR 参考测试在推理前打印设备、模型、前处理环境、图片尺寸及预期文字框数，便于后续区分设备、批量负载与数值回归问题。若仍超时，应检查具体设备/variant、单行工作负载和验证层日志，不能直接扩大到无限等待。此改动需要远程 Linux CI 再确认，本机 Windows 实体卡通过不代表 lavapipe 已通过。

### 后续：Medium 单次网络提交仍超过默认等待

下一轮 CI 已证实 Tiny、Small 完整对拍和 HTTP 测试通过，但 Medium 完整 OCR 首张图仍出现 `submit/wait DET graph ... VkResult=2`。注意旧文案中的 DET 是通用 `Plan::submit_readback` 的硬编码标签，CLS/REC 单图也走这里，不能因此断定一定是检测超时。日志显示 Medium REC 宽 320 时已需约 17.1 秒，完整 OCR 的长文字行会使用更宽输入，因此单行超过 30 秒是合理推断；日志本身没有证明具体超时阶段。

修复采用显式、软件设备限定的 `LWVK_SOFTWARE_GRAPH_TIMEOUT_MS`，不扩大所有设备的默认等待：

- Linux lavapipe CI 设为 180000 毫秒；仅 `VK_PHYSICAL_DEVICE_TYPE_CPU` 生效，合法范围 30000～300000，非法值启动时拒绝，未设置保持 30000。
- 硬件独显、集显和虚拟 GPU 忽略该选项，固定 30 秒；下载包不会保存 runner 的环境变量。
- DET/CLS/REC 单图与网络合批使用同一 Context 策略；图像裁剪、拷贝及轻量 shader probe 的 30 秒等待不变。
- 单图失败信息打印真实模型 task、输入高宽、有效等待毫秒数和 VkResult；参考测试打印并写入软件超时设置，便于下一轮日志确认。
- 主机单元测试覆盖硬件不受影响、默认值、有限上下界、非法数值及溢出。仍保留全部三模型内容/分数/坐标对拍、验证层和同步检查，不将超时当成功。

这不是整张 OCR 的总超时，多个提交的总耗时可能更长；CI job 仍有 90 分钟总上限。fence 超时不取消设备任务，poisoned 标记、销毁前等待设备空闲的安全语义不变，不能将其宣传成硬实时中断。修复后仍须通过远程 Linux CI 验证，Windows 实体卡回归不能代替 lavapipe 验证。
