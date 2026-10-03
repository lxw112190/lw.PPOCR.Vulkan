# v0.6.0-dev.2 本机验收记录

日期：2026-10-03。Windows 10 x64、MSVC 19.40、Vulkan SDK 1.4.350.0，普通 Release FP32 构建；协作矩阵/多 REC lane 两个实验开关 OFF。没有改动模型、GPU 计算算法或五份候选契约内容。

## 已完成

- 23 项 CTest 通过，包含新增原生 ABI、样本派生器、2000 次异常图像解码与恢复；ONNX 九模型、2000 个有界随机 protobuf、截断和恶意 length/tag 检查。
- 配置测试仍为 111 个原生进程正反例，19 个导出/布局与 5 份候选契约哈希通过。
- NVIDIA RTX 4060 Laptop（设备 1）与 AMD Radeon(TM) Graphics（设备 0）：Tiny/Small/Medium 各执行 `--quick --extended` 11 场景完整 OCR 对拍，共 66 个模型/设备/场景组合；NVIDIA 每模型另有 2 次循环，AMD 每模型另有 1 次循环，均通过。同时执行 stride/长度、资源拒绝后恢复、关闭 CLS、结果生命周期和同句柄并发检查。
- 两张卡各完成 GPU 图像探针：DET 8 例、CLS/REC 16 例、crop 111 例；精确位值、stride、padding、canary 与恢复通过。
- 新增 native Tiny C ABI 测试在 NVIDIA 完成真实 OCR/REC、5 种 BGR 参数拒绝、末行无 padding 的正 stride、32 次不同尺寸空白图、结果存活与并发复制。
- 安装目录 HTTP smoke 通过：二进制/Base64、完整/REC/batch、认证、配置拒绝、超限/错误输入、429/503 恢复、日志 Schema/隐私与退出。
- HTTP 异常集完成 9 个固定 + 200 个确定性随机请求（共 209），每个固定用例和每 25 个随机用例后真实 REC 恢复成功，正常退出，访问日志通过 Schema。
- WinForms host-only 检查通过：不同工作目录、GPU 编号可编辑、鼠标正反向/缩放映射；该项不声称 WinForms GPU 推理测试，原生 GPU 与 HTTP 测试独立进行。
- 176 个固定依赖/模型资产、工作流 YAML、所有工作流引用脚本、Python 语法、clang-format 17 及 Git whitespace 检查通过。MSVC 配置 `LWVK_SANITIZERS=ON` 明确被拒绝，没有静默失效。

## 扩展测试发现的两类测试问题

倾斜图的 x1=-4.394726753234863，Vulkan 与 CPU 参考完全相等。裁切边界点后重新拟合旋转矩形，角点可能在图像之外；旧对拍的“所有角点在图内”断言与既有几何和候选 Schema 不符。本轮只修正该断言并加负坐标 Schema 用例，文字相等、分数 0.003、坐标 1.1 像素容差不放宽，DB/crop 算法不改。

HTTP 测试里四张 Base64 图片先超过 64 KiB body 上限，未到批量数量保护；改成小 body 隔离 batch count。随机 JSON 字节可能先触发深度保护，改成明确非法 UTF-8 + 无括号的随机 hex 字符隔离 JSON parser；原有深度保护用例仍在 HTTP smoke 中保留，不通过同时接受多个业务码掩盖顺序问题。

## 二进制和报告

`build/v062/staging`：

- DLL SHA-256：`18c635c128fa3d1b7e970104196bc8c790f396a5950f5bb06d6e5bd21f8bb4d2`
- HTTP EXE SHA-256：`2d639e6876c7fb39779ef251d1d5fe38e393832c27bf9038bb6e5f144096bcd7`

[原始报告](reports/v062/)保存六份对拍 JSON、HTTP smoke 和 209 请求异常集报告。每份对拍包含输入像素 hash、设备、模型、环境和尺寸。

本地 Windows 预览包位于 `dist/v062/`，使用实际验证的 DLL/EXE，带三模型、WinForms/C#、HTTP/Web、服务脚本、Schema 与逐文件清单；打包后独立校验附件 hash 和完整文件集合。

## 不能据此声明

本机没有 Linux sanitizer 环境；新 [sanitizer CI](SANITIZERS.md) 要在推送后实际运行，不能将工作流已编写等同于 ASan/UBSan/LSan 已通过。11 场景是同一公开样图的派生集，不是多来源真实准确率集。DB/crop 参考共用 C 几何，只有网络/预处理/CTC 独立对拍。上述是短回归，没有重跑三模型 1000 次、整夜 soak、VRAM/private commit 或服务账户验证；旧 dev.1 长测仅对应旧二进制。Windows 结果不代替 Linux/lavapipe/实体 Linux GPU、Intel 或其他架构资格验证。

通用 OpenCV 发布技能的预检仍有 7 PASS / 2 WARN / 1 FAIL：三个 OpenCV 限定检查在本纯 Vulkan 项目中不适用，已明确记录，不为消除它们引入 OpenCV。实际 SBOM/供应链审查及正式平台验收继续保留为 v1.0 欠项。

English: 23 host tests, six official-model/physical-device extended references (11
derivative scenarios each), exact GPU image probes, native ABI recovery, installed
HTTP smoke and 209 hostile requests passed locally. New Linux sanitizers remain
pending CI. These short regressions do not prove leak freedom, diverse real-world
accuracy, Linux hardware compatibility, or production readiness.
