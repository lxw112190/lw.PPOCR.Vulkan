#include "graph_optimizer.hpp"
#include "graph.hpp"
#include <cmath>
#include <cstring>
namespace lwvk {
uint32_t fold_gelu(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, const std::vector<uint8_t>& weights,
                   uint32_t output, bool conv_epilogue) {
    auto counts = [&] {
        // 图输出也视为一次使用，避免融合后删除客户端可见的输出或残差分支。
        std::vector<uint32_t> uses(tensors.size());
        for (const auto& n : nodes)
            for (auto id : n.inputs)
                ++uses.at(id);
        ++uses.at(output);
        return uses;
    };
    auto scalar = [&](uint32_t id, float expected) {
        const auto& t = tensors.at(id);
        if (!t.constant || t.shape != Shape{1, 1, 1, 1} || t.bytes != 4 || t.file_offset > weights.size() ||
            weights.size() - t.file_offset < 4)
            return false;
        float value;
        std::memcpy(&value, weights.data() + t.file_offset, 4);
        return std::isfinite(value) && value == expected;
    };
    auto binary = [&](const Node& n, const char* op, uint32_t a, uint32_t b) {
        return n.op == op && n.inputs.size() == 2 &&
               ((n.inputs[0] == a && n.inputs[1] == b) || (n.inputs[0] == b && n.inputs[1] == a));
    };
    auto uses = counts();
    std::vector<Node> result;
    uint32_t fused = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto n = nodes[i];
        if (n.op == "Div" && n.inputs.size() == 2 && scalar(n.inputs[1], std::sqrt(2.0f)) && i + 4 < nodes.size()) {
            const auto& erf = nodes[i + 1];
            const auto& add = nodes[i + 2];
            const auto& mul = nodes[i + 3];
            const auto& half = nodes[i + 4];
            uint32_t one = UINT32_MAX, scale = UINT32_MAX;
            if (add.op == "Add" && add.inputs.size() == 2) {
                if (add.inputs[0] == erf.output)
                    one = add.inputs[1];
                else if (add.inputs[1] == erf.output)
                    one = add.inputs[0];
            }
            if (half.op == "Mul" && half.inputs.size() == 2) {
                if (half.inputs[0] == mul.output)
                    scale = half.inputs[1];
                else if (half.inputs[1] == mul.output)
                    scale = half.inputs[0];
            }
            if (erf.op == "Erf" && erf.inputs == std::vector<uint32_t>{n.output} && one != UINT32_MAX &&
                scale != UINT32_MAX && scalar(one, 1.0f) && scalar(scale, 0.5f) &&
                binary(mul, "Mul", n.inputs[0], add.output) && uses[n.output] == 1 && uses[erf.output] == 1 &&
                uses[add.output] == 1 && uses[mul.output] == 1) {
                n.op = "Gelu";
                n.inputs.resize(1);
                n.output = half.output;
                n.attrs = Json::object();
                i += 4;
                ++fused;
            }
        }
        result.push_back(std::move(n));
    }
    nodes = std::move(result);
    if (!conv_epilogue)
        return fused;
    uses = counts();
    result.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto n = nodes[i];
        if (n.op == "Conv" && n.attrs.value("group", 1u) == 1 && !n.attrs.value("fused_relu", false) &&
            uses[n.output] == 1 && i + 1 < nodes.size() && nodes[i + 1].op == "Gelu" &&
            nodes[i + 1].inputs == std::vector<uint32_t>{n.output}) {
            n.output = nodes[++i].output;
            n.attrs["fused_gelu"] = true;
        }
        result.push_back(std::move(n));
    }
    nodes = std::move(result);
    return fused;
}
uint32_t fold_transpose_epilogue(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output) {
    std::vector<uint32_t> uses(tensors.size());
    for (const auto& n : nodes)
        for (auto id : n.inputs)
            ++uses.at(id);
    ++uses.at(output);
    std::vector<Node> result;
    uint32_t fused = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto n = nodes[i];
        if (n.op == "ConvTranspose" && n.inputs.size() == 2 && n.attrs.value("group", 1u) == 1 && uses[n.output] == 1 &&
            i + 1 < nodes.size()) {
            const auto& add = nodes[i + 1];
            uint32_t bias = UINT32_MAX;
            if (add.op == "Add" && add.inputs.size() == 2) {
                if (add.inputs[0] == n.output)
                    bias = add.inputs[1];
                else if (add.inputs[1] == n.output)
                    bias = add.inputs[0];
            }
            if (bias != UINT32_MAX) {
                const auto channels = tensors.at(n.inputs[1]).shape[1];
                const auto& t = tensors.at(bias);
                if (t.constant && t.shape == Shape{1, channels, 1, 1} && t.bytes == uint64_t(channels) * 4) {
                    n.inputs.push_back(bias);
                    n.output = add.output;
                    ++i;
                    ++fused;
                }
            }
        }
        if (n.op == "ConvTranspose" && uses[n.output] == 1 && i + 1 < nodes.size() &&
            !n.attrs.value("fused_relu", false) && !n.attrs.value("fused_sigmoid", false) &&
            nodes[i + 1].inputs == std::vector<uint32_t>{n.output} &&
            (nodes[i + 1].op == "Relu" || nodes[i + 1].op == "Sigmoid")) {
            n.attrs[nodes[i + 1].op == "Relu" ? "fused_relu" : "fused_sigmoid"] = true;
            n.output = nodes[++i].output;
            ++fused;
        }
        result.push_back(std::move(n));
    }
    nodes = std::move(result);
    return fused;
}
uint32_t fold_silu_epilogue(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output) {
    std::vector<uint32_t> uses(tensors.size());
    for (const auto& node : nodes)
        for (auto id : node.inputs)
            ++uses.at(id);
    ++uses.at(output);
    std::vector<Node> result;
    uint32_t fused = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto node = nodes[i];
        // Do not remove a shared/observable intermediate or combine activations.
        // All group-one FP32 kernels support the epilogue; depthwise stays unchanged.
        if (node.op == "Conv" && node.attrs.value("group", 1u) == 1 && uses.at(node.output) == 1 &&
            !node.attrs.value("fused_relu", false) && !node.attrs.value("fused_gelu", false) &&
            !node.attrs.value("fused_sigmoid", false) && !node.attrs.value("fused_silu", false) &&
            i + 1 < nodes.size() && nodes[i + 1].op == "SiLU" &&
            nodes[i + 1].inputs == std::vector<uint32_t>{node.output}) {
            node.output = nodes[++i].output;
            node.attrs["fused_silu"] = true;
            ++fused;
        }
        result.push_back(std::move(node));
    }
    nodes = std::move(result);
    return fused;
}
} // namespace lwvk
