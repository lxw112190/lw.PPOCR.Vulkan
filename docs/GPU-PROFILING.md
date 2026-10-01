# GPU 算子定位与第二轮 FP32 优化

本页保留第二轮测量记录；后续结果见[第三轮 GELU 融合与 DML 完整对拍](GELU-FUSION.md)。

## 两个独立门槛

DET 长边 960 保持不变。默认 512 MiB 是每个图的 arena/IO 按需分配上限，
不是启动时固定占用，也不包括常驻权重、CPU 内存和驱动分配。
旧 256 MiB 报错不能通过缩小检测输入解决；本轮再次验证三模型的 DET 960，
以及用户原始 2448×3264 图片的 Medium 完整 OCR：85 个区域、5 次重复一致。
私人图片和识别内容不写入工作区报告。
本轮 Medium 960×960 的 arena 需求为 335,462,400 字节（约 319.92 MiB），
尚未计 IO，直接说明 256 MiB 对这个常用尺寸不足。

性能是另外一个门槛：同模型、同图片、同硬件、同预处理、DET 960、开启 CLS，
比较完整 OCR 的 median/P95，并通过文字、框、分数和稳定性检查。
**提高内存上限不是性能优化；目前仍未全面超过强化 DML 基线。**

## 新增诊断工具

`LWVK_GPU_PROFILE=1` 在创建图之前启用工程诊断，默认关闭，不改 C ABI/配置 Schema。
检查选中计算队列的 `timestampValidBits` 和设备 `timestampPeriod`；不支持时明确报错。
normal/CTC 命令各自拥有 query pool，每次提交先在命令内 reset，再给每次 dispatch
写入开始/结束时间戳。fence 完成后读取 64 位结果，按有效位数处理回绕并换算毫秒。
query pool 随计划失效、缓存淘汰或销毁一起释放，关闭诊断时不创建这些资源。
实现依据：[Khronos timestamp](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)
和 [query results](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)。

每个录制计划的 normal/CTC 命令最多各输出前三次 `LWVK_GPU_PROFILE ` 前缀 JSONL，
包含任务、输入尺寸、设备、shader、dispatch 参数和耗时，不含图像、权重、概率、识别文字或密钥。
工作区重绑会重新录制；它不是线上 access/runtime 日志 Schema，也不建议用于长期服务。

这里使用 BOTTOM_OF_PIPE 时间戳来分隔 dispatch，会扰动和串行化执行。
算子时间总和不包含宿主处理、上传/下载及算子间空隙，**不能当端到端吞吐结果**。
三个 benchmark 脚本在发现此开关为 1 时拒绝测量，避免误用。

```powershell
python tests/profile_gpu.py --library build/profile-optimized/Release/lw.PPOCR.Vulkan.dll --device 1 --validation --report build/reports/gpu-profile.json
```

测试使用确定性归一化随机输入，每个图/命令跑四次，记录前三次、丢弃第一份计时预热样本，
并验证第四次没有额外日志。独立子进程分别开启/关闭诊断，输出摘要必须相同。
覆盖 Medium DET 736×960、960×960，REC 48×320、48×960，CLS 80×160。
`--variants tiny small medium` 可扩充范围；`--coop` 仅用于单独编译的实验模式，
不代表通过真实模型精度门槛。`--validation` 请求 Khronos validation layer，
本机已安装；这不是 GPU-assisted validation 或无越界/泄漏证明。

## 根据热点做的 FP32 优化

1. 固定 K32 内层循环显式展开，减少寄存器分块逐点/普通卷积的循环成本，仍为 FP32。
2. 深度卷积权重由逐通道布局预排布为 kernel-major，四个连续 NHWC 通道一起计算，
   保持 dy/dx 累加顺序；仅 C 可被 4 整除时选用，其他情况保留标量路径。
   原始权重仍保留，`LWVK_REFERENCE_KERNELS` 或 `LWVK_DISABLE_VECTOR_DEPTHWISE` 可用于对照。
3. 单消费者 `HardSigmoid(x) × x` / `Sigmoid(x) × x` 融合成 HardSwish/SiLU，
   保留共享分支和图输出，沿用导入的 alpha/beta，不擅自固定为 1/6。

新的深度卷积探针在 AMD 核显和 RTX 4060 分别通过 6 个 CPU 参考/输出尾部 canary 测试：
通道 4/12/32/64/256、1×1/3×3/5×5/9×9/矩形核、stride、padding、bias/ReLU。
两设备最大误差 0，没有报告 Vulkan validation errors。输入是精确二进制小数，
不能替代真实模型精度检查。没有启用失败的混合 FP16 协作矩阵路径。

## 最新完整 OCR 对拍

2026-10-01 本机 RTX 4060 Laptop，FP32，原始公共 500×500 图片及双线性放大的 1000×1000。
DETaligned 分别为 512×512 / 960×960。共享原生宿主预处理/DB/crop/CLS/REC 预处理及 JSON；
DML 使用 32 项固定形状会话缓存和原生 CPU CTC，Vulkan 使用 GPU greedy CTC。
预热 3 次、交替顺序测量 20 次；计时开关关闭，排除图片文件解码、HTTP/Python JSON 解析。
双方资源模型不同：DML 多会话与 Vulkan 共享 arena，不宣称相同显存预算。

| 模型 / 图片 | Vulkan median | DML median | Vulkan P95 | DML P95 |
|---|---:|---:|---:|---:|
| Tiny 500×500 | 57.46 ms | 65.53 ms | 58.69 ms | 67.83 ms |
| Tiny 1000×1000 | 106.02 ms | 101.85 ms | 116.44 ms | 108.94 ms |
| Small 500×500 | 79.70 ms | 98.32 ms | 82.75 ms | 100.81 ms |
| Small 1000×1000 | 142.62 ms | 139.00 ms | 156.16 ms | 147.60 ms |
| Medium 500×500 | 152.24 ms | 153.06 ms | 154.01 ms | 154.79 ms |
| Medium 1000×1000 | 241.03 ms | 215.63 ms | 242.75 ms | 217.04 ms |

六组均为 16 个区域，文字和 CLS 标签一致、坐标差 0、最大分数差 ≤1.32e-6。
相较此前 dev.2 报告的 Medium 187.74/288.86 ms，现在约 152.24/241.03 ms；
这是不同时间的运行，不能把全部差值归因某一个改动。Medium 小图仅近似持平，
Tiny/Small/Medium 大图都没有稳定胜出，**DML 性能目标保持未完成**。
Medium 大图分阶段 DET 77.36 ms vs DML 42.45 ms，REC 85.45 ms vs DML 92.87 ms；
下一步优先继续优化大图 DET，而非以缩图、丢框或放宽误差换速度。

[本轮完整对照](reports/gpu-profile/dml-full.json)记录所有样本、模型/DLL SHA 和方法。
最新测试 DLL SHA-256：`fe6bb79f47932e2ca3cc6b94c233ff6db71be445a9987391e59b463b1682468d`。
本轮还通过 7 项 CTest、174 项依赖/模型校验、三模型独立 ORT CPU quick 对拍，
异常长度/stride、资源限制拒绝后恢复、结果生命周期、同句柄并发及工作区 LRU/增长/canary。
AMD 核显也通过了 [Medium 完整 OCR quick 对拍](reports/gpu-profile/amd-medium.json)，
不推广为所有 AMD 型号/驱动均已验证。
[工作区/私人图片摘要](reports/gpu-profile/workspace.json)、
[优化前诊断](reports/gpu-profile/before.json)、[优化后诊断](reports/gpu-profile/after.json)。
诊断时间只用于定位，不能将随机张量计时与完整 OCR 数据混为一谈。

本轮没有重新跑 1000 次长测、HTTP/WinForms 部署验证、sanitizer、远程 CI 或其他驱动组合。
先前已验证 dev.2 压缩包保持原 SHA/内容，未用本轮 DLL 覆盖。新构建位于
`build/profile-optimized/Release`，不是已经发布或打包的新版。

English: DET-960 resource correctness and beating DML remain independent gates.
The default-off operator profiler checks timestamp support, uses separate normal/CTC pools,
and keeps output digests identical to an uninstrumented control. It is deliberately not a
throughput benchmark. FP32 loop unrolling, kernel-major vector depthwise weights and safe
HardSwish/SiLU fusion improved the local pipeline, but the latest matched 20-iteration full-OCR
run still trails fixed-shape cached DML on large images. The previously validated archive is unchanged.
