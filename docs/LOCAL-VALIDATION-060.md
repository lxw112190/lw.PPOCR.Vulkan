# v0.6.0-dev.1 本机验收记录

日期：2026-10-02～2026-10-03，Windows 10 x64，MSVC 19.40，Vulkan SDK 1.4.350.0。
这是本地候选构建验证，不是正式 ABI 冻结、全平台支持或无泄漏证明。

## 契约与正确性

- 21 项 CTest 全部通过，包含 19 个真实 C 导出、native/ctypes x64 布局、状态码、契约哈希与包损坏检查。
- 配置 Schema 和实际 `--check-config` 完成 111 个正反例；进程正确退出且不初始化 GPU。
- RTX 4060 Laptop：Tiny/Small/Medium 各执行完整 OCR quick 对拍（320×320 原图、180°、空白，额外 20 次循环），涵盖 stride/缓冲区边界、分类关闭、资源拒绝恢复、结果生命周期和同句柄并发。原生 JSON 严格符合候选 Schema；文字/分数/坐标仍按独立 ORT CPU 参考比较。
- AMD Radeon(TM) Graphics：同样三个模型各完成 quick 对拍，额外 2 次循环；这是短回归，不是 AMD 长测。
- RTX GPU 图像探针：DET 8 例、CLS/REC 16 例、crop 111 例通过精确位值、stride、补边、canary 和恢复检查。
- 安装目录 HTTP 测试通过：二进制/Base64、整图/仅识别/批量、认证、异常/超限输入、429/503、恢复、正常退出，以及真实响应/访问日志 Schema 和隐私检查。

## 三模型重复稳定性回归

使用 `tests/stress_http.py`，从同一安装目录依次启动三个独立服务进程，串行测试，默认能力感知 GPU 流水线，DET 长边 960，不开启性能实验或 profiler。

每模型先覆盖 5 种尺寸，再额外预热 100 次整图 OCR；正式统计 1000 次整图 OCR 与 41 次仅识别请求。图片为公开 sample 的 250×250、500×500、1000×1000、900×450、450×900 变体。

| 模型 | 正式 OCR 请求 | 统计段时间（秒） | 预热后 RSS（MiB） | 结束 RSS（MiB） | 净变化（MiB） |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tiny | 1000 | 69.08 | 179.91 | 179.80 | -0.11 |
| Small | 1000 | 85.47 | 246.24 | 213.35 | -32.89 |
| Medium | 1000 | 144.47 | 429.65 | 215.40 | -214.25 |

三组均成功退出，文字结果与各尺寸预热基线一致，所有响应符合 Schema。采样线程数 18～23、Windows 句柄数 419～424，未出现无界增长；RSS 净增长未超过预设 96 MiB 信号阈值。

**RSS 是 Windows 进程驻留工作集，下降可能来自操作系统回收/分页，不能解释为分配内存全部释放，更不能证明没有 CPU/GPU 内存泄漏。** 本轮没有量测 dedicated VRAM/private commit，也没有做 sanitizer 或整夜 soak。正式统计只用了数分钟，是重复稳定性回归，不是长时间生产负载认证。压力段比较文字基线和 Schema，详细分数/坐标由上述独立参考短回归覆盖；不要把二者描述成相同覆盖。

HTTP 统计耗时包含图片解码、CPU/driver 与 HTTP 客户端开销，不是纯 GPU 推理基准，也不用于新的跨后端提速结论。

## 二进制与原始记录

本轮 `build/v060` 正常 Release 构建，两个实验构建开关均 OFF：

- DLL SHA-256：`6c673c5c7bf59a7a087addced2f876d12a96c706c82df0753e67c5c6804ce511`
- HTTP EXE SHA-256：`dce756d8f30b73b93071253178b66d75c392da6f215af99d5b8c66303ad6a5e3`

[三模型与双显卡原始报告](reports/v060/)包括 HTTP 契约 smoke、六份 OCR 参考报告和三份重复测试报告。依赖/模型锁检查仍为 176 个固定文件通过，没有自动修改模型哈希。

最终 Windows 本地预览 ZIP 包含同一 DLL/EXE、模型、WinForms/C# 示例、网页、服务脚本、候选 Schema 和文件清单；打包后用 `verify_archive.py` 校验附件 SHA-256 与全部 payload 文件。尚未验证远程 0.6 CI、Linux 实体 GPU、服务账户/session 0、Win11、Intel 或其他架构。

后续门槛见 [RELEASE-GATES.md](RELEASE-GATES.md)：ASan/UBSan、更广公开正确性集、SBOM/依赖与许可证审查、最终 CI 包的目标机与服务部署复验，再进入 RC。
