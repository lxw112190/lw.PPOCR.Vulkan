# Pointwise Bias 融合与大词表投影优化（2026-10-04）

本轮是本地开发快照，不重发或覆盖已发布的 v1.0.1。保持三模型、FP32、DET 长边上限 960、CLS 默认开启、C ABI、HTTP/配置/日志 Schema 与阶段计时口径不变。

## 保留的优化

- 将单消费者的 `1×1 Conv → 通道 Add` 合并：卷积仍按原顺序完成 FP32 点积，随后加原 Bias。减少中间张量写入、再次读取与独立 dispatch，不重新计算权重。
- 仅接受 group=1、输入通道能被 4 整除、stride=1、零 padding、常量通道 Bias。已有 Bias、已有融合激活、共享中间值、客户端可观察的中间输出或布局/字节数不符均不融合。参考 kernel 模式不启用本轮 Bias 融合。
- NVIDIA REC 大词表尾部投影采用 M16/N128/K32 kernel；权重允许通道补齐，但 Bias 与输出行保持实际通道数。其他设备保留原 kernel。遵守禁用 tiled kernel 的诊断选项。
- 增加 14 项融合安全门槛测试，以及 136 组投影张量/通道尾部/激活/输出 canary 检查。

主要收益来自 Bias Add 融合，不把投影 tile 单独宣称为明显提速。

## 没有默认接入的方案

试过上传与 DET 合并提交、裁剪命令缓存和 M32 通道尾部投影。固定样本收益很小，部分变尺寸流出现回退，因此没有用它们替换生产默认路径。裁剪命令缓存、M32 尾部扩展已撤回；合并上传的内部接口仅保留用于工程探针，生产 OCR 仍使用原有串行上传/等待。

这不修改电源策略，不隐式循环预热 GPU，也不能消除笔记本显卡空闲后降频带来的首轮延迟。

## 固定样本交替 A/B

Windows 10 x64、Ryzen 7 7735H、RTX 4060 Laptop。预先解码 BGR，每个尺寸每版预热 3 次，然后交替调用各 30 次，并反转引擎初始化顺序。计入原生 OCR、结果复制与 Python JSON 解析；不含文件解码、模型初始化或 GUI。原始数据见 [pair.json](reports/pointwise-bias/pair.json)。

| 模型 | 图片 | 旧版中位数 ms | 新版中位数 ms | 耗时降低 |
| --- | --- | ---: | ---: | ---: |
| Tiny | 500×500 | 20.63 | 19.46 | 5.66% |
| Tiny | 1000×1000 | 32.52 | 31.38 | 3.50% |
| Small | 500×500 | 39.36 | 36.06 | 8.38% |
| Small | 1000×1000 | 56.47 | 53.42 | 5.40% |
| Medium | 500×500 | 91.71 | 85.94 | 6.29% |
| Medium | 1000×1000 | 126.78 | 121.32 | 4.31% |

全部 16 个结果对象的文字、坐标、方向与置信度严格相等。

## 100 张变尺寸图片流

使用 lw.PPOCR.C 的 `lw-generated-ocr` 100 张生成图片，每模型两轮、新旧交替，总计 1200 次 OCR；不逐图预热，第二轮逆序。原始数据见 [stream.json](reports/pointwise-bias/stream.json)。

| 模型 | 首轮平均旧→新 ms | 首轮降低 | 第二轮平均旧→新 ms | 第二轮降低 |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 22.43 → 21.52 | 4.06% | 19.92 → 19.43 | 2.47% |
| Small | 38.95 → 35.78 | 8.14% | 34.50 → 32.73 | 5.13% |
| Medium | 77.61 → 74.75 | 3.68% | 73.71 → 71.30 | 3.28% |

所有图片的完整结果对象严格相等；这是保持原结果，不代表识别准确率为 100%。

## 大小图切换与内存观察

原先 2448×3264 合格证图片的临时附件已失效，没有将替代图冒充原图。本轮明确使用 500×500 的 sample 与生成集 `img-016.jpg`（1792×1392），每模型 20 轮大/小交替，分别在独立进程测新旧版本。结果哈希相同。

| 模型 | 新版切换前小图 ms | 交替中的小图 ms | 切换结束后小图 ms | 旧→新进程峰值 RSS MiB |
| --- | ---: | ---: | ---: | ---: |
| Tiny | 19.33 | 19.52 | 19.48 | 180.71 → 179.70 |
| Small | 35.90 | 35.98 | 36.12 | 241.86 → 241.07 |
| Medium | 86.09 | 86.11 | 86.43 | 606.23 → 602.47 |

报告见 [Tiny](reports/pointwise-bias/size-tiny.json)、[Small](reports/pointwise-bias/size-small.json)、[Medium](reports/pointwise-bias/size-medium.json)。RSS 是整个 Python 测试进程的观测，不是显存；短测不能证明完全无泄漏，也不能承诺其他机器相同内存或速度。

## 验证范围与版本标识

- 30 项 CTest 通过；优化器单元测试共 59 个门槛。
- NVIDIA、AMD 的投影探针各 136 组与既有 kernel 逐位一致；GPU 裁剪、stride、尾部与恢复另有 183 组检查。
- RTX 4060 三模型独立 ORT CPU/几何参考通过；Tiny 另完成 11 个派生场景；AMD Tiny 独立参考通过。
- C# WinForms 实体 GPU、连续 5 次、框选、可填写 GPU、异工作目录通过。
- HTTP 二进制/Base64、OCR/REC、认证、异常输入、日志、429/503 与恢复通过。
- 软件 Vulkan 工作流补充新投影探针；本机通过不等于远程 CI 已通过。本轮没有执行 Linux 构建或新的长期单句柄 soak。

本轮开始时保存的对照 DLL 已含此前 REC 缓存修复，但版本字符串仍为 1.0.0；不能把它当作未修复的 v1.0.0 正式包。对照 SHA-256：
`1f1e1bcdb4e7de3b161bf5cf20617f97d0658ddf0d893b375af1f158e9fa8699`。
本轮最终本地 DLL 的版本字符串为 1.0.1，SHA-256：
`a08b047fc94deae0f01c70402721dee9ac5b68c1f0a99c8798eafb5b55c6ac3d`。
原始报告均绑定相同最终 DLL 哈希，不覆盖正式 Release。

English: This local revision fuses safe, single-consumer pointwise bias additions after the unchanged FP32 dot product and qualifies a tail-aware vocabulary projection on NVIDIA. Paired full OCR medians improve by 3.5–8.4%; the warmed changing stream improves by 2.5–5.1%. Exact item equality is preserved, not a claim of perfect OCR accuracy. No power-policy changes, Linux CI success or new long-duration leak qualification are claimed.
