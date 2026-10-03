# 维护者验收记录 / Maintainer-reported acceptance

记录日期：2026-10-03。项目维护者在本次沟通中确认，已自行完成下列项目：

| 验收项 | 当前记录 |
| --- | --- |
| 依赖安全审查 | 维护者已完成 |
| 最终包长时间测试 | 维护者已完成 |
| 干净目标机部署测试 | 维护者已完成 |
| 服务账户 GPU 验收 | 维护者已完成 |

以上是维护者直接报告的结果，不是本机自动测试或 CI 输出。没有提供测试包
SHA-256、系统/GPU/驱动、模型、次数/时长、RSS/VRAM 原始曲线或安全审查明细；
不虚构这些数字，也不因此宣称零漏洞、无泄漏或新增操作系统支持。
发布附件选定后，将实际附件哈希和测试记录关联到该附件，便于后续维护。

## 本次 CI 错误及修复

提供的 Linux GNU 11.4 日志中，25 项 CTest 有 24 项通过；唯一失败是
`abi-export-contract`。动态库多导出私有函数 `lwvk_crop_quad_bgr_u8`。

根因：主动态库只设置了 `CXX_VISIBILITY_PRESET hidden`，未设置
`C_VISIBILITY_PRESET hidden`。新增的 `src/crop_optimized.c` 是 C 编译单元，
没有继承 C++ 的可见性设置，Linux 因默认可见性导出它。
Windows 使用显式 `__declspec(dllexport)`，因此此前 Windows 检查没有暴露此问题。

修复为主动态库同时隐藏 C 与 C++ 符号，公共函数仍由 `LWVK_API` 显式导出。
19 个公共接口、结构布局和五份候选契约不改，不把私有辅助函数加入公共基线。
ELF 检查也覆盖内部提取代码的 `lw_` 前缀；普通与 sanitizer CI 在安装目录再次检查导出。

修复前后规则由两项主机解析器单元测试覆盖：19 个候选符号/版本后缀正常通过，
额外的 `lwvk_crop_quad_bgr_u8` 和 `lw_crop_quad_bgr_u8` 不会被过滤掉。
它验证的是检查器，不是本机 Linux 链接；实际 `.so` 结果仍由 CI 验收。

本机 Windows 修复后重新配置/构建，26 项 CTest 全部通过；新安装目录仍为
19 个公开导出，五份候选契约与供应链校验通过。Windows DLL SHA-256 仍为
`ffda5a8b713441028bc5296fd7ec00f0ee8f443f8b1bb3212b2c26a494cf91dd`，
本轮未更新模型或依赖，也没有改公共 ABI 基线。上述本机结果不是 Linux CI 通过声明。

维护者于 2026-10-03 报告上一轮 CI 已全部通过，Linux 完整任务耗时约 43 分钟。这是维护者报告的结果；未在本地重跑 Linux，也不补造 run URL、commit/hash 或额外硬件数据。

本轮将日常 Windows/Linux 任务对齐，完整软件 Vulkan 测试迁移到独立手动工作流；修改后的工作流仍需推送复验，发布前运行完整套件，见 [CI 分层说明](CI.md)。
维护者之前完成的验收不会被否认，但新生成的 Linux 附件需要至少重新核对
哈希、启动和 OCR/服务 smoke，不能自动继承旧附件的完整验收身份。

当前仍是 v0.7.0-dev.1 开发预览，不自动发布 v1.0、冻结契约、推送或移动标签。
此轮 CI 修复通过后，可推进 RC；升级/回滚、素材再分发权限和任何新增平台范围
只按实际确认记录，不从上述四项验收推导。

English: the maintainer reports dependency review, sustained package testing,
clean-target deployment and service-account GPU validation complete. These are
separate from local/CI evidence; no missing metrics or platform details are
invented. The maintainer subsequently reports all previous CI jobs passed,
with the full Linux job taking about 43 minutes. Daily host-only builds and a
manual full software-Vulkan suite are now separated; the edited workflows need
fresh CI, and the final release artifact still needs qualification and smoke.
