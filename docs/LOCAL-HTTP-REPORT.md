# HTTP 本地验证 / Local HTTP validation

2026-09-30，Windows 10 x64，0.4.0-dev.1，Vulkan FP32 / PP-OCRv6 Tiny。
不是跨平台正式发布、性能对比或无泄漏证明。

AMD Radeon(TM) Graphics（0）和 NVIDIA RTX 4060 Laptop（1）分别通过：

- 实际安装目录启动，工作目录与配置文件目录不同；模型/网页路径按配置正确解析。
- 完整 OCR 16 个结果，标题“纯臻营养护发素”；仅识别标题 ROI 输出相同文字。
- 二进制 JPEG/PNG/BMP、JSON/Base64 等价、批量顺序、API Key 缺失/错误/正确。
- 非法 JSON、字段/Base64、无效/空图、像素/请求/批量超限及合法请求恢复。
- 连接队列满 429、等引擎 503、正常关闭，日志时间/request_id 和正文/密钥隐私。
- 网页本机浏览器加载测试图、OCR 检测框、结果和耗时实际展示检查。

报告：[AMD](reports/http-amd.json)、[NVIDIA](reports/http-nvidia.json)。

## NVIDIA 1000 次变尺寸完整 OCR

在五种尺寸 250×250、500×500、1000×1000、900×450、450×900 间循环，
每次文字与对应尺寸的预热结果比较；每 25 次额外做一次仅识别。
主循环 1000 次完整 OCR、含预热的 41 次仅识别均成功；约 492 秒。
观察到中位往返 453ms、P95 703ms；包含 HTTP/CPU/计划切换，非纯 GPU 基准，
浏览器检查短时共享同一测试服务，不能当作独占设备性能报告。

预热后 RSS 231.04 MiB，结束 289.01 MiB，净增约 57.97 MiB；
采样峰值 363.44 MiB，随后回落，未呈持续单调增长。句柄末期 433、线程约 26～27，
没有观察到持续增加。RSS/driver 缓存行为不能排除泄漏；尚未测 GPU 分配趋势、ASan/UBSan 或设备丢失注入。

详细样本：[http-stress-nvidia-1000.json](reports/http-stress-nvidia-1000.json)。
此长测是开发快照；其后补充配置预检 CLI/主机测试入口、SCM 状态保护、JSON 解析预算和网页元数据处理，二进制推理处理路径未改。
最终安装目录中的程序已再次通过 AMD/NVIDIA 接口 smoke（含 JSON 解析预算异常输入），并通过 50 次变尺寸完整 OCR、3 次仅识别回归，正常关闭。
此短测记录了最终服务 EXE 与原生 DLL 的 SHA-256，可与部署包内文件核对：
[http-final-50.json](reports/http-final-50.json)。不能把 Windows 测试写成 Linux/CI/SCM 已验证。

## 打包检查

5 项本机 CTest、YAML/Python/Bash 语法与 150 个固定依赖/模型文件校验通过。
服务 PE 直接依赖为本项目 DLL、WS2_32、ADVAPI32、KERNEL32；原生 DLL 另需系统 Vulkan loader。
通用 OpenCV 发布预检的“缺少 OpenCV 版本约束”不适用本 Vulkan 项目；
缺 SBOM 提醒接受为开发阶段欠项，正式发布前仍需补齐安全/供应链门禁。
网页本机额外检查了 600×400 / EXIF=6 JPEG：移除元数据后保留原始像素方向，框与显示一致。
