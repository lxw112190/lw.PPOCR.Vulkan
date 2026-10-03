# v1.0 发布门槛 / Release gates

当前版本：0.7.0-dev.1，仍是开发预览。以“可集成、可部署、可诊断、可复验”为验收目标，不以超过 DML/TensorRT、增加 FP16 或无限扩展平台为发布前提。

## 本轮范围

维护者已报告完成依赖安全审查、最终包长测、干净目标机与服务账户 GPU 验收，
记录见 [维护者验收](MAINTAINER-ACCEPTANCE.md)。不再把这四项笼统列为未做；
表中最终二进制复验是针对本次修复后新 CI 附件，不否定维护者已完成的测试。

| 门禁 | 本轮实现 | 尚需的验收 |
| --- | --- | --- |
| C ABI | 19 个导出、状态码、头文件 token 指纹，native/ctypes x64 大小与偏移 | 最终包基线复验、RC 后冻结 |
| HTTP/config/log | OpenAPI、JSON Schema 和哈希锁；实际 HTTP 响应/JSONL 校验；111 项配置进程矩阵 | RC 后冻结；新增字段/业务码兼容规则审查 |
| 部署包 | Schema、逐文件清单、附件 SHA-256；维护者报告干净目标机与服务账户 GPU 验收完成 | 修复后附件哈希与 smoke、升级/回滚记录 |
| 稳定性 | 异常恢复、限内存、429/503、同句柄并发；维护者报告最终包长测完成 | 将维护者测试条件/原始记录关联实际发布附件，不捏造次数或内存指标 |
| GPU 正确性 | 独立 ORT 对拍、严格 GPU 前处理探针、历史 100 图三模型；新增 11 场景派生集 | 更多实际公开样本；最终二进制跨设备验证 |
| 安全/供应链 | 固定资产/SBOM/完整性门禁、构建元数据；ASan/UBSan/CodeQL CI；维护者报告依赖安全审查完成 | 当前 CI 实际结果、关联审查记录、样图/素材再分发权确认 |
| 平台 | Windows x64 和 Linux x64 CI；Windows AMD/NVIDIA 实测；Linux 软件 Vulkan 对拍 | 当前 CI 结果、Linux 实体 GPU及服务部署；其他系统不自动纳入支持 |

表内“实现”不是所有验收通过。历史报告的 DLL 哈希和版本只对应当时二进制；不能挪用作 0.6 或 v1.0 的最终资格证明。

dev.2 新增门禁的范围与限制见 [SANITIZERS.md](SANITIZERS.md)。常规部署包与 sanitizer 诊断产物分开；不以生成工作流代替 Linux 检测通过。

v0.7 补齐 [供应链](SUPPLY-CHAIN.md)、[兼容矩阵](COMPATIBILITY.md)、[部署/升级/回滚](DEPLOYMENT-UPGRADE.md)。SBOM 与构建元数据是证据，不是安全认证。模型、依赖和公共候选接口未变；不未经实测扩展平台承诺。

## 建议推进顺序

1. **v0.6**：候选契约、本轮 CI 与本机回归；继续补 ASan/UBSan 和更广正确性集。
2. **v0.7**：SBOM、完整性门禁、兼容矩阵和升级/回滚说明已实现；依赖安全审查由维护者确认完成，验证修复后的新 CI 并确认素材权限。
3. **v0.8～v0.9**：最终默认 FP32 构建，三模型长测、验证层、服务模式和干净机器部署；候选 C ABI 冻结。
4. **v1.0.0-rc.N**：仅修 Bug，使用实际 CI 附件在承诺的平台复验；候选标签不可覆盖。
5. **v1.0.0**：所有承诺门槛有记录，正式冻结 C ABI/HTTP/config/log，公布准确支持边界和兼容升级政策。不未经授权自动承诺 LTS 年限。

FP16、协作矩阵和多 REC lane 不作为 v1.0 默认：本机实验尚未形成可靠收益/精度结论，构建开关保持 OFF。CPU 图像前处理并不代表 CPU OCR 回退；模型网络仍由 Vulkan 执行。没有 Vulkan 驱动的客户不能凭 DLL 获得 GPU。

## 复验命令

```powershell
python -m pip install -r requirements-dev.txt
python scripts/verify_assets.py
python scripts/contracts.py --check
python scripts/supply_chain.py
cmake -S . -B build/v070 -G "Visual Studio 17 2022" -A x64 -DLWVK_EXPERIMENTAL_COOP=OFF -DLWVK_EXPERIMENTAL_REC_LANES=OFF
cmake --build build/v070 --config Release --parallel 4
ctest --test-dir build/v070 -C Release --output-on-failure
cmake --install build/v070 --config Release --prefix build/v070/staging
python tests/test_http.py --package build/v070/staging --output build/v070/http --device 1
python scripts/supply_chain.py --root build/v070/staging --package
python scripts/package.py --staging build/v070/staging --output dist/v070 --platform windows-x64
python scripts/verify_archive.py --archive dist/v070/lw.PPOCR.Vulkan-v0.7.0-dev.1-windows-x64-full-ocr-preview.zip
```

`--device 1` 是本机示例，须用探测器显示的实际编号。Linux 用 Ninja/Release、`.so` 和 `--platform linux-x64`；共享 CI 的软件 Vulkan 超时策略见开发指南。上列命令不替代三模型对拍/长测。测试工具需要 Python，客户运行预编译包不需要 Python/SDK/Schema 验证库。

`PACKAGE-MANIFEST.json` 是完整文件校验清单，不是 SBOM、安全认证、数字签名或无泄漏证明。附件未重新构建验证前，不要只替换版本号发布。

English: v0.6 adds candidate contracts and integrity gates, not a production freeze.
Proceed through sanitizers/correctness, supply-chain audits, final-artifact soak and
platform qualification, then immutable RCs and v1.0. Do not claim hardware support
from software Vulkan CI, reuse historical binary evidence as a new qualification,
or promise LTS terms without an explicit maintainer commitment.
