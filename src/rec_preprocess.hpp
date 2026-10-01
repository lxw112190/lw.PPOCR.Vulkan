#pragma once
#include <cstdint>
#include <vector>
namespace lwvk {
struct RecInput {
    uint32_t width{};
    std::vector<float> data;
};
RecInput preprocess_rec_bgr(const uint8_t* pixels, uint64_t buffer_bytes, uint32_t width, uint32_t height,
                            uint32_t stride, uint32_t target_width);
} // namespace lwvk
