# 三模型主机流水线优化 / Host-pipeline optimization

2026-10-01，Windows 10 / Ryzen 7 7735H / RTX 4060 Laptop GPU。本轮不是调整 Demo 的计时显示，而是实际减少原生 OCR 的 CPU 工作量。默认 FP32、DET 长边上限 960、方向分类、模型、GPU shaders、C ABI 和 JSON 字段均不变。

## 改动

- CLS/REC 双线性缩放：横坐标与边界夹取预计算，纵坐标每行只计算一次，复用行指针。保留原浮点计算顺序。
- 透视裁剪：B/G/R 三通道共享采样坐标、边界夹取和权重；不修改第三方原始裁剪源码，保留许可证与来源。
- 工程诊断 `LWVK_HOST_PROFILE=1`：在加载 DLL 前设置，向 stderr 输出 `LWVK_HOST_PROFILE` 前缀 JSON，分解检测预处理、DB 后处理、裁剪、CLS/REC 预处理及图执行墙钟时间。不输出识别文本或图片。默认关闭，性能测试必须关闭；不属于公共日志 Schema。

先前 sample.jpg 的 CPU CLS/REC 预处理约 15～19 ms、裁剪约 6～7 ms。本轮针对这些重复计算；Medium 的 REC GPU 算子仍然是主要瓶颈，后续继续优化，尚不宣称全面超过 DML。

## 同图交替对照

预解码 BGR → 原生 OCR → JSON 复制/Python 解析；不含文件解码、模型初始化与 GUI。每个模型/尺寸旧新各预热 3 次，交替运行各 30 次，奇数轮反转顺序；同进程同 GPU，无其他测试 GPU 负载。下表是墙钟中位数，不是纯 GPU 时间。

| 模型 | 图片边长 | 旧版 ms | 新版 ms | 耗时降低 |
|---|---:|---:|---:|---:|
| Tiny | 500 | 48.32 | 36.94 | 23.56% |
| Tiny | 1000 | 81.28 | 64.71 | 20.39% |
| Small | 500 | 71.12 | 59.91 | 15.76% |
| Small | 1000 | 109.62 | 91.88 | 16.19% |
| Medium | 500 | 137.56 | 124.28 | 9.66% |
| Medium | 1000 | 197.12 | 178.83 | 9.28% |

全部 `items` 字段严格相等（文本、四点坐标、分数、分类结果等），不是只比较条数。GPU 阶段时间基本不变，收益来自主机流水线。

AMD Radeon(TM) 集显补测每模型各 10 次：Tiny 86.43 → 72.60 ms；Small 190.81 → 193.21 ms；Medium 645.61 → 650.75 ms。后两者没有稳定提速，不能将 NVIDIA 的改善百分比推广到其他显卡。

## 100 图回归

同一生成测试集 100 图、614 行标注、20125 字符；每模型预热一遍，再计时三遍，合计 1200 次 OCR（900 次计时）。与上一轮报告的每图完整预测对象、各项准确率严格相等，失败与不稳定结果均为零。

| 模型 | 旧平均 ms | 新平均 ms | 耗时降低 | 行完全匹配率 |
|---|---:|---:|---:|---:|
| Tiny | 51.02 | 41.04 | 19.57% | 60.26% |
| Small | 58.55 | 50.53 | 13.71% | 79.15% |
| Medium | 106.27 | 98.98 | 6.85% | 80.94% |

这组语料旧新测试发生在不同时间段，性能百分比不如同图交替实验严格；合成集准确率不代表客户真实图片准确率。没有本轮新增长测无泄漏结论。

## 正确性与资源边界

- 12/12 CTest；90 个 resize 张量逐位相等，72 个裁剪样例逐字节相等（含透视、竖排、边界、stride、canary 和异常输入）。
- 三模型独立 ORT CPU 对照：原图、180°、空图、非法缓冲区后恢复、结果生命周期、同句柄并发通过。参考几何使用保留的原始 C 实现，预处理/CTC 为独立 NumPy 实现。
- DET 960、REC 多尺寸、共享工作区增长、32 项 LRU、回读保护、低预算拒绝后恢复通过；此前报错的私有大图另做 Medium 五次回归。私有图片/识别文本不打包。
- GPU shared-memory 行填充实验在六组交替测试中没有稳定收益，已撤回，未进入默认路径。

证据与源码/二进制哈希：[qualification.json](reports/pipeline-optimization/qualification.json)。旧 DLL：`0cfd9b5bbc192193b3208392eaee1cdb9e7012202e5f944522a504570ea81894`；新 DLL：`91bf808abe1fdb26afb0ce692b54d2b64286d20a2f54d3aa97c6590ab251cfee`。

```powershell
cmake --build build/pipeline-opt --config Release --parallel 4
ctest --test-dir build/pipeline-opt -C Release --output-on-failure
python tests/benchmark_vulkan_pair.py --before build/transfer-optimized/Release/lw.PPOCR.Vulkan.dll --after build/pipeline-opt/Release/lw.PPOCR.Vulkan.dll --device 1 --iterations 30 --report build/reports/pipeline-opt/recheck.json
```

English: This revision removes redundant coordinate/clamp work in CLS/REC bilinear preprocessing and shares perspective-sampling calculations across color channels. It preserves FP32 arithmetic, models, shaders, DET 960 and public contracts. On the tested RTX 4060 Laptop, paired 500/1000-pixel tests reduce Tiny latency by 20–24%, Small by ~16%, and Medium by 9–10%, with every output item strictly equal. The 100-image three-pass regression preserves all predictions/accuracy. AMD Small/Medium showed no stable gain. Results exclude initialization, file decoding and GUI; they are not universal speed or long-run leak guarantees. Medium GPU recognition remains the next major optimization target.
