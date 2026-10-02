"""Windows process engine activity + NVIDIA board activity; independent metrics."""
import ctypes as C
from ctypes import wintypes as W
import math
import os
import re
import statistics
from .memory import GpuCounters, CounterItem, MemoryMonitor, ram


class ActivityCounters(GpuCounters):
    def __init__(self):
        super().__init__()
        self.activity=C.c_void_p()
        try:
            self.check(self.pdh.PdhAddEnglishCounterW(self.query,
                r'\GPU Engine(*)\Utilization Percentage',0,C.byref(self.activity)))
        except BaseException:
            self.close();raise

    def sample(self):
        result=super().sample()
        size,count=W.DWORD(),W.DWORD()
        status=self.pdh.PdhGetFormattedCounterArrayW(self.activity,0x200,
            C.byref(size),C.byref(count),None)
        if status&0xffffffff not in (0,0x800007d2):self.check(status)
        engines={}
        result['process_busiest_engine_percent']=None
        result['process_engine_percent']=engines
        if size.value:
            buf=C.create_string_buffer(size.value)
            status=self.pdh.PdhGetFormattedCounterArrayW(self.activity,0x200,
                C.byref(size),C.byref(count),buf)
            # A rate counter needs two collections; no valid first interval is
            # expected, not a zero or a failure of the memory counters.
            if status&0xffffffff in (0xc0000bba,0x800007d5):return result
            self.check(status)
            if count.value*C.sizeof(CounterItem)>len(buf):raise RuntimeError('invalid PDH activity array')
            items=C.cast(buf,C.POINTER(CounterItem))
            for i in range(count.value):
                item=items[i];name=item.name or ''
                if not re.match(r'pid_'+str(os.getpid())+r'_',name) or item.formatted.CStatus not in (0,1):continue
                value=item.formatted.value.doubleValue
                if not math.isfinite(value) or value<0:raise RuntimeError('invalid utilization counter')
                # PDH may otherwise cap independently active engines at 100.
                # Report the busiest engine, never sum engine % as board %.
                engines[name]=min(value,100.)
        result['process_busiest_engine_percent']=max(engines.values()) if engines else None
        result['process_engine_percent']=engines
        return result


class Utilization(C.Structure):
    _fields_=[('gpu',C.c_uint),('memory',C.c_uint)]


class NvidiaBoard:
    def __init__(self,index=0):
        self.lib=C.WinDLL(os.path.join(os.environ.get('SystemRoot',r'C:\Windows'),'System32','nvml.dll'))
        self.lib.nvmlDeviceGetHandleByIndex_v2.argtypes=[C.c_uint,C.POINTER(C.c_void_p)]
        self.lib.nvmlDeviceGetUtilizationRates.argtypes=[C.c_void_p,C.POINTER(Utilization)]
        self.lib.nvmlDeviceGetName.argtypes=[C.c_void_p,C.c_void_p,C.c_uint]
        self.lib.nvmlDeviceGetUUID.argtypes=[C.c_void_p,C.c_void_p,C.c_uint]
        self.lib.nvmlSystemGetDriverVersion.argtypes=[C.c_void_p,C.c_uint]
        self.check(self.lib.nvmlInit_v2());self.handle=C.c_void_p()
        try:
            self.check(self.lib.nvmlDeviceGetHandleByIndex_v2(index,C.byref(self.handle)))
            def string(fn,*prefix):
                buf=C.create_string_buffer(256);self.check(fn(*prefix,buf,len(buf)));return buf.value.decode()
            self.info=dict(index=index,name=string(self.lib.nvmlDeviceGetName,self.handle),
                uuid=string(self.lib.nvmlDeviceGetUUID,self.handle),driver=string(self.lib.nvmlSystemGetDriverVersion))
        except BaseException:self.close();raise

    @staticmethod
    def check(status):
        if status:raise RuntimeError('NVML error '+str(status))

    def sample(self):
        value=Utilization();self.check(self.lib.nvmlDeviceGetUtilizationRates(self.handle,C.byref(value)))
        return dict(board_gpu_percent=value.gpu,board_memory_utilization_percent=value.memory)

    def close(self):self.lib.nvmlShutdown()


def activity_summary(rows):
    result={}
    for key in ('process_busiest_engine_percent','board_gpu_percent','board_memory_utilization_percent'):
        values=[row[key] for row in rows if row.get(key) is not None]
        result[key]=dict(mean=statistics.mean(values),peak=max(values),samples=len(values)) if values else None
    result['per_engine_type']={}
    for row in rows:
        for name,value in row.get('process_engine_percent',{}).items():
            kind=name.split('engtype_',1)[-1]
            result['per_engine_type'][kind]=max(result['per_engine_type'].get(kind,0),value)
    return result


class ResourceMonitor(MemoryMonitor):
    def __init__(self):
        super().__init__()
        if self.gpu:self.gpu.close()
        self.gpu=None;self.gpu_error=None;self.phase='load';self.nvml_error=None;self.board=None
        self.nvml_failures=[]
        try:self.gpu=ActivityCounters()
        except Exception as e:self.gpu_error=str(e)
        try:self.board=NvidiaBoard()
        except Exception as e:self.nvml_error=str(e)

    def snapshot(self):
        row=super().snapshot();row['phase']=self.phase
        if self.board:
            try:row.update(self.board.sample())
            except Exception as e:
                self.nvml_error=str(e)
                self.nvml_failures.append(dict(phase=self.phase,elapsed_s=row['elapsed_s'],error=str(e)))
        return row

    def finish(self):
        result=super().finish()
        result['gpu_activity']=activity_summary([r for r in self.samples if r['phase']=='measure'])
        result['gpu_activity_measurement']='250ms sampling during the measured stream, including between-call decode/JSON work; not shader active-time %'
        result['nvml_error']=self.nvml_error
        result['nvml_failed_samples']=self.nvml_failures
        result['nvml_board']=self.board.info if self.board else None
        if self.board:self.board.close();self.board=None
        result['utilization_caveat']='Process busiest-engine % is a WDDM per-PID interval counter, not an engine sum. NVML GPU % is device-wide, can include other processes and has driver-dependent averaging. Memory utilization % means memory activity, NOT VRAM capacity used. Missing counters remain null.'
        return result
