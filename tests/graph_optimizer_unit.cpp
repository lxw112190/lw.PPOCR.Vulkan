#include "graph.hpp"
#include "graph_optimizer.hpp"
#include <cstring>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace lwvk;
namespace {
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Fixture {
    std::vector<Tensor> tensors = std::vector<Tensor>(16);
    std::vector<uint8_t> weights = std::vector<uint8_t>(12);
    std::vector<Node> nodes;
    Fixture() {
        for (uint32_t id = 7; id <= 9; ++id) {
            auto& t = tensors[id];
            t.constant = true;
            t.bytes = 4;
            t.file_offset = (id - 7) * 4;
        }
        set(7, std::sqrt(2.0f));
        set(8, 1);
        set(9, .5f);
        nodes = {{"Div", {0, 7}, 1, Json::object()},
                 {"Erf", {1}, 2, Json::object()},
                 {"Add", {2, 8}, 3, Json::object()},
                 {"Mul", {0, 3}, 4, Json::object()},
                 {"Mul", {4, 9}, 5, Json::object()}};
    }
    void set(uint32_t id, float v) {
        std::memcpy(weights.data() + (id - 7) * 4, &v, 4);
    }
    uint32_t fold(uint32_t output = 5, bool epilogue = true) {
        return fold_gelu(nodes, tensors, weights, output, epilogue);
    }
    void conv() {
        nodes.insert(nodes.begin(), {"Conv", {10, 11}, 0, {{"group", 1}}});
    }
};
} // namespace
int main() {
    try {
        unsigned tests = 0;
        for (unsigned condition = 0; condition < 22; ++condition) {
            Fixture f;
            f.tensors[11].constant = true;
            f.tensors[11].shape = {6, 4, 1, 1};
            f.tensors[7].shape = {1, 6, 1, 1};
            f.tensors[7].bytes = 24;
            f.nodes = {{"Conv", {0, 11}, 1, Json::object()}, {"Add", {1, 7}, 2, Json::object()}};
            uint32_t output = 2;
            if (condition == 1)
                f.nodes[1].inputs = {7, 1};
            if (condition == 2)
                output = 1;
            if (condition == 3)
                f.nodes.push_back({"Mul", {1, 0}, 12, Json::object()});
            if (condition == 4)
                f.nodes[0].inputs.push_back(8);
            if (condition == 5)
                f.nodes[0].attrs["group"] = 4;
            if (condition == 6)
                f.nodes[0].attrs["fused_relu"] = true;
            if (condition == 7)
                f.nodes[0].attrs["fused_silu"] = true;
            if (condition == 8)
                f.tensors[7].constant = false;
            if (condition == 9)
                f.tensors[7].shape = {1, 1, 1, 6};
            if (condition == 10)
                f.tensors[7].bytes = 20;
            if (condition == 11)
                f.tensors[11].shape[1] = 3;
            if (condition == 12)
                f.nodes[0].attrs["pads"] = {1, 0, 0, 0};
            if (condition == 13)
                f.tensors[11].shape[2] = 3;
            if (condition == 14) {
                f.tensors[11].shape = {6, 3, 3, 3};
                f.nodes[0].attrs["strides"] = {2, 2};
                f.nodes[0].attrs["pads"] = {1, 1, 1, 1};
            }
            if (condition == 15)
                f.tensors[11].constant = false;
            if (condition == 16)
                f.nodes[0].attrs["fused_gelu"] = true;
            if (condition == 17)
                f.nodes[0].attrs["fused_sigmoid"] = true;
            if (condition == 18)
                f.tensors[11].shape[0] = 0;
            if (condition == 19)
                f.tensors[11].shape[1] = 2;
            if (condition == 20)
                f.tensors[11].shape[2] = 0;
            if (condition == 21)
                f.nodes[0].op = "ConvTranspose";
            const bool expected = condition < 2;
            require(fold_pointwise_bias(f.nodes, f.tensors, output) == (expected ? 1u : 0u),
                    "unsafe pointwise bias fold");
            if (expected)
                require(f.nodes.size() == 1 && f.nodes[0].inputs == std::vector<uint32_t>{0, 11, 7} &&
                            f.nodes[0].output == 2,
                        "pointwise bias lost output/operand");
            ++tests;
        }
        for (unsigned condition = 0; condition < 8; ++condition) {
            Fixture f;
            f.nodes = {{"Conv", {0, 11}, 1, Json::object()}, {"SiLU", {1}, 2, Json::object()}};
            uint32_t output = 2;
            if (condition == 1)
                output = 1;
            if (condition == 2)
                f.nodes.push_back({"Add", {1, 0}, 12, Json::object()});
            if (condition == 3)
                f.nodes[0].attrs["group"] = 4;
            if (condition == 4)
                f.nodes[0].attrs["fused_relu"] = true;
            if (condition == 5)
                f.nodes[0].attrs["fused_gelu"] = true;
            if (condition == 6)
                f.nodes[1].inputs.push_back(0);
            if (condition == 7)
                f.nodes[1].op = "HardSwish";
            auto count = fold_silu_epilogue(f.nodes, f.tensors, output);
            require(count == (condition == 0 ? 1u : 0u), "unsafe SiLU epilogue fold");
            if (condition == 0)
                require(f.nodes.size() == 1 && f.nodes[0].output == 2 && f.nodes[0].attrs.at("fused_silu") == true,
                        "SiLU epilogue lost output");
            ++tests;
        }
        for (bool commute : {false, true}) {
            Fixture f;
            if (commute) {
                f.nodes[2].inputs = {8, 2};
                f.nodes[3].inputs = {3, 0};
                f.nodes[4].inputs = {9, 4};
            }
            auto weights = f.weights;
            require(f.fold() == 1 && f.nodes.size() == 1, "canonical chain not fused");
            require(f.nodes[0].op == "Gelu" && f.nodes[0].inputs == std::vector<uint32_t>{0} && f.nodes[0].output == 5,
                    "GELU IO changed");
            require(f.weights == weights, "shared constants mutated");
            ++tests;
        }
        for (uint32_t intermediate = 1; intermediate <= 4; ++intermediate) {
            Fixture f;
            auto original = f.nodes;
            require(f.fold(intermediate) == 0 && f.nodes.size() == original.size(), "graph output was removed");
            ++tests;
            Fixture branch;
            branch.nodes.push_back({"Add", {intermediate, 0}, 12, Json::object()});
            require(branch.fold() == 0 && branch.nodes.size() == 6, "shared intermediate removed");
            ++tests;
        }
        for (float value : {1.4f, -1.414213538f, 0.0f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
            Fixture f;
            f.set(7, value);
            require(f.fold() == 0, "noncanonical divisor fused");
            ++tests;
        }
        for (uint32_t id : {8u, 9u}) {
            Fixture f;
            f.set(id, .7f);
            require(f.fold() == 0, "noncanonical scalar fused");
            ++tests;
        }
        {
            Fixture f;
            f.tensors[7].file_offset = UINT64_MAX;
            require(f.fold() == 0, "out-of-bounds scalar read");
            ++tests;
        }
        {
            Fixture f;
            f.tensors[8].shape = {1, 2, 1, 1};
            require(f.fold() == 0, "channel vector mistaken for scalar");
            ++tests;
        }
        {
            Fixture f;
            f.nodes[1].inputs.push_back(0);
            require(f.fold() == 0, "bad Erf arity fused");
            ++tests;
        }
        {
            Fixture f;
            f.nodes[3].inputs[0] = 10;
            require(f.fold() == 0, "wrong original operand fused");
            ++tests;
        }
        {
            Fixture f;
            f.nodes[0].inputs = {7, 0};
            require(f.fold() == 0, "commuted Div fused");
            ++tests;
        }
        {
            Fixture f;
            f.conv();
            require(f.fold() == 1 && f.nodes.size() == 1 && f.nodes[0].op == "Conv" && f.nodes[0].output == 5 &&
                        f.nodes[0].attrs.at("fused_gelu") == true,
                    "dense epilogue not fused");
            ++tests;
        }
        {
            Fixture f;
            f.conv();
            require(f.fold(5, false) == 1 && f.nodes.size() == 2, "standalone path lost");
            ++tests;
        }
        for (unsigned condition = 0; condition < 4; ++condition) {
            Fixture f;
            f.conv();
            uint32_t output = 5;
            if (condition == 0)
                f.nodes[0].attrs["group"] = 32;
            if (condition == 1)
                f.nodes[0].attrs["fused_relu"] = true;
            if (condition == 2)
                f.nodes.push_back({"Add", {0, 10}, 12, Json::object()});
            if (condition == 3)
                output = 0;
            require(f.fold(output) == 1 && f.nodes[0].output == 0 && !f.nodes[0].attrs.contains("fused_gelu"),
                    "unsafe convolution epilogue fused");
            ++tests;
        }
        for (auto op : {"Relu", "Sigmoid"})
            for (bool commute : {false, true}) {
                Fixture f;
                f.tensors[11].constant = true;
                f.tensors[11].shape = {8, 8, 2, 2};
                f.tensors[8].shape = {1, 8, 1, 1};
                f.tensors[8].bytes = 32;
                f.nodes = {
                    {"ConvTranspose", {0, 11}, 1, Json::object()},
                    {"Add", commute ? std::vector<uint32_t>{8, 1} : std::vector<uint32_t>{1, 8}, 2, Json::object()},
                    {op, {2}, 3, Json::object()}};
                require(fold_transpose_epilogue(f.nodes, f.tensors, 3) == 2 && f.nodes.size() == 1 &&
                            f.nodes[0].inputs == std::vector<uint32_t>{0, 11, 8} && f.nodes[0].output == 3 &&
                            f.nodes[0].attrs.at(std::string(op) == "Relu" ? "fused_relu" : "fused_sigmoid") == true,
                        "transpose epilogue mismatch");
                ++tests;
            }
        for (unsigned condition = 0; condition < 5; ++condition) {
            Fixture f;
            f.tensors[11].shape = {8, 8, 2, 2};
            f.tensors[8].shape = {1, 8, 1, 1};
            f.tensors[8].bytes = 32;
            f.nodes = {{"ConvTranspose", {0, 11}, 1, Json::object()}, {"Add", {1, 8}, 2, Json::object()}};
            uint32_t output = 2;
            if (condition == 0)
                f.tensors[8].constant = false;
            if (condition == 1)
                f.tensors[8].shape = {1, 1, 8, 1};
            if (condition == 2)
                f.tensors[8].bytes = 4;
            if (condition == 3)
                output = 1;
            if (condition == 4)
                f.nodes.push_back({"Mul", {1, 0}, 12, Json::object()});
            require(fold_transpose_epilogue(f.nodes, f.tensors, output) == 0 && f.nodes[0].inputs.size() == 2,
                    "unsafe transpose bias fused");
            ++tests;
        }
        for (unsigned condition = 0; condition < 17; ++condition) {
            Fixture f;
            f.tensors[11].constant = true;
            f.tensors[11].shape = {4, 1, 3, 3};
            f.nodes = {{"Conv", {0, 11}, 1, {{"group", 4}}},
                       {"HardSwish", {1}, 2, {{"alpha", 1.0f / 6.0f}, {"beta", 0.5f}}}};
            uint32_t output = 2;
            if (condition == 1)
                output = 1;
            if (condition == 2)
                f.nodes.push_back({"Add", {1, 0}, 12, Json::object()});
            if (condition == 3)
                f.nodes[0].attrs["group"] = 1;
            if (condition == 4)
                f.tensors[11].shape[0] = 8;
            if (condition == 5)
                f.tensors[11].shape[1] = 4;
            if (condition == 6)
                f.tensors[11].constant = false;
            if (condition == 7)
                f.nodes[1].attrs["alpha"] = 0.2f;
            if (condition == 8)
                f.nodes[1].attrs["beta"] = 0.6f;
            if (condition == 9)
                f.nodes[1].attrs["alpha"] = std::nextafter(1.0f / 6.0f, 1.0f);
            if (condition == 10)
                f.nodes[1].op = "SiLU";
            if (condition == 11)
                f.nodes[0].attrs["fused_relu"] = true;
            if (condition == 12)
                f.nodes[0].attrs["fused_gelu"] = true;
            if (condition == 13)
                f.nodes[0].attrs["fused_silu"] = true;
            if (condition == 14)
                f.nodes[0].attrs["fused_sigmoid"] = true;
            if (condition == 15)
                f.nodes[1].inputs = {0};
            if (condition == 16)
                f.nodes[0].inputs.push_back(7);
            const bool expected = condition == 0 || condition == 16;
            require(fold_conv_hardswish(f.nodes, f.tensors, output) == (expected ? 1u : 0u),
                    "unsafe depthwise HardSwish fold");
            if (expected)
                require(f.nodes.size() == 1 && f.nodes[0].output == 2 &&
                            f.nodes[0].attrs.value("fused_hardswish", false),
                        "lost depthwise HardSwish epilogue");
            ++tests;
        }
        for (unsigned condition = 0; condition < 9; ++condition) {
            Fixture f;
            f.tensors[11].constant = true;
            f.tensors[11].shape = {16, 8, 1, 1};
            f.nodes = {{"Conv", {0, 11}, 1, {{"group", 1}}},
                       {"HardSwish", {1}, 2, {{"alpha", 1.0f / 6.0f}, {"beta", 0.5f}}}};
            if (condition == 1)
                f.tensors[11].shape[1] = 3;
            if (condition == 2)
                f.tensors[11].shape[2] = 3;
            if (condition == 3)
                f.nodes[0].attrs["strides"] = std::vector<uint32_t>{2, 2};
            if (condition == 4)
                f.nodes[0].attrs["pads"] = std::vector<uint32_t>{1, 1, 1, 1};
            if (condition == 5)
                f.nodes[0].attrs["group"] = 2;
            if (condition == 6)
                f.nodes[0].attrs["fused_hardswish"] = true;
            if (condition == 7)
                f.tensors[11].shape[1] = 0;
            if (condition == 8)
                f.nodes[0].inputs.push_back(7);
            const bool expected = condition == 0 || condition == 8;
            require(fold_conv_hardswish(f.nodes, f.tensors, 2) == (expected ? 1u : 0u),
                    "unsafe pointwise HardSwish fold");
            ++tests;
        }
        std::cout << "PASS: " << tests << " canonical/scalar/arity/branch/output/epilogue optimizer gates\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
