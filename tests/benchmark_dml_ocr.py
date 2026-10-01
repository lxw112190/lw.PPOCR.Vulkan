"""Matched native full-OCR benchmark; optional DML helper, never a runtime dependency.

Both use the same compiled ocr_host.cpp preprocess/DB/crop/CLS/REC-preprocess/JSON.
Vulkan performs CTC argmax on GPU; ORT DML exposes probabilities for native CPU CTC.
This measures the actual complete pipelines, not identical intermediate transfers.
No private image bytes or recognized text are written into reports.
"""
import argparse
import atexit
import ctypes as C
import hashlib
import json
import os
import statistics
import sys
import time
from pathlib import Path
import numpy as np
from PIL import Image
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/python"))
from lwvk import load, check, DeviceInfo, OcrConfig


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--library", type=Path, required=True)
    p.add_argument("--dml-library", type=Path, required=True)
    p.add_argument("--device", type=int, default=1)
    p.add_argument("--require-device", default="RTX 4060")
    p.add_argument("--iterations", type=int, default=20)
    p.add_argument("--dml-shape-cache",action="store_true",help="bounded per-shape sessions with named free-dimension overrides")
    p.add_argument("--image", type=Path, help="optional private image; report excludes text")
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    if os.environ.get("LWVK_GPU_PROFILE")=="1":
        p.error("LWVK_GPU_PROFILE=1 perturbs execution; disable it for performance comparisons")
    if not 1 <= a.iterations <= 5000:
        p.error("iterations must be 1..5000")
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="backslashreplace")
    vk = load(a.library)
    os.environ["LWVK_DML_SHAPE_CACHE"]="1" if a.dml_shape_cache else "0"
    dml = C.CDLL(str(a.dml_library.resolve()))
    dml.lwvk_dml_error.restype = dml.lwvk_dml_version.restype = C.c_char_p
    dml.lwvk_dml_adapter.argtypes = [C.c_uint32, C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
    dml.lwvk_dml_ocr_create.argtypes = [C.c_char_p, C.POINTER(OcrConfig), C.POINTER(C.c_void_p)]
    dml.lwvk_dml_ocr_destroy.argtypes = [C.c_void_p]
    dml.lwvk_dml_ocr_destroy.restype = None
    dml.lwvk_dml_ocr_run.argtypes = vk.lwvk_ocr_run_bgr.argtypes
    dml.lwvk_dml_ocr_json.argtypes = vk.lwvk_ocr_result_json.argtypes
    dml.lwvk_dml_ocr_result_destroy.argtypes = [C.c_void_p]
    dml.lwvk_dml_ocr_result_destroy.restype = None
    def checked(status):
        if status:
            raise RuntimeError(dml.lwvk_dml_error().decode("utf-8", "replace"))
    info = DeviceInfo(struct_size=C.sizeof(DeviceInfo))
    check(vk, vk.lwvk_device_get(a.device, C.byref(info)))
    assert a.require_device in info.name.decode(), "wrong Vulkan hardware"
    selected = None
    for index in range(16):
        name, vendor, device = C.create_string_buffer(256), C.c_uint32(), C.c_uint32()
        if dml.lwvk_dml_adapter(index, name, len(name), C.byref(vendor), C.byref(device)):
            break
        if vendor.value == info.vendor_id and device.value == info.device_id and a.require_device in name.value.decode():
            selected = index, name.value.decode()
            break
    assert selected is not None, "matching DXGI hardware missing"
    image = ROOT / "test-images/sample.jpg"
    rgb = Image.open(image).convert("RGB")
    inputs = [("sample-500", rgb, image), ("sample-1000", rgb.resize((1000,1000), Image.Resampling.BILINEAR), image)]
    if a.image:
        inputs.append(("private", Image.open(a.image).convert("RGB"), a.image))
    comparisons = []
    completed = False
    pending_case = {}
    def preserve_failed_run():
        if completed:
            return
        # A failed correctness gate must still retain measured preceding cases,
        # but never be mistaken for a complete/passing performance report.
        data = dict(status="failed_or_interrupted",version=vk.lwvk_version().decode(),
            vulkan_gpu=info.name.decode(),vulkan_index=a.device,dxgi_gpu=selected[1],dxgi_index=selected[0],
            library_sha256=hashlib.sha256(a.library.read_bytes()).hexdigest(),
            experimental_coop_requested=os.environ.get("LWVK_EXPERIMENTAL_COOP")=="1",
            experimental_coop_det_only_requested=os.environ.get("LWVK_EXPERIMENTAL_COOP_DET_ONLY")=="1",
            experimental_tile64_requested=os.environ.get("LWVK_EXPERIMENTAL_TILE64")=="1",
            dml_shape_cache=a.dml_shape_cache,iterations=a.iterations,
            pending_case=pending_case,comparisons=comparisons,
            note="Incomplete or failed gate. Do not use this report to claim DML performance success. No OCR text stored.")
        a.report.parent.mkdir(parents=True,exist_ok=True)
        a.report.write_text(json.dumps(data,indent=2)+"\n",encoding="utf-8")
    atexit.register(preserve_failed_run)
    for variant in ("tiny", "small", "medium"):
        models = ROOT / "models/onnx" / ("ppocrv6-" + variant)
        cfg = OcrConfig()
        check(vk, vk.lwvk_ocr_config_default(C.byref(cfg)))
        cfg.device_index = a.device
        hv, hd = C.c_void_p(), C.c_void_p()
        check(vk, vk.lwvk_ocr_create(str(models).encode(), C.byref(cfg), C.byref(hv)))
        try:
            cfg.device_index = selected[0]
            checked(dml.lwvk_dml_ocr_create(str(models).encode(), C.byref(cfg), C.byref(hd)))
            for label, pil, file in inputs:
                pending_case = dict(model=variant,image=label)
                bgr = np.ascontiguousarray(np.asarray(pil)[:,:,::-1])
                def invoke(backend):
                    result = C.c_void_p()
                    if backend == "vulkan":
                        run, get, destroy, verify, handle = vk.lwvk_ocr_run_bgr, vk.lwvk_ocr_result_json, vk.lwvk_ocr_result_destroy, lambda status:check(vk,status), hv
                    else:
                        run, get, destroy, verify, handle = dml.lwvk_dml_ocr_run, dml.lwvk_dml_ocr_json, dml.lwvk_dml_ocr_result_destroy, checked, hd
                    t = time.perf_counter()
                    verify(run(handle,bgr.ctypes.data_as(C.POINTER(C.c_uint8)),bgr.nbytes,bgr.shape[1],bgr.shape[0],bgr.strides[0],C.byref(result)))
                    try:
                        required = C.c_uint64()
                        assert get(result,None,0,C.byref(required)) == 5
                        text = C.create_string_buffer(required.value)
                        verify(get(result,text,len(text),C.byref(required)))
                        raw = text.value
                    finally:
                        destroy(result)
                    wall = (time.perf_counter()-t)*1000
                    return json.loads(raw), wall
                def compare(left, right):
                    assert left["det_width"] == right["det_width"] and left["det_height"] == right["det_height"]
                    assert len(left["items"]) == len(right["items"]), (variant,label,"item count")
                    max_box = max_score = 0.
                    for i,(x,y) in enumerate(zip(left["items"],right["items"])):
                        assert x["text"] == y["text"] and x["cls_label"] == y["cls_label"], (variant,label,i,"text/class mismatch")
                        max_box = max(max_box,*(abs(x[k]-y[k]) for k in ("x1","y1","x2","y2","x3","y3","x4","y4")))
                        max_score = max(max_score,*(abs(x[k]-y[k]) for k in ("score","det_score","cls_score")))
                    accuracy=dict(items=len(left["items"]),text_equal=True,cls_equal=True,max_box_delta=max_box,max_score_delta=max_score)
                    pending_case.update(accuracy)
                    assert max_box <= 1.0 and max_score <= .002, (variant,label,max_box,max_score)
                    return accuracy
                for _ in range(3):
                    expected, _ = invoke("vulkan"); reference, _ = invoke("dml")
                accuracy = compare(expected, reference)
                samples = {"vulkan":[], "dml":[]}
                diagnostics=[]
                for i in range(a.iterations):
                    order = ("vulkan","dml") if i % 2 == 0 else ("dml","vulkan")
                    results = {}
                    for backend in order:
                        value, wall = invoke(backend)
                        samples[backend].append(dict(wall_ms=wall,**value["timing"]))
                        if backend=="dml":diagnostics.append(value["reference_diagnostics"])
                        results[backend] = value
                    compare(results["vulkan"],results["dml"])
                    compare(expected,results["vulkan"])
                row = dict(model=variant,image=label,image_sha256=hashlib.sha256(file.read_bytes()).hexdigest(),
                    size=[bgr.shape[1],bgr.shape[0]],det_size=[expected["det_width"],expected["det_height"]],
                    bgr_sha256=hashlib.sha256(bgr.tobytes()).hexdigest(),accuracy=accuracy,samples=samples,dml_diagnostics=diagnostics,
                    median_ms={b:{k:statistics.median(x[k] for x in values) for k in values[0]} for b,values in samples.items()},
                    p95_ms={b:float(np.percentile([x["wall_ms"] for x in values],95)) for b,values in samples.items()})
                comparisons.append(row)
                print(json.dumps({k:row[k] for k in ("model","image","accuracy","median_ms")}),flush=True)
        finally:
            if hd:dml.lwvk_dml_ocr_destroy(hd)
            if hv:vk.lwvk_ocr_destroy(hv)
    sha = lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
    report = dict(status="passed",version=vk.lwvk_version().decode(),ort_version=dml.lwvk_dml_version().decode(),
        vulkan_gpu=info.name.decode(),vulkan_index=a.device,dxgi_gpu=selected[1],dxgi_index=selected[0],
        library_sha256=sha(a.library),dml_helper_sha256=sha(a.dml_library),
        onnxruntime_sha256=sha(a.dml_library.parent/"onnxruntime.dll"),directml_sha256=sha(a.dml_library.parent/"DirectML.dll"),
        host_sources_sha256={n:sha(ROOT/"src"/n) for n in ("ocr_host.cpp","ocr_host.hpp","det_preprocess.cpp","det_preprocess.hpp","rec_preprocess.cpp","ctc_decode.cpp")},
        vulkan_sources_sha256={n:sha(ROOT/n) for n in ("src/graph.cpp","src/graph_optimizer.cpp","src/vulkan_context.cpp",
            "scripts/prepare_shaders.py","third_party/simd-paddleocr/shaders/gelu.comp",
            "third_party/simd-paddleocr/shaders/conv_pointwise_tiled64.comp","third_party/simd-paddleocr/shaders/conv_coop_gemm.comp")},
        model_sha256={v:{n:sha(ROOT/"models/onnx"/("ppocrv6-"+v)/n) for n in ("det.onnx","cls.onnx","rec.onnx","dictionary.txt")} for v in ("tiny","small","medium")},
        method=dict(warmup=3,iterations=a.iterations,det_limit_side=960,classifier=True,
            dml_shape_cache=a.dml_shape_cache,dml_cache_limit_per_graph=32 if a.dml_shape_cache else 0,
            dml_cache_caveat="fixed-shape sessions replicate weights/resources; not equivalent VRAM budgets",
            input="same predecoded BGR",host="same compiled native preprocessing/DB/crop/JSON",
            scope="full native OCR plus C ABI JSON copy, excludes file decoding/HTTP/Python JSON parse",
            ctc="Vulkan GPU argmax, DML native std::max_element greedy (prior project style), validates winning score",
            precision=(("EXPERIMENTAL DET-only mixed precision requested: selected DET Conv operands FP16 / accumulation and activations FP32; all REC/CLS dispatch FP32; DML internal precision not established"
                if os.environ.get("LWVK_EXPERIMENTAL_COOP_DET_ONLY")=="1" else
                "EXPERIMENTAL mixed precision requested (requires opt-in build): FP32 tensors, selected FP16 Conv operands / FP32 accumulation, CLS and vocabulary projection FP32; DML internal precision not established")
                if os.environ.get("LWVK_EXPERIMENTAL_COOP")=="1" else "FP32 inputs/outputs; DML internal precision not established"),
            experimental_coop_requested=os.environ.get("LWVK_EXPERIMENTAL_COOP")=="1",
            order="alternating; only one GPU workload runs at a time",private_text_in_report=False),comparisons=comparisons)
    report['method']['experimental_tile64_requested']=os.environ.get('LWVK_EXPERIMENTAL_TILE64')=='1'
    report['method']['experimental_coop_det_only_requested']=os.environ.get('LWVK_EXPERIMENTAL_COOP_DET_ONLY')=='1'
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
    completed = True


if __name__ == "__main__":
    main()
