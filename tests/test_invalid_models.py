"""Malformed model load/plan cases, then a real-model recovery request."""
import argparse
import copy
import ctypes as C
import json
from pathlib import Path
import shutil
import sys
import tempfile
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"examples/python"))
from lwvk import load, Detector, Network

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--library",required=True)
p.add_argument("--device",type=int,default=0)
a=p.parse_args()
root=Path(__file__).resolve().parents[1]
lib=load(a.library)
model=root/"models/ppocrv6-tiny"
original=json.loads((model/"det.json").read_text(encoding="utf-8"))
with tempfile.TemporaryDirectory(prefix="lwvk-model-") as temporary:
    directory=Path(temporary)
    shutil.copyfile(model/"weights.bin",directory/"weights.bin")
    for case in ("version","weights-path","weights-size","constant-bounds","topology","operator","truncated-json"):
        j=copy.deepcopy(original)
        if case=="version": j["format_version"]=999
        if case=="weights-path": j["weights_file"]="../weights.bin"
        if case=="weights-size": j["weights_bytes"]+=4
        if case=="constant-bounds": next(t for t in j["tensors"] if "offset" in t)["offset"]=2**63
        if case=="topology": j["nodes"][0]["inputs"]=[1000000]
        if case=="operator": j["nodes"][0]["op"]="Unknown"
        (directory/"det.json").write_text("{" if case=="truncated-json" else json.dumps(j),encoding="utf-8")
        handle=C.c_void_p()
        status=lib.lwvk_detector_create(str(directory/"det.json").encode("utf-8"),a.device,0,C.byref(handle))
        if not status or handle.value:
            if handle.value: lib.lwvk_detector_destroy(handle)
            raise AssertionError(f"{case} unexpectedly accepted")
        print(f"PASS: {case}: {lib.lwvk_last_error().decode('utf-8','replace')}")
    weights=bytearray((directory/"weights.bin").read_bytes()); weights[0]^=1
    (directory/"weights.bin").write_bytes(weights)
    (directory/"det.json").write_text(json.dumps(original),encoding="utf-8")
    handle=C.c_void_p()
    assert lib.lwvk_detector_create(str(directory/"det.json").encode(),a.device,0,C.byref(handle))==3 and not handle.value
with Detector(lib,model/"det.json",a.device,workspace=4096) as detector:
    try: detector.run(np.zeros((1,3,32,32),dtype=np.float32))
    except RuntimeError as error: assert "max_workspace_bytes" in str(error)
    else: raise AssertionError("workspace limit not enforced")
with Detector(lib,model/"det.json",a.device) as detector:
    result,_=detector.run(np.zeros((1,3,32,32),dtype=np.float32))
    assert np.isfinite(result).all()
print("PASS: invalid model/checksum/workspace limits and recovery")
rec=model/"rec"
original=json.loads((rec/"model.json").read_text(encoding="utf-8"))
with tempfile.TemporaryDirectory(prefix="lwvk-dictionary-") as temporary:
    directory=Path(temporary)
    shutil.copyfile(rec/"weights.bin",directory/"weights.bin")
    for case in ("dictionary-path", "dictionary-classes", "dictionary-corrupted"):
        j=copy.deepcopy(original)
        raw=bytearray((rec/"dictionary.txt").read_bytes())
        if case=="dictionary-path": j["dictionary_file"]="../dictionary.txt"
        if case=="dictionary-classes": j["classes"]=6905
        if case=="dictionary-corrupted": raw[0]^=1
        (directory/"dictionary.txt").write_bytes(raw)
        (directory/"model.json").write_text(json.dumps(j),encoding="utf-8")
        handle=C.c_void_p()
        assert lib.lwvk_network_create(str(directory/"model.json").encode(),a.device,0,C.byref(handle))==3
        assert not handle.value
        print(f"PASS: {case}: {lib.lwvk_last_error().decode()}")
with Network(lib,model/"cls/model.json",a.device) as classifier:
    required, score=C.c_uint64(), C.c_float()
    assert lib.lwvk_recognize_tensor(classifier.handle,None,0,32,None,0,C.byref(required),C.byref(score),None)==1
    assert lib.lwvk_network_shape(classifier.handle,48,32,None,None)==1
    probabilities,_=classifier.run(np.zeros((1,3,80,160),dtype=np.float32))
    assert np.isfinite(probabilities).all()
print("PASS: dictionary rejection, wrong-task request, normal CLS recovery")
