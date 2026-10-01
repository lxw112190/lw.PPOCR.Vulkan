#pragma once
#include <cstdint>
#include <vector>
namespace lwvk {
std::vector<float> preprocess_det_bgr(const uint8_t*, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
}
