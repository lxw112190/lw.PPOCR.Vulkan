"""Offline lowering of the exact PP-OCRv6 Tiny DET ONNX to internal LWVK-DET v0."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import onnx
from onnx import helper, numpy_helper

DET_SHA256 = "193bab7a04fca699a6c82e6abb5b81bdb28177f0abd4062552b04908dafb19f8"
SUPPORTED = {"Conv", "ConvTranspose", "Add", "Mul", "Div", "Erf", "HardSigmoid",
             "Relu", "ReduceMean", "GlobalAveragePool", "MaxPool", "Resize", "Concat", "Sigmoid"}


def export(source: Path, output: Path) -> None:
    if hashlib.sha256(source.read_bytes()).hexdigest() != DET_SHA256:
        raise ValueError("Only the pinned PP-OCRv6 Tiny DET model is supported")
    model = onnx.load(source)
    onnx.checker.check_model(model)
    weights = {v.name: numpy_helper.to_array(v) for v in model.graph.initializer}
    tensors, ids, data = [], {}, bytearray()

    def tensor(name):
        if name not in ids:
            ids[name] = len(tensors)
            item = {"name": name}
            if name in weights:
                value = weights[name]
                if value.dtype != np.float32:
                    raise ValueError(f"unsupported constant dtype: {name}")
                item.update(shape=list(value.shape), offset=len(data), count=int(value.size))
                data.extend(value.astype("<f4").tobytes())
            tensors.append(item)
        return ids[name]

    nodes = []
    input_id = tensor(model.graph.input[0].name)
    for node in model.graph.node:
        if node.op_type not in SUPPORTED or len(node.output) != 1:
            raise ValueError(f"unsupported node: {node.op_type}")
        attrs = {a.name: helper.get_attribute_value(a) for a in node.attribute}
        attrs = {k: v.decode() if isinstance(v, bytes) else v for k, v in attrs.items()}
        inputs = list(node.input)
        if node.op_type == "Conv" and attrs.get("auto_pad") == "SAME_UPPER":
            if attrs.get("kernel_shape") != [2, 2] or attrs.get("strides") != [1, 1] or attrs.get("dilations") != [1, 1]:
                raise ValueError("only exact Tiny stem SAME_UPPER 2x2 stride1 lowering supported")
            attrs["auto_pad"] = "NOTSET"
            attrs["pads"] = [0, 0, 1, 1]
        if node.op_type == "Resize":
            scales = weights[inputs[2]].tolist()
            if scales[:2] != [1.0, 1.0] or any(int(x) != x or x < 1 for x in scales[2:]):
                raise ValueError("only integer spatial Resize is supported")
            attrs["factors"] = [int(x) for x in scales[2:]]
            inputs = inputs[:1]
        nodes.append({"op": node.op_type, "inputs": [tensor(x) for x in inputs],
                      "output": tensor(node.output[0]), "attrs": attrs})
    output.mkdir(parents=True, exist_ok=True)
    (output / "weights.bin").write_bytes(data)
    checksum = 14695981039346656037
    for byte in data:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    manifest = {"format": "LWVK-DET", "format_version": 0, "source_sha256": DET_SHA256,
                "weights_file": "weights.bin", "weights_bytes": len(data),
                "weights_sha256": hashlib.sha256(data).hexdigest(), "weights_fnv1a64": str(checksum), "input": input_id,
                "output": tensor(model.graph.output[0].name), "tensors": tensors, "nodes": nodes}
    (output / "det.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {len(nodes)} DET nodes, {len(tensors)} tensors, {len(data)} weight bytes")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    export(a.input, a.output)
