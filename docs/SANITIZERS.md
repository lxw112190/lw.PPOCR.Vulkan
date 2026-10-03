# v0.6.0-dev.2：sanitizer、异常恢复与正确性门禁

本轮不改 C ABI、HTTP/config/log 候选契约，不改模型、FP32 网络或 GPU 默认策略。新增质量门禁，仍不等于 v1.0 已验收。

## 统一原生插桩

`LWVK_SANITIZERS=ON` 只支持 Linux GCC/Clang；其他平台明确配置失败，避免“开关打开但未实际检查”。CMake 在创建任何 target 前设置 ASan/UBSan、禁止错误后继续执行、保留 frame pointer，覆盖核心库、C 几何代码、CLI、HTTP、所有 native 单测与 GPU probe。着色器不是 C/C++，不受 ASan 插桩；GPU 内存错误仍依赖 Vulkan validation/probe/canary 测试。

独立工作流 `.github/workflows/sanitizers.yml` 使用 Ubuntu 22.04、Clang、固定 Vulkan SDK 与 lavapipe，并设置：

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

没有全局关闭 LeakSanitizer，也没有宽泛的驱动泄漏抑制规则。若第三方驱动报告问题，应先保留栈并定位，再审查是否需要精确抑制，不能直接删掉检测。

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
