#include "graph.hpp"
#include "onnx_import.hpp"
#include "workspace_planner.hpp"
#include "graph_optimizer.hpp"
#include "ocr_host.hpp"
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>

namespace lwvk {
namespace {
constexpr const char* det_sha = "193bab7a04fca699a6c82e6abb5b81bdb28177f0abd4062552b04908dafb19f8";
constexpr const char* cls_sha = "dd8b2b61983d76ab230a58da9e0e0e84956b71c3877f2ce6e438fe22d74d2cf2";
constexpr const char* rec_sha = "9ef676d6ed3c88256a2d92c640c44f25b0c40947e111b14b8be8f594091563e6";
std::string fnv(const std::vector<uint8_t>& data) {
    auto value = UINT64_C(14695981039346656037);
    for (auto b : data)
        value = (value ^ b) * UINT64_C(1099511628211);
    return std::to_string(value);
}
std::vector<uint8_t> read_file(const std::filesystem::path& path, uint64_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("cannot open model file: " + path.u8string());
    auto end = file.tellg();
    if (end <= 0 || static_cast<uint64_t>(end) > limit)
        throw std::runtime_error("model file size rejected");
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("truncated model file");
    return bytes;
}
uint64_t elements(const Shape& shape) {
    uint64_t count = 1;
    for (auto value : shape) {
        if (!value || value > 10000000 || count > UINT32_MAX / value)
            throw std::runtime_error("tensor shape exceeds bounded FP32 baseline");
        count *= value;
    }
    return count;
}
uint64_t align(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}
std::vector<uint32_t> ints(const Json& attrs, const char* key, std::vector<uint32_t> fallback) {
    if (!attrs.contains(key))
        return fallback;
    auto v = attrs.at(key).get<std::vector<int64_t>>();
    if (v.size() > 4)
        throw std::runtime_error("too many operator parameters");
    std::vector<uint32_t> out;
    for (auto x : v) {
        if (x < 0 || x > 4096)
            throw std::runtime_error("operator parameter out of range");
        out.push_back(static_cast<uint32_t>(x));
    }
    return out;
}
uint32_t float_bits(float value) {
    if (!std::isfinite(value))
        throw std::runtime_error("non-finite activation parameter");
    uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
bool reference_kernels() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_REFERENCE_KERNELS");
    return length > 0;
#else
    return std::getenv("LWVK_REFERENCE_KERNELS") != nullptr;
#endif
}
bool reference_graph() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_REFERENCE_GRAPH");
    return length > 0;
#else
    return std::getenv("LWVK_REFERENCE_GRAPH") != nullptr;
#endif
}
bool tiled_pointwise_disabled() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_DISABLE_TILED_POINTWISE");
    return length > 0;
#else
    return std::getenv("LWVK_DISABLE_TILED_POINTWISE") != nullptr;
#endif
}
bool tiled_gemm_disabled() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_DISABLE_TILED_GEMM");
    return length > 0;
#else
    return std::getenv("LWVK_DISABLE_TILED_GEMM") != nullptr;
#endif
}
bool vector_depthwise_disabled() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_DISABLE_VECTOR_DEPTHWISE");
    return length > 0;
#else
    return std::getenv("LWVK_DISABLE_VECTOR_DEPTHWISE") != nullptr;
#endif
}
bool gelu_fusion_disabled() {
#ifdef _WIN32
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_DISABLE_GELU_FUSION");
    return length > 0;
#else
    return std::getenv("LWVK_DISABLE_GELU_FUSION") != nullptr;
#endif
}
bool tile64_requested() {
#ifdef _WIN32
    char value[2]{};
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_EXPERIMENTAL_TILE64");
    if (length == 2)
        getenv_s(&length, value, sizeof(value), "LWVK_EXPERIMENTAL_TILE64");
    return value[0] == '1';
#else
    const auto* value = std::getenv("LWVK_EXPERIMENTAL_TILE64");
    return value && std::strcmp(value, "1") == 0;
#endif
}
bool cooperative_det_only_requested() {
#ifdef _WIN32
    char value[2]{};
    size_t length = 0;
    getenv_s(&length, nullptr, 0, "LWVK_EXPERIMENTAL_COOP_DET_ONLY");
    if (length == 2)
        getenv_s(&length, value, sizeof(value), "LWVK_EXPERIMENTAL_COOP_DET_ONLY");
    return value[0] == '1';
#else
    const auto* value = std::getenv("LWVK_EXPERIMENTAL_COOP_DET_ONLY");
    return value && std::strcmp(value, "1") == 0;
#endif
}
// Single-consumer Conv -> channel Mul -> channel Add folding (including BN).
// Shared constants are copied rather than modified. Residual branches and graph
// outputs prevent fusion. Independent FP32 model regression gates the rounding.
void fold_affine_and_relu(Model& model) {
    auto& nodes = model.nodes;
    auto& tensors = model.tensors;
    std::vector<uint32_t> uses(tensors.size());
    for (auto& n : nodes)
        for (auto id : n.inputs)
            ++uses[id];
    ++uses[model.output];
    auto value = [&](uint32_t id, uint64_t k) {
        float v;
        std::memcpy(&v, model.weights.data() + tensors[id].file_offset + k * 4, 4);
        return v;
    };
    auto write = [&](uint32_t id, uint64_t k, float v) {
        std::memcpy(model.weights.data() + tensors[id].file_offset + k * 4, &v, 4);
    };
    auto append = [&](Shape shape, const std::vector<float>& data) {
        if (tensors.size() >= 4096 || model.weights.size() + data.size() * 4 > 256ull * 1024 * 1024)
            throw std::runtime_error("fused constants exceed model budget");
        Tensor t;
        t.constant = true;
        t.shape = shape;
        t.file_offset = model.weights.size();
        t.bytes = data.size() * 4;
        const auto* begin = reinterpret_cast<const uint8_t*>(data.data());
        model.weights.insert(model.weights.end(), begin, begin + t.bytes);
        auto id = uint32_t(tensors.size());
        tensors.push_back(t);
        uses.push_back(0);
        return id;
    };
    std::vector<Node> result;
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node n = nodes[i];
        if (n.op == "Conv" && i + 2 < nodes.size() && n.inputs.size() >= 2 && n.inputs.size() <= 3) {
            const auto& mul = nodes[i + 1];
            const auto& add = nodes[i + 2];
            const auto weight = n.inputs[1];
            const auto shape = tensors[weight].shape;
            const auto channels = shape[0];
            const bool valid_bias = n.inputs.size() == 2 ||
                                    (tensors[n.inputs[2]].constant && elements(tensors[n.inputs[2]].shape) == channels);
            if (tensors[weight].constant && valid_bias && uses[n.output] == 1 && mul.op == "Mul" &&
                mul.inputs.size() == 2 && mul.inputs[0] == n.output && uses[mul.output] == 1 && add.op == "Add" &&
                add.inputs.size() == 2 && add.inputs[0] == mul.output && tensors[mul.inputs[1]].constant &&
                tensors[add.inputs[1]].constant && tensors[mul.inputs[1]].shape == Shape{1, channels, 1, 1} &&
                tensors[add.inputs[1]].shape == Shape{1, channels, 1, 1}) {
                const uint64_t per_channel = elements(shape) / channels;
                std::vector<float> weights(elements(shape)), bias(channels);
                bool finite = true;
                for (uint32_t c = 0; c < channels; ++c) {
                    const float scale = value(mul.inputs[1], c), offset = value(add.inputs[1], c);
                    bias[c] = (n.inputs.size() == 3 ? value(n.inputs[2], c) : 0) * scale + offset;
                    finite &= std::isfinite(bias[c]);
                    for (uint64_t k = 0; k < per_channel; ++k) {
                        auto& v = weights[c * per_channel + k];
                        v = value(weight, c * per_channel + k) * scale;
                        finite &= std::isfinite(v);
                    }
                }
                if (finite) {
                    if (uses[weight] == 1) {
                        for (size_t k = 0; k < weights.size(); ++k)
                            write(weight, k, weights[k]);
                    } else
                        n.inputs[1] = append(shape, weights);
                    if (n.inputs.size() == 3 && uses[n.inputs[2]] == 1) {
                        for (uint32_t c = 0; c < channels; ++c)
                            write(n.inputs[2], c, bias[c]);
                    } else {
                        auto id = append(Shape{1, 1, 1, channels}, bias);
                        if (n.inputs.size() == 3)
                            n.inputs[2] = id;
                        else
                            n.inputs.push_back(id);
                    }
                    n.output = add.output;
                    i += 2;
                }
            }
        }
        if (n.op == "Conv" && i + 1 < nodes.size() && uses[n.output] == 1 && nodes[i + 1].op == "Relu" &&
            nodes[i + 1].inputs == std::vector<uint32_t>{n.output}) {
            n.output = nodes[++i].output;
            n.attrs["fused_relu"] = true;
        }
        result.push_back(std::move(n));
    }
    nodes = std::move(result);
    // Fuse only a single-consumer sigmoid temporary, preserving shared branches
    // and graph outputs. Keep alpha/beta exactly as imported (no fixed 1/6).
    uses.assign(tensors.size(), 0);
    for (const auto& node : nodes)
        for (auto id : node.inputs)
            ++uses[id];
    ++uses[model.output];
    result.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto n = nodes[i];
        if ((n.op == "HardSigmoid" || n.op == "Sigmoid") && n.inputs.size() == 1 && uses[n.output] == 1 &&
            i + 1 < nodes.size()) {
            const auto& mul = nodes[i + 1];
            if (mul.op == "Mul" && mul.inputs.size() == 2 &&
                ((mul.inputs[0] == n.output && mul.inputs[1] == n.inputs[0]) ||
                 (mul.inputs[1] == n.output && mul.inputs[0] == n.inputs[0]))) {
                n.op = n.op == "HardSigmoid" ? "HardSwish" : "SiLU";
                n.output = mul.output;
                ++i;
            }
        }
        result.push_back(std::move(n));
    }
    nodes = std::move(result);
}
uint32_t mode(const Shape& operand, const Shape& result) {
    if (operand == result)
        return 0;
    if (elements(operand) == 1)
        return 2;
    if (operand == Shape{1, result[1], 1, 1})
        return 1;
    throw std::runtime_error("unsupported DET broadcast layout");
}
void barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to, VkAccessFlags read,
             VkAccessFlags write) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = read;
    mb.dstAccessMask = write;
    vkCmdPipelineBarrier(cmd, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
}
} // namespace
Model::Model(const std::filesystem::path& path) {
    const bool onnx = path.extension() == ".onnx";
    auto bytes = read_file(path, onnx ? 256ull * 1024 * 1024 : 2 * 1024 * 1024);
    auto j = onnx ? import_onnx(bytes, weights) : Json::parse(bytes);
    if (onnx)
        task = j.at("format") == "LWVK-DET" ? "det" : j.at("format") == "LWVK-CLS" ? "cls" : "rec";
    else if (j.at("format") == "LWVK-DET" && j.at("source_sha256") == det_sha)
        task = "det";
    else if (j.at("format") == "LWVK-CLS" && j.at("source_sha256") == cls_sha)
        task = "cls";
    else if (j.at("format") == "LWVK-REC" && j.at("source_sha256") == rec_sha)
        task = "rec";
    if (task.empty() || j.at("format_version") != 0)
        throw std::runtime_error("unsupported model format/version/source (pinned Tiny only)");
    if (!onnx) {
        // A manifest cannot escape its directory through a weights path.
        if (j.at("weights_file") != "weights.bin")
            throw std::runtime_error("unsupported weights filename");
        weights = read_file(path.parent_path() / "weights.bin", 8 * 1024 * 1024);
        if (weights.size() != j.at("weights_bytes").get<uint64_t>())
            throw std::runtime_error("weights size mismatch");
        if (fnv(weights) != j.at("weights_fnv1a64").get<std::string>())
            throw std::runtime_error("weights checksum mismatch");
    }
    if (task == "rec") {
        if ((!onnx && j.at("dictionary_file") != "dictionary.txt") ||
            (j.at("classes") != 6906 && j.at("classes") != 18710))
            throw std::runtime_error("unsupported dictionary manifest");
        auto raw = read_file(path.parent_path() / "dictionary.txt", 256 * 1024);
        if (!onnx && fnv(raw) != j.at("dictionary_fnv1a64").get<std::string>())
            throw std::runtime_error("dictionary checksum mismatch");
        dictionary.emplace_back();
        size_t start = 0;
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i] == '\n') {
                size_t end = i;
                if (end > start && raw[end - 1] == '\r')
                    --end;
                dictionary.emplace_back(reinterpret_cast<const char*>(raw.data() + start), end - start);
                start = i + 1;
            }
        if (start < raw.size())
            dictionary.emplace_back(reinterpret_cast<const char*>(raw.data() + start), raw.size() - start);
        dictionary.emplace_back(" ");
        if (dictionary.size() != j.at("classes").get<size_t>())
            throw std::runtime_error("dictionary class count mismatch");
    }
    const auto& ts = j.at("tensors");
    const auto& ns = j.at("nodes");
    if (!ts.is_array() || ts.empty() || ts.size() > 4096 || !ns.is_array() || ns.empty() || ns.size() > 1024)
        throw std::runtime_error("invalid model graph size");
    for (const auto& item : ts) {
        Tensor t;
        if (item.contains("offset")) {
            t.constant = true;
            auto dims = item.at("shape").get<std::vector<int64_t>>();
            if (dims.size() > 4)
                throw std::runtime_error("constant rank exceeds four");
            for (size_t k = 0; k < dims.size(); ++k) {
                if (dims[k] <= 0 || dims[k] > 10000000)
                    throw std::runtime_error("invalid constant dimensions");
                t.shape[4 - dims.size() + k] = static_cast<uint32_t>(dims[k]);
            }
            t.bytes = elements(t.shape) * 4;
            if (elements(t.shape) != item.at("count").get<uint64_t>())
                throw std::runtime_error("constant count mismatch");
            t.file_offset = item.at("offset").get<uint64_t>();
            if (t.file_offset % 4 || t.file_offset > weights.size() || t.bytes > weights.size() - t.file_offset)
                throw std::runtime_error("constant weights out of bounds");
        }
        tensors.push_back(t);
    }
    input = j.at("input").get<uint32_t>();
    output = j.at("output").get<uint32_t>();
    if (input >= tensors.size() || output >= tensors.size() || tensors[input].constant)
        throw std::runtime_error("invalid model I/O");
    std::vector<bool> available(tensors.size());
    for (size_t i = 0; i < tensors.size(); ++i)
        available[i] = tensors[i].constant;
    available[input] = true;
    const std::set<std::string> ops{"Conv",    "ConvTranspose", "Add",       "Mul",        "Div",
                                    "Erf",     "HardSigmoid",   "Relu",      "ReduceMean", "GlobalAveragePool",
                                    "MaxPool", "Resize",        "Concat",    "Sigmoid",    "AveragePool",
                                    "Softmax", "View",          "LayerNorm", "Attention"};
    for (const auto& item : ns) {
        Node node{item.at("op").get<std::string>(), item.at("inputs").get<std::vector<uint32_t>>(),
                  item.at("output").get<uint32_t>(), item.at("attrs")};
        if (!ops.count(node.op) || !node.attrs.is_object() || node.inputs.empty() || node.inputs.size() > 4 ||
            node.output >= tensors.size() || available[node.output])
            throw std::runtime_error("invalid DET node");
        for (auto i : node.inputs)
            if (i >= available.size() || !available[i])
                throw std::runtime_error("invalid graph topology");
        available[node.output] = true;
        nodes.push_back(std::move(node));
    }
    if (!available[output] || tensors[output].constant || output == input)
        throw std::runtime_error("invalid DET graph output");
    if (!reference_graph()) {
        fold_affine_and_relu(*this);
        if (!gelu_fusion_disabled())
            fold_gelu(nodes, tensors, weights, output);
        fold_transpose_epilogue(nodes, tensors, output);
        fold_silu_epilogue(nodes, tensors, output);
    }
}
Plan::Plan(Context& c, const Model& model, Buffer& constants, uint32_t h, uint32_t w, uint64_t max_bytes)
    : context_(c), model_(model), tensors_(model.tensors), constants_(constants), height_(h), width_(w),
      input_(model.input), output_(model.output) {
    try {
        tensors_[input_].shape = {1, 3, h, w};
        for (size_t step = 0; step < model.nodes.size(); ++step) {
            const auto& n = model.nodes[step];
            const auto& x = tensors_[n.inputs[0]].shape;
            Shape out = x;
            auto requires_inputs = [&](size_t minimum, size_t maximum) {
                if (n.inputs.size() < minimum || n.inputs.size() > maximum)
                    throw std::runtime_error("invalid operator input count");
            };
            if (n.op == "Conv" || n.op == "ConvTranspose") {
                requires_inputs(2, 3);
                auto k = ints(n.attrs, "kernel_shape", {}), s = ints(n.attrs, "strides", {1, 1}),
                     d = ints(n.attrs, "dilations", {1, 1}), p = ints(n.attrs, "pads", {0, 0, 0, 0});
                if (k.size() != 2 || s.size() != 2 || d != std::vector<uint32_t>{1, 1} || p.size() != 4 || !k[0] ||
                    !k[1] || !s[0] || !s[1] || n.attrs.value("auto_pad", std::string("NOTSET")) != "NOTSET")
                    throw std::runtime_error("unsupported convolution geometry");
                const auto& wt = tensors_[n.inputs[1]];
                auto group = n.attrs.value("group", 1u);
                if (!wt.constant || wt.shape[2] != k[0] || wt.shape[3] != k[1] || group == 0)
                    throw std::runtime_error("invalid convolution weights");
                if (n.op == "Conv") {
                    if (x[1] % group || wt.shape[1] != x[1] / group ||
                        (group != 1 && (group != x[1] || wt.shape[0] != x[1])))
                        throw std::runtime_error("only dense / channel-multiplier-one depthwise supported");
                    out[1] = wt.shape[0];
                    for (size_t i = 0; i < 2; ++i) {
                        int64_t span = int64_t(x[2 + i]) + p[i] + p[2 + i] - k[i];
                        if (span < 0)
                            throw std::runtime_error("kernel exceeds input");
                        out[2 + i] = static_cast<uint32_t>(span / s[i] + 1);
                    }
                } else {
                    if (group != 1 || wt.shape[0] != x[1] || k != std::vector<uint32_t>{2, 2} ||
                        s != std::vector<uint32_t>{2, 2} || p != std::vector<uint32_t>{0, 0, 0, 0} ||
                        n.attrs.contains("output_padding") || n.attrs.contains("output_shape"))
                        throw std::runtime_error("only ConvTranspose 2x2 s2 supported");
                    out[1] = wt.shape[1];
                    out[2] = x[2] * 2;
                    out[3] = x[3] * 2;
                }
                if (n.inputs.size() == 3 &&
                    (!tensors_[n.inputs[2]].constant || elements(tensors_[n.inputs[2]].shape) != out[1]))
                    throw std::runtime_error("invalid convolution bias");
            } else if (n.op == "Add" || n.op == "Mul" || n.op == "Div") {
                requires_inputs(2, 2);
                const auto& b = tensors_[n.inputs[1]].shape;
                for (size_t i = 0; i < 4; ++i) {
                    if (x[i] != b[i] && x[i] != 1 && b[i] != 1)
                        throw std::runtime_error("invalid broadcast");
                    out[i] = std::max(x[i], b[i]);
                }
                mode(x, out);
                mode(b, out);
            } else if (n.op == "GlobalAveragePool" || n.op == "ReduceMean") {
                requires_inputs(1, 1);
                if (n.op == "ReduceMean" &&
                    (ints(n.attrs, "axes", {}) != std::vector<uint32_t>{2, 3} || n.attrs.value("keepdims", 1) != 1))
                    throw std::runtime_error("only spatial keepdims ReduceMean supported");
                out[2] = out[3] = 1;
            } else if (n.op == "Concat") {
                if (n.attrs.value("axis", -1) != 1)
                    throw std::runtime_error("only channel concat supported");
                out[1] = 0;
                for (auto i : n.inputs) {
                    auto sh = tensors_[i].shape;
                    if (sh[0] != x[0] || sh[2] != x[2] || sh[3] != x[3])
                        throw std::runtime_error("concat shape mismatch");
                    if (out[1] > 10000000 - sh[1])
                        throw std::runtime_error("concat overflow");
                    out[1] += sh[1];
                }
            } else if (n.op == "Resize") {
                requires_inputs(1, 1);
                auto f = ints(n.attrs, "factors", {});
                if (f.size() != 2 || f[0] < 1 || f[1] < 1 || f[0] > 8 || f[1] > 8 || x[1] % 4 ||
                    n.attrs.value("mode", std::string()) != "nearest" ||
                    n.attrs.value("coordinate_transformation_mode", std::string()) != "asymmetric" ||
                    n.attrs.value("nearest_mode", std::string()) != "floor")
                    throw std::runtime_error("unsupported Resize");
                out[2] *= f[0];
                out[3] *= f[1];
            } else if (n.op == "AveragePool") {
                requires_inputs(1, 1);
                auto k = ints(n.attrs, "kernel_shape", {}), s = ints(n.attrs, "strides", {}),
                     p = ints(n.attrs, "pads", {});
                if (k != std::vector<uint32_t>{3, 2} || s != std::vector<uint32_t>{3, 2} ||
                    p != std::vector<uint32_t>{0, 0, 0, 0} || x[1] % 4 || x[2] < 3 || x[3] < 2 ||
                    n.attrs.value("ceil_mode", 0) != 0 || n.attrs.value("count_include_pad", 0) != 0)
                    throw std::runtime_error("unsupported AveragePool");
                out[2] = (x[2] - 3) / 3 + 1;
                out[3] = (x[3] - 2) / 2 + 1;
            } else if (n.op == "View") {
                requires_inputs(1, 1);
                const auto kind = n.attrs.value("kind", std::string());
                if ((kind == "nc" && (x[2] != 1 || x[3] != 1)) ||
                    ((kind == "ncw" || kind == "nwc" || kind == "nchw") && x[2] != 1) ||
                    (kind != "nc" && kind != "ncw" && kind != "nwc" && kind != "nchw"))
                    throw std::runtime_error("invalid canonical view");
            } else if (n.op == "LayerNorm") {
                requires_inputs(3, 3);
                if (x[2] != 1 || x[1] > 1024 || !tensors_[n.inputs[1]].constant || !tensors_[n.inputs[2]].constant ||
                    elements(tensors_[n.inputs[1]].shape) != x[1] || elements(tensors_[n.inputs[2]].shape) != x[1])
                    throw std::runtime_error("invalid LayerNorm shape");
            } else if (n.op == "Attention") {
                requires_inputs(1, 1);
                const uint32_t heads = n.attrs.at("heads"), dim = n.attrs.at("dim");
                if (!heads || heads > 32 || !dim || dim > 32 || x[2] != 1 || x[1] != 3 * heads * dim)
                    throw std::runtime_error("invalid Attention shape");
                out[1] /= 3;
            } else if (n.op == "Softmax") {
                requires_inputs(1, 1);
                if (x[2] != 1 || x[1] > 18710)
                    throw std::runtime_error("invalid Softmax shape");
            } else if (n.op == "MaxPool") {
                requires_inputs(1, 1);
                if (ints(n.attrs, "kernel_shape", {}) != std::vector<uint32_t>{2, 2} ||
                    ints(n.attrs, "strides", {}) != std::vector<uint32_t>{1, 1} ||
                    n.attrs.value("auto_pad", std::string()) != "SAME_UPPER" || x[1] % 4 ||
                    n.attrs.value("ceil_mode", 0) != 0)
                    throw std::runtime_error("unsupported MaxPool");
            } else {
                requires_inputs(1, 1);
            }
            if (out[0] != 1)
                throw std::runtime_error("batch-one graphs only");
            tensors_[n.output].shape = out;
            for (auto i : n.inputs)
                tensors_[i].last_use = static_cast<int>(step);
        }
        const auto out_shape = tensors_[output_].shape;
        if ((model.task == "det" && out_shape != Shape{1, 1, h, w}) ||
            (model.task == "cls" && out_shape != Shape{1, 2, 1, 1}) ||
            (model.task == "rec" && (out_shape[1] != model.dictionary.size() || out_shape[2] != 1)))
            throw std::runtime_error("unexpected graph output shape");
        tensors_[output_].last_use = static_cast<int>(model.nodes.size());
        uint64_t alignment = std::max<uint64_t>(16, c.properties.limits.minStorageBufferOffsetAlignment);
        std::vector<ArenaRequest> requests;
        std::vector<uint32_t> allocated;
        auto allocate = [&](uint32_t id, int birth) {
            auto& t = tensors_[id];
            t.bytes = elements(t.shape) * 4;
            if (t.bytes > c.properties.limits.maxStorageBufferRange)
                throw std::runtime_error("tensor exceeds device maxStorageBufferRange");
            requests.push_back({t.bytes, birth, std::max(birth, t.last_use)});
            allocated.push_back(id);
        };
        allocate(input_, -1);
        for (size_t i = 0; i < model.nodes.size(); ++i)
            allocate(model.nodes[i].output, static_cast<int>(i));
        auto layout = plan_arena(requests, alignment);
        const uint64_t arena_size = layout.bytes;
        for (size_t i = 0; i < allocated.size(); ++i)
            tensors_[allocated[i]].offset = layout.offsets[i];
        if (arena_size > max_bytes)
            throw std::runtime_error("max_workspace_bytes exceeded: arena needs " + std::to_string(arena_size) +
                                     " bytes; budget " + std::to_string(max_bytes));
        arena_bytes_ = arena_size;
        input_bytes_ = uint64_t(h) * w * 3 * 4;
        output_bytes_ = elements(out_shape) * 4;
        ctc_bytes_ = model.task == "rec" ? uint64_t(out_shape[3]) * 8 : 0;
        const auto required = arena_bytes_ + input_bytes_ + output_bytes_ + ctc_bytes_;
        if (required > max_bytes)
            throw std::runtime_error("max_workspace_bytes exceeded: plan needs " + std::to_string(required) +
                                     " bytes; budget " + std::to_string(max_bytes) + "; input " + std::to_string(w) +
                                     "x" + std::to_string(h));
    } catch (...) {
        close();
        throw;
    }
}
void Plan::attach(SharedWorkspace& workspace) {
    attach_buffers(workspace.arena.get(), workspace.upload.get(), workspace.readback.get(), workspace.ctc.get());
}
void Plan::attach_buffers(Buffer* arena, Buffer* upload, Buffer* readback, Buffer* ctc, bool bgr_only,
                          Buffer* gpu_source) {
    if (gpu_source_ != gpu_source) {
        close();
        gpu_source_ = gpu_source;
    }
    if (command_)
        return;
    arena_ = arena;
    upload_ = upload;
    readback_ = readback;
    ctc_readback_ = ctc;
    bgr_only_ = bgr_only;
    if (!arena_ || !upload_ || (!bgr_only && !readback_) || (ctc_bytes_ && !ctc_readback_))
        throw std::runtime_error("workspace not allocated");
    if (bgr_only && (model_.task != "rec" || !context_.gpu_text_preprocess))
        throw std::invalid_argument("BGR-only plan requires GPU REC preprocessing");
    auto& c = context_;
    const auto& model = model_;
    try {
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = c.queue_family;
        check(vkCreateCommandPool(c.device, &cp, nullptr, &command_pool_), "create graph command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = command_pool_;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(c.device, &ca, &command_), "allocate graph command buffer");
        uint32_t dispatches = 1;
        for (auto& node : model.nodes)
            dispatches += node.op == "Concat" ? uint32_t(node.inputs.size()) : 1;
        const bool bgr = (c.gpu_det_preprocess && model.task == "det") ||
                         (c.gpu_text_preprocess && (model.task == "cls" || model.task == "rec"));
        const uint32_t sets = dispatches * (bgr_only ? 1 : ((ctc_bytes_ ? 2 : 1) + (bgr ? 1 : 0)));
        VkDescriptorPoolSize ds{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 5}; // ConvTranspose has five bindings
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = sets;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ds;
        check(vkCreateDescriptorPool(c.device, &dp, nullptr, &descriptor_pool_), "create graph descriptor pool");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(c.device, &fc, nullptr, &fence_), "create graph fence");
        if (c.gpu_profile) {
            auto create = [&](Profile& profile) {
                profile.capacity = dispatches;
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = dispatches * 2;
                check(vkCreateQueryPool(c.device, &qi, nullptr, &profile.pool), "create GPU profile query pool");
            };
            create(profile_);
            if (ctc_bytes_)
                create(ctc_profile_);
        }
        if (bgr_only) {
            bgr_command_ = command_;
            record(model, true, true);
            return;
        }
        record(model);
        if (bgr) {
            auto normal = command_;
            check(vkAllocateCommandBuffers(c.device, &ca, &bgr_command_), "allocate BGR DET graph command");
            command_ = bgr_command_;
            record(model, model.task == "rec", true);
            command_ = normal;
        }
        if (ctc_bytes_) {
            auto normal = command_;
            check(vkAllocateCommandBuffers(c.device, &ca, &ctc_command_), "allocate CTC graph command");
            command_ = ctc_command_;
            record(model, true);
            command_ = normal;
        }
    } catch (...) {
        close();
        throw;
    }
}
void Plan::close() noexcept {
    if (poisoned_)
        vkDeviceWaitIdle(context_.device); // timeout is not cancellation
    if (fence_) {
        vkDestroyFence(context_.device, fence_, nullptr);
        fence_ = VK_NULL_HANDLE;
    }
    if (command_pool_) {
        vkDestroyCommandPool(context_.device, command_pool_, nullptr);
        command_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(context_.device, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    for (auto* profile : {&profile_, &ctc_profile_}) {
        if (profile->pool)
            vkDestroyQueryPool(context_.device, profile->pool, nullptr);
        *profile = Profile{};
    }
    recording_profile_ = nullptr;
    command_ = ctc_command_ = bgr_command_ = VK_NULL_HANDLE;
}
Plan::~Plan() {
    close();
}
VkDescriptorBufferInfo Plan::binding(uint32_t id) const {
    const auto& t = tensors_.at(id);
    return {t.constant ? constants_.handle : arena_->handle, t.offset, t.bytes};
}
void Plan::dispatch(const std::string& shader, const std::vector<VkDescriptorBufferInfo>& bindings,
                    const std::vector<uint32_t>& push, uint32_t groups, uint32_t groups_y) {
    if (!groups || groups > context_.properties.limits.maxComputeWorkGroupCount[0])
        throw std::runtime_error("dispatch exceeds device workgroup limit");
    auto& pipe =
        context_.pipeline(shader, static_cast<uint32_t>(bindings.size()), static_cast<uint32_t>(push.size() * 4));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &pipe.descriptor_layout;
    VkDescriptorSet set{};
    check(vkAllocateDescriptorSets(context_.device, &ai, &set), "allocate graph descriptor set");
    std::vector<VkWriteDescriptorSet> writes(bindings.size());
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto& wr = writes[i];
        wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr.dstSet = set;
        wr.dstBinding = static_cast<uint32_t>(i);
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo = &bindings[i];
    }
    vkUpdateDescriptorSets(context_.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.handle);
    vkCmdBindDescriptorSets(command_, VK_PIPELINE_BIND_POINT_COMPUTE, pipe.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command_, pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(push.size() * 4),
                       push.data());
    if (!groups_y || groups_y > context_.properties.limits.maxComputeWorkGroupCount[1])
        throw std::runtime_error("dispatch Y limit");
    uint32_t query = 0;
    if (recording_profile_) {
        auto& profile = *recording_profile_;
        if (profile.dispatches.size() >= profile.capacity)
            throw std::runtime_error("GPU profile query capacity exceeded");
        query = static_cast<uint32_t>(profile.dispatches.size()) * 2;
        profile.dispatches.push_back(Json{{"shader", shader}, {"groups", {groups, groups_y, 1}}, {"push", push}});
        // Intentionally serializing diagnostics: these are NOT benchmark timings.
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile.pool, query);
    }
    vkCmdDispatch(command_, groups, groups_y, 1);
    if (recording_profile_)
        vkCmdWriteTimestamp(command_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, recording_profile_->pool, query + 1);
    // Includes RAW, WAW and WAR execution hazards for reused arena regions.
    barrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}
void Plan::record(const Model& model, bool ctc, bool bgr) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command_, &bi), "begin graph recording");
    if (bgr_only_)
        // Cross-command RAW/WAR/WAW hazards: the preceding line finishes all
        // arena reads before this line overwrites the shared tensor storage.
        barrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    recording_profile_ = context_.gpu_profile ? (ctc ? &ctc_profile_ : &profile_) : nullptr;
    if (recording_profile_) {
        recording_profile_->dispatches.clear();
        vkCmdResetQueryPool(command_, recording_profile_->pool, 0, recording_profile_->capacity * 2);
    }
    barrier(command_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_HOST_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    auto linear = [](uint64_t count) { return static_cast<uint32_t>((count + 255) / 256); };
    if (bgr && gpu_source_ && model.task != "det")
        // Crop pixels stay device-local; only a 32-byte orientation header is uploaded.
        dispatch("bgr_shared_text_preprocess",
                 {{gpu_source_->handle, 0, gpu_source_->size}, {upload_->handle, 0, 32}, binding(input_)},
                 {height_, width_}, linear(uint64_t(height_) * width_ * 3));
    else if (bgr)
        // 缩放、归一化直接写入图的 NHWC 输入，不回读中间张量。
        dispatch(
            model.task == "det" ? "bgr_det_preprocess" : "bgr_text_preprocess",
            {{gpu_source_ ? gpu_source_->handle : upload_->handle, 0, gpu_source_ ? gpu_source_->size : upload_->size},
             binding(input_)},
            {height_, width_}, linear(uint64_t(height_) * width_ * 3));
    else
        dispatch("nchw2nhwc", {{upload_->handle, 0, input_bytes_}, binding(input_)}, {height_ * width_, 3, 3},
                 linear(uint64_t(height_) * width_ * 3));
    // Decide device/task policy once per recording, not for every projection.
    // Wider register tiles helped RTX 4060 but regressed our AMD iGPU: keep its
    // qualified kernel. This is a tuning policy, not a precision requirement.
    const bool wide_rec = model.task == "rec" && context_.properties.vendorID == 0x10de &&
                          context_.properties.limits.maxComputeSharedMemorySize >= 20480;
    for (const auto& n : model.nodes) {
        uint32_t xi = n.inputs[0], yi = n.output;
        const auto& x = tensors_[xi].shape;
        const auto& y = tensors_[yi].shape;
        auto xb = binding(xi), yb = binding(yi);
        uint32_t count = static_cast<uint32_t>(elements(y));
        if (n.op == "Conv" || n.op == "ConvTranspose") {
            auto wt = binding(n.inputs[1]);
            auto bias = n.inputs.size() == 3 ? binding(n.inputs[2]) : xb;
            auto k = ints(n.attrs, "kernel_shape", {}), s = ints(n.attrs, "strides", {1, 1}),
                 p = ints(n.attrs, "pads", {0, 0, 0, 0});
            uint32_t flag = (n.inputs.size() == 3 ? 1u : 0u) | (n.attrs.value("fused_relu", false) ? 16u : 0u) |
                            (n.attrs.value("fused_gelu", false) ? 32u : 0u) |
                            (n.attrs.value("fused_sigmoid", false) ? 64u : 0u) |
                            (n.attrs.value("fused_silu", false) ? 128u : 0u);
            if (n.op == "ConvTranspose")
                dispatch("convt2s2", {xb, wt, bias, yb, yb}, {y[3], y[2], y[1], x[1], x[3], flag}, linear(count));
            else if (n.attrs.value("group", 1u) == 1) {
                const bool reference = reference_kernels();
                const auto& weight = tensors_[n.inputs[1]];
                // Keep the large vocabulary projection FP32: a small logit change
                // can change CTC repeat/blank choices and thus the mean score.
                if (!reference && context_.cooperative_matrix && model.task != "cls" &&
                    (!cooperative_det_only_requested() || model.task == "det") && weight.packed_bytes &&
                    !n.attrs.value("fused_silu", false) && y[2] * y[3] >= 32 && y[1] >= 128 && y[1] < 1024 &&
                    x[1] >= 64)
                    dispatch("conv_coop_gemm",
                             {xb, {constants_.handle, weight.packed_offset, weight.packed_bytes}, bias, yb},
                             {y[3], y[2], y[1], x[1], k[0], k[1], s[0], s[1], p[0], p[1], x[3], x[2], flag},
                             (y[2] * y[3] + 63) / 64, (y[1] + 127) / 128);
                else if (!reference && weight.packed_bytes && k == std::vector<uint32_t>{1, 1} && x[1] % 4 == 0 &&
                         s == std::vector<uint32_t>{1, 1} && p == std::vector<uint32_t>{0, 0, 0, 0}) {
                    const bool tiled = !tiled_pointwise_disabled() && y[2] * y[3] >= 16 && y[1] >= 64 && x[1] >= 64;
                    const bool tile64 = tiled && tile64_requested() && y[2] * y[3] >= 64 && y[1] % 4 == 0 &&
                                        context_.properties.limits.maxComputeSharedMemorySize >= 16384;
                    const bool vector = tiled && !tile64 && y[1] % 4 == 0;
                    // Large REC channel projections amortize a wider output tile.
                    // Keep small shapes and DET on their qualified kernels.
                    const bool wide = wide_rec && y[1] >= 512 && x[1] >= 512 && y[2] * y[3] >= 32 && vector;
                    const bool small_m = y[2] * y[3] <= 4 && y[1] >= 64 && x[1] >= 64;
                    dispatch(tile64    ? "conv_pointwise_tiled64"
                             : small_m ? "conv_pointwise_smallm"
                             : wide    ? "conv_pointwise_wide"
                             : vector  ? "conv_pointwise_vector"
                             : tiled   ? "conv_pointwise_tiled"
                                       : "conv_pointwise",
                             {xb, {constants_.handle, weight.packed_offset, weight.packed_bytes}, bias, yb},
                             {y[2] * y[3], y[1], x[1], flag},
                             small_m  ? (y[2] * y[3] * y[1] + 63) / 64
                             : tile64 ? (y[2] * y[3] + 63) / 64
                             : tiled  ? (y[2] * y[3] + 31) / 32
                                      : linear(uint64_t((y[2] * y[3] + 3) / 4) * ((y[1] + 3) / 4)),
                             wide    ? (y[1] + 127) / 128
                             : tiled ? (y[1] + 63) / 64
                                     : 1);
                } else if (!reference && !tiled_gemm_disabled() && weight.packed_bytes && y[2] * y[3] >= 32 &&
                           y[1] >= 32)
                    // NHWC vec4 loads share address arithmetic across channels.
                    // Enable for DET only: changing-width REC streams regressed
                    // despite warmed kernel gains. Retain their established path,
                    // and the scalar loader for RGB inputs/channel tails.
                    dispatch(model.task == "det" && x[1] % 4 == 0 && y[1] % 4 == 0 ? "conv_gemm_vector"
                                                                                   : "conv_gemm_tiled",
                             {xb, {constants_.handle, weight.packed_offset, weight.packed_bytes}, bias, yb},
                             {y[3], y[2], y[1], x[1], k[0], k[1], s[0], s[1], p[0], p[1], x[3], x[2], flag},
                             (y[2] * y[3] + 31) / 32, (y[1] + 63) / 64);
                else
                    dispatch(reference ? "conv_dense" : "conv_gemm", {xb, wt, bias, yb},
                             {y[3], y[2], y[1], x[1], k[0], k[1], s[0], s[1], p[0], p[1], x[3], x[2], flag},
                             reference ? linear(count) : ((y[2] * y[3] + 15) / 16) * ((y[1] + 15) / 16));
            } else {
                const auto& weight = tensors_[n.inputs[1]];
                const bool vector =
                    !reference_kernels() && !vector_depthwise_disabled() && y[1] % 4 == 0 && weight.packed_bytes;
                if (vector)
                    wt = {constants_.handle, weight.packed_offset, weight.packed_bytes};
                dispatch(vector ? "conv_dw4" : "conv_dw", {xb, wt, bias, yb},
                         {y[3], y[2], y[1], k[0], k[1], s[0], s[1], p[0], p[1], x[3], x[2], flag},
                         linear(vector ? count / 4 : count));
            }
        } else if (n.op == "Gelu") {
            dispatch("gelu", {xb, yb}, {count}, linear(count));
        } else if (n.op == "GlobalAveragePool" || n.op == "ReduceMean") {
            dispatch("reduce_hw", {xb, yb}, {x[2] * x[3], x[1]}, x[1]);
        } else if (n.op == "Resize") {
            auto f = ints(n.attrs, "factors", {});
            dispatch("resize4", {xb, yb, xb, xb}, {y[3], y[2], y[1], f[0], f[1], 0, 0, 0}, linear(count / 4));
        } else if (n.op == "AveragePool") {
            dispatch("avgpool4", {xb, yb}, {y[3], y[2], y[1], 3, 2, 3, 2, 0, 0, x[3], x[2]}, linear(count / 4));
        } else if (n.op == "LayerNorm") {
            dispatch("layernorm", {xb, binding(n.inputs[1]), binding(n.inputs[2]), yb},
                     {x[3], x[1], float_bits(n.attrs.at("epsilon").get<float>())}, x[3]);
        } else if (n.op == "Attention") {
            dispatch("attn", {xb, yb},
                     {x[3], n.attrs.at("heads").get<uint32_t>(), n.attrs.at("dim").get<uint32_t>(),
                      float_bits(n.attrs.at("scale").get<float>())},
                     (x[3] + 63) / 64, n.attrs.at("heads").get<uint32_t>());
        } else if (n.op == "Softmax") {
            if (ctc && yi == output_) {
                dispatch("softmax_argmax", {xb, {ctc_readback_->handle, 0, ctc_bytes_}}, {x[3], x[1]}, x[3]);
                continue;
            }
            const bool reference = reference_kernels() || x[1] < 256;
            dispatch(reference ? "softmax" : "softmax_parallel", {xb, yb}, {x[2] * x[3], x[1]},
                     reference ? linear(x[2] * x[3]) : x[2] * x[3]);
        } else if (n.op == "MaxPool") {
            dispatch("maxpool4", {xb, yb}, {x[3], x[2], x[1], 0, 0}, linear(count / 4));
        } else if (n.op == "Concat") {
            uint32_t offset = 0;
            for (auto id : n.inputs) {
                auto channels = tensors_[id].shape[1];
                dispatch("concat_c", {binding(id), yb}, {y[2] * y[3], channels, y[1], offset},
                         linear(uint64_t(y[2]) * y[3] * channels));
                offset += channels;
            }
        } else {
            uint32_t op = n.op == "View"          ? 0
                          : n.op == "Relu"        ? 1
                          : n.op == "HardSigmoid" ? 3
                          : n.op == "HardSwish"   ? 14
                          : n.op == "SiLU"        ? 15
                          : n.op == "Sigmoid"     ? 4
                          : n.op == "Erf"         ? 5
                          : n.op == "Add"         ? 8
                          : n.op == "Mul"         ? 9
                          : n.op == "Div"         ? 11
                                                  : UINT32_MAX;
            if (op == UINT32_MAX)
                throw std::runtime_error("unimplemented DET operator");
            auto bb = n.inputs.size() == 2 ? binding(n.inputs[1]) : xb;
            uint32_t bm = n.inputs.size() == 2 ? mode(tensors_[n.inputs[1]].shape, y) : 0;
            dispatch("elem", {xb, bb, yb},
                     {count, y[1], op, mode(x, y), bm, float_bits(n.attrs.value("alpha", 0.2f)),
                      float_bits(n.attrs.value("beta", 0.5f))},
                     linear(count));
        }
    }
    if (ctc) {
        barrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_HOST_READ_BIT);
        check(vkEndCommandBuffer(command_), "end CTC recording");
        recording_profile_ = nullptr;
        return;
    }
    barrier(command_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{tensors_[output_].offset, 0, output_bytes_};
    vkCmdCopyBuffer(command_, arena_->handle, readback_->handle, 1, &copy);
    barrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_HOST_READ_BIT);
    check(vkEndCommandBuffer(command_), "end graph recording");
    recording_profile_ = nullptr;
}
void Plan::profile_result(bool ctc) {
    auto& profile = ctc ? ctc_profile_ : profile_;
    if (!profile.pool || profile.samples >= 3)
        return;
    const auto count = static_cast<uint32_t>(profile.dispatches.size());
    std::vector<uint64_t> values(count * 2);
    // Fence completion guarantees availability; no WAIT/PARTIAL query flags.
    check(vkGetQueryPoolResults(context_.device, profile.pool, 0, count * 2, values.size() * sizeof(uint64_t),
                                values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT),
          "read GPU profile timestamps");
    const uint64_t mask =
        context_.timestamp_valid_bits == 64 ? UINT64_MAX : ((UINT64_C(1) << context_.timestamp_valid_bits) - 1);
    Json dispatches = Json::array();
    double total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto item = profile.dispatches[i];
        const double ms = static_cast<double>((values[2 * i + 1] - values[2 * i]) & mask) *
                          context_.properties.limits.timestampPeriod / 1e6;
        item["gpu_ms"] = ms;
        item["index"] = i;
        total += ms;
        dispatches.push_back(std::move(item));
    }
    Json result = {{"schema_version", 1},
                   {"task", model_.task},
                   {"input_hw", {height_, width_}},
                   {"device", context_.properties.deviceName},
                   {"ctc", ctc},
                   {"sample", ++profile.samples},
                   {"timestamp_valid_bits", context_.timestamp_valid_bits},
                   {"timestamp_period_ns", context_.properties.limits.timestampPeriod},
                   {"cooperative_matrix", context_.cooperative_matrix},
                   {"dispatch_sum_ms", total},
                   {"dispatches", std::move(dispatches)}};
    const auto line = "LWVK_GPU_PROFILE " + result.dump() + "\n";
    // One bounded record, no image bytes, OCR text, probabilities or model weights.
    static std::mutex output_mutex;
    std::lock_guard<std::mutex> guard(output_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}
double Plan::run(const float* input, float* output, bool ctc) {
    // 耗时包括上传、队列提交、GPU 等待和回读，不是单纯的 shader 时间。
    // Fence 超时不是取消：失败后标记 poisoned，禁止复用仍可能被 GPU 引用的计划。
    if (poisoned_)
        throw std::runtime_error("GPU plan is poisoned; recreate detector after device failure");
    const auto start = std::chrono::steady_clock::now();
    upload_->write(input, static_cast<size_t>(input_bytes_));
    submit_readback(ctc ? ctc_command_ : command_, output, ctc);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
double Plan::run_bgr(const uint8_t* input, uint64_t span, uint32_t width, uint32_t height, uint32_t stride,
                     float* output, bool rotate) {
    if (poisoned_)
        throw std::runtime_error("GPU plan is poisoned; recreate detector after device failure");
    const auto start = std::chrono::steady_clock::now();
    stage_bgr({input, span, width, height, stride}, rotate);
    submit_readback(bgr_command_, output, model_.task == "rec");
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void Plan::stage_bgr(const BgrView& image, bool rotate) {
    const uint64_t span = uint64_t(image.height - 1) * image.stride + uint64_t(image.width) * 3;
    // 动态头支持同一输出尺寸计划复用不同原图；尾部补齐避免 uint 字节提取越界。
    const std::array<uint32_t, 8> header{image.width, image.height, image.stride, rotate ? 1u : 0u, 0, 0, 0, 0};
    if (image.gpu_buffer) {
        if (gpu_source_ != image.gpu_buffer)
            throw std::runtime_error("GPU BGR source binding mismatch");
        upload_->write(header.data(), sizeof(header));
    } else
        upload_->write_parts(header.data(), sizeof(header), image.pixels, static_cast<size_t>(span),
                             (4 - span % 4) % 4);
}
void Plan::bind_gpu_source(Buffer* source) {
    if (gpu_source_ == source)
        return;
    close();
    attach_buffers(arena_, upload_, readback_, ctc_readback_, bgr_only_, source);
}
double Plan::run_gpu_bgr(const BgrView& image, float* output, bool rotate) {
    if (poisoned_)
        throw std::runtime_error("GPU plan is poisoned; recreate handle");
    const auto start = std::chrono::steady_clock::now();
    stage_bgr(image, rotate);
    submit_readback(bgr_command_, output, model_.task == "rec");
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
void Plan::submit_readback(VkCommandBuffer cmd, float* output, bool ctc) {
    check(vkResetFences(context_.device, 1, &fence_), "reset graph fence");
    if (!cmd)
        throw std::runtime_error("CTC plan not available");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult result = vkQueueSubmit(context_.queue, 1, &si, fence_);
    if (result == VK_SUCCESS)
        result = vkWaitForFences(context_.device, 1, &fence_, VK_TRUE, UINT64_C(30000000000));
    if (result != VK_SUCCESS) {
        poisoned_ = true;
        check(result, "submit/wait DET graph (timeout is not cancellation)");
    }
    auto& buffer = ctc ? ctc_readback_ : readback_;
    buffer->read(output, static_cast<size_t>(ctc ? ctc_bytes_ : output_bytes_));
    if (context_.gpu_profile)
        profile_result(ctc);
}
GraphEngine::GraphEngine(const std::filesystem::path& path, uint32_t index, uint64_t max_bytes,
                         const std::string& required_task)
    : model_(path), context_owner_(std::make_shared<Context>(index)), context_(*context_owner_),
      max_bytes_(max_bytes ? max_bytes : UINT64_C(512) * 1024 * 1024), base_max_bytes_(max_bytes_) {
    initialize(required_task);
}
GraphEngine::GraphEngine(const std::filesystem::path& path, std::shared_ptr<Context> context, uint64_t max_bytes,
                         const std::string& required_task)
    : model_(path), context_owner_(std::move(context)),
      context_(context_owner_ ? *context_owner_ : throw std::invalid_argument("missing graph context")),
      max_bytes_(max_bytes ? max_bytes : UINT64_C(512) * 1024 * 1024), base_max_bytes_(max_bytes_) {
    initialize(required_task);
}
void GraphEngine::initialize(const std::string& required_task) {
    if (max_bytes_ > UINT64_C(1024) * 1024 * 1024 || max_bytes_ < 4096)
        throw std::invalid_argument("workspace limit must be 4 KiB..1 GiB");
    if (!required_task.empty() && model_.task != required_task)
        throw std::invalid_argument("model task mismatch");
    uint64_t size = 0, alignment = std::max<uint64_t>(16, context_.properties.limits.minStorageBufferOffsetAlignment);
    for (auto& t : model_.tensors)
        if (t.constant) {
            if (t.bytes > context_.properties.limits.maxStorageBufferRange)
                throw std::runtime_error("constant storage range");
            t.offset = size;
            size += align(t.bytes, alignment);
            if (size > 256ull * 1024 * 1024)
                throw std::runtime_error("packed constants exceed 256 MiB");
        }
    std::set<uint32_t> pointwise; // K-major dense and kernel-major vector depthwise weights
    for (auto& node : model_.nodes)
        if (node.op == "Conv") {
            auto id = node.inputs[1];
            const auto& shape = model_.tensors[id].shape;
            if (node.attrs.value("group", 1u) == 1 || (shape[1] == 1 && shape[0] % 4 == 0))
                pointwise.insert(id);
        }
    for (auto id : pointwise) {
        auto& t = model_.tensors[id];
        t.packed_offset = size;
        t.packed_bytes = uint64_t(t.shape[1]) * t.shape[2] * t.shape[3] * align(t.shape[0], 4) * 4;
        if (t.packed_bytes > context_.properties.limits.maxStorageBufferRange)
            throw std::runtime_error("packed constant storage range");
        size += align(t.packed_bytes, alignment);
        if (size > 256ull * 1024 * 1024)
            throw std::runtime_error("packed constants exceed 256 MiB");
    }
    constants_ = std::make_unique<Buffer>(context_, std::max<uint64_t>(size, 4), false);
    std::vector<uint8_t> packed(static_cast<size_t>(constants_->size));
    for (auto& t : model_.tensors)
        if (t.constant)
            std::memcpy(packed.data() + t.offset, model_.weights.data() + t.file_offset, static_cast<size_t>(t.bytes));
    for (auto id : pointwise) {
        const auto& t = model_.tensors[id];
        const uint32_t N = t.shape[0], C = t.shape[1], span = t.shape[2] * t.shape[3], K = C * span,
                       pad = uint32_t(align(N, 4));
        for (uint32_t n = 0; n < N; ++n)
            for (uint32_t sp = 0; sp < span; ++sp)
                for (uint32_t c = 0; c < C; ++c)
                    std::memcpy(packed.data() + t.packed_offset + 4 * ((uint64_t(sp) * C + c) * pad + n),
                                model_.weights.data() + t.file_offset + 4 * (uint64_t(n) * K + uint64_t(c) * span + sp),
                                4);
    }
    Buffer staging(context_, constants_->size, true);
    staging.write(packed.data(), packed.size());
    constants_->copy_from(staging);
    model_.weights.clear();
    model_.weights.shrink_to_fit();
}
void GraphEngine::ensure_workspace(Plan& plan, uint64_t min_upload) {
    // 尽量保留容量高水位避免频繁分配；高水位之和超预算时回落到本次必要容量。
    auto required = plan.workspace_requirements();
    required[1] = std::max(required[1], min_upload);
    uint64_t minimum = 0;
    // arena 的单个张量范围由 Plan 校验；整个 arena 不是单个 descriptor，不能
    // 按 maxStorageBufferRange 限制整块 arena（会破坏部分设备的默认兼容性）。
    if (required[1] > context_.properties.limits.maxStorageBufferRange)
        throw std::length_error("upload storage range exceeded");
    for (auto bytes : required)
        minimum += bytes;
    if (minimum > max_bytes_)
        throw std::length_error("max_workspace_bytes exceeded (including raw BGR upload)");
    // Optional CLS slots share the same hard budget, not eight separate budgets.
    auto batch_bytes = batch_workspace_bytes();
    if (batch_bytes > max_bytes_ - minimum) {
        clear_batch_workspaces(); // all previous work completed under the engine lock
        batch_bytes = 0;
    }
    auto capacity = required;
    std::unique_ptr<Buffer>* buffers[] = {&workspace_.arena, &workspace_.upload, &workspace_.readback, &workspace_.ctc};
    uint64_t total = 0;
    for (size_t i = 0; i < 4; ++i) {
        capacity[i] = std::max(required[i], *buffers[i] ? (*buffers[i])->size : 0);
        total += capacity[i];
    }
    if (total > max_bytes_ - batch_bytes)
        capacity = required; // shrink high-watermarks rather than exceed the hard budget
    bool changed = false;
    for (size_t i = 0; i < 4; ++i)
        changed |= capacity[i] != (*buffers[i] ? (*buffers[i])->size : 0);
    if (changed) {
        // All graph calls are serialized, and the previous submission has completed.
        // Destroy recorded references before replacing buffers. A failed allocation
        // leaves metadata plans reusable; next request reallocates and rebinds them.
        for (auto& entry : plans_)
            entry.plan->invalidate();
        plan.invalidate();
        for (size_t i = 0; i < 4; ++i)
            if (capacity[i] != (*buffers[i] ? (*buffers[i])->size : 0))
                buffers[i]->reset();
        for (size_t i = 0; i < 4; ++i)
            if (capacity[i] && !*buffers[i])
                *buffers[i] = std::make_unique<Buffer>(context_, capacity[i], i != 0, i >= 2);
    }
    plan.attach_buffers(workspace_.arena.get(), workspace_.upload.get(), workspace_.readback.get(),
                        workspace_.ctc.get(), false, plan.gpu_source_);
}
void GraphEngine::reserve_shared_source(uint64_t bytes, const std::vector<Buffer*>& replaced) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (batch_poisoned_)
        throw std::runtime_error("GPU batch failed; recreate handle");
    for (const auto& entry : plans_)
        if (entry.plan->poisoned())
            throw std::runtime_error("GPU plan failed; recreate handle");
    // Even an unchanged reservation must reject a failed graph before the OCR
    // owner uploads the next image. Timeout does not cancel pending GPU reads.
    if (bytes == source_reservation_ && replaced.empty())
        return;
    if (bytes > base_max_bytes_ - 4096)
        throw std::length_error("max_workspace_bytes exceeded by shared image/crop buffers");
    // Called only by the serialized OCR owner, before replacing shared buffers.
    // Most growth changes one crop slot, not the network's activation arena.
    // Invalidate its recorded references, retaining metadata and all other slots.
    auto retire = [&](Plan& plan) {
        if (plan.gpu_source_ && std::find(replaced.begin(), replaced.end(), plan.gpu_source_) != replaced.end()) {
            plan.invalidate();
            plan.gpu_source_ = nullptr;
        }
    };
    for (auto& entry : plans_)
        retire(*entry.plan);
    for (auto& entry : rec_batch_plans_)
        retire(*entry.plan);
    for (auto& slot : cls_batch_)
        retire(*slot->plan);
    source_reservation_ = bytes;
    max_bytes_ = base_max_bytes_ - bytes;
    uint64_t primary = 0;
    for (auto* b : {workspace_.arena.get(), workspace_.upload.get(), workspace_.readback.get(), workspace_.ctc.get()})
        primary += b ? b->size : 0;
    // A tighter reservation must release capacity before the caller allocates.
    // Normal sized images retain all arenas; only pressure invokes this fallback.
    if (primary + batch_workspace_bytes() > max_bytes_) {
        clear_batch_workspaces();
        if (primary > max_bytes_) {
            for (auto& entry : plans_)
                entry.plan->invalidate();
            workspace_ = {};
        }
    }
}
double GraphEngine::run_det_gpu_bgr(const BgrView& image, uint32_t oh, uint32_t ow, float* output, uint64_t capacity) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (model_.task != "det" || !context_.gpu_crop_preprocess || !image.gpu_buffer ||
        !image.gpu_buffer->belongs_to(context_))
        throw std::invalid_argument("invalid shared DET image");
    prepare(oh, ow);
    if (!output || capacity < elements(plan_->output_shape()))
        throw std::length_error("output capacity too small");
    plan_->bind_gpu_source(image.gpu_buffer);
    return plan_->run_gpu_bgr(image, output);
}
void GraphEngine::prepare(uint32_t h, uint32_t w, uint64_t min_upload) {
    if (batch_poisoned_)
        throw std::runtime_error("GPU CLS batch failed; recreate handle before reuse");
    // 尺寸计划按 LRU 有界缓存。GPU 故障必须先重建句柄，不能靠切换尺寸绕过。
    for (auto& entry : plans_)
        if (entry.plan->poisoned())
            throw std::runtime_error("GPU plan failed; recreate handle before any shape change");
    if ((model_.task == "det" && (!h || !w || h > 960 || w > 960 || h % 32 || w % 32)) ||
        (model_.task == "cls" && (h != 80 || w != 160)) ||
        (model_.task == "rec" && (h != 48 || w < 32 || w > 960 || w % 8)))
        throw std::invalid_argument("invalid input dimensions for model task");
    for (auto& entry : plans_)
        if (entry.height == h && entry.width == w) {
            ensure_workspace(*entry.plan, min_upload);
            entry.stamp = ++stamp_;
            plan_ = entry.plan.get();
            return;
        }
    auto evict = [&] {
        auto it = std::min_element(plans_.begin(), plans_.end(), [](auto& a, auto& b) { return a.stamp < b.stamp; });
        plans_.erase(it);
        plan_ = nullptr;
    };
    if (plans_.size() >= 32)
        evict();
    auto next = std::make_unique<Plan>(context_, model_, *constants_, h, w, max_bytes_);
    ensure_workspace(*next, min_upload);
    plan_ = next.get();
    plans_.push_back({h, w, ++stamp_, std::move(next)});
}
Shape GraphEngine::output_shape(uint32_t h, uint32_t w) {
    std::lock_guard<std::mutex> guard(mutex_);
    prepare(h, w);
    return plan_->output_shape();
}
double GraphEngine::run(const float* input, uint32_t h, uint32_t w, float* output, uint64_t capacity) {
    std::lock_guard<std::mutex> guard(mutex_);
    prepare(h, w);
    if (capacity < elements(plan_->output_shape()))
        throw std::length_error("output capacity too small");
    return plan_->run(input, output);
}
double GraphEngine::run_det_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height, uint32_t stride,
                                uint32_t oh, uint32_t ow, float* output, uint64_t capacity) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (model_.task != "det" || !context_.gpu_det_preprocess)
        throw std::invalid_argument("GPU BGR preprocessing requires an enabled DET engine");
    const auto span = prepare_bgr(input, bytes, width, height, stride, oh, ow);
    if (!output || capacity < elements(plan_->output_shape()))
        throw std::length_error("output capacity too small");
    return plan_->run_bgr(input, span, width, height, stride, output);
}
uint64_t GraphEngine::prepare_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t oh, uint32_t ow) {
    validate_bgr(input, bytes, width, height, stride);
    const uint64_t span = uint64_t(height - 1) * stride + uint64_t(width) * 3;
    if (span > UINT32_MAX - 35)
        throw std::length_error("raw BGR upload exceeds shader address range");
    prepare(oh, ow, 32 + ((span + 3) / 4) * 4);
    return span;
}
double GraphEngine::classify_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height, float* out) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (model_.task != "cls" || !context_.gpu_text_preprocess || !out)
        throw std::invalid_argument("GPU BGR classification requires an enabled CLS engine and output");
    const auto span = prepare_bgr(input, bytes, width, height, width * 3, 80, 160);
    return plan_->run_bgr(input, span, width, height, width * 3, out);
}
TextResult GraphEngine::recognize_bgr(const uint8_t* input, uint64_t bytes, uint32_t width, uint32_t height,
                                      uint32_t stride, uint32_t target, bool rotate, double& ms) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (model_.task != "rec" || !context_.gpu_text_preprocess)
        throw std::invalid_argument("GPU BGR recognition requires an enabled REC engine");
    validate_bgr(input, bytes, width, height, stride);
    const auto scaled = (uint64_t(48) * width + height - 1) / height;
    if (!target)
        target = uint32_t(std::clamp<uint64_t>((scaled + 7) / 8 * 8, 32, 960));
    if (target < 32 || target > 960 || target % 8)
        throw std::invalid_argument("REC width must be 0 (auto) or 32..960, multiple of 8");
    const auto span = prepare_bgr(input, bytes, width, height, stride, 48, target);
    std::vector<float> pairs(uint64_t(plan_->output_shape()[3]) * 2);
    ms = plan_->run_bgr(input, span, width, height, stride, pairs.data(), rotate);
    return decode_pairs(pairs);
}
TextResult GraphEngine::recognize(const float* input, uint32_t width, double& ms) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (model_.task != "rec")
        throw std::invalid_argument("REC model required");
    prepare(48, width);
    const uint32_t rows = plan_->output_shape()[3];
    std::vector<float> pairs(uint64_t(rows) * 2);
    ms = plan_->run(input, pairs.data(), true);
    return decode_pairs(pairs);
}
TextResult GraphEngine::decode_pairs(const std::vector<float>& pairs) {
    TextResult result;
    uint32_t previous = UINT32_MAX, count = 0;
    double sum = 0;
    for (size_t i = 0; i < pairs.size() / 2; ++i) {
        float label = pairs[i * 2], score = pairs[i * 2 + 1];
        if (!std::isfinite(label) || label < 0 || label >= model_.dictionary.size() || label != std::floor(label) ||
            !std::isfinite(score) || score < 0 || score > 1)
            throw std::runtime_error("invalid GPU CTC output");
        auto best = uint32_t(label);
        if (best && best != previous) {
            result.text += model_.dictionary[best];
            sum += score;
            ++count;
        }
        previous = best;
    }
    result.score = count ? float(sum / count) : 0;
    return result;
}
} // namespace lwvk
