# Sanitizer、异常恢复与正确性门禁

本页记录质量门禁及开发以来的诊断过程。不因版本转正移除检测；本轮诊断调整不改冻结的 C ABI、HTTP/config/log 契约、模型、FP32 网络或客户包的 GPU 默认策略。

## 统一原生插桩

`LWVK_SANITIZERS=ON` 只支持 Linux GCC/Clang；其他平台明确配置失败，避免“开关打开但未实际检查”。CMake 在创建任何 target 前设置 ASan/UBSan、禁止错误后继续执行、保留 frame pointer，覆盖核心库、C 几何代码、CLI、HTTP、所有 native 单测与 GPU probe。着色器不是 C/C++，不受 ASan 插桩；GPU 内存错误仍依赖 Vulkan validation/probe/canary 测试。

独立工作流 `.github/workflows/sanitizers.yml` 使用 Ubuntu 22.04、Clang、固定 Vulkan SDK 与 lavapipe，并设置：

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

没有全局关闭 LeakSanitizer，也没有宽泛的驱动泄漏抑制规则。若第三方驱动报告问题，应先保留栈并定位，再审查是否需要精确抑制，不能直接删掉检测。

### 退出阶段的 `128 bytes / <unknown module>` 诊断

本节保留开发阶段报错的诊断方法，不作为 v1.0 当前 CI 状态声明。正式发布确认见 MAINTAINER-ACCEPTANCE.md；后续发布仍须审核原始探针与实际 Linux CI 结果，不因版本转正移除检测。

DET 探针打印 PASS 只表示张量对拍与尾部 canary 通过；随后进程退出的 LSan 报错仍是门禁失败。当前报告只有 `realloc` 与未知模块，不能据此认定为项目泄漏、驱动泄漏或误报。已检查正常路径的 buffer/mapping、fence、command/descriptor pool、pipeline、device 和 instance 释放，暂未找到遗漏；Linux 根因仍待有符号调用栈验证。

CI 安装匹配的 `llvm-symbolizer-14` 并检查可执行性。`scripts/sanitizer_gpu_probes.py` 使用正常库卸载依次运行三个 shader 探针；即使 DET 失败，也收集 TEXT/CROP 各自的结果。正式探针使用 `fast_unwind_on_malloc=1`、30 层分配栈，仍启用 ASan/UBSan 和退出泄漏检查。项目原生代码保留 frame pointer；第三方库栈不足时再收集诊断对照。

原始探针失败后只收集三次**设备枚举**对照（不创建网络/着色器）：原始环境；设置 `NODEVICE_SELECT=1` 移除 Mesa 隐式选卡层但保留 Khronos validation；再移除显式 validation 的无 layer 对照。`NODEVICE_SELECT` 是 Mesa manifest 定义的禁用开关，[LunarG 说明](https://www.lunarg.com/wp-content/uploads/2022/03/1.3-Vulkan-Loader-Improvements-MAR2022.pdf)也介绍了它，适用于本次 Jammy 旧 loader；单独设置新版本才支持的 `VK_LOADER_LAYERS_DISABLE=*` 不足以隔离隐式层。对照使用慢速分配栈展开、40 层调用栈、`LD_DEBUG=libs` 和 `VK_LOADER_DEBUG=layer`；不再反复重编译失败或超时的 shader。原始三个门禁探针不设置 `NODEVICE_SELECT`，保留原有环境。

所有运行继续正常卸载动态库，不使用保留 DSO 来改变 LSan 全局可达对象判断。关闭 layer 的对照不替代原始 validation 测试。任何原始探针非零退出（含超时、启动失败）仍令步骤失败；不会因为对照通过而变绿，不设置泄漏 suppression。

下载 `host-sanitizer-diagnostics-not-a-release-package` 附件，查看 `shader-probes/summary.json` 和三个原始 `.log`，以及 `control-enumeration.log`、`control-enumeration-without-mesa-select.log` / `control-enumeration-without-layers.log`。若同样的泄漏在纯枚举程序中出现，说明不执行 OCR/shader 也能复现；若仅移除 layer 后消失，则进一步调查 layer 路径，但二者均不足以直接宣称所有项目路径无泄漏。

### 完整诊断附件确认的旧 loader 限制

维护者提供的完整 ZIP 记录：loader `1.3.204.1-2`、Mesa `23.2.1-1ubuntu3.1~22.04.4`、Clang/LLVM 14，实际链接系统 `/lib/x86_64-linux-gnu/libvulkan.so.1`。SDK 1.4.350 的头文件/工具版本不等于 loader 版本。三个原始 shader 探针分别用时 2.977、0.418、0.570 秒，功能/canary 全部通过，但各自退出时报 128 字节泄漏；纯枚举每次创建两个 instance，共报告两份 128 字节泄漏。枚举源码的正常/异常路径都调用 `vkDestroyInstance`，不能把栈中存在项目调用方直接解释为项目遗漏释放。

旧 loader 忽略此前 `VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1` 的对照，日志仍实际卸载驱动；所谓“无 layer”对照也仍加载 `libVkLayer_MESA_device_select.so`。因此旧对照不能判断泄漏来自 Khronos layer、Mesa layer 或 driver 本身。本轮修正的是诊断失效，不宣称 128 字节泄漏已修复。

仅 `LWVK_SANITIZERS=ON` 的原生构建包含模块快照代码；在枚举对照中再显式设置 `LWVK_DIAG_MODULE_MAPS=1`，于 `vkDestroyInstance` 前记录已加载 ELF 的 PT_LOAD 地址范围及 load bias。代码不持有 DSO 引用，不阻止卸载，不添加 suppression。脚本将退出栈中未知 PC 与快照匹配，保存 `*-unloaded-frames.json`；若地址在不同快照中被不同模块复用则保留多个候选，不猜测归因。正式 Release 包没有此诊断实现。

单一候选包含 `path` 与 `elf_address`，可在同一 CI 环境进一步查询：

```bash
llvm-symbolizer-14 --obj=/actual/path/to/library.so 0xELF_ADDRESS
```

发行版 `.so` 如果已剥离符号，可能仍输出 `??`，但模块与 ELF 偏移已定位；后续应取**同版本**调试符号或审查对应源码，而不是按字节数或未知模块设置宽泛抑制。CI 还保留实际 Mesa layer manifest，便于核对 `NODEVICE_SELECT` 的定义。

### 诊断本身造成的 60 分钟超时

实际 CI 日志显示，旧脚本给三个 shader 正式探针都设置了 `fast_unwind_on_malloc=0:malloc_context_size=40`，三者各超时 420 秒；随后对失败探针逐个进行保留库/移除 layer 重跑，最终触发 job 的 60 分钟总上限。慢速展开作用于每次分配，而 Mesa/LLVM 编译 shader 有大量分配；[Sanitizer 官方说明](https://github.com/google/sanitizers/wiki/AddressSanitizer#faq)也提醒该设置可能严重影响性能。这是本轮调整的主要依据，仍需要下一次 Linux CI 验证实际耗时。

新脚本保持全部三个正式探针及其 420 秒单进程上限，慢速展开只用于三次枚举对照，每次最多 90 秒。所有子进程等待预算合计最多 1530 秒（25.5 分钟），步骤上限 30 分钟；总 job 仍为 60 分钟。每个子进程开始时输出日志位置，完成时输出实际耗时；每次完成都更新 `summary.json`，中途取消时 `completed: false`、`passed: false`，已完成结果不会等到最后才保存。

同一份日志中正常枚举也报告 128 字节未知模块泄漏，且使用系统 `libvulkan.so.1`；移除显式 Khronos layer 的诊断仍加载 Mesa device-select 隐式 layer。因此现有日志不能精确归因，也不能称泄漏已修复。超时、泄漏、启动失败都继续令原始门禁失败。该工作流的四个 GitHub Actions 已改用 Node 24 版本；Node 20 弃用警告与 shader 超时是不同问题。

参考：[Clang 符号化配置](https://clang.llvm.org/docs/AddressSanitizer.html#symbolizing-the-reports)、[Khronos loader 环境变量](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md#environment-variable-table)。此修改增强根因定位，不声称已修复 128 字节泄漏；待实际 Linux CI 报告后再采取精确修复。

### Ubuntu 22.04 工具链包名

Jammy 的 Clang 14 sanitizer 运行库位于 `libclang-common-14-dev`，不能套用其他发行版的 `libclang-rt-14-dev` 包名；否则 apt 在编译前就会报 `Unable to locate package`。安装命令为：

```bash
sudo apt-get update
sudo apt-get install -y clang-14 llvm-14 libclang-common-14-dev
```

CI 保留 `clang-14` / `clang++-14`，不添加额外 LLVM 软件源、不切换编译器版本。安装后通过 `clang-14 -print-resource-dir` 获取实际资源目录，检查 x86_64 的 ASan、ASan C++、UBSan 和 LSan 静态运行库，缺失即失败。版本、包版本和检查路径保存在诊断附件的 `toolchain.txt`；后续故障探针仍验证检测实际生效，运行库文件存在本身不等于 sanitizer 测试通过。

Ubuntu 22.04 包信息：[clang-14](https://packages.ubuntu.com/jammy/clang-14)、[libclang-common-14-dev](https://packages.ubuntu.com/jammy/libclang-common-14-dev)。升级 runner 时需要重新核对该发行版的包布局，不能假设不同 Ubuntu 版本的包名相同。

门禁同时检查每条 `compile_commands.json` 的插桩选项，并执行 CI 专用故障探针：堆越界、整数溢出、丢失分配必须非零退出且出现对应诊断。三个故障是测试刻意制造的，程序不会安装进客户包；它们用来证明检测真的启用，不代表 OCR 程序发生这些错误。

## Python 与运行库顺序

Python/ctypes 直接加载 ASan 动态库可能报“ASan runtime does not come first”。不向所有 Python/ORT 进程全局注入 `LD_PRELOAD`，也不关闭 ASan 的顺序检查。

sanitizer CTest 用链接到核心库的 `lwvk_api_native --host` 替代该配置下的 Python ABI 调用测试；19 个导出与 ctypes 结构布局检查仍执行，它不加载动态库。其他 Python 测试只通过子进程/HTTP 调用原生程序，ASan 运行库由被测可执行程序正常加载。普通 Release 构建继续运行原有 Python ABI 用例以及新增原生用例。

## 安装后的真实程序

1. 主机单测：ONNX 九模型解析、截断、非法 protobuf 长度/tag、2000 个有界随机输入；图像解码新增 2000 个随机/截断输入，检查预算拒绝后正常恢复。
2. 安装到独立 staging，检查服务动态依赖无 `not found`；用 `ldd` 确认 native helper 实际加载 staging 核心库，而不是编译目录中的库。
3. Tiny 原生 C ABI：真实 OCR/REC、短 buffer/stride/尺寸拒绝、无最后一行 padding 的正 stride、32 次变尺寸空白图、结果独立存活及并发拷贝。
4. 现有 HTTP smoke：配置、二进制/Base64、REC/batch、API Key、429/503、日志/隐私、恢复与正常退出。
5. 新增 HTTP hostile corpus：9 个固定错误场景 + 默认 200 个确定性非法 JSON/图片请求，每个固定场景和每 25 次随机请求后执行真实 REC，最后校验 JSONL 日志 Schema。

HTTP 子进程非零退出会打印服务输出尾部。无论 CI 成败，上传 CTest、编译命令、服务日志与 JSON 诊断；sanitizer staging 不是下载部署包，不进入正式 Release 附件。常规 Windows/Linux CI 仍构建普通 FP32 部署包。

## 正确性派生集

`tests/correctness_cases.py` 固定检查原样图 SHA-256。使用锁定的 Pillow 版本，`--quick --extended` 生成 11 个场景：样图、180°/90°/270°、宽/竖压缩、低对比度、9°倾斜、灰度、黑/白空图。普通快速模式仍保留原来 3 例。

完整 OCR 对照独立 ORT CPU 图、NumPy 预处理与 CTC，文字完全相等，置信度容差 0.003，坐标容差 1.1 像素；DB/crop 使用共享 C 几何实现，因此不是独立几何算法证明。报告记录每例尺寸和 BGR 像素 SHA-256，防止把不同生成条件的结果混在一起。

这只是同一张图的派生测试。多来源实际中文/英文/数字/混排、小字、相机透视、长行等带明确授权与标准答案的公开集，仍是正式发布欠项；不将派生集宣传成“11 张真实场景准确率验证”。

新增倾斜场景发现旧测试错误假设“所有角点都在图内”：实测 Vulkan 与参考的 x1 均为 -4.394726753234863。映射并裁切边界点后重新拟合的旋转矩形可超出图像范围，Schema 原本就允许数字坐标。本轮删除这个额外错误假设，保留所有文字/分数/坐标对拍容差，并加入负坐标 Schema 测试；不改 DB/crop 算法或模型。

## 本地与 CI 复验

Linux：

```bash
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
cmake -S . -B build/sanitize -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLWVK_SANITIZERS=ON \
  -DLWVK_EXPERIMENTAL_COOP=OFF -DLWVK_EXPERIMENTAL_REC_LANES=OFF
cmake --build build/sanitize --parallel 2
ctest --test-dir build/sanitize --output-on-failure --parallel 1
```

先安装 Python 测试依赖与 Vulkan 开发包，并提供 `glslc`；CI 明确使用 Clang 14 及其运行库。软件 Vulkan 网络测试三个 GPU 图像处理环境开关显式为 0，网络仍走 Vulkan；另一个步骤设为 1，运行已插桩的轻量 GPU 图像 shader 精确探针。软件图等待保留有限 180 秒上限，job 总上限 60 分钟，不更改实体 GPU 的 30 秒默认。

Windows 普通构建的扩展参考测试：

```powershell
python tests/test_ocr_reference.py --library build/v062/Release/lw.PPOCR.Vulkan.dll --geometry build/v062/Release/lwvk_geometry_reference.dll --models models/onnx/ppocrv6-tiny --device 1 --quick --extended --iterations 2 --report build/v062/reports/tiny-extended.json
python tests/test_http_invalid.py --package build/v062/staging --output build/v062/reports/http-invalid --device 1 --iterations 200
```

设备编号以本机探测结果为准；本机 Windows 没有 Linux sanitizer 环境，本轮 sanitizer 必须在推送后以 CI 结果验收，不能声称已经通过。即使 ASan/UBSan/LSan 通过，也仅证明所覆盖的执行路径未发现相关问题，不是所有内存/显存或所有驱动的无泄漏保证。

English: all native Linux GCC/Clang targets are instrumented, fault tripwires verify
activation, linked native ABI tests avoid Python ASan-runtime ordering, and tests
exercise installed binaries with leak detection enabled. Dedicated CI retains failure
diagnostics but does not distribute instrumented deployment packages. The extended
11-case corpus derives from one reviewed sample, not diverse real-world ground truth.
Host sanitizers do not validate GPU memory. Results require the actual Linux CI run.
