# 短行 REC 研究与多场景长测验收（2026-10-07）

本轮属于 v1.0.1 发布后的开发快照，版本暂不提升，不覆盖已发布附件。普通构建保留原 FP32 路径、DET 长边 960、CLS 默认开启和冻结的 v1 接口。

结论：短行内核在独立 REC 上有效，但完整 OCR 没有稳定收益，因此**研究调度默认关闭，不进入普通部署路径**。本轮主要可交付成果是候选内核、严格回归、可复现的分场景延迟/资源观测工具和验收记录，而不是新的整图提速承诺。

## 1. 实现与资格门槛

- 新增项目自编 M8/M16、N64、K32 的 FP32 pointwise kernel，64 个线程，约 9/10 KiB shared memory；采用 vec4 输入/权重读写，保留原 K 累加、Bias、激活顺序。
- 单份 shader 源码生成两种行 tile；行尾、通道尾和 shared-memory barrier 有专门测试。
- 研究调度仅作用于 NVIDIA REC 的 packed 1×1、group=1、stride=1、零 padding、5≤M<16、64≤N<4096、N/K 可被 4 整除且 K≥64 的分支。参考/禁用 tiled/既有 tile64 路径保持原样。
- 初始 M5..31 候选扩大了覆盖范围，却导致部分短输入的骨干投影回退，收紧为 M5..15。即使收紧，整体收益仍不成立。
- CMake `LWVK_EXPERIMENTAL_SHORT_REC` 默认 `OFF`；只有明确设置 `ON` 才调度新内核。没有新增客户配置字段、环境变量或 C ABI。
- 正式构建 CI 显式设置 `OFF`。打包元数据新增该标记；发布校验拒绝 `ON` 或缺少该标记的构建，避免研究 DLL 混入普通发布包。历史发布附件不被重写。

这不是任意 NVIDIA 显卡性能保证，也不是新增设备支持声明。AMD shader 正确性测试通过不表示 AMD 调度启用了该优化。

## 2. 独立短 REC：有局部收益

Windows 10 x64（19045）、Ryzen 7 7735H、RTX 4060 Laptop GPU，原模型、FP32。输入为固定种子的归一化随机 NCHW FP32 张量；每个宽度预热 3 次，然后两个 DLL 交替各调用 60 次，候选优先初始化。计时为 REC 网络、GPU 贪心标签和 Python 结果返回，不含图片前处理、完整 OCR 或 GUI。

最终 OFF/ON 构建的 24 个模型/宽度组合，完整输出概率逐位相等，CTC 文字与分数严格相等。以下只列实际触发短行路径的宽度；其他宽度的波动不能当成优化收益。

| 模型 | REC 宽度 | 默认中位数 ms | 研究中位数 ms | 耗时降低 |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 48 | 0.510 | 0.488 | 4.35% |
| Tiny | 64 | 0.505 | 0.483 | 4.43% |
| Tiny | 96 | 0.461 | 0.447 | 3.09% |
| Small | 48 | 1.108 | 0.946 | 14.63% |
| Small | 64 | 1.120 | 0.952 | 15.05% |
| Small | 96 | 1.186 | 1.002 | 15.51% |
| Medium | 48 | 2.465 | 1.980 | 19.67% |
| Medium | 64 | 2.519 | 2.056 | 18.39% |
| Medium | 96 | 2.601 | 2.143 | 17.62% |

原始采样、所有宽度和 DLL/model hashes：[rec-qualified.json](reports/short-rec/rec-qualified.json)。这是特定设备的局部网络微基准，不是真实短文本识别准确率，也不能外推为整图降低 20%。

## 3. 完整 OCR：不接入默认

宽范围候选的 500/1000 sample 交替测试基本持平（[ocr-pair-final.json](reports/short-rec/ocr-pair-final.json)）。收紧候选使用 lw.PPOCR.C 的 `lw-generated-ocr` 100 张公开生成图，每模型 3 轮、每轮新旧交替、第二轮逆序；没有逐图预热，首轮包含执行计划首次使用。完整结果字段严格相等。

| 模型 | 第 1 轮平均旧→候选 ms | 第 2 轮平均旧→候选 ms | 第 3 轮平均旧→候选 ms |
| --- | ---: | ---: | ---: |
| Tiny | 21.73 → 22.08 | 19.39 → 19.66 | 19.34 → 19.44 |
| Small | 37.17 → 36.72 | 32.77 → 33.05 | 32.45 → 32.71 |
| Medium | 75.37 → 75.35 | 72.38 → 72.73 | 74.73 → 74.89 |

多数热身轮略慢，故资格不通过。原始数据：[stream-narrow.json](reports/short-rec/stream-narrow.json)，其中候选为收紧策略原型，hash 与最终加上显式研究构建开关的 DLL 不同；最终构建另做了逐位 REC 对照和六组长测，不混淆二者身份。

没有使用曾与编译同时进行的早期采样作为报告依据。性能采样期间没有并行编译或另开 OCR/GPU 测试；电源、温度和 GPU 时钟未锁定，所以很小的差异不作普适结论。

## 4. 单句柄多场景与长测

新增 `tests/benchmark_latency_modes.py`：每个进程只加载一个 DLL、一个模型、一个 OCR 句柄。预解码 sample 500×500、250×250、1000×1000、900×450、450×900，以及上述 100 张生成图，共 105 张。各阶段不重建引擎：

1. 首次调用 1 次、其余图片首次使用 104 次。
2. 105 张图片正/逆序预热，共 210 次。
3. 同一 sample 连续 40 次；5 个尺寸交替 40 次。
4. 105 张混合图片正/逆序循环长测 1000 次。
5. 每次间隔 2 秒调用同一 sample，共 5 次。

默认/研究 × Tiny/Small/Medium 共六个独立串行进程，每组 1400 次完整 OCR，合计 8400 次，其中长测阶段合计 6000 次。计时包含 BGR 到原生 OCR、JSON 复制和 Python 解析，不含文件解码、UI 或 HTTP。

每次检查全部 items 字段的稳定哈希、非负有限耗时，记录 RAM、250 ms WDDM 进程显存采样、每 100 次的线程/句柄检查点。六组均完成，组内反复调用一致，默认/研究的 105 张结果哈希也逐图一致。

以下为**普通默认构建**；固定 sample 和混合图片不能互相比作同一负载的快慢：

| 模型 | 连续 sample 中位数 ms | 混合 1000 次 P95 ms | 预热后 Private 增量 MiB | 采样进程 dedicated GPU 峰值 MiB |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 19.34 | 25.16 | 1.20 | 163.75 |
| Small | 36.02 | 43.61 | 1.38 | 280.90 |
| Medium | 87.53 | 99.48 | 1.91 | 691.93 |

研究构建 Private 增量为 Tiny 1.22、Small 0.91、Medium 1.89 MiB。没有观察到线程/句柄持续增加，但这不是无泄漏证明。Private 增量以预热后到关闭前为基准，包含 Python 保存采样记录等分配。

RAM 包括 105 张预解码图片、Python/NumPy/Pillow、结果、引擎和驱动；默认进程采样 RSS 峰值约 585.60/658.34/868.50 MiB，Private 峰值约 756.48/900.74/1324.63 MiB，**不是单独 OCR 库内存**。WDDM 数据是跨适配器进程归属，不是显卡物理总显存；250 ms 采样可能漏掉瞬时峰值。默认/研究在独立进程顺序运行，不据此宣称内存节省或精确 A/B 耗时收益。

六份完整采样：[默认 Tiny](reports/short-rec/soak-default-tiny.json)、[默认 Small](reports/short-rec/soak-default-small.json)、[默认 Medium](reports/short-rec/soak-default-medium.json)、[研究 Tiny](reports/short-rec/soak-experimental-tiny.json)、[研究 Small](reports/short-rec/soak-experimental-small.json)、[研究 Medium](reports/short-rec/soak-experimental-medium.json)。

### 间隔调用波动仍待处理

默认构建同一 sample 连续调用中位数为 19.34/36.02/87.53 ms；间隔 2 秒调用则为 **156.63/170.94/310.09 ms**。研究构建也存在该现象。不能把连续热态数字当成手动 Demo 每次点击的延迟。

本轮没有采集 GPU clock/power 状态，不能仅靠耗时把原因确定为降频；也没有改变用户显卡电源策略、常驻循环预热或用阶段 GPU 时间代替真实调用耗时。下一轮优先采集阶段/提交等待/时钟证据，分别评估可控计算开销和系统调度因素，再决定是否需要显式可选的交互预热。

## 5. 正确性、应用与范围

- 32/32 本机 CTest 通过，新增 72 项短行调度门槛及 4 项延迟工具主机测试。
- NVIDIA/AMD full shader 探针各 880 组：既有 kernel 逐位一致、输出尾部 canary 无改写；验证层与同步检查开启。最终 quick 探针另各通过 168 组。
- 最终默认 DLL 与本轮前保存 DLL：三模型 DET/CLS/REC 共 27 组形状，完整概率逐位一致，REC 文字/分数一致：[default-regression.json](reports/short-rec/default-regression.json)。
- 最终默认三模型及研究 Medium 的 ORT CPU 图网络/独立 NumPy 前处理与 CTC/共享几何参考 quick 对照通过；包含 sample、180°、空白、长度/stride、资源限额、错误恢复、结果生命周期和并发调用。它不是人工 GT，几何参考共享生产代码，独立性有边界。报告：[Tiny](reports/short-rec/reference-default-tiny.json)、[Small](reports/short-rec/reference-default-small.json)、[Medium](reports/short-rec/reference-default-medium.json)、[研究 Medium](reports/short-rec/reference-experimental-medium.json)。
- 新安装目录的 WinForms 测试 5 次、框选、GPU 选择/输入、错误编号、异工作目录和干净 PATH 通过：[winforms-device1.json](reports/short-rec/winforms-device1.json)。它仍是在装有驱动的开发电脑，不冒充全新目标机。
- HTTP 图片二进制/Base64、OCR/REC/批量、认证、异常输入、日志、429/503 和恢复通过：[http-smoke.json](reports/short-rec/http-smoke.json)。
- 181 个锁定资产及 5 份冻结契约检查通过。没有改模型、精度、ABI/HTTP/配置/日志 Schema。

本轮没有运行新的远程 CI、Linux 实体 GPU 或 ASan/UBSan 构建，没有更新正式包。真实业务正确性集仍需用户提供可用于仓库的脱敏图片、人工文字/框标注与使用授权，不能用输出一致冒充准确率。

## 6. 复现

Windows x64、VS2022、Vulkan SDK、Python，安装 `requirements-dev.txt`。正式路径和研究路径使用不同构建目录，不能在进程持有 DLL 时覆盖构建：

```powershell
cmake -S . -B build/default -G "Visual Studio 17 2022" -A x64 -DLWVK_EXPERIMENTAL_SHORT_REC=OFF
cmake --build build/default --config Release --parallel 4
ctest --test-dir build/default -C Release --output-on-failure
cmake -S . -B build/short-rec-research -G "Visual Studio 17 2022" -A x64 -DLWVK_EXPERIMENTAL_SHORT_REC=ON
cmake --build build/short-rec-research --config Release --parallel 4

python tests/benchmark_rec_short.py --before build/default/Release/lw.PPOCR.Vulkan.dll --after build/short-rec-research/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 60 --reverse-initialization --report build/rec-qualified.json
```

设备编号请以 probe 输出为准。两个模式/各模型分别串行运行长测，可选 `--corpus` 指向含 `img-*.jpg` 的公开生成集；不传则使用 5 张 sample 尺寸变体：

```powershell
python tests/benchmark_latency_modes.py --library build/default/Release/lw.PPOCR.Vulkan.dll --model tiny --device 1 --stress-iterations 1000 --idle-seconds 2 --report build/soak-default-tiny.json
```

真实业务验收还应覆盖客户图片、设备、驱动、服务账户、空闲/冷启动和最终打包后的程序。下一版本先解决延迟波动的证据与可控开销，再评估真实 batch-N/FP16；后两者仍是独立研究，不在本轮默认接入。

本轮默认 DLL SHA-256：
`c717cab0465813e325cb9baedd66252a8096b5194699fdf7639041e3e8879232`。
研究 DLL SHA-256：
`879a22f15715a8f80dd26b129bfba4c3be221c0f7137963dc83c8b53f6a1cd36`。
进入本轮前保存的开发 DLL SHA-256：
`a08b047fc94deae0f01c70402721dee9ac5b68c1f0a99c8798eafb5b55c6ac3d`；它是 Pointwise Bias 优化后的开发构建，不代表原始 v1.0.1 Release 附件。
