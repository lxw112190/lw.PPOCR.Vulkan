// Apache-2.0. C++ adaptation of SimdPaddleOCR's bounded OnnxProtoReader and
// Identity/metadata normalization. Reference commit d94a79ec56ec0d9bec1cb7c45f91dc1081161080.
// Modified: strict resource budgets, FP32 NHWC lowering, recognized LN/attention patterns.
#include "onnx_import.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
namespace lwvk {
namespace {
using J = nlohmann::json;
[[noreturn]] void bad(const std::string& reason) {
    throw std::runtime_error("ONNX: " + reason);
}
struct Reader {
    // 每个 Reader 仅能访问自己的消息区间；未知字段跳过也必须遵守同样的边界。
    // 这是固定 OCR 模型的受限解析器，不支持任意 ONNX 或 external tensor data。
    const uint8_t *p, *end;
    uint64_t var() {
        uint64_t v = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (p == end)
                bad("truncated varint");
            uint8_t b = *p++;
            if (i == 9 && b > 1)
                bad("varint overflow");
            v |= uint64_t(b & 127) << (i * 7);
            if (!(b & 128))
                return v;
        }
        bad("invalid varint");
    }
    uint32_t fixed() {
        if (end - p < 4)
            bad("truncated fixed32");
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    Reader child() {
        auto n = var();
        if (n > uint64_t(end - p))
            bad("length exceeds message");
        Reader r{p, p + n};
        p += n;
        return r;
    }
    std::string str() {
        auto r = child();
        if (r.end - r.p > 4096)
            bad("string exceeds 4096 bytes");
        return {reinterpret_cast<const char*>(r.p), size_t(r.end - r.p)};
    }
    bool tag(unsigned& f, unsigned& w) {
        if (p == end)
            return false;
        auto t = var();
        if (t > UINT32_MAX || !(t >> 3))
            bad("invalid tag");
        f = unsigned(t >> 3);
        w = unsigned(t & 7);
        return true;
    }
    void skip(unsigned w) {
        if (w == 0) {
            var();
            return;
        }
        if (w == 2) {
            child();
            return;
        }
        unsigned n = w == 1 ? 8 : w == 5 ? 4 : 0;
        if (!n || end - p < n)
            bad("unsupported/truncated wire field");
        p += n;
    }
};
struct Value {
    std::vector<int64_t> shape;
    std::vector<uint8_t> data;
    int type = 0;
};
struct Op {
    std::string op, out;
    std::vector<std::string> in;
    J attrs = J::object();
};
Value tensor(Reader r, std::string& name) {
    Value v;
    unsigned f, w;
    std::vector<float> floats;
    std::vector<int64_t> ints;
    while (r.tag(f, w)) {
        if (f == 1 && (w == 0 || w == 2)) {
            auto add = [&](Reader& c) {
                if (v.shape.size() >= 8)
                    bad("tensor rank exceeds eight");
                v.shape.push_back(int64_t(c.var()));
            };
            if (w == 0)
                add(r);
            else {
                auto c = r.child();
                while (c.p != c.end)
                    add(c);
            }
        } else if (f == 2 && w == 0)
            v.type = int(r.var());
        else if (f == 8 && w == 2)
            name = r.str();
        else if (f == 9 && w == 2) {
            auto c = r.child();
            if (!v.data.empty())
                bad("duplicate raw_data");
            v.data.assign(c.p, c.end);
        } else if (f == 4 && (w == 5 || w == 2)) {
            auto read = [&](Reader& c) {
                if (floats.size() >= 64000000)
                    bad("float_data budget");
                uint32_t b = c.fixed();
                float x;
                std::memcpy(&x, &b, 4);
                floats.push_back(x);
            };
            if (w == 5)
                read(r);
            else {
                auto c = r.child();
                while (c.p != c.end)
                    read(c);
            }
        } else if ((f == 5 || f == 7) && (w == 0 || w == 2)) {
            auto add = [&](Reader& c) {
                if (ints.size() >= 1048576)
                    bad("integer metadata budget");
                ints.push_back(int64_t(c.var()));
            };
            if (w == 0)
                add(r);
            else {
                auto c = r.child();
                while (c.p != c.end)
                    add(c);
            }
        } else if (f == 13 || f == 14)
            bad("external tensor data is not supported");
        else
            r.skip(w);
    }
    uint64_t count = 1;
    for (auto x : v.shape) {
        if (x < 0 || x > 10000000 || (x && count > 64000000 / uint64_t(x)))
            bad("tensor size budget");
        count *= uint64_t(x);
    }
    if (v.type != 1 && v.type != 7 && v.type != 6)
        bad("only FLOAT/INT64/INT32 constants supported");
    const uint64_t size = count * (v.type == 7 ? 8 : 4);
    if (size > 256ull * 1024 * 1024)
        bad("tensor byte budget");
    if (v.data.empty() && size) {
        v.data.resize(size_t(size));
        if (v.type == 1) {
            if (floats.size() != count)
                bad("float_data count mismatch");
            std::memcpy(v.data.data(), floats.data(), size_t(size));
        } else {
            if (ints.size() != count)
                bad("integer data count mismatch");
            for (size_t i = 0; i < ints.size(); ++i) {
                if (v.type == 7)
                    std::memcpy(v.data.data() + 8 * i, &ints[i], 8);
                else {
                    int32_t x = int32_t(ints[i]);
                    std::memcpy(v.data.data() + 4 * i, &x, 4);
                }
            }
        }
    }
    if (v.data.size() != size)
        bad("raw_data size mismatch");
    if (v.type == 1)
        for (size_t i = 0; i < v.data.size(); i += 4) {
            float x;
            std::memcpy(&x, v.data.data() + i, 4);
            if (!std::isfinite(x))
                bad("nonfinite constant");
        }
    return v;
}
std::pair<std::string, J> attribute(Reader r) {
    std::string name;
    J value;
    unsigned f, w;
    int type = 0;
    while (r.tag(f, w)) {
        if (f == 1 && w == 2)
            name = r.str();
        else if (f == 20 && w == 0)
            type = int(r.var());
        else if (f == 2 && w == 5) {
            auto b = r.fixed();
            float x;
            std::memcpy(&x, &b, 4);
            if (!std::isfinite(x))
                bad("nonfinite attribute");
            value = x;
        } else if (f == 3 && w == 0)
            value = int64_t(r.var());
        else if (f == 4 && w == 2)
            value = r.str();
        else if (f == 8 && (w == 0 || w == 2)) {
            if (value.is_null())
                value = J::array();
            auto add = [&](Reader& c) {
                if (!value.is_array() || value.size() >= 32)
                    bad("attribute array budget");
                value.push_back(int64_t(c.var()));
            };
            if (w == 0)
                add(r);
            else {
                auto c = r.child();
                while (c.p != c.end)
                    add(c);
            }
        } else if (f == 5 || f == 6 || f == 10 || f == 11)
            bad("tensor/graph attributes unsupported");
        else
            r.skip(w);
    }
    if (name.empty() || value.is_null() || (type && type != 1 && type != 2 && type != 3 && type != 7))
        bad("unsupported attribute");
    return {name, value};
}
Op node(Reader r) {
    Op o;
    unsigned f, w;
    while (r.tag(f, w)) {
        if (f == 1 && w == 2) {
            if (o.in.size() >= 8)
                bad("node input budget");
            o.in.push_back(r.str());
        } else if (f == 2 && w == 2) {
            if (!o.out.empty())
                bad("multiple output nodes unsupported");
            o.out = r.str();
        } else if (f == 4 && w == 2)
            o.op = r.str();
        else if (f == 5 && w == 2) {
            auto a = attribute(r.child());
            if (o.attrs.contains(a.first))
                bad("duplicate attribute");
            o.attrs[a.first] = a.second;
        } else if (f == 7 && w == 2) {
            auto domain = r.str();
            if (!domain.empty() && domain != "ai.onnx")
                bad("custom operator domain");
        } else
            r.skip(w);
    }
    if (o.op.empty() || o.out.empty())
        bad("missing node op/output");
    // Validate before pattern matching accesses inputs by index. Unknown operators
    // are rejected rather than trusted merely because their protobuf is valid.
    static const std::map<std::string, std::pair<size_t, size_t>> arities = {{"Identity", {1, 1}},
                                                                             {"Shape", {1, 1}},
                                                                             {"Slice", {1, 5}},
                                                                             {"Concat", {1, 8}},
                                                                             {"Squeeze", {1, 1}},
                                                                             {"Unsqueeze", {1, 1}},
                                                                             {"Transpose", {1, 1}},
                                                                             {"Reshape", {2, 2}},
                                                                             {"Add", {2, 2}},
                                                                             {"Sub", {2, 2}},
                                                                             {"Mul", {2, 2}},
                                                                             {"Div", {2, 2}},
                                                                             {"Pow", {2, 2}},
                                                                             {"MatMul", {2, 2}},
                                                                             {"ReduceMean", {1, 1}},
                                                                             {"Sqrt", {1, 1}},
                                                                             {"BatchNormalization", {5, 5}},
                                                                             {"Conv", {2, 3}},
                                                                             {"ConvTranspose", {2, 3}},
                                                                             {"Resize", {3, 4}},
                                                                             {"Erf", {1, 1}},
                                                                             {"HardSigmoid", {1, 1}},
                                                                             {"Relu", {1, 1}},
                                                                             {"GlobalAveragePool", {1, 1}},
                                                                             {"MaxPool", {1, 1}},
                                                                             {"AveragePool", {1, 1}},
                                                                             {"Sigmoid", {1, 1}},
                                                                             {"Softmax", {1, 1}}};
    auto arity = arities.find(o.op);
    if (arity == arities.end())
        bad("unsupported operator " + o.op);
    if (o.in.size() < arity->second.first || o.in.size() > arity->second.second)
        bad("invalid input arity for " + o.op);
    return o;
}
std::string value_name(Reader r) {
    unsigned f, w;
    std::string s;
    while (r.tag(f, w)) {
        if (f == 1 && w == 2)
            s = r.str();
        else
            r.skip(w);
    }
    if (s.empty())
        bad("missing value name");
    return s;
}
std::vector<int64_t> iv(const Value& v) {
    if (v.type != 7 && v.type != 6)
        bad("expected integer metadata");
    std::vector<int64_t> out;
    for (size_t i = 0; i < v.data.size(); i += (v.type == 7 ? 8 : 4)) {
        int64_t x = 0;
        if (v.type == 7)
            std::memcpy(&x, v.data.data() + i, 8);
        else {
            int32_t y;
            std::memcpy(&y, v.data.data() + i, 4);
            x = y;
        }
        out.push_back(x);
    }
    return out;
}
std::vector<float> fv(const Value& v) {
    if (v.type != 1)
        bad("expected FLOAT data");
    std::vector<float> out(v.data.size() / 4);
    std::memcpy(out.data(), v.data.data(), v.data.size());
    return out;
}
Value floats(const std::vector<float>& a, std::vector<int64_t> shape) {
    Value v;
    v.type = 1;
    v.shape = std::move(shape);
    v.data.resize(a.size() * 4);
    std::memcpy(v.data.data(), a.data(), v.data.size());
    return v;
}
bool eq(const Op& o, const char* key, std::initializer_list<int64_t> xs) {
    return o.attrs.contains(key) && o.attrs.at(key) == std::vector<int64_t>(xs);
}
} // namespace
J import_onnx(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& weights) {
    if (bytes.empty() || bytes.size() > 256ull * 1024 * 1024)
        bad("file budget 256 MiB");
    Reader root{bytes.data(), bytes.data() + bytes.size()};
    Reader g{nullptr, nullptr};
    unsigned f, w;
    unsigned graphs = 0;
    while (root.tag(f, w)) {
        if (f == 7 && w == 2) {
            g = root.child();
            ++graphs;
        } else if (f == 8 && w == 2) {
            auto c = root.child();
            std::string domain;
            uint64_t version = 0;
            while (c.tag(f, w)) {
                if (f == 1 && w == 2)
                    domain = c.str();
                else if (f == 2 && w == 0)
                    version = c.var();
                else
                    c.skip(w);
            }
            if ((!domain.empty() && domain != "ai.onnx") || version < 7 || version > 21)
                bad("unsupported opset");
        } else
            root.skip(w);
    }
    if (graphs != 1)
        bad("exactly one graph required");
    std::map<std::string, Value> values;
    std::vector<Op> original;
    std::vector<std::string> inputs, outputs;
    while (g.tag(f, w)) {
        if (f == 1 && w == 2) {
            if (original.size() >= 2048)
                bad("node budget");
            original.push_back(node(g.child()));
        } else if (f == 5 && w == 2) {
            std::string name;
            auto v = tensor(g.child(), name);
            if (name.empty() || values.size() >= 4096 || !values.emplace(name, std::move(v)).second)
                bad("duplicate/too many initializers");
        } else if ((f == 11 || f == 12) && w == 2) {
            auto s = value_name(g.child());
            (f == 11 ? inputs : outputs).push_back(s);
            if (inputs.size() > 32 || outputs.size() > 8)
                bad("I/O budget");
        } else
            g.skip(w);
    }
    inputs.erase(std::remove_if(inputs.begin(), inputs.end(), [&](auto& s) { return values.count(s) != 0; }),
                 inputs.end());
    if (inputs.size() != 1 || outputs.size() != 1 || original.empty())
        bad("one input/output required");
    std::map<std::string, std::string> aliases;
    std::set<std::string> available{inputs[0]};
    for (auto& v : values)
        available.insert(v.first);
    auto resolve = [&](std::string s) {
        for (size_t hops = 0; aliases.count(s); ++hops) {
            if (hops >= 2048)
                bad("alias cycle");
            s = aliases.at(s);
        }
        return s;
    };
    std::vector<Op> ops;
    for (auto o : original) {
        for (auto& s : o.in) {
            s = resolve(s);
            if (!s.empty() && !available.count(s))
                bad("non-topological/unknown input " + s);
        }
        if (!available.insert(o.out).second)
            bad("duplicate producer");
        if (o.op == "Identity") {
            if (o.in.size() != 1)
                bad("Identity arity");
            aliases[o.out] = o.in[0];
        } else
            ops.push_back(std::move(o));
    }
    const std::string out = resolve(outputs[0]);
    if (ops.empty() || !available.count(out))
        bad("missing executable graph/output");
    std::string task;
    uint32_t classes = 0;
    if (ops.back().op == "Sigmoid")
        task = "det";
    else if (ops.back().op == "Softmax") {
        for (auto it = ops.rbegin(); it != ops.rend(); ++it)
            if (it->op == "MatMul" && values.count(it->in.at(1))) {
                auto& v = values.at(it->in[1]);
                if (v.shape.size() != 2)
                    bad("output MatMul rank");
                classes = uint32_t(v.shape[1]);
                break;
            }
        task = classes == 2 ? "cls" : "rec";
        if (task == "rec" && classes != 6906 && classes != 18710)
            bad("unsupported CTC vocabulary");
    } else
        bad("expected OCR probability output");
    std::map<std::string, std::string> layouts{{inputs[0], "nchw"}};
    std::map<std::string, uint32_t> ids;
    J tensors = J::array(), nodes = J::array();
    weights.clear();
    auto id = [&](const std::string& s) {
        auto found = ids.find(s);
        if (found != ids.end())
            return found->second;
        uint32_t i = uint32_t(tensors.size());
        if (i >= 4096)
            bad("lowered tensor budget");
        J t = {{"name", s}};
        if (values.count(s)) {
            auto& v = values.at(s);
            if (v.type != 1 || v.shape.size() > 4)
                bad("execution constant type/rank");
            t["shape"] = v.shape;
            t["offset"] = weights.size();
            t["count"] = v.data.size() / 4;
            if (weights.size() + v.data.size() > 256ull * 1024 * 1024)
                bad("weights budget");
            weights.insert(weights.end(), v.data.begin(), v.data.end());
        }
        ids[s] = i;
        tensors.push_back(t);
        return i;
    };
    auto emit = [&](std::string op, std::vector<std::string> in, std::string o, J a = J::object()) {
        J ins = J::array();
        for (auto& s : in)
            ins.push_back(id(s));
        nodes.push_back({{"op", op}, {"inputs", ins}, {"output", id(o)}, {"attrs", a}});
    };
    const uint32_t input = id(inputs[0]);
    auto scalar = [&](const std::string& name) {
        auto v = fv(values.at(name));
        if (v.size() != 1)
            bad("expected scalar " + name);
        return v[0];
    };
    for (size_t i = 0; i < ops.size(); ++i) {
        auto n = ops[i];
        if (n.op == "Shape" || n.op == "Slice" || (n.op == "Concat" && n.attrs.value("axis", -1) == 0) ||
            ((n.op == "Squeeze" || n.op == "Unsqueeze") && !layouts.count(n.in.at(0))))
            continue;
        if (!layouts.count(n.in.at(0)))
            bad("unknown execution layout at " + n.op);
        auto kind = layouts.at(n.in[0]);
        // Same nine-node LayerNorm recognition as upstream, with all edges checked.
        if (n.op == "ReduceMean" && eq(n, "axes", {-1}) && kind == "nwc") {
            if (i + 8 >= ops.size())
                bad("truncated LayerNorm");
            const char* seq[] = {"ReduceMean", "Sub", "Pow", "ReduceMean", "Add", "Sqrt", "Div", "Mul", "Add"};
            for (size_t k = 0; k < 9; ++k)
                if (ops[i + k].op != seq[k])
                    bad("unsupported LayerNorm chain");
            auto& s = ops[i + 1];
            auto& p = ops[i + 2];
            auto& rm = ops[i + 3];
            auto& e = ops[i + 4];
            auto& sq = ops[i + 5];
            auto& d = ops[i + 6];
            auto& m = ops[i + 7];
            auto& b = ops[i + 8];
            if (s.in != std::vector<std::string>{n.in[0], n.out} || p.in[0] != s.out || scalar(p.in[1]) != 2 ||
                rm.in[0] != p.out || !eq(rm, "axes", {-1}) || e.in[0] != rm.out || sq.in[0] != e.out ||
                d.in != std::vector<std::string>{s.out, sq.out} || m.in[0] != d.out || b.in[0] != m.out)
                bad("LayerNorm edge mismatch");
            const float eps = scalar(e.in[1]);
            if (eps <= 0 || eps > .01f)
                bad("LayerNorm epsilon");
            for (auto name : {m.in[1], b.in[1]}) {
                auto& v = values.at(name);
                if (v.type != 1 || v.shape.size() != 1)
                    bad("LayerNorm affine rank");
            }
            emit("LayerNorm", {n.in[0], m.in[1], b.in[1]}, b.out, {{"epsilon", eps}});
            layouts[b.out] = kind;
            i += 8;
            continue;
        }
        if (n.op == "Reshape" && values.count(n.in.at(1)) && iv(values.at(n.in[1])).size() == 5) {
            // Exact QKV attention block; never reinterpret arbitrary rank-five views.
            const auto sh = iv(values.at(n.in[1]));
            if (sh[0] != 0 || sh[1] != -1 || sh[2] != 3 || sh[3] < 1 || sh[3] > 32 || sh[4] < 1 || sh[4] > 32 ||
                kind != "nwc" || i + 16 >= ops.size())
                bad("unsupported QKV reshape");
            const char* seq[] = {"Reshape", "Transpose", "Slice",   "Squeeze",   "Mul",     "Shape",
                                 "Reshape", "Slice",     "Squeeze", "Slice",     "Squeeze", "Transpose",
                                 "MatMul",  "Softmax",   "MatMul",  "Transpose", "Reshape"};
            for (size_t k = 0; k < 17; ++k)
                if (ops[i + k].op != seq[k])
                    bad("unsupported attention chain");
            auto& tr = ops[i + 1];
            auto& q = ops[i + 2];
            auto& qs = ops[i + 3];
            auto& mul = ops[i + 4];
            auto& shape = ops[i + 5];
            auto& qview = ops[i + 6];
            auto& k = ops[i + 7];
            auto& ks = ops[i + 8];
            auto& v = ops[i + 9];
            auto& vs = ops[i + 10];
            auto& kt = ops[i + 11];
            auto& mm = ops[i + 12];
            auto& sm = ops[i + 13];
            auto& mm2 = ops[i + 14];
            auto& ot = ops[i + 15];
            auto& ov = ops[i + 16];
            auto slice = [&](const Op& o, int role) {
                return o.in.size() == 5 && o.in[0] == tr.out && iv(values.at(o.in[1])) == std::vector<int64_t>{role} &&
                       iv(values.at(o.in[2])) == std::vector<int64_t>{role + 1} &&
                       iv(values.at(o.in[3])) == std::vector<int64_t>{0} &&
                       iv(values.at(o.in[4])) == std::vector<int64_t>{1};
            };
            if (tr.in[0] != n.out || !eq(tr, "perm", {2, 0, 3, 1, 4}) || !slice(q, 0) || !slice(k, 1) || !slice(v, 2) ||
                qs.in[0] != q.out || ks.in[0] != k.out || vs.in[0] != v.out || !eq(qs, "axes", {0}) ||
                !eq(ks, "axes", {0}) || !eq(vs, "axes", {0}) || mul.in[0] != qs.out || shape.in[0] != qs.out ||
                qview.in != std::vector<std::string>{mul.out, shape.out} || kt.in[0] != ks.out ||
                !eq(kt, "perm", {0, 1, 3, 2}) || mm.in != std::vector<std::string>{qview.out, kt.out} ||
                sm.in[0] != mm.out || sm.attrs.value("axis", -1) != 3 ||
                mm2.in != std::vector<std::string>{sm.out, vs.out} || ot.in[0] != mm2.out ||
                !eq(ot, "perm", {0, 2, 1, 3}) || ov.in[0] != ot.out ||
                iv(values.at(ov.in[1])) != std::vector<int64_t>{0, -1, sh[3] * sh[4]})
                bad("attention edge/shape mismatch");
            emit("Attention", {n.in[0]}, ov.out, {{"heads", sh[3]}, {"dim", sh[4]}, {"scale", scalar(mul.in[1])}});
            layouts[ov.out] = "nwc";
            i += 16;
            continue;
        }
        if (n.op == "Reshape" || n.op == "Squeeze" || n.op == "Unsqueeze" || n.op == "Transpose") {
            std::string next;
            if (n.op == "Squeeze" && kind == "nchw" && eq(n, "axes", {2}))
                next = "ncw";
            else if (n.op == "Unsqueeze" && kind == "ncw" && eq(n, "axes", {2}))
                next = "nchw";
            else if (n.op == "Transpose" && (kind == "ncw" || kind == "nwc") && eq(n, "perm", {0, 2, 1}))
                next = kind == "ncw" ? "nwc" : "ncw";
            else if (n.op == "Reshape" && kind == "nchw" && !values.count(n.in[1]))
                next = task == "cls" ? "nc" : "ncw";
            else if (n.op == "Reshape" && kind == "nwc" && !values.count(n.in[1]))
                next = "n1wc";
            else if (n.op == "Transpose" && kind == "n1wc" && eq(n, "perm", {0, 3, 1, 2}))
                next = "nchw";
            else
                bad("unsupported canonical view " + n.op + " " + kind);
            emit("View", {n.in[0]}, n.out, {{"kind", next == "n1wc" ? "nwc" : next}});
            layouts[n.out] = next;
            continue;
        }
        if (n.op == "BatchNormalization") {
            if (n.in.size() != 5 || n.attrs.value("training_mode", 0))
                bad("training BN");
            auto scale = fv(values.at(n.in[1])), bias = fv(values.at(n.in[2])), mean = fv(values.at(n.in[3])),
                 var = fv(values.at(n.in[4]));
            if (scale.size() != bias.size() || scale.size() != mean.size() || scale.size() != var.size())
                bad("BN size");
            float eps = n.attrs.value("epsilon", 1e-5f);
            if (!std::isfinite(eps) || eps <= 0)
                bad("BN epsilon");
            for (size_t j = 0; j < scale.size(); ++j) {
                if (!(var[j] + eps > 0))
                    bad("BN variance");
                scale[j] /= std::sqrt(var[j] + eps);
                bias[j] -= mean[j] * scale[j];
                if (!std::isfinite(scale[j]) || !std::isfinite(bias[j]))
                    bad("nonfinite BN lowering");
            }
            std::string a = n.out + "/alpha", b = n.out + "/beta", t = n.out + "/scaled";
            values[a] = floats(scale, {1, int64_t(scale.size()), 1, 1});
            values[b] = floats(bias, {1, int64_t(bias.size()), 1, 1});
            emit("Mul", {n.in[0], a}, t);
            emit("Add", {t, b}, n.out);
        } else if (n.op == "MatMul") {
            if (n.in.size() != 2 || !values.count(n.in[1]) || (kind != "nwc" && kind != "nc"))
                bad("only constant canonical MatMul supported");
            auto& v = values.at(n.in[1]);
            if (v.shape.size() != 2)
                bad("MatMul rank");
            auto src = fv(v);
            const size_t K = size_t(v.shape[0]), N = size_t(v.shape[1]);
            std::vector<float> dst(src.size());
            for (size_t k = 0; k < K; ++k)
                for (size_t c = 0; c < N; ++c)
                    dst[c * K + k] = src[k * N + c];
            std::string name = n.out + "/conv-weight";
            values[name] = floats(dst, {int64_t(N), int64_t(K), 1, 1});
            emit("Conv", {n.in[0], name}, n.out,
                 {{"kernel_shape", {1, 1}},
                  {"strides", {1, 1}},
                  {"pads", {0, 0, 0, 0}},
                  {"dilations", {1, 1}},
                  {"group", 1}});
        } else {
            if (n.op == "Add" || n.op == "Mul" || n.op == "Div")
                if (values.count(n.in[1])) {
                    auto& v = values.at(n.in[1]);
                    if (v.shape.size() == 1 && v.data.size() > 4) {
                        std::string name = n.in[1] + "/channel";
                        auto copy = v;
                        copy.shape = {1, v.shape[0], 1, 1};
                        values[name] = std::move(copy);
                        n.in[1] = name;
                    }
                }
            if (n.op == "Conv" && n.attrs.value("auto_pad", std::string()) == "SAME_UPPER") {
                if (!eq(n, "kernel_shape", {2, 2}) || !eq(n, "strides", {1, 1}))
                    bad("unsupported SAME_UPPER convolution");
                n.attrs["auto_pad"] = "NOTSET";
                n.attrs["pads"] = {0, 0, 1, 1};
            }
            if (n.op == "Resize") {
                auto scales = fv(values.at(n.in.at(2)));
                if (scales.size() != 4 || scales[0] != 1 || scales[1] != 1 || scales[2] != std::floor(scales[2]) ||
                    scales[3] != std::floor(scales[3]))
                    bad("integer spatial Resize required");
                n.attrs["factors"] = {int(scales[2]), int(scales[3])};
                n.in.resize(1);
            }
            if (n.op == "Softmax" && !((kind == "nwc" && n.attrs.value("axis", -1) == 2) ||
                                       (kind == "nc" && n.attrs.value("axis", -1) == 1)))
                bad("only canonical output Softmax supported");
            const std::set<std::string> supported{
                "Conv",       "ConvTranspose",     "Add",     "Mul",    "Div",    "Erf",     "HardSigmoid", "Relu",
                "ReduceMean", "GlobalAveragePool", "MaxPool", "Resize", "Concat", "Sigmoid", "AveragePool", "Softmax"};
            if (!supported.count(n.op))
                bad("unsupported operator " + n.op);
            emit(n.op, n.in, n.out, n.attrs);
        }
        layouts[n.out] = kind;
    }
    return {{"format", "LWVK-" + std::string(task == "det"   ? "DET"
                                             : task == "cls" ? "CLS"
                                                             : "REC")},
            {"format_version", 0},
            {"input", input},
            {"output", id(out)},
            {"tensors", tensors},
            {"nodes", nodes},
            {"classes", classes}};
}
} // namespace lwvk
