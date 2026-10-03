# CI 分层与发布验收 / CI layers

日常 Windows/Linux 任务按同等级主机检查设计，不使用软件 Vulkan 执行完整 OCR。
原完整 Linux 验证套件没有删除，迁移为独立手动工作流；ASan/UBSan 工作流保持不变。

| 工作流 | 触发方式 | 范围 | 下载产物 |
| --- | --- | --- | --- |
| `build.yml` | push / PR / 手动 | Windows/Linux 构建、主机 ABI/配置/单元测试、安装目录、完整性与打包 | Windows zip、Linux tar.gz、SHA-256 |
| `linux-software-validation.yml` | 仅手动 | lavapipe shader 精确探针、DET/CLS/REC、11 个派生场景、三模型、真实 HTTP 推理、安装包复验 | 软件对拍验收包、JSON 报告、失败诊断 |
| `sanitizers.yml` | 原有 push / PR / 手动 | ASan/UBSan/LSan、故障探针、软件 Vulkan 探针、原生接口和 HTTP 异常恢复 | 诊断文件，不是正式部署包 |
| `security.yml` | 原有 push / PR / 手动 | 固定资产、许可证/SBOM、契约与供应链故障注入 | SBOM 与审计报告，不运行 CodeQL |

Windows 额外检查 WinForms 主机布局/框选映射，Linux 额外检查 ELF 动态依赖和 systemd unit；
平台专属检查不必机械地相同。两者都保留模型、字典、网页、SDK 示例、Schema、许可证和校验清单。

## 日常 CI

- Windows 与 Linux job 总上限均为 30 分钟；这是失败边界，不是预计耗时。
- Linux 仍缓存官方 SDK，校验下载哈希；不再安装仅完整网络对拍需要的 ORT/ONNX Python 包。
- 设备探测使用 `--allow-no-device`。即使成功列出设备，也只证明枚举，不证明 OCR 或显卡兼容。
- 日常 artifact 为 `windows-x64-full-ocr` / `linux-x64-full-ocr`；手动完整验证为 `linux-x64-software-validated-full-ocr`。full-ocr 指包包含完整 OCR 能力，不代表日常 CI 已跑真实推理；正式版本的压缩包文件名不再带 preview。
- 不缓存最终发布二进制来跳过编译，也不关闭主机接口、配置、SBOM、依赖闭包或安装目录检查。

这会减少日常工作量，但实际耗时仍受 SDK 缓存命中、runner 负载和下载影响；不承诺固定 5 分钟。

## 发布前完整验证

1. 在 Actions 手动运行 **Linux software Vulkan full validation (manual)**，选择候选发布对应的 ref。
2. 原 90 分钟总上限、CPU 软件设备限定的 180 秒网络 fence 等待、验证层与同步检查保持不变。
3. 查看 shader、Tiny、Small、Medium、HTTP 与归档校验步骤；任一步失败不能作为发布验收通过。
4. 下载该工作流的验收包、报告及 SHA-256，核对来源 commit 和 `BUILD-INFO.json`，在目标机复测。

Tiny/Small/Medium 已拆成独立步骤，便于直接看到每个模型耗时；失败时保留 CTest 和服务日志。
手动套件不再在每次 push/PR 自动运行，意味着真实网络回归可能延后发现。
发布前必须由维护者主动运行并审核；日常绿色不能代替这项检查。

软件 Vulkan 实际由 CPU 执行 shader，不能用于 GPU 性能宣传，也不证明实体 GPU、服务账户或其他系统兼容。
手动套件重建并验收的是它自己的附件；即使同一个 ref，也不能把另一个不同哈希的日常包自动视为同一已验收包。
最后发布的实际附件仍需核对 SHA-256、启动、真实 OCR、HTTP 和服务部署。

English: daily Windows/Linux jobs have equivalent host-only build/package scope,
with platform-specific UI/ELF/service checks retained. Full software-Vulkan
references remain in a manual-only workflow and must run on the candidate ref
before release. Sanitizer CI is unchanged. Daily green checks do not qualify
inference, hardware performance or a different archive's hash. No fixed runtime
or universal compatibility is promised.
