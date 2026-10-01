#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lwvk {
struct TextResult {
    std::string text;
    float score{};
};
// Greedy CTC: class 0 is blank; suppress adjacent repeats before removing blank.
// Preserve exact dictionary indexes, including the trained empty label.
TextResult decode_ctc(const float* probabilities, uint32_t rows, const std::vector<std::string>& dictionary);
} // namespace lwvk
