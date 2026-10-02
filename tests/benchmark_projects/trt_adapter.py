"""Use the original PP-OCRv5_Test_trt native DLL and engine assets unchanged."""
import ctypes as C
import os
from .adapters import DmlProject


class TrtProject(DmlProject):
    def __init__(self,library,runtime,engine_dir,models,variant,predictors=4,batch=4):
        self.dirs=[os.add_dll_directory(str(runtime.resolve()))]
        cuda=os.environ.get('CUDA_PATH')
        if cuda and os.path.isdir(os.path.join(cuda,'bin')):
            self.dirs.append(os.add_dll_directory(os.path.join(cuda,'bin')))
        self.lib=C.CDLL(str(library.resolve()));l=self.lib
        self.handle=C.c_void_p();self.message=C.create_string_buffer(256)
        l.init.argtypes=[C.POINTER(C.c_void_p),C.c_bool,C.c_int,C.c_char_p,C.c_int,C.c_double,C.c_double,C.c_double,C.c_bool,
            C.c_bool,C.c_bool,C.c_char_p,C.c_double,C.c_double,C.c_char_p,C.c_char_p,C.c_int,C.c_int,C.c_int,C.c_int,C.c_void_p]
        l.init.restype=C.c_int
        l.ocr2.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_void_p,C.c_void_p,C.POINTER(C.c_void_p),C.POINTER(C.c_int)]
        l.ocr2.restype=C.c_int
        l.destroy.argtypes=[C.c_void_p,C.c_void_p];l.destroy.restype=C.c_int
        self.ole=C.WinDLL('ole32');self.ole.CoTaskMemFree.argtypes=[C.c_void_p];self.ole.CoTaskMemFree.restype=None
        # Preserve the actual demo's Tiny plans. Only missing Small/Medium
        # plans are borrowed from the matching local SDK asset directory.
        selected=runtime/'inference_trt' if variant=='tiny' else engine_dir
        self.assets=dict(det=selected/f'PP-OCRv6_{variant}_det_fp16.engine',
            rec=selected/f'PP-OCRv6_{variant}_rec_fp16.engine',
            cls=runtime/'inference_trt/PP-OCRv5_mobile_cls_onnx.engine')
        for path in self.assets.values():
            if not path.is_file():raise FileNotFoundError(path)
        encoded=lambda p:str(p.resolve()).encode('utf-8')
        self.checked(l.init(C.byref(self.handle),True,0,encoded(self.assets['det']),960,.3,.6,1.5,False,
            True,True,encoded(self.assets['cls']),.9,1.,encoded(self.assets['rec']),encoded(models/'dictionary.txt'),
            batch,48,320,predictors,self.message))
        self.config=dict(cuda_device=0,precision='existing FP16-enabled TensorRT plans with FP32 I/O and possible FP32 fallback',
            det_limit_side=960,cls=True,cls_batch=1,rec_batch=batch,rec_predictors=predictors,
            rec_height=48,rec_min_width=320,rec_max_width=1280,rec_width_alignment=32,
            pipeline='original project OpenCV preprocessing/DB/crop and CPU CTC; no pipeline replacement')

    def checked(self,status):
        if status:raise RuntimeError(f'TensorRT project error {status}: '+self.message.value.decode('utf-8','replace'))

    def close(self):
        super().close()
        for directory in self.dirs:directory.close()
        self.dirs=[]
