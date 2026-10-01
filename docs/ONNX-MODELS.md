# ONNX 加载、模型来源与性能边界

版本 0.5.0-dev.2。C++17 原生 protobuf reader 直接解析已固定的 OCR 模型，
带消息长度、常量和图规模预算。没有引入 Protobuf/ORT 运行时。
相关实现的原始快照保留在项目内，来源提交、修改与许可证说明集中见 [NOTICE](../NOTICE)。

## 官方模型

采用下表列出的 PaddlePaddle 官方发布模型，许可证 Apache-2.0，版权归 PaddlePaddle Authors。
本地字节取自作者的 lw.PPOCR.C 已审查模型副本；SHA-256 固定在
`models/onnx/catalog.json` 及 `dependencies.lock.json`，不是每次构建下载 latest。

| 模型 | 官方 DET | 官方 REC | CTC 类别（含 blank/space） |
| --- | --- | --- | --- |
| Tiny | [tiny_det](https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_det_onnx) | [tiny_rec](https://huggingface.co/PaddlePaddle/PP-OCRv6_tiny_rec_onnx) | 6906 |
| Small | [small_det](https://huggingface.co/PaddlePaddle/PP-OCRv6_small_det_onnx) | [small_rec](https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec_onnx) | 18710 |
| Medium | [medium_det](https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_det_onnx) | [medium_rec](https://huggingface.co/PaddlePaddle/PP-OCRv6_medium_rec_onnx) | 18710 |

共用方向模型：[PP-LCNet_x0_25_textline_ori_onnx](https://huggingface.co/PaddlePaddle/PP-LCNet_x0_25_textline_ori_onnx)。
项目并非 PaddlePaddle 官方产品或获其背书；官方模型来源不等于我们的移植已无缺陷。

## 使用

每套目录 `models/onnx/ppocrv6-{tiny,small,medium}` 包含 `det.onnx`、`cls.onnx`、
`rec.onnx`、`dictionary.txt`。完整 C ABI 传入该目录；仅识别传入 `rec.onnx`。
HTTP 修改 `model_root` 后重启；WinForms 使用新增模型下拉选项后重新初始化。
旧 Tiny JSON/BIN 图仍兼容，但不向 Small/Medium 扩展离线 JSON 导出器。

REC 的字典是同目录 `dictionary.txt`，Small/Medium 与 Tiny 不是同一个字典。
运行时检查字典类别数；字节完整性由发布/CI 的固定 SHA 校验负责。

## 解析器不是通用 ONNX Runtime

只保证已固定的三套 FP32 模型，共用 CLS，batch=1；DET 每边 32..960、32 的倍数；
REC 高 48、宽 32..960、8 的倍数。外部数据文件、自定义域、控制流、任意秩/布局变换不支持。
单 ONNX 上限 256 MiB，图/常量/字符串均有预算，截断/非法 protobuf 失败而不越界读取。
仅明确匹配的九节点 LayerNorm、QKV attention 子图融合；不是把所有 Reshape/Transpose 当成无操作。

## 已实现的优化

- 权重一次上传到 device-local 常驻区；1×1 权重预排布 K-major，输入/输出尾部有边界保护。
- FP32 implicit GEMM 共享内存分块；1×1 的 4×4 寄存器分块。
- 并行 Softmax；仅识别/完整 OCR 从 GPU 只读回每时间步 label + score，避免整套概率搬回 CPU。
- 生命周期分配器对空闲区间拆分/合并，避免整块占用和碎片浪费。
- 最多 32 个 LRU 执行计划，共享 arena、上传/概率/CTC 读回缓冲区；默认上限 512 MiB/图，按需分配。
  权重单独限制 256 MiB/图，进程还包含三个模型、主机图元数据/图片/驱动内存，不能把工作区当作进程总内存。
- GPU 故障后整个句柄失败，不绕过 poisoned 计划；正常销毁释放所有计划和权重。

`LWVK_REFERENCE_KERNELS` 仅供工程对拍，存在时改用原始逐元素卷积/串行概率 Softmax；
不是 CPU 回退，也不能用它代表旧版全部调度行为。当前仍为 FP32，未提供 FP16/协作矩阵档。

单消费者 Conv → 通道 Mul → Add 可折叠为卷积权重/偏置；ReLU 合并入卷积输出。
共享常量不原地修改，残差分支或图输出不跨越融合。`LWVK_REFERENCE_GRAPH` 存在时关闭这些图融合，供工程对拍。
新增 M32/N64/K32 的共享内存+4×4 寄存器 FP32 GEMM，通用卷积采用 kernel-position/channel K 顺序以连续读取 NHWC。
`LWVK_DISABLE_TILED_POINTWISE` / `LWVK_DISABLE_TILED_GEMM` 关闭对应新内核，仅用于对照。
这些开发开关不是 CPU 回退，也不是稳定的客户配置接口；精度和速度均须重新验证。

English: official PP-OCRv6 Tiny/Small/Medium ONNX files are loaded by a bounded native
C++ importer; implementation provenance is recorded in NOTICE. This is a narrow OCR importer,
not a general ONNX implementation. Models and matching dictionaries are pinned by SHA-256.
Device-local weights, bounded LRU plans, FP32 tiled kernels and GPU greedy CTC are implemented;
the existing public probability-output API remains available for independent reference testing.
