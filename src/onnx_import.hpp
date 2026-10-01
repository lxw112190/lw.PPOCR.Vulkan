#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <vector>
namespace lwvk {
// Bounded protobuf reader + PP-OCR lowering; no protobuf/ORT dependency.
nlohmann::json import_onnx(const std::vector<uint8_t>& bytes, std::vector<uint8_t>& weights);
} // namespace lwvk
