# v1.0 发布验收与兼容政策 / Release qualification

当前发布版本：**1.0.1**；本页记录 v1.0.0 起的冻结政策。此前维护者报告的依赖审查、最终包长测、干净目标机和服务账户 GPU 验收见 [验收记录](MAINTAINER-ACCEPTANCE.md)。不补造未提供的运行链接、附件哈希、硬件或内存指标；新开发快照须按下述门禁重新验收。

## 正式冻结范围

- C ABI v1：19 个公共导出、状态码、UTF-8/所有权规则与 x64 结构布局。
- HTTP API v1：路由、输入输出、错误码与请求 ID 规则。
- 配置 Schema v1：版本、字段、默认值/约束与未知字段拒绝。
- JSONL 访问日志 Schema v1：稳定字段与时间格式；runtime.log 诊断文字不属于机器契约。

五份机器契约及锁文件位于 `schemas/`，状态为 `frozen`。本次从 candidate 转正仅修改契约标题、说明与状态，不修改 API 字段、结构布局、符号或约束。CI 只检查，不自动重生成哈希。默认模型/字典快照继续由资产锁固定；旧 Tiny 内部 v0 格式不是通用模型格式标准。

后续 1.x 修复保持上述现有接口兼容。破坏性变化需新增版本化接口/Schema，不能用重生成锁文件掩盖；模型、依赖升级需独立审核与正确性回归。正式版不自动承诺无限 LTS 年限或所有平台/显卡支持。

## 每次发布仍执行的门禁

| 层次 | 要求与边界 |
| --- | --- |
| 日常 CI | Windows/Linux 构建、主机单测、真实导出/布局、配置、安装目录与依赖闭包 |
| 软件 Vulkan | 发布 ref 手动完整套件：shader、三模型对拍、HTTP；不是实体 GPU 性能验收 |
| Sanitizer | ASan/UBSan/LSan 插桩与故障探针、安装后的接口/异常恢复；原始失败不能由诊断对照覆盖 |
| 供应链 | 固定资产、完整许可证/模型、SBOM、契约和构建信息；不等于漏洞清零 |
| 最终附件 | 重新构建目标版本、核对 SHA-256、启动/真实 OCR/HTTP、服务账户与升级回滚 |
| 平台 | 只按 [兼容矩阵](COMPATIBILITY.md) 声明；额外发行版/架构/GPU 单独验收 |

本次文档/版本元数据变更后仍需推送、CI 重建并确认结果；不可将旧开发包改名为正式包。相同 ref 的不同附件也不能自动共用哈希验收身份。记录实际附件、源码 commit、环境/模型和原始测试结果。

操作见 [CI 分层](CI.md)、[部署/升级/回滚](DEPLOYMENT-UPGRADE.md)、[Sanitizer 门禁](SANITIZERS.md)。普通 build.yml 绿色不替代手动完整推理与目标机测试。

## 本机复验示例

```powershell
python -m pip install -r requirements-dev.txt
python scripts/verify_assets.py
python scripts/contracts.py --check
python scripts/supply_chain.py
cmake -S . -B build/v100 -G "Visual Studio 17 2022" -A x64 -DLWVK_EXPERIMENTAL_COOP=OFF -DLWVK_EXPERIMENTAL_REC_LANES=OFF
cmake --build build/v100 --config Release --parallel 4
ctest --test-dir build/v100 -C Release --output-on-failure
cmake --install build/v100 --config Release --prefix build/v100/staging
python tests/test_http.py --package build/v100/staging --output build/v100/http --device 1
python scripts/package.py --staging build/v100/staging --output dist/v100 --platform windows-x64
python scripts/verify_archive.py --archive dist/v100/lw.PPOCR.Vulkan-v1.0.0-windows-x64-full-ocr.zip
```

`--device 1` 只是示例，使用 probe 实际编号。Linux 使用 Ninja/Release、`.so` 和 `--platform linux-x64`。上列 smoke 不替代三模型正确性、长测或服务验收。测试工具需要 Python，运行预编译包不需要 Python/SDK。

默认保持 FP32、DET960 和预算；FP16/协作矩阵、多 REC lane 实验开关为 OFF，不属于正式模式。没有兼容 Vulkan 驱动时明确失败，CPU 图像处理不是 CPU OCR 回退。SHA-256/清单是完整性检查，不是数字签名或无泄漏保证。

English: v1.0.0 freezes the existing x64 C ABI and HTTP/config/access-log v1 contracts.
Only status/title/description metadata changes at graduation. Preserve existing 1.x interfaces;
version breaking changes separately. Each release still needs CI, manual software references,
sanitizers, source/model inventory and final-artifact target/service qualification.
No historical archive may be renamed as the new release; no universal GPU support or indefinite LTS is promised.
