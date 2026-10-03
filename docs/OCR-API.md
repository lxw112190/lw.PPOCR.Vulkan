# Full OCR experimental API / 完整 OCR 实验接口

Version: 1.0.0. Frozen, machine-checked C ABI v1 for x64; preserve existing layouts, exports and semantics throughout 1.x. Platform qualification is documented separately in COMPATIBILITY.md.
See `schemas/c-abi-v1.json` and `schemas/http-response-v1.schema.json` (OcrResult).

Native SDK entry: `sdk/include/lw_ppocr_vulkan.h` in deployment packages.
No STL/exception/allocation ownership crosses the C ABI. UTF-8 paths and JSON.

```text
BGR -> DET bilinear/normalization -> Vulkan DET -> CPU DB/reading order
    -> one perspective crop -> optional Vulkan CLS/180° -> Vulkan REC/greedy CTC -> CPU character assembly
    -> next crop -> independent UTF-8 JSON result
```

## Defaults and bounds / 默认值与边界

| Field | Default | Accepted range |
| --- | --- | --- |
| device_index | 0 | Enumerate before choosing; no silent CPU fallback |
| det_limit_side | 960 | 32..960, multiple of 32, longest-side limit |
| max_candidates | 1000 | 1..1000, includes candidates rejected by DB |
| enable_classifier | 1 | 0/1 |
| use_dilation | 0 | 0/1 |
| reading_order | 0 | 0=horizontal LTR; 1=vertical RTL; 2=vertical LTR |
| max_workspace_bytes | 0 | 0=512 MiB PER graph, explicit <=1 GiB; allocated on demand |
| max_crop_pixels | 4,000,000 | 1..8,000,000 |
| max_total_crop_pixels | 32,000,000 | >=single crop limit, <=64,000,000 |
| bitmap_threshold | 0.3 | finite 0..1 |
| box_threshold | 0.6 | finite 0..1 |
| unclip_ratio | 1.5 | finite >0..5 |
| cls_threshold | 0.9 | finite 0..1; rotate if label=1 and score>threshold |

Call `lwvk_ocr_config_default` first. Exact `struct_size`, reserved=0 required.
Source: positive stride, dimensions 1..20000, <=40M pixels, buffer length at least
`(height-1)*stride+width*3`. Raw BGR8, NOT encoded JPEG/PNG/Base64 or Halcon handles.
Caller retains the pixels until run returns. Unknown/invalid config rejected early.

DET does not enlarge small images to 960: it shrinks the longest side only when
above the limit, then rounds dimensions to multiples of 32 (minimum 32).
Channel planes remain BGR; ImageNet mean/std follow reviewed lw.PPOCR.C float path.
CLS uses left min(width,4*height) stretched to 160x80, BGR [-1,1].
REC uses height 48, adaptive width multiple of 8 in 32..960, right-pad normalized 128.
Tall crops (height>=1.5*width) rotate to horizontal before optional CLS.

逐行裁剪，不会先保存所有图片。单区域/累计裁剪像素超过配置时整次调用失败，
不返回“看起来成功”的部分结果；限制候选数则只处理配置数量内候选。
DB workspace retains one bounded high-watermark allocation (map <=960x960).
GPU workspace limit is not total RAM/VRAM; three independent model contexts,
weights, DB buffers, driver and returned results consume additional memory.
The budget includes one shared arena and upload/probability/CTC readback buffers per graph.
Up to 32 LRU plans retain bounded metadata/commands, not 32 independent arenas.
Buffer growth invalidates recorded references before release/reallocation; plans rebind lazily.
Callers must release results promptly rather than accumulate them indefinitely.

## Ownership and failures / 所有权与异常

1. `lwvk_ocr_create(root,config,&engine)` loads models; CLS skipped if disabled.
2. `lwvk_ocr_run_bgr(...,&result)` runs once; output reset to null on failure.
3. `lwvk_ocr_result_json(result,NULL,0,&required)` returns 5 plus bytes incl NUL.
4. Allocate required bytes, copy JSON (no rerun), `lwvk_ocr_result_destroy(result)`.
5. `lwvk_ocr_destroy(engine)` after all engine calls finish.

Result can outlive engine and multiple readers can copy it; do not destroy it
during copies. Same engine serializes calls. Different engines allocate separately.
Validation/crop limits return 1; model load failures 3; GPU/internal failures 4;
small JSON buffer 5, no truncation. Thread-local `lwvk_last_error` explains failure.
Inputs rejected normally allow recovery. GPU lost/timeout poisons its graph plan;
do not continue serving with it. The 30s fence timeout does NOT cancel GPU work;
destruction may wait for device idle. No hard end-to-end cancellation deadline.

## JSON and timing / 结果与耗时

Root: `items`, `image_width`, `image_height`, `det_width`, `det_height`,
`classifier_enabled`, `timing`. Max serialized JSON 4 MiB.
Each item: `x1,y1,x2,y2,x3,y3,x4,y4,text,score,det_score,cls_label,cls_score`.
Four-point source coordinates TL/TR/BR/BL, no duplicate `box`.
The DB/PaddleX-compatible crop rectangle is re-fitted after mapping/clipping
boundary points; rotated corners can extend outside the image (negative or
greater than its dimensions). These remain valid source-space coordinates.
Crop sampling uses border handling; draw overlays clipped to the image, but do
not silently clamp each returned corner before perspective cropping.
检测框的旋转矩形角点可能在原图之外；这是重新拟合后的几何，不是无效 buffer
或 GPU 越界。网页绘制可以由画布裁切，客户透视裁剪不要擅自截断角点。
`score`: mean emitted CTC label probability (not calibrated accuracy).
`cls_label=-1`, `cls_score=0` when disabled. Empty CTC strings remain visible;
there is no undocumented recognition-score filter. No text is guaranteed correct.

`det_ms`, `cls_ms`, `rec_ms`: accumulated graph execution/readback timers; they
exclude plan creation, CPU preprocessing/geometry/CTC. `total_ms` covers mutex
waiting and native pipeline up to JSON assembly, excluding image decoding,
final JSON serialization and result copy. First call/width changes may be slower.
These are diagnostic durations, not a performance claim or comparable DML benchmark.
