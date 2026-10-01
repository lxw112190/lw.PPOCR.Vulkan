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

## Linux CI：Vulkan 头文件已找到但链接库缺失

若 CMake 输出 `found suitable version "1.4.350"`，同时报 `missing: Vulkan_LIBRARY`，
版本来自头文件，并不代表 Vulkan Loader 的链接库已找到。`glslc` 存在也只证明着色器编译工具可用。
Ubuntu 的 `libvulkan1` 提供运行时库；构建需要 `libvulkan-dev` 提供的 `libvulkan.so` 链接文件。

工作流安装 `libvulkan-dev`（自动带入运行依赖）和 `pkg-config`，保留固定 SDK 用于头文件与着色器工具。
配置前通过 `pkg-config --exists vulkan` 和 `test -r "$(pkg-config --variable=libdir vulkan)/libvulkan.so"`
检查开发链接文件。不要手工创建软链接，也不要通过修改模型或锁文件掩盖工具链问题。
SDK 缓存不包含 runner 的 apt 安装状态，因此系统依赖安装在缓存命中时也必须执行。

这是构建依赖修复，不是 GPU 驱动验证；客户运行部署包仍需要可用的 Vulkan Loader/驱动。
