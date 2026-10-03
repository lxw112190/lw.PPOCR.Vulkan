# 兼容矩阵 / Compatibility matrix

当前为 0.7.0-dev.1 开发预览；正式承诺以最终 RC 附件的实测记录为准。
其他项目的 Win7、Linux ARM、统信、openEuler 或 macOS 成功不能移用于本项目。

维护者现已报告干净目标机与服务账户 GPU 验收完成，见
[维护者验收](MAINTAINER-ACCEPTANCE.md)。未提供具体 OS/GPU/服务账户环境，
不据此将下表所有“未测试”平台改成支持。下表细项保留有明确环境的验证边界。

| 目标 | 构建/CI 范围 | 运行证据与状态 |
| --- | --- | --- |
| Windows x64 | MSVC / Windows-2022 CI，默认 FP32，WinForms .NET Framework 4 | 本机 Windows 10 AMD/NVIDIA 有历史及新版回归；干净机器、Windows 11、服务账户 GPU 仍需最终 RC 验证 |
| Linux x64 | Ubuntu 22.04 / GCC / Ninja，软件 Vulkan + 验证层对拍，独立 Clang ASan/UBSan CI | lavapipe 是 CPU 软件设备，不能证明实体 GPU 性能或显卡兼容；本輪新 CI 及实体 GPU/服务部署待验证 |
| Intel Vulkan GPU | 通用设备枚举/FP32代码路径 | 尚无物理测试记录，不能写已支持/已验收 |
| Windows 7/8、x86 | 不提供该版本 CI/包 | 当前 cpp-httplib 要求 Windows 10+，不能因 .NET 4 或一个 DLL 能加载就承诺 Win7 |
| Linux ARM64 / 国产化系统 | 当前无本项目对应 CI/包 | 未支持；需匹配架构驱动、发行版 ABI 和实际 GPU 验证后再扩展 |
| macOS / MoltenVK | 当前无本项目 CI/包 | 未支持；不能仅依据 Vulkan API 推导 Metal/MoltenVK 可用 |

## 已有物理机证据（历史版本，不是新附件自动验收）

| 项目 | 已记录证据 |
| --- | --- |
| AMD Radeon(TM) Graphics | 驱动 25.10.30.02，Vulkan 1.4.315；Tiny/Small/Medium 对拍、恢复和图像精确探针 |
| NVIDIA RTX 4060 Laptop GPU | 驱动 596.36，Vulkan 1.4.329；三模型对拍、历史长测与性能对照 |
| WinForms / Windows 10 | 历史安装包整图 OCR/ROI 双卡，GPU 可编辑/鼠标映射；本轮按验收报告区分 host-only 与实际 GPU 测试 |
| HTTP / Windows 10 | 历史 binary/Base64/full/REC/batch/429/503/恢复，三模型各 1000 次混合尺寸；对应版本与哈希见原报告 |

## GPU 与系统边界

- 最低 API 构建契约为 Vulkan 1.1；仍需所选设备满足实际图/资源/队列能力与可靠驱动。不承诺所有声称 Vulkan 1.1 的 GPU 都能执行全部模型。
- 当前要求 compute queue、至少 256 workgroup invocations、至少 128-byte push constants 及足够 storage buffer / memory 限制；默认 FP32 不要求 FP16/协作矩阵。能力检查和实际模型加载仍必须通过。
- 不支持 CPU 自动 OCR 回退：没有兼容设备时明确失败；软件 Vulkan 是显式安装的开发/测试路径，不是客户 GPU 性能档。
- GPU 图像前处理/透视裁剪额外要求 `shaderFloat64`；不满足时只将图像处理留在 CPU，网络仍使用 Vulkan FP32。不要把该回退解释成 CPU 网络推理。
- 显卡编号以每台机器探测输出为准。多 GPU、驱动升级或设备顺序改变后必须重新核对编号；HTTP 与 WinForms 均允许显式选择。
- Windows 普通包使用静态 MSVC C/C++ runtime，但仍依赖 Windows 系统 API、Vulkan loader 和 GPU 厂商驱动；WinForms 依赖已安装 .NET Framework。不能复制开发机驱动 DLL 替代驱动安装。
- Linux 保留动态 libc/libstdc++/Vulkan loader 依赖，不打包 glibc。Ubuntu 22.04 是 CI 构建基线，不以发行版名称推导统一最低 GLIBC/GLIBCXX 版本；最终附件须查看实际 `readelf` 符号与 `ldd` 并在目标机测试。

## 最终 RC 验收记录最少字段

记录附件 SHA-256、DLL/SO SHA-256、`BUILD-INFO.json`、OS/架构、CPU、GPU 名称/驱动/API、设备编号、模型/样本哈希、三模型正确性/长测、RSS/VRAM/句柄趋势、服务账户、启动/停止/升级/回滚结果。截图不替代原始结果。

```bash
ldd ./lw-ppocr-vulkan-http-service
ldd ./liblw.PPOCR.Vulkan.so
readelf --version-info ./liblw.PPOCR.Vulkan.so
readelf --version-info ./lw-ppocr-vulkan-http-service
./lw-ppocr-vulkan-probe
```

桌面用户成功不代表 Session 0 或 systemd 服务账户可访问 GPU。Linux 校验设备权限与 ICD，Windows 用真实服务账户测试；不为通过测试而给服务管理员权限。兼容问题优先提供这些信息，并隐藏本机路径/密钥等隐私。

English: configured CI, software-device references and physical GPU qualification
are distinct. Only Windows/Linux x64 are current build targets; additional OS,
GPU vendors and service accounts need artifact-specific qualification before
production claims.
