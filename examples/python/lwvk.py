"""Experimental DET C ABI binding; NumPy only in the example, not the runtime."""
import ctypes as C
from pathlib import Path
import numpy as np
import json


class DeviceInfo(C.Structure):
    _fields_ = [(x, C.c_uint32) for x in ("struct_size", "device_index", "vendor_id", "device_id",
                "api_version", "device_type", "max_shared_memory_bytes", "subgroup_size")] + [("name", C.c_char * 256)]


class OcrConfig(C.Structure):
    _fields_ = [(x, C.c_uint32) for x in ("struct_size", "device_index", "det_limit_side", "max_candidates",
        "enable_classifier", "use_dilation", "reading_order", "reserved")] + [
        (x, C.c_uint64) for x in ("max_workspace_bytes", "max_crop_pixels", "max_total_crop_pixels")] + [
        (x, C.c_float) for x in ("bitmap_threshold", "box_threshold", "unclip_ratio", "cls_threshold")]


def load(path):
    lib = C.CDLL(str(Path(path).resolve()))
    lib.lwvk_last_error.restype = C.c_char_p
    lib.lwvk_version.restype = C.c_char_p
    lib.lwvk_device_count.argtypes = [C.POINTER(C.c_uint32)]
    lib.lwvk_device_get.argtypes = [C.c_uint32, C.POINTER(DeviceInfo)]
    lib.lwvk_detector_create.argtypes = [C.c_char_p, C.c_uint32, C.c_uint64, C.POINTER(C.c_void_p)]
    lib.lwvk_detector_destroy.argtypes = [C.c_void_p]
    lib.lwvk_detector_destroy.restype = None
    fp = C.POINTER(C.c_float)
    lib.lwvk_detector_run.argtypes = [C.c_void_p, fp, C.c_uint64, C.c_uint32, C.c_uint32, fp, C.c_uint64, C.POINTER(C.c_double)]
    lib.lwvk_network_create.argtypes = [C.c_char_p, C.c_uint32, C.c_uint64, C.POINTER(C.c_void_p)]
    lib.lwvk_network_destroy.argtypes = [C.c_void_p]
    lib.lwvk_network_destroy.restype = None
    lib.lwvk_network_shape.argtypes = [C.c_void_p, C.c_uint32, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
    lib.lwvk_network_run.argtypes = lib.lwvk_detector_run.argtypes
    lib.lwvk_recognize_tensor.argtypes = [C.c_void_p, fp, C.c_uint64, C.c_uint32,
        C.POINTER(C.c_char), C.c_uint64, C.POINTER(C.c_uint64), C.POINTER(C.c_float), C.POINTER(C.c_double)]
    lib.lwvk_recognize_bgr.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_uint64,
        C.c_uint32, C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(C.c_char), C.c_uint64,
        C.POINTER(C.c_uint64), C.POINTER(C.c_float), C.POINTER(C.c_double)]
    lib.lwvk_ocr_config_default.argtypes = [C.POINTER(OcrConfig)]
    lib.lwvk_ocr_create.argtypes = [C.c_char_p, C.POINTER(OcrConfig), C.POINTER(C.c_void_p)]
    lib.lwvk_ocr_destroy.argtypes = [C.c_void_p]
    lib.lwvk_ocr_destroy.restype = None
    lib.lwvk_ocr_run_bgr.argtypes = [C.c_void_p, C.POINTER(C.c_uint8), C.c_uint64,
        C.c_uint32, C.c_uint32, C.c_uint32, C.POINTER(C.c_void_p)]
    lib.lwvk_ocr_result_json.argtypes = [C.c_void_p, C.POINTER(C.c_char), C.c_uint64, C.POINTER(C.c_uint64)]
    lib.lwvk_ocr_result_destroy.argtypes = [C.c_void_p]
    lib.lwvk_ocr_result_destroy.restype = None
    return lib


def check(lib, status):
    if status:
        raise RuntimeError(f"lwvk error {status}: {lib.lwvk_last_error().decode('utf-8', 'replace')}")


class Detector:
    def __init__(self, lib, model, device=0, workspace=0):
        self.lib = lib
        self.handle = C.c_void_p()
        check(lib, lib.lwvk_detector_create(str(Path(model).resolve()).encode("utf-8"), device, workspace, C.byref(self.handle)))

    def run(self, tensor):
        value = np.ascontiguousarray(tensor, dtype=np.float32)
        if value.ndim != 4 or value.shape[:2] != (1, 3):
            raise ValueError("input must be NCHW [1,3,H,W]")
        h, w = value.shape[2:]
        out = np.empty((1, 1, h, w), dtype=np.float32)
        elapsed = C.c_double()
        fp = C.POINTER(C.c_float)
        check(self.lib, self.lib.lwvk_detector_run(self.handle, value.ctypes.data_as(fp), value.size,
              h, w, out.ctypes.data_as(fp), out.size, C.byref(elapsed)))
        return out, elapsed.value

    def close(self):
        if self.handle:
            self.lib.lwvk_detector_destroy(self.handle)
            self.handle = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class OCR:
    def __init__(self, lib, model_root, device=0, **options):
        self.lib, self.handle = lib, C.c_void_p()
        config = OcrConfig()
        check(lib, lib.lwvk_ocr_config_default(C.byref(config)))
        config.device_index = device
        editable = {x[0] for x in config._fields_} - {"struct_size", "reserved", "device_index"}
        for name, value in options.items():
            if name not in editable:
                raise ValueError(f"unknown OCR option: {name}")
            setattr(config, name, value)
        check(lib, lib.lwvk_ocr_create(str(Path(model_root).resolve()).encode("utf-8"),
            C.byref(config), C.byref(self.handle)))

    def run(self, image):
        value = np.asarray(image)
        if value.dtype != np.uint8 or value.ndim != 3 or value.shape[2] != 3:
            raise ValueError("image must be uint8 HxWx3 BGR")
        value = np.ascontiguousarray(value)
        result = C.c_void_p()
        check(self.lib, self.lib.lwvk_ocr_run_bgr(self.handle, value.ctypes.data_as(C.POINTER(C.c_uint8)),
            value.nbytes, value.shape[1], value.shape[0], value.strides[0], C.byref(result)))
        try:
            required = C.c_uint64()
            status = self.lib.lwvk_ocr_result_json(result, None, 0, C.byref(required))
            if status != 5:
                check(self.lib, status)
                raise RuntimeError("expected JSON size query")
            text = C.create_string_buffer(required.value)
            check(self.lib, self.lib.lwvk_ocr_result_json(result, text, len(text), C.byref(required)))
            return json.loads(text.value.decode("utf-8"))
        finally:
            self.lib.lwvk_ocr_result_destroy(result)

    def close(self):
        if self.handle:
            self.lib.lwvk_ocr_destroy(self.handle)
            self.handle = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class Network(Detector):
    """Tiny DET/CLS/REC: row-major probabilities and optional native REC CTC."""
    def __init__(self, lib, model, device=0, workspace=0):
        self.lib = lib
        self.handle = C.c_void_p()
        check(lib, lib.lwvk_network_create(str(Path(model).resolve()).encode("utf-8"), device, workspace, C.byref(self.handle)))

    def run(self, tensor):
        value = np.ascontiguousarray(tensor, dtype=np.float32)
        if value.ndim != 4 or value.shape[:2] != (1, 3):
            raise ValueError("input must be NCHW [1,3,H,W]")
        h, w = value.shape[2:]
        rows, classes = C.c_uint32(), C.c_uint32()
        check(self.lib, self.lib.lwvk_network_shape(self.handle, h, w, C.byref(rows), C.byref(classes)))
        out = np.empty((rows.value, classes.value), dtype=np.float32)
        elapsed = C.c_double()
        fp = C.POINTER(C.c_float)
        check(self.lib, self.lib.lwvk_network_run(self.handle, value.ctypes.data_as(fp), value.size,
            h, w, out.ctypes.data_as(fp), out.size, C.byref(elapsed)))
        return out, elapsed.value

    def recognize(self, tensor):
        value = np.ascontiguousarray(tensor, dtype=np.float32)
        if value.ndim != 4 or value.shape[:3] != (1, 3, 48):
            raise ValueError("REC input must be [1,3,48,W]")
        # Pinned dictionary max label <= 8 UTF-8 bytes; max 120 timesteps.
        text = C.create_string_buffer(4096)
        required, score, elapsed = C.c_uint64(), C.c_float(), C.c_double()
        check(self.lib, self.lib.lwvk_recognize_tensor(self.handle,
            value.ctypes.data_as(C.POINTER(C.c_float)), value.size, value.shape[3],
            text, len(text), C.byref(required), C.byref(score), C.byref(elapsed)))
        return text.value.decode("utf-8"), score.value, elapsed.value

    def close(self):
        if self.handle:
            self.lib.lwvk_network_destroy(self.handle)
            self.handle = C.c_void_p()

    def recognize_bgr(self, image, rec_width=0):
        value = np.asarray(image)
        if value.dtype != np.uint8 or value.ndim != 3 or value.shape[2] != 3:
            raise ValueError("image must be uint8 HxWx3 BGR")
        value = np.ascontiguousarray(value)
        text = C.create_string_buffer(4096)
        required, score, elapsed = C.c_uint64(), C.c_float(), C.c_double()
        check(self.lib, self.lib.lwvk_recognize_bgr(self.handle,
            value.ctypes.data_as(C.POINTER(C.c_uint8)), value.nbytes,
            value.shape[1], value.shape[0], value.strides[0], rec_width,
            text, len(text), C.byref(required), C.byref(score), C.byref(elapsed)))
        return text.value.decode("utf-8"), score.value, elapsed.value
