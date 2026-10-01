"""Host ABI and early invalid argument checks; does not claim GPU coverage."""
import argparse
import ctypes as C
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/python"))
from lwvk import load, DeviceInfo, OcrConfig

p = argparse.ArgumentParser()
p.add_argument("--library", required=True)
a = p.parse_args()
lib = load(a.library)
assert lib.lwvk_version().decode() == (Path(__file__).resolve().parents[1] / "RELEASE_VERSION").read_text().strip()
assert lib.lwvk_device_count(None) == 1
assert lib.lwvk_device_get(0, None) == 1
info = DeviceInfo()
assert lib.lwvk_device_get(0, C.byref(info)) == 1
handle = C.c_void_p(123)
assert lib.lwvk_detector_create(None, 0, 0, C.byref(handle)) == 1 and not handle.value
assert lib.lwvk_detector_run(None, None, 0, 32, 32, None, 0, None) == 1
lib.lwvk_detector_destroy(None)
handle = C.c_void_p(123)
assert lib.lwvk_network_create(None, 0, 0, C.byref(handle)) == 1 and not handle.value
assert lib.lwvk_network_shape(None, 48, 32, None, None) == 1
assert lib.lwvk_network_run(None, None, 0, 48, 32, None, 0, None) == 1
assert lib.lwvk_recognize_tensor(None, None, 0, 32, None, 0, None, None, None) == 1
assert lib.lwvk_recognize_bgr(None, None, 0, 1, 1, 3, 0, None, 0, None, None, None) == 1
lib.lwvk_network_destroy(None)
assert lib.lwvk_ocr_config_default(None) == 1
config = OcrConfig()
assert lib.lwvk_ocr_config_default(C.byref(config)) == 0
assert config.struct_size == C.sizeof(config) and config.enable_classifier == 1
assert lib.lwvk_ocr_create(None, C.byref(config), C.byref(handle)) == 1 and not handle.value
for field, value in (("struct_size", 0), ("reserved", 1), ("det_limit_side", 961),
                     ("max_candidates", 1001), ("cls_threshold", float("nan")),
                     ("max_crop_pixels", 0), ("max_total_crop_pixels", 1)):
    broken = OcrConfig.from_buffer_copy(config)
    setattr(broken, field, value)
    # Config rejection before a filesystem/device call, even on GPU-less CI.
    assert lib.lwvk_ocr_create(b"missing", C.byref(broken), C.byref(handle)) == 1 and not handle.value
assert lib.lwvk_ocr_run_bgr(None, None, 0, 1, 1, 3, C.byref(handle)) == 1 and not handle.value
required = C.c_uint64(123)
assert lib.lwvk_ocr_result_json(None, None, 0, C.byref(required)) == 1 and required.value == 0
lib.lwvk_ocr_destroy(None)
lib.lwvk_ocr_result_destroy(None)
print("PASS: host ABI/invalid arguments; GPU inference is a separate test")
