# 安全部署、升级与回滚 / Deployment and rollback

v1.0.0 已正式冻结 C ABI/HTTP/config/JSONL v1；后续 1.x 保持现有接口兼容。旧开发版升级仍须核对 Schema、模型和默认参数，不只替换 DLL。

## 首次启动

1. 从维护者 GitHub Release/CI 下载匹配平台包和 `.sha256`，保留原附件。对照可信发布页面的哈希；同目录的附件和哈希若同时被篡改，单独计算不能证明来源。
2. 解压到新目录，不覆盖正在使用的版本。查看 `RELEASE_VERSION`、`BUILD-INFO.json`、SBOM、README 与许可证。可用仓库工具 `python scripts/verify_archive.py --archive <包路径>` 重新读取附件的完整文件清单。
3. 用包内 probe 核对所需设备，在 config 中设置设备/模型。先运行 `run-http-service.bat` 或 `./run-http-service.sh`，使用随包样图验证整图 OCR、框选/仅识别、耗时和日志。
4. 桌面验证后再运行安装服务脚本，以实际服务账户验证 GPU、模型目录只读权限与日志写权限。先验证停止/重启/开机启动，再对外提供服务。

包内包含模型、页面、客户端示例和原生程序；Python/Vulkan SDK 只用于开发/校验，客户启动预编译 HTTP 服务不需要。GPU 厂商驱动、系统 Vulkan loader、系统 ABI 与 .NET 并不全部随普通包分发，详见兼容矩阵。

## 网络安全边界

- 默认监听 `127.0.0.1`。向局域网开放前配置 API Key，浏览器与客户端使用 `X-API-Key`；可通过 `LWVK_API_KEY` 环境变量覆盖，不将真实 Key 提交仓库或贴入 issue。
- API Key 是共享令牌，不是用户权限管理或加密。HTTP 无 TLS，不直接暴露公网；使用可信反向代理终止 HTTPS，设置 body/连接/超时/速率限制，按部署需要限制来源并保护静态页面。
- 有界队列、引擎等待超时、单张/累计批次解码限制仍应保持。不要为绕过 413/429/503 而随意扩大预算；GPU fence 超时不是取消正在执行的任务。
- 非管理员服务账户、模型/程序只读、日志目录单独写权限；保持 API Key、图片、Base64、OCR 文本不进入默认日志。`request_id` 用于关联请求，限制日志保留和目录容量。
- 模型与配置从可信管理员提供，不开放任意模型上传或自动下载。原生解析器的边界校验/ASan 不等于可以安全执行任意不可信 ONNX。

## 升级步骤

1. 记录旧包版本/哈希、配置、模型选择、服务名称/账户、设备编号及日志位置；备份配置并保护其中的 Key。保留旧完整目录，不删除回滚基础。
2. 将新包解压到并列新目录。先核对版本/Schema/许可证/完整文件清单；比较默认 config，将业务值逐项迁移，避免把旧配置整个覆盖新模板。未知字段/版本会明确拒绝启动。
3. 在测试端口前台启动新包，验证 `/health`、样图/业务样图、API Key、OCR/REC、异常返回与日志。确认三套模型/字典来自同一审核快照，不混用新 DLL、旧页面和未知模型。
4. 安排维护窗口，停止旧服务并确认进程退出；运行旧目录卸载脚本，再按新目录脚本安装/启动。检查脚本输出与服务账户/GPU；安装失败不得留两个进程争用生产端口。
5. 保存新附件、哈希、构建信息与验收记录。默认推荐完整包并列部署；C ABI 兼容不代表可以任意混用旧网页、配置、模型或库。仅当对应版本的升级说明明确列出可替换文件并完成回归后，才进行增量 DLL/SO 更新。

## 回滚步骤

停止/卸载新服务，确认进程退出；恢复旧完整目录及其对应配置/模型/网页，用旧脚本安装并启动。复验健康、业务样图、API Key、日志和服务重启，保留故障版本日志与附件供诊断。不得只倒退一个 DLL 而保留新版本配置/模型。

这些是部署操作说明，本轮没有在用户生产机器执行安装、卸载、迁移或修改服务权限。维护者的既有验收见 MAINTAINER-ACCEPTANCE.md；每次新附件/新环境仍需复验服务账户 GPU 与回滚路径。

English: verify provenance and artifact bytes, stage side-by-side, migrate config
explicitly, test before switching, and retain the entire prior package for rollback.
Use TLS/reverse-proxy and least privilege for network deployment. v1 contracts
are frozen, but binary compatibility alone does not authorize mixing package assets.
Use incremental replacements only when explicitly documented and regression-tested.
