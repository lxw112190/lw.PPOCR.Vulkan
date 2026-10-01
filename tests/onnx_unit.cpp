#include "onnx_import.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
using namespace lwvk;
namespace {
using Bytes = std::vector<uint8_t>;
void var(Bytes& out, uint64_t value) {
    do {
        uint8_t b = uint8_t(value & 127);
        value >>= 7;
        out.push_back(b | (value ? 128 : 0));
    } while (value);
}
void field(Bytes& out, unsigned id, const Bytes& bytes) {
    var(out, (id << 3) | 2);
    var(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}
void text(Bytes& out, unsigned id, const std::string& value) {
    field(out, id, Bytes(value.begin(), value.end()));
}
Bytes malformed_graph(const std::string& op, bool input) {
    Bytes node, graph, in, out, model, opset{0x10, 13};
    if (input)
        text(node, 1, "input");
    text(node, 2, "output");
    text(node, 4, op);
    text(in, 1, "input");
    text(out, 1, "output");
    field(graph, 1, node);
    field(graph, 11, in);
    field(graph, 12, out);
    field(model, 7, graph);
    field(model, 8, opset);
    return model;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error("model root required");
        std::filesystem::path root = std::filesystem::u8path(argv[1]);
        std::vector<uint8_t> weights;
        for (auto variant : {"tiny", "small", "medium"})
            for (auto task : {"det", "cls", "rec"}) {
                std::ifstream file(root / (std::string("ppocrv6-") + variant) / (std::string(task) + ".onnx"),
                                   std::ios::binary);
                if (!file)
                    throw std::runtime_error("missing ONNX asset");
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
                auto j = import_onnx(bytes, weights);
                if (j.at("nodes").empty() || weights.empty())
                    throw std::runtime_error("empty lowered model");
                const uint32_t expected = std::string(task) == "rec"   ? (std::string(variant) == "tiny" ? 6906 : 18710)
                                          : std::string(task) == "cls" ? 2
                                                                       : 0;
                if (j.at("classes") != expected)
                    throw std::runtime_error("class count mismatch");
                std::cout << variant << " " << task << ": " << j.at("nodes").size() << " nodes\n";
                for (size_t n : {size_t(0), size_t(1), bytes.size() / 2, bytes.size() - 1}) {
                    std::vector<uint8_t> short_bytes(bytes.begin(), bytes.begin() + n);
                    bool rejected = false;
                    try {
                        import_onnx(short_bytes, weights);
                    } catch (const std::exception&) {
                        rejected = true;
                    }
                    if (!rejected)
                        throw std::runtime_error("truncated protobuf accepted");
                }
            }
        std::mt19937 rng(27);
        for (auto op : {"Sigmoid", "Conv", "Add", "Reshape", "Identity"}) {
            auto bytes = malformed_graph(op, std::string(op) != "Sigmoid");
            bool rejected = false;
            try {
                import_onnx(bytes, weights);
            } catch (const std::exception&) {
                rejected = true;
            }
            if (!rejected)
                throw std::runtime_error("invalid arity/Identity-only graph accepted");
        }
        for (unsigned i = 0; i < 2000; ++i) {
            std::vector<uint8_t> bytes(rng() % 256);
            for (auto& b : bytes)
                b = uint8_t(rng());
            try {
                import_onnx(bytes, weights);
            } catch (const std::exception&) {
            }
        }
        std::cout << "PASS: nine ONNX imports, truncation rejection and 2000 bounded random inputs\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
