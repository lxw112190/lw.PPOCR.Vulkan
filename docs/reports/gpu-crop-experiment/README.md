# GPU 裁剪实验原始报告

完整解释见 [GPU-CROP-EXPERIMENT.md](../../GPU-CROP-EXPERIMENT.md)。本轮最终 DLL SHA-256：`518073024b6d39cc18b6357b3475c0015ae3a6bea732b214e4753efc1daeafa6`。基线为 `rec-wide-opt1`，不是 C/DML。

- `correctness-final.json`：三模型 100 图、十组变形、stride、上传限额/LRU/恢复，含逐图预热后的配对耗时。
- `sample-qualified.json`：500/1000 两尺寸，各 30 轮交替顺序、中位数；`stream-qualified.json`：不逐图预热、100 图正向/反向两遍。
- `amd-qualified.json`：AMD 核显 500 图样例各 15 轮；`default-off-final.json`、`preprocess-only-final.json`：普通入口与上一实验入口，crop 关闭，不宣称新路径的收益。
- `crop-contract*.json/txt`：开关依赖、CLS 关闭、独立句柄、共享高水位挤压网络预算后的恢复。
- `crop-probe-final-device*.txt`：两卡各 111 个裁剪像素案例，GPU/CPU BGR8 逐字节一致、尾部保护、异常恢复。
- `validation-device*.json/txt`：两卡三模型整图同步验证；这里的计时被 validation 扰动，不作性能依据。
- `ort-reference-*.json`：三模型独立 ORT CPU 图/NumPy 前处理与 CTC 的完整 OCR 参考；几何仍共享已有 C 参考，不能称为完全独立几何验证。
- `workspace-final.json`：最终 DLL 工作区资格，私有大图只记录哈希/尺寸，不保存客户文字；`raw-network-final.json`：RTX 27 组原始概率逐位一致；`rec-only-final.json`：仅识别 ABI/长度/stride/LRU/并发/恢复。
- `http-smoke.json`：二进制/Base64、完整/仅识别/批量、认证、429/503、日志与恢复。
- `csharp-staged*.json`、`winforms-device*.json`：完整体验包在开发 PATH 被移除、包外工作目录下的真实 C# 程序验证。最终 ZIP 的解压校验报告不混入 ZIP 内，保存在压缩文件旁。
- `rejected-full-cache-clear.json`、`rejected-full-capacity-copy.json`：早期候选的反例，具有不同 DLL 哈希，**不是本轮最终 DLL 的指标**。

这是本地有限回归证据，不是长期稳定性/无泄漏证明或所有显卡、所有操作系统的支持认证。ORT/HTTP/WinForms 报告不一定自带原生哈希，其运行日志与构建输入已在本轮核对；部署资格以 `workspace-final.json` 和包内 `PACKAGE-INFO.json` 的哈希为准。
