"""Generate a reproducible Chinese report from the complete 100-image matrix."""
import argparse
import copy
import hashlib
import json
from pathlib import Path

MODELS=('tiny','small','medium')
BACKENDS=('c','dml','vulkan')
NAMES={'c':'C / CPU','dml':'原 DML / GPU','vulkan':'Vulkan / GPU'}
MIB=1024*1024


def write_json(path,value):
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


def memory(value):return '—' if value is None else f'{value/MIB:.1f}'


def compare(reports):
    by_case={(r['model'],r['backend']):r for r in reports};groups={};statistics=[]
    for model in MODELS:
        rows={b:{r['file']:r for r in by_case[model,b]['results']} for b in BACKENDS}
        lines=[]
        for file in rows['c']:
            ds={b:rows[b][file]['accuracy']['details'] for b in BACKENDS}
            if len({len(v) for v in ds.values()})!=1:raise ValueError('ground-truth line mismatch')
            for index in range(len(ds['c'])):
                expected=ds['c'][index]['expected']
                if any(ds[b][index]['expected']!=expected for b in BACKENDS):raise ValueError('GT text mismatch')
                lines.append(dict(file=file,gt_index=index,expected=expected,
                    orientation_degrees=ds['c'][index]['orientation_degrees'],
                    texts={b:ds[b][index]['predicted'] for b in BACKENDS},
                    exact={b:ds[b][index]['exact'] for b in BACKENDS}))
        groups[model]=lines
        for a,b in (('c','dml'),('c','vulkan'),('dml','vulkan')):
            common=[x for x in lines if x['texts'][a] is not None and x['texts'][b] is not None]
            statistics.append(dict(model=model,a=a,b=b,common_matched_lines=len(common),
                identical_text_lines=sum(x['texts'][a]==x['texts'][b] for x in common),
                a_only_exact=sum(x['exact'][a] and not x['exact'][b] for x in lines),
                b_only_exact=sum(x['exact'][b] and not x['exact'][a] for x in lines)))
    return dict(statistics=statistics,lines=groups,
        meaning='Content agreement is calculated only for lines matched by both projects; it is NOT ground-truth accuracy.')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();matrix=json.loads((a.input/'matrix.json').read_text(encoding='utf-8'))
    reports=matrix['reports'];by_case={(r['model'],r['backend']):r for r in reports}
    if len(reports)!=9 or set(by_case)!={(m,b) for m in MODELS for b in BACKENDS}:
        raise ValueError('require all nine project/model cases')
    if matrix.get('images')!=100 or matrix.get('gt_lines')!=614 or matrix.get('repeats')!=3:
        raise ValueError('require complete 100-image, 614-line, three-repeat benchmark')
    for model in MODELS:
        r=[by_case[model,b] for b in BACKENDS]
        if any(x['images']!=100 or len(x['results'])!=100 for x in r):raise ValueError('incomplete case')
        if any(x['source_models_sha256']!=r[0]['source_models_sha256'] for x in r):raise ValueError('model hash mismatch')
        if any(x['dataset_manifest_sha256']!=matrix['dataset_manifest_sha256'] for x in r):raise ValueError('corpus hash mismatch')
    a.output.mkdir(parents=True,exist_ok=True)
    compact=copy.deepcopy(matrix)
    for r in compact['reports']:r['memory'].pop('samples',None)
    write_json(a.output/'matrix.json',compact)
    root=Path(__file__).resolve().parents[1]
    cache=root.parent/'lw.PPOCR.C/build-vulkan-comparison/CMakeCache.txt'
    provenance=dict(c_make_cache_sha256=hashlib.sha256(cache.read_bytes()).hexdigest(),
        c_build_options={line.split('=',1)[0]:line.split('=',1)[1] for line in cache.read_text(encoding='utf-8').splitlines()
            if line.startswith('LW_') and ':' in line and '=' in line},c_model_conversion={})
    for model in MODELS:
        provenance['c_model_conversion'][model]={}
        for task in ('det','cls','rec'):
            path=root/'build/comparison-c-models'/model/(task+'.json')
            provenance['c_model_conversion'][model][task]=dict(metadata_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                metadata=json.loads(path.read_text(encoding='utf-8')))
    write_json(a.output/'build-provenance.json',provenance)
    comparison=compare(reports);write_json(a.output/'content-comparison.json',comparison)
    averages={m:{b:by_case[m,b]['latency_ms']['mean'] for b in BACKENDS} for m in MODELS}
    vk_wins={}
    for m in MODELS:
        values={b:{x['file']:sum(x['samples_ms'])/len(x['samples_ms']) for x in by_case[m,b]['results']} for b in BACKENDS}
        vk_wins[m]={b:sum(values['vulkan'][f]<values[b][f] for f in values['vulkan']) for b in ('c','dml')}
    cs=by_case['small','c']['accuracy'];vs=by_case['small','vulkan']['accuracy']
    extra_misses=[x for x in comparison['lines']['small'] if x['texts']['c'] is not None and x['texts']['vulkan'] is None]
    total_failures=sum(len(r['failures']) for r in reports)
    total_unstable=sum(len(r['unstable_content_or_boxes']) for r in reports)
    conclusions=[
        f"- Tiny：C CPU {averages['tiny']['c']:.2f} ms/张，原 DML {averages['tiny']['dml']:.2f}，Vulkan {averages['tiny']['vulkan']:.2f}；Vulkan/C 相对速度 {averages['tiny']['c']/averages['tiny']['vulkan']:.3f}×。Vulkan 与原 DML 的平均耗时差距 {abs(1-averages['tiny']['vulkan']/averages['tiny']['dml']):.2%} 较小，应结合逐图和重复结果，不只看平均值。",
        f"- Small：Vulkan {averages['small']['vulkan']:.2f} ms/张，对原 DML 为 {averages['small']['dml']/averages['small']['vulkan']:.3f}×、对 C 为 {averages['small']['c']/averages['small']['vulkan']:.3f}×；逐图三轮平均快于原 DML {vk_wins['small']['dml']}/100 张。",
        f"- Medium：Vulkan {averages['medium']['vulkan']:.2f} ms/张，对原 DML 为 {averages['medium']['dml']/averages['medium']['vulkan']:.3f}×、对 C 为 {averages['medium']['c']/averages['medium']['vulkan']:.3f}×；逐图三轮平均快于原 DML {vk_wins['medium']['dml']}/100 张，不能从平均值推断所有输入都胜出。",
        f"- **Small 字符准确率 C={cs['end_to_end_character_accuracy']:.2%}、Vulkan={vs['end_to_end_character_accuracy']:.2%}**。Vulkan Small 有 {len(extra_misses)} 条 C 已匹配、Vulkan 未匹配的 GT 行，共 {sum(len(x['expected']) for x in extra_misses)} 字符（{', '.join(x['file'] for x in extra_misses) or '无'}）。两者总编辑数 C={cs['end_to_end_edits']}、Vulkan={vs['end_to_end_edits']}；不能用整行准确率或调整评分规则掩盖字符指标上的差异。",
        '- 不同批量/会话/权重复用策略影响内存，RAM 和 GPU 专用/共享归属值必须分别比较，不能只归因于 GPU API。',
        f'- 3600 次完整 OCR 调用执行完毕；2700 次计时调用失败 {total_failures} 次，后两轮文字/坐标变化 {total_unstable} 次。轮次结束内存见后文，不把三轮趋势作为无泄漏证明。',
        '- 下一步根据逐图延迟和 GT 差异定位瓶颈；此次不改变推理代码，不据此宣称已达到所有输入全面超过 DML 的目标。']
    text=['# 100 张生成图片：C、原 DML、Vulkan 三项目比较','',
        '测试日期：2026-10-01。本报告比较 Tiny、Small、Medium 三组相同来源模型的实际完整 OCR 流程；不是 HTTP 吞吐测试，也不是相同张量/算子的微基准。','',
        '## 结论','',*conclusions,'',
        '## 测试条件','',
        '- 本机：AMD Ryzen 7 7735H（8 核 16 线程），Windows 10 x64；GPU 为 NVIDIA GeForce RTX 4060 Laptop GPU。',
        '- 本机可见物理内存约 15.24 GiB；NVIDIA 驱动 Windows 版本 `32.0.15.9636`。GPU/CPU 峰值占用来自本次进程测量，不采用显卡规格标称值。',
        '- C 项目为 `lw.PPOCR.C` 的新编译 Release DLL，CPU 内部文字行 worker=8；保留项目默认 AVX2、Conv3x3 FMA、固定点缩放、自适应线程预算、持久文字行池和并行裁剪设置。',
        '- DML 为 `lw.OnnxRuntime.PPOCRSharp_dml` 的新编译原始 Release DLL，不使用 Vulkan 项目的 DML 对拍器代替推理。ORT DirectML 1.23.0、DirectML 1.15.4、OpenCV 4.8.1；REC batch=8、predictor=4、CLS batch=1。',
        '- Vulkan 使用已完成准确性验证的 FP32/GELU 优化 DLL，版本 0.5.0-dev.2；不启用协作矩阵、FP16、M64 实验、GPU profiler 或 validation layer。此次不是对实验性最新混合精度代码作发布验证。',
        '- Vulkan device=1、DML DXGI device=1，控制器核验 vendor/device ID 一致。每组独立进程，逐组运行，不同时占用 CPU/GPU；轮换项目的测试先后顺序。',
        '- 三套：DET 长边 960、阈值 0.3/0.6、unclip=1.5、dilation=false、启用 CLS、CLS 阈值 0.9，使用同一解码 BGR 输入与同一模型/字典 SHA-256。',
        '- **仍有流程差异**：C/Vulkan REC 最大宽度 960；原 DML REC 按 320～1280 桶批量执行。缩放、裁剪/排序、预处理浮点与固定点路径、内部并行策略亦来自各自项目。因此结果代表现有项目的实际使用表现，不能全部归因于 CPU、DML、Vulkan 后端本身。',
        '- 语料：`lw.PPOCR.C/build-local-data/lw-generated-ocr`，100 张生成 JPEG，614 条带文字和位置的标准答案；含 0°/180°、不同尺寸、中文/英文/数字/混合文本。没有使用另一组不带标准答案的样本作为正确率依据。',
        '- 每组完整预热 100 张一次，再顺序执行 3 轮，每轮 100 张；共 9×400=3600 次 OCR 调用，计时样本应为每组 300 次。仅保留当前一张解码图，不预加载 100 张到内存。',
        '- 计时包含完整 OCR 原生接口及转换到 Python 结果列表，排除 JPEG 解码、模型初始化、标准答案评分和 HTTP；三种返回格式的包装开销也包含在内。原 DML 的原生 console 日志未关闭，其成本在实际调用时间内。',
        '- 无人工指定功耗/频率锁定，无法排除笔记本温度和调度影响；单机合成语料结果不构成所有 GPU、实拍图片或生产环境的性能/正确率保证。','',
        f"语料 manifest SHA-256：`{matrix['dataset_manifest_sha256']}`。",'',
        '## 速度','',
        '单位 ms/张。平均值、P50、P95 均来自三轮成功调用；初始化单独记录，不混入推理平均值。','',
        '| 模型 | 项目 | 平均 | P50 | P95 | 初始化 ms | 成功 / 计时调用 |',
        '| --- | --- | ---: | ---: | ---: | ---: | ---: |']
    for m in MODELS:
        for b in BACKENDS:
            r=by_case[m,b];t=r['latency_ms']
            text.append(f"| {m.title()} | {NAMES[b]} | {t['mean']:.2f} | {t['median']:.2f} | {t['p95']:.2f} | {r['initialization_ms']:.1f} | {t['samples']} / 300 |")
    text+=['','Vulkan 相对速度 = 对照项目平均耗时 / Vulkan 平均耗时；大于 1× 表示 Vulkan 更快。','',
        '| 模型 | 相对 C | 相对原 DML |','| --- | ---: | ---: |']
    for m in MODELS:
        vk=by_case[m,'vulkan']['latency_ms']['mean']
        text.append(f"| {m.title()} | {by_case[m,'c']['latency_ms']['mean']/vk:.3f}× | {by_case[m,'dml']['latency_ms']['mean']/vk:.3f}× |")
    text+=['','### 逐图速度差异','',
        '每张图片取三轮平均再比较；以下统计不能替代总体平均，也不承诺所有图片都更快。','',
        '| 模型 | Vulkan 快于 C 的图片数 / 100 | Vulkan 快于原 DML 的图片数 / 100 |',
        '| --- | ---: | ---: |']
    for m in MODELS:
        text.append(f"| {m.title()} | {vk_wins[m]['c']} | {vk_wins[m]['dml']} |")
    text+=['','## 内存','',
        '单位 MiB（1 MiB=2²⁰ 字节）。RAM 工作集峰值由 Windows 进程生命周期峰值计数器取得；Private/GPU 每 250 ms 采样，可能漏掉短暂尖峰。峰值包括加载、预热和测量阶段，不只统计测量结束时。','',
        'RAM 包含统一 Python/NumPy/Pillow 测试壳、输入/输出、运行时及驱动；GPU 为 WDDM 对该 PID 的专用/共享内存归属值，跨该进程的适配器求和，并非显卡全局实际 VRAM。**这些列不能相加作为总物理占用**。CPU 无 GPU counter 实例显示“—”，不是测得 0。','',
        '| 模型 | 项目 | RAM 工作集峰值 | Private 峰值 | GPU 专用峰值 | GPU 共享峰值 |',
        '| --- | --- | ---: | ---: | ---: | ---: |']
    for m in MODELS:
        for b in BACKENDS:
            mem=by_case[m,b]['memory']
            vals=[memory(mem.get(k)) for k in ('os_lifetime_peak_working_set_bytes','private_peak_bytes',
                'gpu_dedicated_peak_bytes','gpu_shared_peak_bytes')]
            text.append('| '+m.title()+' | '+NAMES[b]+' | '+' | '.join(vals)+' |')
    text+=['','### 预热后 RAM 变化','',
        '列出完整预热后及每轮结束时的 Private bytes，辅助判断缓存/输出留存的变化。不同尺寸计划、allocator/驱动缓存以及第一轮保留评分结果都会影响占用；三轮趋势和峰值不能证明没有泄漏。','',
        '| 模型 | 项目 | 预热结束 | 第 1 轮结束 | 第 2 轮结束 | 第 3 轮结束 |',
        '| --- | --- | ---: | ---: | ---: | ---: |']
    for m in MODELS:
        for b in BACKENDS:
            r=by_case[m,b];vals=[r['ram_after_warmup']['private_bytes']]+[x['private_bytes'] for x in r['ram_after_each_repeat']]
            text.append('| '+m.title()+' | '+NAMES[b]+' | '+' | '.join(memory(v) for v in vals)+' |')
    text+=['','## 正确率','',
        '只按第一轮 100 张独立图片评分，不能把三轮相同图片算成 300 张独立语料。按轴对齐位置 IoU≥0.30 贪心一对一匹配；NFC/CRLF 规范化，保留空格、大小写和标点。','',
        '- 整行准确率：文本完全相同的匹配行 / 全部 614 条 GT 行，漏检也记错误。',
        '- 字符 CER：匹配行 Levenshtein 编辑数 + 漏检行全部字符（删除）+ 未匹配输出全部字符（插入），除以全部 GT 字符数。字符准确率 = max(0, 1−CER)，不是平均置信度。',
        '- 检测召回：位置匹配行 / 614；严格的单行匹配可能对合并/拆分行计为漏检或多余行，三套使用同一规则。','',
        '| 模型 | 项目 | 完全正确行 / 614 | 整行准确率 | 字符准确率 | CER | 检测召回 | 输出行数 |',
        '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for m in MODELS:
        for b in BACKENDS:
            r=by_case[m,b]['accuracy']
            text.append(f"| {m.title()} | {NAMES[b]} | {r['exact_lines']} / {r['gt_lines']} | {r['exact_line_rate']:.2%} | {r['end_to_end_character_accuracy']:.2%} | {r['end_to_end_cer']:.2%} | {r['detection_recall']:.2%} | {r['predicted_lines']} |")
    text+=['','### 方向分类相关分组','',
        '| 模型 | 项目 | 0° 完全正确 / GT | 180° 完全正确 / GT |',
        '| --- | --- | ---: | ---: |']
    for m in MODELS:
        for b in BACKENDS:
            groups=by_case[m,b]['accuracy']['orientation']
            values=[f"{groups[o]['exact']} / {groups[o]['gt_lines']}" for o in ('0','180')]
            text.append('| '+m.title()+' | '+NAMES[b]+' | '+' | '.join(values)+' |')
    text+=['','### 三项目输出内容比较','',
        '这里的一致率仅在两项目都匹配到 GT 的行上计算，**不是正确率**。A/B 独有正确行数则在全部 614 条 GT 上统计。','',
        '| 模型 | A / B | 两者匹配行 | 文字完全相同行 | A 独有正确 | B 独有正确 |',
        '| --- | --- | ---: | ---: | ---: | ---: |']
    for r in comparison['statistics']:
        text.append(f"| {r['model'].title()} | {NAMES[r['a']]} / {NAMES[r['b']]} | {r['common_matched_lines']} | {r['identical_text_lines']} | {r['a_only_exact']} | {r['b_only_exact']} |")
    text+=['','## 重复稳定性与范围','',
        '| 模型 | 项目 | 计时调用失败 | 后两轮文字/坐标变化 | GPU counter 错误 |',
        '| --- | --- | ---: | ---: | --- |']
    for m in MODELS:
        for b in BACKENDS:
            r=by_case[m,b]
            text.append(f"| {m.title()} | {NAMES[b]} | {len(r['failures'])} | {len(r['unstable_content_or_boxes'])} | {r['memory']['gpu_counter_error'] or '无'} |")
    text+=['',
        '坐标重复检查使用小数点后 3 位，文字要求相同；不把置信度的微小浮点差异算作文字错误。这是 3600 次交替尺寸完整 OCR 的观察，不是 ASan/UBSan、validation-layer 长测、无泄漏证明或跨平台支持验证。','',
        '## 原始证据与复现','',
        '- [matrix.json](reports/three-project-100/matrix.json)：九组逐图片结果、三次耗时、评分明细、配置、DLL/模型/语料/脚本 SHA-256、内存统计及轮次结束占用。发布到文档的副本仅删去完整 250 ms 内存采样序列，原文件保留在本机 build/reports。',
        '- [content-comparison.json](reports/three-project-100/content-comparison.json)：每条 GT 的三套输出，可查找长行、180°、标点/空格和漏检差异。',
        '- [build-provenance.json](reports/three-project-100/build-provenance.json)：C 项目的实际 CMake 开关和 Tiny/Small/Medium 模型转换工具 metadata，不把不支持的通用入口偷偷改成任意模型转换。',
        '- 本机完整采样和原生日志：`build/reports/three-project-100/`；图片及测试日志不进入部署包。','',
        '```powershell',
        'python -m pip install -r requirements-dev.txt',
        'python tests/test_benchmark_three_projects.py',
        'python tests/test_report_three_projects.py',
        'python tests/benchmark_three_projects.py --output build/reports/three-project-100',
        'python scripts/report_three_projects.py --input build/reports/three-project-100 --output docs/reports/three-project-100',
        '```','',
        '复现前必须准备相邻 C/DML 源码、100 张带 metadata.json 的原语料、新编译 Release DLL、同哈希 ONNX/字典以及 C 项目工具转换出的 `.lwm`。脚本默认路径和配置见 `tests/benchmark_three_projects.py`，所有 DLL 路径、GPU 编号及内部线程参数可显式传入。C Small/Medium 必须使用项目内各自的稳定转换工具，不能绕过 Tiny 通用转换入口的模型限制。','',
        'C 默认构建：运行时共享库 Release，`LW_RUNTIME_ONLY=OFF`、`LW_BUILD_HTTP_DEMO=OFF`、`LW_BUILD_CSHARP_DEMOS=OFF`、`BUILD_TESTING=OFF`。DML 直接构建原始 vcxproj（Release/x64），运行时依赖隔离在新输出目录。Vulkan 使用 `build/gelu-optimized/Release` 已验证快照；具体 DLL 字节以报告 SHA-256 为准，不能把当前正在开发的协作矩阵实验代码自动当作同一已测产物。','']
    destination=a.output.parent.parent/'THREE-PROJECT-100.md'
    destination.write_text('\n'.join(text),encoding='utf-8')
    print(destination)
    return 0


if __name__=='__main__':raise SystemExit(main())
