# v0.7.0-dev.1 本机验收 / Local validation

日期：2026-10-03。Windows 10 x64 / MSVC 19.40.33813.0 / CMake 3.31.0-rc2 /
Vulkan SDK 1.4.350.0，默认 FP32，两个实验选项 OFF，sanitizer OFF。
本轮改供应链/打包/文档/CI，不改推理算法、官方模型或公共候选契约。

## 实际二进制

安装目录：`build/v070/staging`。`BUILD-INFO.json` 标明源码基线
`970cd7b96653eaae38e77a86722367328d1588d4`，`source_dirty: true`，这是本轮未提交的本地构建，不是该 HEAD 的干净发布产物。

| 文件 | SHA-256 |
| --- | --- |
| `lw.PPOCR.Vulkan.dll` | `ffda5a8b713441028bc5296fd7ec00f0ee8f443f8b1bb3212b2c26a494cf91dd` |
| `lw-ppocr-vulkan-http-service.exe` | `905e02225b34a475a77b84f9a883e3458edd9e8213e73d1ec64bc2abe9dad7de` |

压缩包/sidecar 在 `dist/v070/`；实际附件哈希以生成的 `.sha256` 为准。
历史 v0.6 包/报告保留，没有覆盖。

## 通过项

- Windows 默认 Release 编译，25 项 CTest 全部通过；含 19 导出/结构布局、111 项实际配置矩阵、模型/图像异常输入和恢复。
- 181 个固定资产校验；16 组件 CycloneDX 1.6 SBOM 通过官方 Schema 的离线校验。
- 9 项供应链故障注入：缺失/篡改许可证、重复/不安全路径、字典不可改换行、未锁定文件、版本标记、许可元数据、SBOM 格式/漂移、错误平台/架构/Debug/sanitizer/实验 Release 元数据。
- 从安装目录运行 AMD device 0 / NVIDIA device 1，Tiny/Small/Medium 各 11 个扩展场景（66 组合），对拍通过；包含输入长度/stride、资源限制、恢复、结果寿命和同句柄并发。
- 同一公开样图的原始/旋转/压缩比例/低对比度/倾斜/灰度/空白派生集；不是 66 张独立真实样本。网络/CTC/前处理 ORT/NumPy 对拍独立，但 DB/crop 共享 C 几何参考，不是完全独立几何实现。
- 安装后的 HTTP smoke：binary/Base64、完整/仅识别/顺序批次、API Key、非法/超大输入、静态页、JSONL、日志隐私、429/503、恢复和正常退出。
- 209 次确定性异常 HTTP 请求（seed 6102），固定异常后及每 25 个随机异常后实际 REC 恢复通过。
- WinForms host-only，以及两张物理显卡分别完整 OCR/ROI、GPU 下拉/手填/无效值、正反框选/resize、延迟详情页通过；从非部署工作目录启动。
- 3 份工作流 YAML 可解析，全部引用脚本存在；5 份候选契约哈希和头文件指纹未变。
- 从实际 staging 临时移走 fmt 许可证，真正的 package 命令明确拒绝且没有生成附件，随后恢复并复验成功。三个旧锁维护脚本 dry-run 后锁文件原始 SHA-256 未变。

原始对拍/HTTP/WinForms JSON 见 [reports/v070](reports/v070/)；不要把短回归的一次调用耗时作为性能提升结论。

## 未验证与正式发布前的工作

后续补充：维护者已自行完成依赖安全审查、最终包长测、干净目标机及服务账户 GPU 验收，
见 [维护者验收](MAINTAINER-ACCEPTANCE.md)。下面描述的是本机这一轮未执行的项目，
不是否定维护者后来完成的验收；修复后的新附件需关联其实际哈希。

本轮没有 Linux 执行环境，也没有执行远程 CodeQL/ASan/UBSan。新工作流须推送后检查真实结果；SBOM 不是 CVE 清零。本轮没有 1000～5000 次长测、GPU validation probes 重跑、无泄漏认证、Intel GPU、Linux 实体 GPU、干净目标机、Windows Service/systemd GPU 权限或升级回滚验收。

下一步按 RELEASE-GATES：新 CI、依赖安全公告/适用 CVE 审查、素材再分发权确认；之后对最终默认二进制做三模型长测、RSS/VRAM趋势和服务/目标机验收，再进入不可覆盖的 RC，而不是直接改版本号成 v1.0。

通用发布技能的 OpenCV preflight 本轮为 7 PASS / 2 WARN / 1 FAIL：三个非 PASS 全部仅针对 OpenCV 发现/版本/拒绝检查。本项目是纯 Vulkan，无 OpenCV 运行依赖，明确不适用；不是本项目 CI 失败，也不为消除这些条目添加 OpenCV。

English: these are artifact-specific Windows regression and source-supply-chain
results. Remote security/sanitizer CI, sustained soak, physical Linux GPUs,
service-account access and production upgrade/rollback remain unqualified.
