# 第三轮 FP32 优化：GELU 融合与 DML 完整对拍

## 验收条件不变

这是两件独立的事：**DET 长边保持 960，解决工作区不足；完整 OCR 性能至少超过同条件 DML。**
默认工作区仍为每图 512 MiB、按需增长，不是预分配；常驻权重、CPU 内存和驱动内存另计。
本轮 Medium DET 960×960 的 arena 仍需 335,462,400 字节（约 319.92 MiB），尚未计 IO。
旧 256 MiB 默认值确实不足，不能用缩小输入代替修复。

**性能目标尚未完成。** 默认 FP32 路径的 Medium 大图从上一轮约 241 ms 降到约 223 ms，
但强化 DML 基线约 214 ms。不同时间的运行有波动，不把全部差值归因于单个改动。

## 实现内容

- 精确识别 `Div(x,sqrt(2)) -> Erf -> Add(1) -> Mul(x) -> Mul(0.5)`，合成内部 GELU。
  检查常量类型/形状/字节范围/实际值、输入关系、消费者数和图输出；不吞掉共享分支。
  沿用项目原来的 FP32 Erf 多项式与运算顺序，不替换成另一种近似激活。
- 单消费者的普通卷积可直接执行 GELU epilogue，省去中间张量读写和四次额外 dispatch。
  深度卷积、已有 ReLU、共享结果和图输出保留原路径；实验协作矩阵不执行不支持的 GELU epilogue。
- 检测头 ConvTranspose 的常量通道 bias、ReLU/Sigmoid 安全融合，保留分支/图输出语义。
- 项目新写 M64/N64/K32 FP32 pointwise 分块和 vec4 输出实验。
  `LWVK_EXPERIMENTAL_TILE64=1` 才启用，并检查尺寸、通道可整除及 shared-memory 能力。
  **默认关闭，未凭单次小幅改善升级为发布默认。**

公共 C ABI、DET 参数、模型/字典、输出字段和精度容差均未改变。
工程对照开关必须在加载 DLL 前设置；`LWVK_DISABLE_GELU_FUSION` 存在即禁用 GELU 融合，
`LWVK_REFERENCE_GRAPH` 用于原始图对照。它们不是正式 HTTP 配置。

## 最新默认路径完整 OCR

2026-10-01，本机 Ryzen 7 7735H / RTX 4060 Laptop，Vulkan index 1 与 DXGI index 1 核对设备。
Tiny/Small/Medium 同 ONNX、同预解码 BGR、同原生预处理/DB/crop/CLS/REC-preprocess/JSON。
公共图片 500×500 和双线性放大的 1000×1000，实际 DET 对齐尺寸为 512×512 / 960×960。
开启 CLS、预热 3 次、交替测量 30 次；一次只运行一个 GPU 负载。
计时覆盖原生完整 OCR 与 ABI JSON copy，不含文件解码、HTTP 和 Python JSON 解析。
两实验开关和 profiler 均关闭。

DML 使用每图最多 32 个固定形状会话缓存和原生 CPU greedy CTC；Vulkan 使用 GPU greedy CTC。
DML 缓存会复制权重/资源，Vulkan 共享 arena，**并非相同显存预算**。
Vulkan 为 FP32；DML 内部算子精度未建立证据，不宣称两者内部精度路径完全相同。

| 模型 / 图片 | Vulkan median | DML median | Vulkan P95 | DML P95 |
|---|---:|---:|---:|---:|
| Tiny 500×500 | 56.50 ms | 66.30 ms | 58.62 ms | 69.22 ms |
| Tiny 1000×1000 | 118.26 ms | 112.27 ms | 127.32 ms | 121.96 ms |
| Small 500×500 | 77.50 ms | 98.66 ms | 81.06 ms | 103.83 ms |
| Small 1000×1000 | 138.48 ms | 143.21 ms | 151.10 ms | 154.85 ms |
| Medium 500×500 | 141.00 ms | 150.68 ms | 143.46 ms | 152.96 ms |
| Medium 1000×1000 | 223.28 ms | 213.65 ms | 225.20 ms | 217.04 ms |

六组各 16 个区域，文字/CLS 标签一致、坐标差 0、最大分数差 ≤1.08e-6；
验收容差仍是坐标 ≤1 像素、分数 ≤0.002，没有放宽。
Small 大图在本次胜出，但上一轮 30 次结果为 137.28 / 135.92 ms，并不证明稳定胜出。
Tiny 大图双方也比先前测试慢；保留所有数据，未建立频率/系统负载原因，不挑选最快的一轮。

Medium 大图 DET median 为 Vulkan 65.64 / DML 42.19 ms，而 REC 为 78.03 / 91.11 ms。
当前主要缺口仍是 DET，而不是识别阶段；接下来优先降低卷积成本、数据搬运和 dispatch 空隙。

[默认完整原始报告](reports/gelu-fusion/dml-full.json)；
[此前 GELU+Transpose 30 次结果](reports/gelu-fusion/previous-default-dml-full.json)。
最新 DLL SHA-256：`a973523b657b2a3e5e0a306c5f56fc496a8bf0e31550776903d86fb5762ee17e`。

## 默认关闭的分块实验

同一 DLL，显式开启 M64 实验，另一次 30 次交替测量：

| 模型 / 图片 | Vulkan median | DML median |
|---|---:|---:|
| Tiny 500×500 | 56.44 ms | 66.02 ms |
| Tiny 1000×1000 | 119.78 ms | 115.06 ms |
| Small 500×500 | 77.42 ms | 98.01 ms |
| Small 1000×1000 | 138.27 ms | 141.57 ms |
| Medium 500×500 | 141.81 ms | 152.32 ms |
| Medium 1000×1000 | 220.84 ms | 214.03 ms |

[实验原始报告](reports/gelu-fusion/tile64-dml-full.json)仍全部通过结果容差，但 Medium/Tiny 大图仍落后。
这不是足够的启用依据；分块尺寸/输出向量化效果与运行波动也未单独隔离。
之前未通过真实模型分数门槛的混合 FP16 协作矩阵实验仍不属于默认路径。

## 正确性与资源验证

- 8 项 CTest 通过，包含 37 个融合形状/常量/分支/图输出安全条件；176 项固定资产校验通过。
- [AMD 探针](reports/gelu-fusion/gelu-probe-0.json)与 [4060 探针](reports/gelu-fusion/gelu-probe-1.json)
  各通过 19 个卷积/GELU/分块边界用例及输出尾部 canary，最大误差 4.77e-7；
  请求 Khronos validation layer，未报告 VUID/Validation Error。
  这是合成用例，不代替真实模型精度；没有单独的 standalone GELU GPU 探针。
- 最新 DLL 分别通过 [Tiny](reports/gelu-fusion/tiny.json)、[Small](reports/gelu-fusion/small.json)、
  [Medium](reports/gelu-fusion/medium.json)的独立 ORT CPU quick 对拍，
  [AMD Medium](reports/gelu-fusion/amd-medium.json)也通过。
  覆盖旋转/空白、CLS 关闭、长度/stride 拒绝、crop 上限/恢复、结果生命周期、4 worker/4 调用。
- [工作区回归](reports/gelu-fusion/workspace.json)通过三模型 DET 960、40 种 REC 形状、
  LRU 淘汰、normal/CTC、增长后重绑、小尺寸 readback canary，以及低预算拒绝后的恢复。
  用户原图 2448×3264，Medium DET 长边 960，85 个区域连续 5 次文字一致；不保存图像或识别正文。
- [默认图诊断](reports/gelu-fusion/default-profile.json)和
  [M64 实验诊断](reports/gelu-fusion/tile64-profile.json)验证计时开关前后输出摘要相同。
  诊断串行化/扰动 GPU，随机张量算子时间不是完整 OCR 性能；不能拿其总和代替表中数据。
  `tests/profile_gpu.py --tile64` 明确标记实验并控制子进程环境，避免继承环境造成误标。

本轮没有重跑 1000 次长测、HTTP/WinForms 部署测试、ASan/UBSan 或远程 CI。
这不是无泄漏、所有驱动兼容或正式发布证明。先前已验证 dev.2 压缩包保持原 SHA
`56e382535674643202e9a89f425a34e218d92cb7cd12091f88b7c1c09687bca9`，没有覆盖。
当前构建位于 `build/gelu-optimized/Release`，尚未制作新版部署包。

English: DET-960 workspace correctness and beating matched full-OCR DML are independent gates.
Guarded FP32 GELU and transpose epilogues passed the listed regressions. Medium large-image
latency improved to about 223 ms but remains behind about 214 ms DML. The M64 FP32 tile stays
default off; the mixed-FP16 experiment is not accuracy-qualified. The validated dev.2 archive is unchanged.
