# 供应链、许可证与 SBOM / Supply chain

当前开发预览：0.7.0-dev.1。目标是可追溯、可复验，不声称安全认证、零漏洞或法律审查完成。

## 三份清单的分工

| 文件 | 内容与边界 |
| --- | --- |
| `dependencies.lock.json` | 固定源码、原始模型/字典、许可证和样图的 SHA-256；固定 SDK 版本与官方下载包哈希。文本标明 LF 归一化，模型/字典保留原始字节。 |
| `SBOM.cdx.json` | CycloneDX 1.6，16 个组件；组件名称/版本或来源 commit、许可证路径、源码树指纹、模型资产和构建 SDK。不是完整二进制、系统、驱动或 Python 测试环境的 SBOM。 |
| `PACKAGE-MANIFEST.json` / `.sha256` | 实际安装目录所有文件的 bytes/SHA-256 与压缩附件 SHA-256。不能代替 SBOM、数字签名或可信发布渠道。 |

SBOM 使用 [CycloneDX 官方 1.6 Schema](https://github.com/CycloneDX/specification/tree/1.6/schema)，原文件与 Apache-2.0 许可证随仓库固定；三个 Schema 随包放入 `metadata/cyclonedx/`。离线引用注册表不请求网络。`lwvk:pinned-files` 保存路径/固定哈希/换行策略；`lwvk:normalized-tree-sha256` 是排序清单的指纹，**不是上游 Git tree、实际源码 ZIP 或编译 DLL 的 SHA-256**。

清单 UUID 与内容/版本确定性关联，不写当前时间、主机路径或用户名。修改依赖版本、模型、许可证或版本号后，必须人工审查再显式重生成 SBOM；校验命令不会自行“修正”旧资产哈希。

## 本轮审核结果

| 组件 | 固定依据 | 许可证/修改 |
| --- | --- | --- |
| cpp-httplib | 0.49.0，头文件版本与内容哈希 | MIT；本地队列满 best-effort 429 patch，非完全原版快照 |
| spdlog / bundled fmt | 1.17.0 / 12.1.0，头文件版本与内容哈希 | MIT；fmt 的原文含可选 compiled-object exception。本轮将误写的 BSD 标注改正，许可证正文不变 |
| nlohmann/json | 3.12.0，三段版本宏与内容哈希 | MIT |
| stb_image | 2.30，commit 与内容哈希 | 采用原文 MIT alternative；保留完整双许可正文 |
| C host 几何/前处理 | 锁文件 commit 与提取后的逐文件哈希 | MIT；修改和精简说明保留在 NOTICE / 第三方 README |
| Vulkan shader / ONNX 读取参考 | 各自 commit 与快照哈希 | Apache-2.0；NOTICE 保留来源、版权与修改说明 |
| PaddlePaddle 模型/字典 | 12 个 ONNX/字典文件、catalog、旧格式转换图与权重哈希 | Apache-2.0；来源和转换限制见 ONNX-MODELS.md；快照哈希不代表自动追踪最新官方模型 |
| CycloneDX Schema | 1.6 标准数据与内容哈希 | Apache-2.0，validation tooling，不参与推理 |
| Vulkan SDK | Windows/Linux 1.4.350.0 压缩包 SHA-256 | build-only / 不随普通运行包分发；SDK 多组件许可，不假定整个 SDK 是单一 MIT |

[fmt 原始许可证](https://github.com/fmtlib/fmt/blob/12.1.0/LICENSE)明确为 MIT 风格正文及可选例外。仍完整附带该正文，不借例外删除版权声明。本轮未更新任何推理源码快照或模型哈希；仅修正 NOTICE、许可证路径并加入已审核标准数据和现有第三方 README。

用户提供的 `test-images/sample.jpg` 单独列为样例文件，不虚构第三方图片的 Apache 许可；正式发布前由维护者确认它及捐赠/界面素材的可再分发权限。根项目许可证不能自动覆盖第三方图片、商标或上游所有资产。

## 本地与 CI 门禁

```powershell
python -m pip install -r requirements-dev.txt
python scripts/verify_assets.py
python scripts/supply_chain.py
python tests/test_supply_chain.py
python scripts/contracts.py --check
```

明确审核后生成：`python scripts/supply_chain.py --write`。该命令仍先检查全部已有固定资产，不更新锁文件。未锁定的 `third_party/`、`models/` 或 `licenses/` 文件会被拒绝；禁止将调试缓存放入这些目录。

三个维护用锁生成脚本 `update_asset_lock.py` / `lock_http_dependencies.py` / `lock_onnx_assets.py` 现在默认 dry-run，必须明确 `--write` 才会写锁文件；仅在来源/版本/许可证/本地 patch 已逐项审核后使用，不能用于绕过 CI 哈希错误。旧 bootstrap 更新器已改为保留其他组件条目，不再重置整份锁。写锁后检查 diff，再显式生成 SBOM；它们不在 CI 中自动执行。

安装后复验：`python scripts/supply_chain.py --root build/v070/staging --package`。不要求源码快照随客户包；旧 Tiny 的三份转换输入 ONNX 仅为开发来源记录、不重复安装，SBOM 显式列出。全部运行模型/权重/字典、许可证/样图以及离线 Schema 必须存在且匹配。`scripts/package.py` 自动调用这项检查，并要求默认 Release 的 `BUILD-INFO.json`：版本、系统、64 位、实验开关 OFF、sanitizer OFF。

`BUILD-INFO.json` 记录 CMake/编译器/Vulkan 头文件版本、源码 HEAD 与配置时 dirty 标志。未提交本地构建会明确标为 dirty；它不是签名的构建来源证明。CI 下载附件才是该 CI commit 的产物，不可拿本地改版冒充同一 commit 构建。

CI 新增 `.github/workflows/security.yml`：离线供应链审核产物 + C/C++ CodeQL `security-extended` 实际默认原生构建分析；使用 [GitHub 官方 manual build 模式](https://docs.github.com/en/code-security/concepts/code-scanning/codeql/codeql-for-compiled-languages)。只分析被构建的 C/C++，不替代 GLSL/GPU 内存验证、动态测试或依赖 CVE 审查。工作流生成/本地 YAML 检查不代表 CodeQL 已运行通过；如仓库已启用 CodeQL default setup，应由维护者在 GitHub 设置中选择单一 setup，避免与 advanced workflow 冲突。

不启用 Dependabot，也不自动升级任何依赖。维护者已报告依赖安全审查完成，见 [维护者验收](MAINTAINER-ACCEPTANCE.md)；本机未收到审查明细，不将其伪装成自动扫描结果。发布记录应关联上游安全公告/CVE、适用版本、本地 patch 影响与处置结论，并保存审查时间、工具版本、结果和例外理由；不能只因锁定版本、审查完成或 CodeQL 绿色就写“无漏洞”。

English: this is an offline, deterministic source/model SBOM and completeness
gate, not exhaustive binary provenance, CVE clearance, license legal advice or
GPU validation. CodeQL runs after push. Keep reviewed upstream notices and
separate build-only SDKs from customer runtime prerequisites.
