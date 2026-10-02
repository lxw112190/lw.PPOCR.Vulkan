# Vulkan HTTP 服务 / HTTP service

0.6.0-dev.1 技术验证版。HTTP/config/log v1 已提供机器可读候选契约，尚未正式冻结。
规范文件在 `schemas/`：OpenAPI、配置/响应/访问日志 JSON Schema、契约哈希锁。CI 与真实 HTTP 测试会验证输出；JSON Schema 验证库仅是开发依赖，客户运行程序不需要 Python。
单引擎串行处理，Vulkan FP32，不依赖 OpenCV、CUDA、ONNX Runtime。WinForms 同时保留。

## 启动 / Start

先运行设备探测程序，核对实际 Vulkan 编号，修改 `http-service.json` 的 `device_index`。
编号不存在则启动失败；不会自动换卡或回退 CPU。

Windows：运行 `run-http-service.bat`，浏览器访问 `http://127.0.0.1:8787/`。
也可直接运行 `lw-ppocr-vulkan-http-service.exe`；默认配置按可执行程序目录定位。

Linux（等待 CI/实体 GPU 验证）：

```bash
chmod +x *.sh
./lw-ppocr-vulkan-probe
./run-http-service.sh
```

编译包包含 Tiny/Small/Medium ONNX 模型、字典、测试图、网页、捐赠弹窗和服务脚本。
默认 `model_root` 为 `models/onnx/ppocrv6-tiny`。切换为
`models/onnx/ppocrv6-small` 或 `models/onnx/ppocrv6-medium` 后重启服务；
不得混用 Tiny 与 Small/Medium 的字典。详见 [ONNX 模型说明](ONNX-MODELS.md)。
客户无需 Python/Vulkan SDK，但必须安装支持 Vulkan 1.1 的系统 loader 与显卡驱动。
Linux 初始构建基线为 Ubuntu 22.04；不承诺该包兼容 UOS 20/ARM64/Win7。
Windows WinForms 另外需要 .NET Framework 4.0+。

启动输出作者“天天代码码天天”、QQ 819069052、全部有效参数与实际 GPU 名称。
API Key 只显示启用/禁用，不输出具体值。QQ群仅写在 README，不在服务启动信息里输出。

English: select the enumerated device in config, launch the bundled script, then open
the local web page. No implicit GPU remapping or CPU fallback. Paths in config are
relative to the config file; the default config/sample are executable-directory-relative.
Build/driver compatibility still needs separate Linux physical-device validation.

## API Key、监听地址与安全

`api_key: ""` 表示关闭认证。设置非空值后，所有 `/api/` 接口必须带 `X-API-Key` 请求头；
缺失或错误返回 401。环境变量 `LWVK_API_KEY` 覆盖配置值，空环境变量明确禁用认证。
网页填写 Key 后仅通过请求头发送，不放 URL、不保存浏览器存储。
`/health` 和静态网页公开可访问；不可信 `X-Forwarded-For` 不用于访问日志。

默认只监听 `127.0.0.1`。对外监听须自行配置认证、防火墙及 HTTPS 反向代理；
本服务是明文 HTTP，没有 TLS/CORS/管理 API，不适合直接无认证暴露公网。

## 接口 / Endpoints

| 接口 | 内容 |
| --- | --- |
| `GET /health` | 模型就绪/未就绪，200/503；忙碌不等于故障 |
| `GET /api/info` | 版本、实际 GPU、后端、单实例信息；受 API Key 保护 |
| `POST /api/ocr` | 整图 DET → DB/crop → 可选 CLS → REC/CTC |
| `POST /api/recognize` | 已裁剪文字行，仅 REC/CTC；无检测、无自动方向校正 |

两种 POST 输入：

- 二进制 body，`Content-Type: application/octet-stream`、`image/jpeg`、`image/png` 或 `image/bmp`。
- JSON `{"image_base64":"..."}`；也接受 `data:image/png;base64,...`。严格 Base64，不接受空白/非法 padding。
- 仅识别批量：JSON `{"images_base64":["...","..."]}`，结果按输入顺序返回。
  一次只解码一张；超过累计预算则整个请求失败，不返回部分结果。

JSON 在创建 DOM 前检查嵌套（最多 4 层）与分隔符数量（最多 `max_batch_images + 8` 个）；
复杂结构返回 `json_too_complex`，避免异常嵌套/巨大数组占用解析内存。

```powershell
curl.exe -X POST "http://127.0.0.1:8787/api/ocr" -H "Content-Type: application/octet-stream" -H "X-API-Key: YOUR_KEY" --data-binary "@test-images/sample.jpg"
```

Linux 用 `curl` 替换 `curl.exe`。未启用认证时省略 `X-API-Key`。
仅识别将路径换为 `/api/recognize`，并提供已裁剪文字行图片，不能把整页当单行输入。

成功 envelope：`ok`、`api_version: 1`、`request_id`、`operation`、`result`、`server_total_ms`。
整图 `result` 与原生 OCR JSON 一致：`items` 的 `x1/y1` 到 `x4/y4`、文字、置信度和 CLS 信息；
没有重复 `box`。坐标为解码后原图坐标，不做 JPEG EXIF 自动旋转。
网页移除 JPEG APP1 元数据后再预览/上传（不重编码 JPEG 像素），避免浏览器自动 EXIF 旋转与原生坐标错位。
因此网页展示原始像素方向，而非手机相册方向；网页固定编码图片上限为 20 MiB。
仅识别 `result` 为文字、置信度、图像尺寸、`gpu_rec_ms`；批量 `result.items` 是这些对象的数组。
响应 header `X-Request-ID` 与 body 相同，`X-API-Version: 1` 标识 API。开发期变更须审查候选契约和对应测试；不要通过宽松解析掩盖字段漂移。

## 上限、排队和超时

默认 4 HTTP worker、连接队列最多 32、单 OCR 引擎。
空闲引擎等待超过 5000ms 返回 503 `engine_wait_timeout`；它不取消正在执行的 GPU 任务。
原生 GPU fence 30 秒保护也不代表任务已经取消：运行时故障后本进程引擎标记不健康，
后续请求返回 503，需重启服务检查驱动/模型/资源；不自动重试可能仍在执行的 GPU 工作。

连接队列满时 httplib 本地补丁尽力发送 429 `queue_full` 然后关闭。
此时尚未解析 HTTP 请求，因此没有 request_id/access 记录，只写 transport 错误；
断开的客户端可能只看到连接错误，应限次数、带退避重试。路由层 OCR 超载 429 则有完整 request_id。
默认关闭长连接复用，连接读超时 10 秒、写超时 30 秒，防止慢连接长期占用 worker。

| 配置 | 默认值 |
| --- | --- |
| `max_request_bytes` | 20 MiB，含整个 JSON/Base64 body |
| `max_image_pixels` | 4000 万；单边最多 20000 |
| `max_decode_work_bytes` | 256 MiB，stb 解码期间输出及中间分配的硬预算 |
| `max_batch_images` | 32 |
| `max_batch_total_pixels` | 4000 万 |
| `max_batch_decoded_bytes` | 120000000，按 BGR 3 字节/像素累计 |
| `max_workspace_bytes` | 0=默认每个推理图 512 MiB，约束共享 arena/输入输出/CTC 缓冲区合计；按需分配，权重另计，不是整个进程总内存 |

先获取引擎再解码，最多一张解码像素图存活；编码请求仍最多被 worker 数同时保留。
批量顺序处理和结果大小 4 MiB 上限降低峰值，但模型权重、Vulkan driver、请求/JSON 副本等仍额外占内存。
调大请求大小、worker 数、像素上限时须一起评估内存，不能把上述单项上限当作进程总上限。
支持 JPEG/PNG/BMP，暂不支持 TIFF/WebP/PDF；透明图直接使用其 RGB 值，必要时客户先铺底。

错误：400 JSON/字段/Base64；401 认证；413 请求/像素/批次超限；415 类型不支持；
422 解码失败（包括解码工作预算不足）；429 排队过多；500 原生/内部异常；503 未就绪/等待超时。
程序依赖 `error_code`，不要解析 `error` 文本。

## 日志 / Logs

`logging_enabled: false` 关闭文件日志；`access_log_enabled: false` 单独关闭访问日志。
默认 `logs/runtime.log` 是带毫秒时间的文本，包含初始化、request_started、运行错误与停止信息；
`logs/access.log` 为每请求一行 JSON，含 UTC 时间、request_id、peer IP、方法、路径、状态、字节数与耗时。
日志不写 Key、认证头、图片、Base64 或 OCR 正文。每条同步 flush；默认 10 MiB × 3 个历史轮转文件。
日志无法替代 WER/core dump，也不能覆盖断电、强杀、内存破坏和磁盘故障。

## Windows 服务 / Linux systemd

Windows 使用管理员命令行：`install-service.bat`、`stop-service.bat`、`start-service.bat`、
`restart-service.bat`、`uninstall-service.bat`。服务名称 `lw.PPOCR.Vulkan`，自动启动/失败重启。
SCM running 表示宿主已启动，实际模型/HTTP 就绪请查 `/health` 与 runtime.log。
系统账户/session 0 是否可访问目标 GPU，仍需目标机测试，不从控制台测试推断。

Linux 用 `sudo ./install-service.sh`，配套 start/stop/restart/uninstall；服务名 `lw-ppocr-vulkan.service`。
可先 `./install-service.sh --verify-only` 验证 unit，不改变 systemd 状态。
使用 `LWVK_SERVICE_USER` 指定有 GPU 设备权限的用户；如需 render/video 组，按发行版管理员策略配置。
安装脚本复用已验证的 systemd 路径转义，实际 Vulkan GPU/systemd 部署尚待 Linux 目标机验证。

## 测试 / Tests

```powershell
python tests/test_http.py --package dist/staging --output build/reports/http --device 1
python tests/stress_http.py --package dist/staging --output build/reports/http-long --device 1 --iterations 1000
```

测试需 `pip install -r requirements-dev.txt`（包括 JSON Schema 验证库；运行包不需要）。本机 AMD/NVIDIA 已通过二进制/Base64、完整/仅识别/批量、
配置、认证、无效/超限图片、过载 429、等待超时 503、恢复与正常关闭。
长测观察多个尺寸的结果一致性、RSS、线程和句柄；不能因此宣称无 GPU 显存泄漏或 sanitizer 已通过。
