#pragma once
#include <cstdint>
#include <vector>
namespace lwvk {
struct Node;
struct Tensor;
// Internal lowering only: no new accepted ONNX/model-file operator or public ABI.
uint32_t fold_pointwise_bias(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output);
uint32_t fold_gelu(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, const std::vector<uint8_t>& weights,
                   uint32_t output, bool conv_epilogue = true);
uint32_t fold_transpose_epilogue(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output);
uint32_t fold_silu_epilogue(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output);
// Only the canonical FP32 1/6, 1/2 epilogue; arbitrary alpha/beta stays separate.
uint32_t fold_conv_hardswish(std::vector<Node>& nodes, const std::vector<Tensor>& tensors, uint32_t output);
} // namespace lwvk
