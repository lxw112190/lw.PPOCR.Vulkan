"""Call each project's actual C ABI, without replacing its OCR pipeline."""
import ctypes as C
import json
import os
from pathlib import Path
import sys

U32=C.c_uint32
U64=C.c_uint64
F=C.c_float


class DetectorOptions(C.Structure):
    _fields_=[('struct_size',U32),('limit_side_length',U32),('max_candidates',U32),('use_dilation',U32),
        ('bitmap_threshold',F),('box_threshold',F),('unclip_ratio',F),('reserved',U32),
        ('max_model_file_size',U64),('max_workspace_size',U64),('max_tensor_size',U64),('max_image_pixels',U64)]


class ClassifierOptions(C.Structure):
    _fields_=[('struct_size',U32),('reserved',U32),('max_model_file_size',U64),
        ('max_workspace_size',U64),('max_tensor_size',U64),('max_image_pixels',U64)]


class RecognizerOptions(C.Structure):
    _fields_=[('struct_size',U32),('target_width',U32),('reserved0',U32),('reserved1',U32),
        ('max_model_file_size',U64),('max_workspace_size',U64),('max_tensor_size',U64),('max_image_pixels',U64)]


class OcrOptions(C.Structure):
    _fields_=[('struct_size',U32),('use_direction_classification',U32),('classifier_threshold',F),
        ('worker_count',U32),('max_crop_pixels',U64),('detector',DetectorOptions),
        ('classifier',ClassifierOptions),('recognizer',RecognizerOptions)]


class OcrInfo(C.Structure):
    _fields_=[('struct_size',U32),('use_direction_classification',U32),('max_line_capacity',U32),
        ('worker_count',U32),('max_text_capacity',U64),('max_text_capacity_per_line',U64),('max_crop_pixels',U64)]


class Box(C.Structure):
    _fields_=[(f'{axis}{i}',F) for i in range(1,5) for axis in ('x','y')]+[('score',F),('reserved',U32)]


class OcrLine(C.Structure):
    _fields_=[('box',Box),('recognition_score',F),('classification_score',F),('classification_label',U32),
        ('applied_rotation_degrees',U32),('emitted_count',U32),('reserved',U32),('text_offset',U64),('text_length',U64)]


class OcrResult(C.Structure):
    _fields_=[(n,U32) for n in ('struct_size','line_count','required_line_capacity','detected_count',
        'detector_resized_width','detector_resized_height','reserved0','reserved1')]+[('required_text_capacity',U64)]


class Error(C.Structure):
    _fields_=[('struct_size',U32),('code',C.c_int32),('message',C.c_char*256)]


class CProject:
    def __init__(self,library,models,dictionary,workers):
        self.lib=C.CDLL(str(library.resolve()));self.handle=C.c_void_p();self.error=Error(struct_size=C.sizeof(Error))
        l=self.lib
        l.lw_ocr_options_init.argtypes=[C.POINTER(OcrOptions)]
        l.lw_ocr_info_init.argtypes=[C.POINTER(OcrInfo)]
        l.lw_ocr_result_init.argtypes=[C.POINTER(OcrResult)]
        l.lw_ocr_create.argtypes=[C.c_char_p]*4+[C.POINTER(OcrOptions),C.POINTER(C.c_void_p),C.POINTER(Error)]
        l.lw_ocr_get_info.argtypes=[C.c_void_p,C.POINTER(OcrInfo)]
        l.lw_ocr_free.argtypes=[C.c_void_p];l.lw_ocr_free.restype=None
        l.lw_ocr_run_bgr_u8.argtypes=[C.c_void_p,C.c_void_p,U64,U32,U32,U32,C.POINTER(OcrLine),U32,
            C.c_void_p,U64,C.POINTER(OcrResult),C.POINTER(Error)]
        o=OcrOptions();l.lw_ocr_options_init(C.byref(o))
        if o.struct_size!=C.sizeof(o):raise RuntimeError('C config ABI mismatch')
        o.use_direction_classification=1;o.classifier_threshold=.9;o.worker_count=workers
        o.detector.limit_side_length=960;o.detector.max_candidates=1000;o.detector.use_dilation=0
        o.detector.bitmap_threshold=.3;o.detector.box_threshold=.6;o.detector.unclip_ratio=1.5
        o.recognizer.target_width=960
        self.checked(l.lw_ocr_create(*[str((models/name).resolve()).encode('utf-8') for name in ('det.lwm','cls.lwm','rec.lwm')],
            str(dictionary.resolve()).encode('utf-8'),C.byref(o),C.byref(self.handle),C.byref(self.error)))
        try:
            info=OcrInfo();l.lw_ocr_info_init(C.byref(info));self.checked(l.lw_ocr_get_info(self.handle,C.byref(info)))
            self.lines=(OcrLine*info.max_line_capacity)();self.text=C.create_string_buffer(info.max_text_capacity)
            self.config=dict(worker_count=info.worker_count,rec_max_width=960,det_limit_side=960,cls=True,
                c_options_size=C.sizeof(o),output_line_capacity=info.max_line_capacity,output_text_bytes=info.max_text_capacity)
        except BaseException:self.close();raise

    def checked(self,status):
        if status:raise RuntimeError(f'C error {status}: '+self.error.message.decode('utf-8','replace'))

    def run(self,bgr):
        h,w=bgr.shape[:2];r=OcrResult();self.lib.lw_ocr_result_init(C.byref(r))
        self.checked(self.lib.lw_ocr_run_bgr_u8(self.handle,bgr.ctypes.data,bgr.nbytes,w,h,w*3,
            self.lines,len(self.lines),self.text,len(self.text),C.byref(r),C.byref(self.error)))
        if r.line_count>len(self.lines):raise RuntimeError('C result line capacity contract violated')
        result=[]
        for line in self.lines[:r.line_count]:
            if line.text_offset+line.text_length>len(self.text):raise RuntimeError('C result text capacity contract violated')
            text=C.string_at(C.addressof(self.text)+line.text_offset,line.text_length).decode('utf-8')
            result.append(dict(text=text,score=line.recognition_score,
                **{n:getattr(line.box,n) for n in ('x1','y1','x2','y2','x3','y3','x4','y4')}))
        return result

    def close(self):
        if self.handle:self.lib.lw_ocr_free(self.handle);self.handle=C.c_void_p()


class DmlProject:
    def __init__(self,library,deps,models,dictionary,device,predictors,batch):
        self.dirs=[os.add_dll_directory(str(p.resolve())) for p in deps]
        self.lib=C.CDLL(str(library.resolve()));l=self.lib;self.handle=C.c_void_p();self.message=C.create_string_buffer(256)
        l.init.argtypes=[C.POINTER(C.c_void_p),C.c_bool,C.c_int,C.c_char_p,C.c_int,C.c_double,C.c_double,C.c_double,C.c_bool,
            C.c_bool,C.c_bool,C.c_char_p,C.c_double,C.c_double,C.c_char_p,C.c_char_p,C.c_int,C.c_int,C.c_int,C.c_int,C.c_void_p]
        l.ocr2.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.POINTER(C.c_int)]
        l.destroy.argtypes=[C.c_void_p,C.c_void_p]
        self.ole=C.WinDLL('ole32');self.ole.CoTaskMemFree.argtypes=[C.c_void_p];self.ole.CoTaskMemFree.restype=None
        paths=[str((models/n).resolve()).encode('utf-8') for n in ('det.onnx','cls.onnx','rec.onnx')]
        self.checked(l.init(C.byref(self.handle),True,device,paths[0],960,.3,.6,1.5,False,
            True,True,paths[1],.9,1.,paths[2],str(dictionary.resolve()).encode('utf-8'),batch,48,320,predictors,self.message))
        self.config=dict(dxgi_device=device,det_limit_side=960,cls=True,cls_batch=1,rec_batch=batch,
            rec_predictors=predictors,rec_height=48,rec_min_width=320,rec_width_buckets=[320,384,448,512,640,768,960,1152,1280])

    def checked(self,status):
        if status:raise RuntimeError(f'DML project error {status}: '+self.message.value.decode('utf-8','replace'))

    def run(self,bgr):
        h,w=bgr.shape[:2];p=C.c_void_p();n=C.c_int()
        try:
            self.checked(self.lib.ocr2(self.handle,h,w,3,bgr.ctypes.data,self.message,C.byref(p),C.byref(n)))
            if not p or not 0<n.value<=100_000_000:raise RuntimeError('DML result length invalid')
            return json.loads(C.string_at(p,n.value).decode('utf-8'))
        finally:
            if p:self.ole.CoTaskMemFree(p)

    def close(self):
        if self.handle:self.checked(self.lib.destroy(self.handle,self.message));self.handle=C.c_void_p()


class VulkanProject:
    def __init__(self,library,models,device):
        sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'examples/python'))
        from lwvk import load,OCR,DeviceInfo,check
        self.lib=load(library);info=DeviceInfo(struct_size=C.sizeof(DeviceInfo))
        check(self.lib,self.lib.lwvk_device_get(device,C.byref(info)))
        if 'RTX 4060' not in info.name.decode():raise RuntimeError('Vulkan benchmark must use RTX 4060')
        self.engine=OCR(self.lib,models,device,det_limit_side=960,enable_classifier=1,
            bitmap_threshold=.3,box_threshold=.6,unclip_ratio=1.5,cls_threshold=.9)
        self.config=dict(vulkan_device=device,device=info.name.decode(),vendor_id=info.vendor_id,device_id=info.device_id,
            precision='default FP32',version=self.lib.lwvk_version().decode(),det_limit_side=960,cls=True,
            workspace_per_graph_mib=512,rec_min_width=32,rec_max_width=960,rec_width_alignment=8)

    def run(self,bgr):return self.engine.run(bgr)['items']
    def close(self):self.engine.close()
