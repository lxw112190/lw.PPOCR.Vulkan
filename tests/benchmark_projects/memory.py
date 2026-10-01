"""Windows process RAM and WDDM GPU-process-memory sampling, not leak proof."""
import ctypes as C
from ctypes import wintypes as W
import math
import os
import re
import threading
import time
import psutil


class ValueUnion(C.Union):
    _fields_=[('doubleValue',C.c_double),('largeValue',C.c_int64)]


class CounterValue(C.Structure):
    _fields_=[('CStatus',W.DWORD),('value',ValueUnion)]


class CounterItem(C.Structure):
    _fields_=[('name',C.c_wchar_p),('formatted',CounterValue)]


class GpuCounters:
    def __init__(self):
        self.pdh=C.WinDLL('pdh');p=self.pdh
        p.PdhOpenQueryW.argtypes=[C.c_wchar_p,C.c_size_t,C.POINTER(C.c_void_p)]
        p.PdhAddEnglishCounterW.argtypes=[C.c_void_p,C.c_wchar_p,C.c_size_t,C.POINTER(C.c_void_p)]
        p.PdhCollectQueryData.argtypes=[C.c_void_p]
        p.PdhGetFormattedCounterArrayW.argtypes=[C.c_void_p,W.DWORD,C.POINTER(W.DWORD),C.POINTER(W.DWORD),C.c_void_p]
        p.PdhCloseQuery.argtypes=[C.c_void_p]
        self.query=C.c_void_p();self.counters={};self.instances=set()
        self.check(p.PdhOpenQueryW(None,0,C.byref(self.query)))
        try:
            for key,path in [('dedicated','Dedicated Usage'),('shared','Shared Usage')]:
                counter=C.c_void_p();self.check(p.PdhAddEnglishCounterW(self.query,
                    '\\GPU Process Memory(*)\\'+path,0,C.byref(counter)))
                self.counters[key]=counter
            self.check(p.PdhCollectQueryData(self.query))
        except BaseException:self.close();raise

    @staticmethod
    def check(status):
        if status:raise RuntimeError('PDH status 0x%08x'%(status&0xffffffff))

    def sample(self):
        self.check(self.pdh.PdhCollectQueryData(self.query));result={}
        for key,counter in self.counters.items():
            size,count=W.DWORD(),W.DWORD()
            status=self.pdh.PdhGetFormattedCounterArrayW(counter,0x200,C.byref(size),C.byref(count),None)
            if status&0xffffffff not in (0,0x800007d2):self.check(status)
            if not size.value:result[key]=None;continue
            buf=C.create_string_buffer(size.value)
            self.check(self.pdh.PdhGetFormattedCounterArrayW(counter,0x200,C.byref(size),C.byref(count),buf))
            if count.value*C.sizeof(CounterItem)>len(buf):raise RuntimeError('invalid PDH array size')
            items=C.cast(buf,C.POINTER(CounterItem));total=0.;found=False
            for i in range(count.value):
                item=items[i];name=item.name or ''
                if re.match(r'pid_'+str(os.getpid())+r'_',name) and item.formatted.CStatus in (0,1):
                    v=item.formatted.value.doubleValue
                    if not math.isfinite(v) or v<0:raise RuntimeError('invalid GPU memory counter')
                    total+=v;found=True;self.instances.add(name)
            result[key]=int(total) if found else None
        return result

    def close(self):
        if self.query:self.pdh.PdhCloseQuery(self.query);self.query=C.c_void_p()


def ram(process):
    info=process.memory_info()
    return dict(rss_bytes=info.rss,private_bytes=getattr(info,'private',None),
        peak_working_set_bytes=getattr(info,'peak_wset',None))


class MemoryMonitor:
    def __init__(self):
        self.process=psutil.Process();self.stop_event=threading.Event();self.samples=[];self.gpu_error=None
        self.gpu=None
        try:self.gpu=GpuCounters()
        except Exception as e:self.gpu_error=str(e)
        self.thread=threading.Thread(target=self.loop,name='memory-sampler',daemon=True)

    def snapshot(self):
        row=dict(elapsed_s=time.perf_counter()-self.started,**ram(self.process))
        if self.gpu:
            try:row.update(self.gpu.sample())
            except Exception as e:self.gpu_error=str(e)
        self.samples.append(row)
        return row

    def loop(self):
        while not self.stop_event.wait(.25):self.snapshot()

    def start(self):self.started=time.perf_counter();self.snapshot();self.thread.start()

    def finish(self):
        self.stop_event.set();self.thread.join();self.snapshot()
        if self.gpu:self.gpu.close()
        def peak(key):
            values=[s[key] for s in self.samples if s.get(key) is not None]
            return max(values) if values else None
        return dict(sample_period_s=.25,sample_count=len(self.samples),
            rss_peak_bytes=peak('rss_bytes'),os_lifetime_peak_working_set_bytes=peak('peak_working_set_bytes'),
            private_peak_bytes=peak('private_bytes'),gpu_dedicated_peak_bytes=peak('dedicated'),
            gpu_shared_peak_bytes=peak('shared'),gpu_counter_error=self.gpu_error,
            gpu_instances=sorted(self.gpu.instances) if self.gpu else [],samples=self.samples,
            caveat='RAM includes Python/NumPy/Pillow/input/result/runtime/driver. OS peak is lifetime; sampled private/GPU peaks may miss short spikes. GPU counters are per-process WDDM attribution summed across adapters, not physical board VRAM or a leak proof. Missing GPU instance is unknown, not a measured zero.')
