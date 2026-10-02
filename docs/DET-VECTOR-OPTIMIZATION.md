# DET FP32 向量访存优化

2026-10-02，本轮在 REC 合批版本之上优化 DET 的普通卷积访存，不改模型、精度、检测尺寸、输出字段或公共 C ABI。

## 如何借鉴前后处理加速文章

参考作者的 [C++ TensorRT YOLOv8 推理：CUDA 核函数加速前处理、后处理](https://blog.csdn.net/lw112190/article/details/143335419)，迁移的是减少 CPU/GPU 往返、复用缓冲区、在 GPU 上处理数据与分阶段计时的思路，不直接移植 CUDA/TensorRT 代码或 YOLO 的 NMS。

本项目已经实现：

- DET 缩放、归一化与 NHWC 布局转换融合成一个 GPU 前处理 kernel；CLS/REC 的缩放、填充、归一化及 REC 180° 校正也可在 GPU 完成。它们仍为默认关闭的实验选项。
- 权重常驻、按生命周期复用中间张量；CLS/REC 有界合并提交。REC 在 GPU 做贪心标签/置信度选择，仅回读紧凑 CTC 数据，CPU 去重、去空白和映射字典。
- 使用完整调用耗时验证收益，不把异步提交的 CPU 耗时当作 GPU 已执行完成；算子 timestamp 只用于定位热点。

本轮继续把“减少数据处理与搬运开销”用在 GPU 内部：四个相邻 NHWC 通道共用一次地址计算与 `vec4` 加载/写回。没有新增 CPU↔GPU 拷贝、中间 im2col 张量、批次 arena 或显存预算。

DB 轮廓、检测框扩展/排序、透视裁剪和字典映射仍在 CPU；未宣称本轮已经 GPU 化这些步骤。文章中的 YOLO 后处理与 OCR 的 DB 后处理不同，不能直接套用。

## 实现与取舍

新增 `src/shaders/conv_gemm_vector.comp`：FP32 M32/N64/K32 implicit GEMM，维持旧 kernel 的 K 遍历、累加、偏置及激活顺序，只改变加载和写回布局。输入/输出通道必须是 4 的倍数，张量绑定保持原有至少 16 字节对齐；RGB 首层及通道尾部沿用旧 kernel。仍在原 tiled GEMM 的形状门控下选择，reference/禁用 tiled 的诊断路径不变。

**自动启用限定 DET，REC/CLS 保留原路径。** 早期实验对 REC 也启用，虽然预热样例局部变快，但独立连续换图测试中 Tiny/Small 出现约 3%～5% 退化。因此没有发布这条分派，不以单 kernel 耗时替代完整流程表现；退化原因尚未进一步隔离为某个驱动/计划开销。

不新增环境变量；普通 CPU 前处理启动与 GPU 前处理实验都会采用符合条件的 DET kernel。网络保持 FP32，不使用 FP16、Tensor Core 或缩小 DET 输入换取速度。现有预算、LRU、错误处理和输出计时口径不变。

## 基准与结果

Windows 10、本机 NVIDIA RTX 4060 Laptop（设备编号 1）；模型 Tiny/Small/Medium，DET 长边上限 960，CLS 开启。设备编号仅对本机有效。

基准 `rec-batch-opt1` DLL SHA-256：

`a5e773d702492f909f7ec2dfd7dff2d99aeb3e4248c5c86936b0741f59c0b8c7`

最终 `det-vector-opt1` DLL SHA-256：

`05201449ff0705ecd3c70e7dbfbeb7740e300e6be6bbc1597cb8ba3ae8d13c0d`

下表两边都开启 DET/文字行 GPU 前处理，是预解码 BGR → 原生完整 OCR → JSON 复制/解析的调用耗时。排除文件解码、模型初始化和 GUI；GPU 性能测试按顺序执行，不运行其他 GPU 测试。所有完整预测字段逐项一致；不是识别准确率提升，也不是与 CPU/DML 的对比。

### 100 图逐图预热均值

每图预热一轮，交替新旧调用，每模型每条路径计 200 个样本。

| 模型 | 上一版 ms | 本轮 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny | 19.841 | 19.829 | 0.1% |
| Small | 33.724 | 33.693 | 0.1% |
| Medium | 82.627 | 78.656 | 4.8% |

Tiny/Small 本轮不宣称加速；其检测网络在本次形状中没有选择新 kernel。

### 连续换图，不逐图预热

100 图第一遍顺序调用（包括首次尺寸计划创建），第二遍反序，仍交替新旧调用。

| 模型 | 首遍旧→新 ms | 第二遍旧→新 ms |
| --- | ---: | ---: |
| Tiny | 24.274 → 24.448 | 23.043 → 23.065 |
| Small | 40.370 → 40.862 | 38.883 → 38.727 |
| Medium | 87.068 → 83.653 | 84.764 → 81.199 |

Medium 降幅约 3.9%/4.2%；Tiny/Small 波动约 -1.2%～+0.4%，不算收益。不同模型/图片和冷启动不能保证同样改善。

### sample.jpg，30 轮配对中位数

3 次预热、交替新旧调用，均为 16 个区域。

| 模型 / 图尺寸 | 上一版 ms | 本轮 ms | 降幅 |
| --- | ---: | ---: | ---: |
| Tiny / 500×500 | 23.796 | 23.826 | -0.1% |
| Tiny / 1000×1000 | 41.462 | 41.714 | -0.6% |
| Small / 500×500 | 42.689 | 42.919 | -0.5% |
| Small / 1000×1000 | 67.093 | 67.007 | 0.1% |
| Medium / 500×500 | 102.786 | 101.223 | 1.5% |
| Medium / 1000×1000 | 149.156 | 142.793 | 4.3% |

Medium 的 DET 阶段累计中位数分别从 14.471/49.422 ms 降至 12.767/43.343 ms（约 11.8%/12.3%）。阶段包括提交、等待和回读，不是纯 kernel 耗时；“其他”和 REC 不在本轮修改范围内。

原始报告见 [reports/det-vector-optimization](reports/det-vector-optimization)。早期全任务候选的换图报告单独命名 `rejected-rec-stream.json`，不得当作最终 DLL 的数据。算子 timestamp 会扰动执行，仅用于热点解释，不用它宣称上述完整耗时。

## 本轮验证范围

- RTX/AMD 各 27 组三模型原始网络输出逐位一致；最终 100 图、10 组尺寸/旋转/空白变体、填充 stride、40 检测尺寸与输入预算恢复通过。
- 新增 `lwvk_vector_gemm_probe`，直接比较新旧 kernel 的 64 组 M/N/K 尾部、Cin<32、不同卷积核/stride/padding、无偏置/有偏置、ReLU/GELU/SiLU；两卡 FP32 位与尾部保护完全一致，CPU 标量参考最大绝对误差分别约 7.2e-7/4.8e-7。
- 三模型独立 ONNX Runtime CPU 完整 OCR 参考、长度/stride/预算/并发恢复通过；REC-only 三模型每模型 24 组输入、40 宽度、20 次并发及异常恢复通过。
- 原 Medium 报错大图（2448×3264）按 DET960 重复 5 次成功，85 个区域；只保存图片哈希/尺寸/区域数，不保存客户文字。
- 默认 CPU 前处理路径完整预测一致。Medium 两组样例调用中位数下降约 1.3%/2.9%，Tiny/Small 变化约 -0.5%～+0.4%；不把实验启动路径的百分比套用到普通启动。
- CTest 13/13、固定资产 176 项、C++ 格式化与仓库预检通过。公共头文件不变，原生导出仍为 19 个。
- HTTP 二进制/Base64、完整/仅识别/批量、API Key、429/503、日志隐私/关闭日志及异常恢复通过。
- 显式开启 Khronos 同步验证：两卡 64 组卷积探针、RTX Medium 完整图 profiling 与 REC 合批探针通过，无 VUID/SYNC-HAZARD。日志确认同步验证激活；不是 GPU-assisted 越界检测或设备挂起验证。profiling 工具也补充了 validation layer 混合 stdout/stderr 的解析及 3 个主机测试，错误不会因输出流不同而被忽略。
- 完整 C# 暂存包使用自带 loader，在 RTX/AMD 上运行三模型整图/ROI；WinForms GPU 选择/手填编号、鼠标框选和 5 次重复调用通过。普通启动路径也通过。最终 ZIP 解压验证结果保留在 ZIP 旁 `.validation.json`，不回写已生成的 ZIP。

## 复现

```powershell
$env:LWVK_GPU_DET_PREPROCESS="1"
$env:LWVK_GPU_TEXT_PREPROCESS="1"
python tests/benchmark_vulkan_pair.py --before build/rec-batch/Release/lw.PPOCR.Vulkan.dll --after build/conv-vector/Release/lw.PPOCR.Vulkan.dll --iterations 30 --report build/conv-vector/sample-final.json
python tests/benchmark_ocr_stream.py --before build/rec-batch/Release/lw.PPOCR.Vulkan.dll --after build/conv-vector/Release/lw.PPOCR.Vulkan.dll --corpus ../lw.PPOCR.C/build-local-data/lw-generated-ocr --report build/conv-vector/stream-final.json
.\build\conv-vector\Release\lwvk_vector_gemm_probe.exe 1
```

这些 GPU 工程探针不是安装包依赖，不增加 ABI 导出，也不要求客户安装 ONNX Runtime/CUDA。没有新增长时间 soak 或设备丢失故障注入，不将回归测试说成无泄漏/所有 GPU 兼容的证明。
