"""Lower pinned Tiny CLS/REC to the shared FP32 NHWC graph (offline only).

Rank-only views preserve the existing NHWC storage. Tail MatMul [*,K]@[K,N]
becomes a 1x1 convolution with transposed weights, reusing the upstream kernel.
This is a deliberately narrow exporter, not a general ONNX runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import onnx
from onnx import helper, numpy_helper

HASHES = {
    "cls": "dd8b2b61983d76ab230a58da9e0e0e84956b71c3877f2ce6e438fe22d74d2cf2",
    "rec": "9ef676d6ed3c88256a2d92c640c44f25b0c40947e111b14b8be8f594091563e6",
}
DICT_HASH = "46e1b34ef45684cb46d75ac76d355341fe7f0a2c38d6ee02e63ae6b3878019fc"


def fnv(data):
    result = 14695981039346656037
    for byte in data:
        result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return str(result)


def export(source, output, task, dictionary=None):
    if hashlib.sha256(source.read_bytes()).hexdigest() != HASHES[task]:
        raise ValueError("unsupported source model SHA-256")
    model = onnx.load(source)
    onnx.checker.check_model(model)
    values = {v.name: numpy_helper.to_array(v) for v in model.graph.initializer}
    aliases, layouts, ids, tensors, nodes, data = {}, {}, {}, [], [], bytearray()

    def resolve(name):
        while name in aliases:
            name = aliases[name]
        return name

    def tensor(name):
        name = resolve(name)
        if name not in ids:
            ids[name] = len(tensors)
            item = {"name": name}
            if name in values:
                v = values[name]
                if v.dtype != np.float32:
                    raise ValueError(f"non-FP32 execution constant: {name}")
                item.update(shape=list(v.shape), offset=len(data), count=int(v.size))
                data.extend(v.astype("<f4").tobytes())
            tensors.append(item)
        return ids[name]

    def emit(op, inputs, out, attrs=None):
        nodes.append({"op": op, "inputs": [tensor(x) for x in inputs],
                      "output": tensor(out), "attrs": attrs or {}})

    def channel_constant(name):
        name = resolve(name)
        v = values[name]
        if v.ndim == 1 and v.size > 1:
            renamed = name + "/lwvk-channel"
            values[renamed] = v.reshape(1, -1, 1, 1)
            return renamed
        return name

    input_name = model.graph.input[0].name
    input_id = tensor(input_name)
    layouts[input_name] = "nchw"
    shape_names = set()
    for node in model.graph.node:
        op, out = node.op_type, node.output[0]
        ins = [resolve(x) for x in node.input]
        attrs = {a.name: helper.get_attribute_value(a) for a in node.attribute}
        attrs = {k: v.decode() if isinstance(v, bytes) else v for k, v in attrs.items()}
        if len(node.output) != 1:
            raise ValueError("multiple outputs unsupported")
        if op == "Identity":
            aliases[out] = ins[0]
            continue
        if op == "Shape" and task == "cls":
            shape_names.add(out)
            continue
        if op == "Slice" and task == "cls" and ins[0] in shape_names:
            if attrs != {"axes": [0], "starts": [0], "ends": [1]}:
                raise ValueError("unexpected classifier shape slice")
            shape_names.add(out)
            continue
        if op == "Concat" and task == "cls" and ins[0] in shape_names:
            if attrs != {"axis": 0} or values[ins[1]].tolist() != [-1]:
                raise ValueError("unexpected classifier flatten shape")
            shape_names.add(out)
            continue
        kind = layouts[ins[0]]
        if op in ("Squeeze", "Unsqueeze", "Transpose", "Reshape"):
            if op == "Squeeze" and kind == "nchw" and attrs == {"axes": [2]}:
                next_kind = "ncw"
            elif op == "Unsqueeze" and kind == "ncw" and attrs == {"axes": [2]}:
                next_kind = "nchw"
            elif op == "Transpose" and kind in ("ncw", "nwc") and attrs == {"perm": [0, 2, 1]}:
                next_kind = "nwc" if kind == "ncw" else "ncw"
            elif op == "Reshape" and task == "cls" and kind == "nchw" and ins[1] in shape_names:
                next_kind = "nc"
            else:
                raise ValueError(f"unsupported view {op} {kind} {attrs}")
            # Keep a separate view name while resolving storage to its producer.
            layouts[out] = next_kind
            emit("View", ins[:1], out, {"kind": next_kind})
            continue
        if op == "BatchNormalization":
            if kind not in ("nchw", "ncw") or attrs.get("training_mode", 0):
                raise ValueError("unsupported BatchNormalization")
            scale, bias, mean, var = [values[x] for x in ins[1:]]
            alpha = scale / np.sqrt(var + np.float32(attrs.get("epsilon", 1e-5)))
            beta = bias - mean * alpha
            an, bn, tmp = out + "/alpha", out + "/beta", out + "/scaled"
            values[an] = alpha.reshape(1, -1, 1, 1).astype(np.float32)
            values[bn] = beta.reshape(1, -1, 1, 1).astype(np.float32)
            emit("Mul", [ins[0], an], tmp)
            emit("Add", [tmp, bn], out)
        elif op == "MatMul":
            if kind not in ("nwc", "nc") or ins[1] not in values or values[ins[1]].ndim != 2:
                raise ValueError("only constant tail MatMul supported")
            wn = out + "/conv-weight"
            weight = values[ins[1]]
            values[wn] = np.ascontiguousarray(weight.T[:, :, None, None])
            emit("Conv", [ins[0], wn], out, {"kernel_shape": [1, 1], "strides": [1, 1],
                "pads": [0, 0, 0, 0], "dilations": [1, 1], "group": 1})
        elif op in ("Add", "Mul", "Div"):
            emit(op, [ins[0], channel_constant(ins[1]) if ins[1] in values else ins[1]], out)
        elif op == "Softmax":
            if (kind, attrs.get("axis")) not in (("nwc", 2), ("nc", 1)):
                raise ValueError("only last-axis Softmax supported")
            emit("Softmax", ins, out)
        elif op in ("Conv", "Relu", "HardSigmoid", "Erf", "ReduceMean", "GlobalAveragePool", "AveragePool"):
            if op in ("Conv", "AveragePool", "GlobalAveragePool", "ReduceMean") and kind != "nchw":
                raise ValueError(f"unsupported {op} input view {kind}")
            emit(op, ins, out, attrs)
        else:
            raise ValueError(f"unsupported operator: {op}")
        layouts[out] = kind
    output.mkdir(parents=True, exist_ok=True)
    (output / "weights.bin").write_bytes(data)
    manifest = {"format": "LWVK-" + task.upper(), "format_version": 0,
        "source_sha256": HASHES[task], "weights_file": "weights.bin", "weights_bytes": len(data),
        "weights_sha256": hashlib.sha256(data).hexdigest(), "weights_fnv1a64": fnv(data),
        "input": input_id, "output": tensor(model.graph.output[0].name), "tensors": tensors, "nodes": nodes}
    if task == "rec":
        raw = dictionary.read_bytes()
        if hashlib.sha256(raw).hexdigest() != DICT_HASH:
            raise ValueError("dictionary SHA-256 mismatch")
        # Split LF only: preserve the trained empty label and Unicode separators.
        words = raw.decode("utf-8").split("\n")
        if words[-1] == "":
            words.pop()
        if len(words) != 6904:
            raise ValueError("unexpected dictionary contents")
        (output / "dictionary.txt").write_bytes(raw)
        manifest.update(dictionary_file="dictionary.txt", dictionary_fnv1a64=fnv(raw), classes=6906)
    (output / "model.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Exported {task.upper()}: {len(nodes)} nodes, {len(tensors)} tensors, {len(data)} bytes")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--task", choices=HASHES, required=True)
    p.add_argument("--dictionary", type=Path)
    a = p.parse_args()
    export(a.input, a.output, a.task, a.dictionary)
